/*
 * aks-crypt: SEP-less reversible software keystore crypto for vresearch.
 *
 * Without a SEP, AppleSEPKeyStore's wrap/unwrap (IOConnectCallMethod selectors
 * 10 and 11) return kIOReturnNotReady (0xe00002d8): the kernel can't wrap the
 * class keys that usermanagerd needs to build the mobile user's persona, so
 * lsd's persona container query returns nothing, lsd crash-loops and
 * SpringBoard (which depends on it) never draws a frame.
 *
 * iOS 16 sidestepped this by building with data protection off. iOS 26's
 * kernel is closed, so instead the wrap/unwrap handlers are patched to issue
 * an HVC (xnu_patch.c, patch_kc_aks_crypt) and this file answers it with real,
 * reversible AES-256-CBC + HMAC-SHA256 over a fixed machine-local key. Because
 * the transform is genuine and invertible, a key wrapped on one call unwraps
 * back to the same bytes on a later call (even across boots, since the key and
 * format are stable and the wrapped blobs are persisted to the data disk) --
 * unlike a blind "success" stub, which would hand out garbage keys.
 *
 * Security: this is an emulator convenience, not a SEP. The wrapping key lives
 * in the VMM process; there is no hardware isolation, no anti-rollback, and the
 * per-plaintext IV is derived deterministically (so wrap is reproducible, which
 * trades away IND-CPA). Do not treat data wrapped this way as protected.
 */
#include "qemu/osdep.h"
#include "hw/arm/vresearch_akscrypt.h"
#include "cpu.h"
#include "crypto/cipher.h"
#include "crypto/hash.h"
#include "crypto/hmac.h"
#include "qemu/error-report.h"

/*
 * Fixed, machine-local wrapping key. Stable across boots so persisted wrapped
 * blobs stay readable. Emulator-only; carries no real secrecy (see file head).
 */
static const uint8_t aks_machine_key[32] = {
    0x41, 0x53, 0x6f, 0x66, 0x74, 0x41, 0x4b, 0x53, /* "ASoftAKS" */
    0x76, 0x72, 0x65, 0x73, 0x65, 0x61, 0x72, 0x63, /* "vresearc" */
    0x68, 0x32, 0x36, 0x6b, 0x65, 0x79, 0x73, 0x74, /* "h26keyst" */
    0x6f, 0x72, 0x65, 0x6d, 0x6f, 0x64, 0x65, 0x6c, /* "oremodel" */
};

/*
 * Wrapped-blob layout (little-endian), all fields fixed-width so unwrap can
 * parse without the caller's help:
 *   [0]  magic   4   "AKW1"
 *   [4]  class   4   key class the blob was wrapped under
 *   [8]  len     4   plaintext length
 *   [12] iv     16   CBC IV (derived from key+class+plaintext)
 *   [28] ct      N   AES-256-CBC ciphertext, N = roundup(len,16)
 *   [28+N] mac  16   HMAC-SHA256(mac_key, bytes [0 .. 28+N))[:16]
 */
#define AKW_MAGIC0 0x41  /* 'A' */
#define AKW_MAGIC1 0x4b  /* 'K' */
#define AKW_MAGIC2 0x57  /* 'W' */
#define AKW_MAGIC3 0x31  /* '1' */
#define AKW_HDR    28
#define AKW_MAC    16
/* Keystore class keys are small; cap the plaintext so a blob fits the guest's
 * handler buffer (0x80 bytes for wrap). roundup(len,16) <= 80 => len <= 80. */
#define AKW_MAX_PLAIN 64

static bool aks_enabled;

/* out must be >= 32 bytes. Concatenates parts and SHA-256s them. */
static void aks_sha256(uint8_t out[32],
                       const void *a, size_t alen,
                       const void *b, size_t blen,
                       const void *c, size_t clen)
{
    uint8_t *buf;
    size_t n = alen + blen + clen, got = 32;
    uint8_t *res = out;

    buf = g_malloc(n ? n : 1);
    if (alen) {
        memcpy(buf, a, alen);
    }
    if (blen) {
        memcpy(buf + alen, b, blen);
    }
    if (clen) {
        memcpy(buf + alen + blen, c, clen);
    }
    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, (char *)buf, n,
                           &res, &got, NULL) < 0 || got != 32) {
        /* Hash is always available in QEMU; zero out defensively on failure. */
        memset(out, 0, 32);
    }
    g_free(buf);
}

/* Derive the per-class encryption and MAC keys from the machine key. */
static void aks_derive(uint32_t klass, uint8_t enc[32], uint8_t mac[32])
{
    uint8_t kl[4] = { klass, klass >> 8, klass >> 16, klass >> 24 };

    aks_sha256(enc, "AKScryptENC", 11, aks_machine_key, 32, kl, 4);
    aks_sha256(mac, "AKScryptMAC", 11, aks_machine_key, 32, kl, 4);
}

/* HMAC-SHA256(mac_key, buf[:len])[:16] -> tag16. Returns false on error. */
static bool aks_mac(const uint8_t mac_key[32], const uint8_t *buf, size_t len,
                    uint8_t tag16[16])
{
    QCryptoHmac *h;
    uint8_t full[32];
    uint8_t *res = full;
    size_t reslen = 32;
    bool ok;

    h = qcrypto_hmac_new(QCRYPTO_HASH_ALGO_SHA256, mac_key, 32, NULL);
    if (!h) {
        return false;
    }
    ok = qcrypto_hmac_bytes(h, (const char *)buf, len, &res, &reslen, NULL) == 0;
    qcrypto_hmac_free(h);
    if (ok) {
        memcpy(tag16, full, 16);
    }
    return ok;
}

/* AES-256-CBC one shot. enc=true to encrypt. in/out length must be a nonzero
 * multiple of 16. Returns false on error. */
static bool aks_aes_cbc(bool enc, const uint8_t key[32], const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, size_t len)
{
    QCryptoCipher *c;
    bool ok;

    c = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_256, QCRYPTO_CIPHER_MODE_CBC,
                           key, 32, NULL);
    if (!c) {
        return false;
    }
    ok = qcrypto_cipher_setiv(c, iv, 16, NULL) == 0;
    if (ok) {
        ok = enc ? qcrypto_cipher_encrypt(c, in, out, len, NULL) == 0
                 : qcrypto_cipher_decrypt(c, in, out, len, NULL) == 0;
    }
    qcrypto_cipher_free(c);
    return ok;
}

/* wrap: plaintext (in,inlen) -> blob (out, capacity outcap). Returns blob len
 * or 0 on failure. */
static uint32_t aks_wrap(const uint8_t *in, uint32_t inlen,
                         uint8_t *out, uint32_t outcap, uint32_t klass)
{
    uint8_t enc[32], mac[32], iv[32], padded[AKW_MAX_PLAIN + 16];
    uint32_t ctlen = (inlen + 15u) & ~15u;
    uint32_t blen = AKW_HDR + ctlen + AKW_MAC;

    if (inlen == 0 || inlen > AKW_MAX_PLAIN || blen > outcap) {
        return 0;
    }
    aks_derive(klass, enc, mac);
    /* Deterministic IV bound to key+class+plaintext (reproducible wrap). */
    aks_sha256(iv, aks_machine_key, 32, &klass, 4, in, inlen);

    out[0] = AKW_MAGIC0; out[1] = AKW_MAGIC1;
    out[2] = AKW_MAGIC2; out[3] = AKW_MAGIC3;
    out[4] = klass; out[5] = klass >> 8; out[6] = klass >> 16; out[7] = klass >> 24;
    out[8] = inlen; out[9] = inlen >> 8; out[10] = inlen >> 16; out[11] = inlen >> 24;
    memcpy(out + 12, iv, 16);

    memset(padded, 0, ctlen);
    memcpy(padded, in, inlen);
    if (!aks_aes_cbc(true, enc, iv, padded, out + AKW_HDR, ctlen)) {
        return 0;
    }
    if (!aks_mac(mac, out, AKW_HDR + ctlen, out + AKW_HDR + ctlen)) {
        return 0;
    }
    return blen;
}

/* unwrap: blob (in,inlen) -> plaintext (out, capacity outcap). Returns
 * plaintext len or 0 on failure (bad magic, class mismatch, MAC mismatch). */
static uint32_t aks_unwrap(const uint8_t *in, uint32_t inlen,
                           uint8_t *out, uint32_t outcap, uint32_t klass)
{
    uint8_t enc[32], mac[32], tag[16], plain[AKW_MAX_PLAIN + 16];
    uint32_t blobclass, origlen, ctlen;

    if (inlen < AKW_HDR + 16 + AKW_MAC ||
        in[0] != AKW_MAGIC0 || in[1] != AKW_MAGIC1 ||
        in[2] != AKW_MAGIC2 || in[3] != AKW_MAGIC3) {
        return 0;
    }
    blobclass = in[4] | (in[5] << 8) | (in[6] << 16) | ((uint32_t)in[7] << 24);
    origlen = in[8] | (in[9] << 8) | (in[10] << 16) | ((uint32_t)in[11] << 24);
    ctlen = inlen - AKW_HDR - AKW_MAC;
    if (blobclass != klass || origlen == 0 || origlen > AKW_MAX_PLAIN ||
        ctlen == 0 || (ctlen & 15u) || origlen > ctlen || origlen > outcap) {
        return 0;
    }
    aks_derive(klass, enc, mac);
    if (!aks_mac(mac, in, AKW_HDR + ctlen, tag) ||
        memcmp(tag, in + AKW_HDR + ctlen, 16) != 0) {
        return 0;
    }
    if (!aks_aes_cbc(false, enc, in + 12, in + AKW_HDR, plain, ctlen)) {
        return 0;
    }
    memcpy(out, plain, origlen);
    return origlen;
}

static bool aks_hvc(CPUState *cs)
{
    CPUARMState *env = cpu_env(cs);
    uint64_t fn = env->xregs[0];
    uint64_t in_va = env->xregs[1];
    uint64_t inlen = env->xregs[2];
    uint64_t out_va = env->xregs[3];
    uint64_t outcap = env->xregs[4];
    uint32_t klass = (uint32_t)env->xregs[5];
    uint8_t inbuf[256];
    uint8_t outbuf[AKW_HDR + AKW_MAX_PLAIN + 16 + AKW_MAC];
    uint32_t outlen = 0;

    if (fn != VRESEARCH_AKSCRYPT_FN_WRAP && fn != VRESEARCH_AKSCRYPT_FN_UNWRAP) {
        return false;
    }
    if (inlen == 0 || inlen > sizeof(inbuf) || outcap > sizeof(outbuf)) {
        env->xregs[0] = 0;
        return true;
    }
    if (cpu_memory_rw_debug(cs, in_va, inbuf, inlen, false) != 0) {
        env->xregs[0] = 0;
        return true;
    }
    if (fn == VRESEARCH_AKSCRYPT_FN_WRAP) {
        outlen = aks_wrap(inbuf, inlen, outbuf, outcap, klass);
    } else {
        outlen = aks_unwrap(inbuf, inlen, outbuf, outcap, klass);
    }
    if (outlen && cpu_memory_rw_debug(cs, out_va, outbuf, outlen, true) != 0) {
        outlen = 0;
    }
    env->xregs[0] = outlen;
    return true;
}

void vresearch_akscrypt_enable(void)
{
    if (!aks_enabled) {
        aks_enabled = true;
        arm_akscrypt_hvc_hook = aks_hvc;
    }
}
