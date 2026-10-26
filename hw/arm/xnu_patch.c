#include "qemu/compiler.h"
#include "xnu/patch.h"
#include "xnu/mach-o/loader.h"
#include "xnu/mach-o/macho_arm64.h"
#include "xnu/boot/xnuboot.h"
#include "xnu/macho.h"

#define PACIBSP                (( 0xD503237F ))
#define RETAB                  (( 0xD65F0FFF ))
#define BTI_C                  (( 0xD503245F ))
#define BTI_J                  (( 0xD503249F ))
#define MOVX(rd,v)             (( 0xD2800000 | ( ((v)) << 5) | ((rd)) ))

#define IS_LDG(v)              (( (0b11011001011 == (((v)) >> 21)) && (0 == ((((v)) >> 10) & 0b011)) ))

// Assuming all pointers are DYLD_CHAINED_PTR_64_KERNEL_CACHE, strip everything
// but target (treating this pointer as a dyld_chained_ptr_64_kernel_cache_rebase).
#define REMOVE_DYLD_FIXUP_DATA(p)    (( ((p)) & (( BIT(30) - 1 )) ))

struct sysctl_oid {
    void   *oid_parent;
    void   *oid_link;
    int     oid_number;
    int     oid_kind;
    void    *oid_arg1;
    int     oid_arg2;
    char   *oid_name;
    u64     oid_handler;
    char   *oid_fmt;
    char   *oid_descr;
    int     oid_version;
    int     oid_refcnt;
};

static void patch_img4_deadlock(u8 *bkc_macho) {
    // Find the sysctl__security_mac_img4_ignition_blob struct in AppleImage4.kext,
    // then patch its oid handler (_darwin_trap_ignition_get_blob) to return 0.

    fse_t *img4_fse = macho_find_fileset_entry(bkc_macho, "com.apple.security.AppleImage4");
    if (!img4_fse) {
        fprintf(stderr, "warning: couldn't find img4 kext\n");
        return;
    }

    u8 *img4_macho = &bkc_macho[img4_fse->fileoff];

    seg_t *img4_exc_seg = macho_find_seg(img4_macho, "__TEXT_EXEC");
    sect_t *img4_dat_sect = macho_find_sect(img4_macho, "__DATA", "__data");
    sect_t *img4_str_sect = macho_find_sect(img4_macho, "__TEXT", "__cstring");

    i64 *img4_dat = (i64*)&bkc_macho[img4_dat_sect->offset];
    u8  *img4_str = (u8*)&bkc_macho[img4_str_sect->offset];

    u64 ignition_blob_str = 0;
    for (size_t i = 0; i < img4_str_sect->size; i++) {
        if (0 == strcmp("ignition_blob", (char*)&img4_str[i])) {
            ignition_blob_str = img4_str_sect->offset + i;
        }
    }

    if (0 == ignition_blob_str) {
        fprintf(stderr, "couldn't find ignition_blob sysctl struct\n");
        return;
    }

    u64 oid_handler = 0;
    for (size_t i = 0; i < img4_dat_sect->size / sizeof(u64); i++) {
        // This is sysctl__security_mac_img4_ignition_blob.oid_name
        u64 stripped_ptr = REMOVE_DYLD_FIXUP_DATA(img4_dat[i]);
        if (stripped_ptr == ignition_blob_str) {
            struct sysctl_oid *blob_oid = container_of((char**)&img4_dat[i], struct sysctl_oid, oid_name);
            oid_handler = REMOVE_DYLD_FIXUP_DATA(blob_oid->oid_handler);
        }
    }

    if (0 == oid_handler) {
        fprintf(stderr, "couldn't find _darwin_trap_ignition_get_blob\n");
        return;
    }

    if (oid_handler < img4_exc_seg->fileoff || oid_handler > img4_exc_seg->fileoff + img4_exc_seg->filesize) {
        fprintf(stderr, "Warning: _darwin_trap_ignition_get_blob isn't in __TEXT_EXEC\n");
    }

    u32 *sysctl_handler_fn = (u32*)&bkc_macho[oid_handler];

    if (PACIBSP != sysctl_handler_fn[0] && BTI_C != sysctl_handler_fn[0]) {
        fprintf(stderr, "Warning: _darwin_trap_ignition_get_blob doesn't start with PACIBSP or BTI C\n");
        fprintf(stderr, "opcode: 0x%X\n", sysctl_handler_fn[0]);
    }

    sysctl_handler_fn[0] = PACIBSP;
    sysctl_handler_fn[1] = MOVX(0,0);
    sysctl_handler_fn[2] = RETAB;
}

void patch_kc(u8 *bkc_macho) {
    patch_img4_deadlock(bkc_macho);
}

#define NOP                    (( 0xD503201F ))

static i64 sext(u64 v, int bits) {
    return (i64)(v << (64 - bits)) >> (64 - bits);
}

// Target of a conditional branch (b.cond, cbz/cbnz, tbz/tbnz), or 0
static u64 cond_branch_target(u32 w, u64 pc) {
    if ((w & 0xFF000010) == 0x54000000 || (w & 0x7E000000) == 0x34000000) {
        return pc + 4 * sext((w >> 5) & 0x7FFFF, 19);
    }
    if ((w & 0x7E000000) == 0x36000000) {
        return pc + 4 * sext((w >> 5) & 0x3FFF, 14);
    }
    return 0;
}

/*
 * NOP the one conditional branch in com.apple.kernel that leads to the panic
 * whose format string starts with `msg`: find the string, its ADRP+ADD
 * reference, then the branch into the few instructions that set up the panic
 * call. Returns false (and leaves the kernel alone) unless exactly one such
 * branch exists.
 */
static bool nop_branch_to_panic(u8 *bkc, const char *msg) {
    fse_t *fse = macho_find_fileset_entry(bkc, "com.apple.kernel");
    u8 *kernel;
    sect_t *cstr, *text;
    u64 str_va = 0, ref_va = 0, site_off = 0;
    u32 *words;
    size_t i, n, found = 0;

    if (!fse) {
        return false;
    }
    kernel = &bkc[fse->fileoff];
    cstr = macho_find_sect(kernel, "__TEXT", "__cstring");
    text = macho_find_sect(kernel, "__TEXT_EXEC", "__text");
    if (!cstr || !text) {
        return false;
    }
    for (i = 0; i < cstr->size; i++) {
        if (!strncmp((char *)&bkc[cstr->offset + i], msg, strlen(msg)) &&
            (i == 0 || bkc[cstr->offset + i - 1] == 0)) {
            str_va = cstr->addr + i;
            break;
        }
    }
    if (!str_va) {
        return false;
    }

    words = (u32 *)&bkc[text->offset];
    n = text->size / 4;
    for (i = 0; i + 1 < n && !ref_va; i++) {
        u32 w = words[i], a = words[i + 1];
        u64 pc = text->addr + 4 * i;
        u64 page;

        if ((w & 0x9F000000) != 0x90000000) {
            continue;
        }
        page = (pc & ~0xFFFULL) +
               (sext(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2), 21) << 12);
        if (page == (str_va & ~0xFFFULL) && (a & 0xFFC00000) == 0x91000000 &&
            ((a >> 5) & 31) == (w & 31) && ((a >> 10) & 0xFFF) == (str_va & 0xFFF)) {
            ref_va = pc;
        }
    }
    if (!ref_va) {
        return false;
    }

    // The panic block starts at most 8 instructions before the reference
    for (i = 0; i < n; i++) {
        u64 pc = text->addr + 4 * i;
        u64 t = cond_branch_target(words[i], pc);

        if (pc + 0x4000 > ref_va && pc < ref_va && t && t >= ref_va - 0x20 && t <= ref_va) {
            site_off = i;
            found++;
        }
    }
    if (found != 1) {
        return false;
    }
    words[site_off] = NOP;
    return true;
}

void patch_kc_unauthenticated_root(u8 *bkc_macho) {
    // bsd_init: FSIOC_KERNEL_ROOTAUTH fails on a root that isn't a sealed APFS
    // volume (an HFS+ disk); continue instead of panicking.
    if (!nop_branch_to_panic(bkc_macho, "rootvp not authenticated after mounting")) {
        fprintf(stderr, "warning: rootvp authentication check not found, not patched\n");
    }
}

/*
 * aks-stub: let the kernel boot its container daemons without a SEP.
 *
 * AppleSEPKeyStore decides whether to use a SEP endpoint in a "sep-booted
 * answer" routine (sub_7F2D698 in xnu-12377.100.591.502 vphone600). With no
 * SEP it still calls getAppleSEPManager at the top of that routine, and the
 * thread holding the keystore gate hangs there; a stackshot shows ~29
 * container daemons (backboardd, containermanagerd, tccd, keybagd,
 * coreauthd, ...) all blocked on that one gate mutex. Force the routine to
 * return 0 at entry (pacibsp; mov x0, #0; retab) so sep_endpoint() takes its
 * fast "SEP is not booted" path and releases the gate.
 *
 * The routine is identified by its prologue (pacibsp; sub sp, sp, #0xb0); if
 * it does not match (a different build), nothing is patched.
 */
#define SUB_SP_SP_0xB0         (( 0xD102C3FF ))
#define AKS_SEP_BOOTED_ANSWER  (( 0xfffffe0007f2d698ULL ))

/* Emit MOVZ/MOVK into x8 to materialize a 64-bit constant; returns word count. */
static int emit_mov_x8(u32 *out, u64 val) {
    int n = 0;
    bool first = true;
    int hw;
    for (hw = 0; hw < 4; hw++) {
        u16 h = (val >> (16 * hw)) & 0xFFFF;
        if (h == 0) {
            continue;
        }
        if (first) {
            out[n++] = 0xD2800008u | ((u32)h << 5);                 /* movz x8, #h */
            first = false;
        } else {
            out[n++] = 0xF2800008u | ((u32)hw << 21) | ((u32)h << 5); /* movk x8, #h, lsl */
        }
    }
    if (first) {
        out[n++] = 0xD2800008u;                                      /* movz x8, #0 */
    }
    return n;
}

void patch_kc_aks_stub(u8 *bkc_macho) {
    fse_t *fse = macho_find_fileset_entry(bkc_macho, "com.apple.driver.AppleSEPKeyStore");
    seg_t *exec;
    u32 *fn;

    if (!fse) {
        fprintf(stderr, "aks-stub: couldn't find AppleSEPKeyStore kext\n");
        return;
    }
    exec = macho_find_seg(&bkc_macho[fse->fileoff], "__TEXT_EXEC");
    if (!exec) {
        fprintf(stderr, "aks-stub: couldn't find AppleSEPKeyStore __TEXT_EXEC\n");
        return;
    }
    if (AKS_SEP_BOOTED_ANSWER < exec->vmaddr ||
        AKS_SEP_BOOTED_ANSWER >= exec->vmaddr + exec->vmsize) {
        fprintf(stderr, "aks-stub: target not in __TEXT_EXEC; not patched\n");
        return;
    }
    fn = (u32 *)&bkc_macho[exec->fileoff + (AKS_SEP_BOOTED_ANSWER - exec->vmaddr)];
    if (fn[0] != PACIBSP || fn[1] != SUB_SP_SP_0xB0) {
        fprintf(stderr, "aks-stub: sub_7F2D698 prologue mismatch "
                "(0x%X 0x%X); not patched\n", fn[0], fn[1]);
        return;
    }
    fn[1] = MOVX(0, 0);
    fn[2] = RETAB;

    /*
     * Second: make the keystore's SEP transact (sub_7F2C794) return success
     * instead of the error 0xE00002BC when there is no endpoint, so the AKS
     * requests the daemons issue complete rather than fail. The no-endpoint
     * error is built at 0xfffffe0007f2d618 with
     *   movz w27, #0x2bc ; movk w27, #0xe000, lsl #16   (0x5280579b 0x72bc001b)
     * Turn it into w27 = 0 (movz w27, #0 ; nop). The output buffer is not
     * filled on this path (no real reply), so read queries return zeros; the
     * per-selector synthetic values are layered on top of this.
     */
    {
        const u64 err_va = 0xfffffe0007f2d618ULL;
        u32 *e;

        if (err_va < exec->vmaddr || err_va >= exec->vmaddr + exec->vmsize) {
            fprintf(stderr, "aks-stub: transact error site out of range\n");
            return;
        }
        e = (u32 *)&bkc_macho[exec->fileoff + (err_va - exec->vmaddr)];
        if (e[0] != 0x5280579bu || e[1] != 0x72bc001bu) {
            fprintf(stderr, "aks-stub: transact error load mismatch "
                    "(0x%X 0x%X); not patched\n", e[0], e[1]);
            return;
        }
        e[0] = 0x5280001bu;     /* movz w27, #0 */
        e[1] = NOP;
    }

    /*
     * Third: give selector 7 (get lock state) a synthetic value, the way
     * iOS 16's SweetKeyStore did. After the transact-success patch above the
     * AKS requests complete but the reply buffer is zeroed, so sel 7 returns
     * lock state 0 which libaks reads as "locked" and the daemons poll it
     * forever (3301x in a boot). iOS 16 answered 0x4 ("unlocked since boot",
     * lock bits clear); do the same here.
     *
     * The AppleKeyStoreUserClient dispatcher (sub_7F33034) jumps to the sel 7
     * handler at 0xfffffe0007f34e24. Its normal success path stores the lock
     * state scalar into *scalarOutput (args at X19, scalarOutput ptr at
     * X19+0x48, scalarOutputCount at X19+0x50). Like iOS 16's SweetKeyStore,
     * write both the value and the count. Rewrite the handler in place to skip
     * the SEP transact and store 0x4 with count 1, then return 0:
     *   ldr  x9, [x19, #0x48]   ; scalarOutput
     *   movz w8, #4             ; 0x4 = unlocked since boot
     *   str  x8, [x9]           ; *scalarOutput = 4
     *   movz w8, #1
     *   str  w8, [x19, #0x50]   ; scalarOutputCount = 1
     *   movz w24, #0            ; return KERN_SUCCESS (epilogue returns x24)
     *   b    0xfffffe0007f3a940 ; shared epilogue
     * PAC: the branch lands in sub_7F33034's own epilogue, which ends in
     * retab, so the authenticated return is preserved; no new ret is added.
     * The BTI landing pad at the block head is kept (it is a jump-table
     * target). Validated by the head (bti j; str xzr, [x21, #0xa8]); if the
     * bytes differ, nothing is patched.
     */
    {
        const u64 sel7_va = 0xfffffe0007f34e24ULL;
        const u64 epi_va  = 0xfffffe0007f3a940ULL;
        u32 *h;
        u64 bsite;
        i64 off;

        if (sel7_va < exec->vmaddr || sel7_va >= exec->vmaddr + exec->vmsize ||
            epi_va  < exec->vmaddr || epi_va  >= exec->vmaddr + exec->vmsize) {
            fprintf(stderr, "aks-stub: sel 7 handler out of range\n");
            return;
        }
        h = (u32 *)&bkc_macho[exec->fileoff + (sel7_va - exec->vmaddr)];
        if (h[0] != BTI_J || h[1] != 0xf90056bfu) {
            fprintf(stderr, "aks-stub: sel 7 handler head mismatch "
                    "(0x%X 0x%X); not patched\n", h[0], h[1]);
            return;
        }
        h[1] = 0xf9402669u;             /* ldr  x9, [x19, #0x48] */
        h[2] = 0x52800088u;             /* movz w8, #4 */
        h[3] = 0xf9000128u;             /* str  x8, [x9] */
        h[4] = 0x52800028u;             /* movz w8, #1 */
        h[5] = 0xb9005268u;             /* str  w8, [x19, #0x50] */
        h[6] = 0x52800018u;             /* movz w24, #0 */
        bsite = sel7_va + 4 * 7;        /* VA of h[7] */
        off = (i64)(epi_va - bsite) >> 2;
        h[7] = 0x14000000u | ((u32)off & 0x03FFFFFFu);  /* b epilogue */
    }

    /*
     * Fourth-b: selector 14 (get system bag). keybagd's MKB_INIT (MobileKeyBag)
     * asks AppleKeyStore for the system bag at boot; with no SEP the handler
     * fails, MKB_INIT falls back to /private/var/keybags/systembag.kb, which is
     * absent, and dies ("No system keybag found on filesystem ... FATAL KEYBAG
     * ERROR"). usermanagerd then cannot build the personal-persona session bag
     * (ACMTRM _currentDeviceStateFromKeybag -> 0xe00002d8), so containermanagerd
     * blocks in -[MCMUserIdentityCache _refreshFromUserManagementIfNecessary]
     * and every process launched as the mobile user (backboardd) hangs forever
     * in exec waiting on that container-label upcall; the UI never comes up.
     *
     * iOS 16's SweetKeyStore answered sel 14 with system handle 0 ("answering
     * with a handle is the path a userspace reboot takes, so no bag file is
     * needed"). Do the same: the sel 14 handler at 0xfffffe0007f34950
     * (dispatcher sub_7F33034, jump-table index sel-1) has the same scalar-out
     * shape as sel 7, so write handle 0 to *scalarOutput, set
     * scalarOutputCount = 1, return 0, and branch to the shared retab epilogue.
     * Head validated (bti j; ldr w8, [x19, #0x28]); on mismatch nothing is
     * patched. There are 27 words before the next jump-table landing pad at
     * 0xfffffe0007f349bc, so the 8-word rewrite stays inside this handler.
     */
    {
        const u64 sel14_va = 0xfffffe0007f34950ULL;
        const u64 epi_va   = 0xfffffe0007f3a940ULL;
        u32 *h;
        u64 bsite;
        i64 off;

        if (sel14_va < exec->vmaddr || sel14_va >= exec->vmaddr + exec->vmsize ||
            epi_va   < exec->vmaddr || epi_va   >= exec->vmaddr + exec->vmsize) {
            fprintf(stderr, "aks-stub: sel 14 handler out of range\n");
            return;
        }
        h = (u32 *)&bkc_macho[exec->fileoff + (sel14_va - exec->vmaddr)];
        if (h[0] != BTI_J || h[1] != 0xb9402a68u) {
            fprintf(stderr, "aks-stub: sel 14 handler head mismatch "
                    "(0x%X 0x%X); not patched\n", h[0], h[1]);
            return;
        }
        h[1] = 0xf9402669u;             /* ldr  x9, [x19, #0x48]  (scalarOutput) */
        h[2] = 0x52800008u;             /* movz w8, #0  (system bag handle 0) */
        h[3] = 0xf9000128u;             /* str  x8, [x9]          (*scalarOutput = 0) */
        h[4] = 0x52800028u;             /* movz w8, #1 */
        h[5] = 0xb9005268u;             /* str  w8, [x19, #0x50]  (scalarOutputCount = 1) */
        h[6] = 0x52800018u;             /* movz w24, #0           (KERN_SUCCESS) */
        bsite = sel14_va + 4 * 7;       /* VA of h[7] */
        off = (i64)(epi_va - bsite) >> 2;
        h[7] = 0x14000000u | ((u32)off & 0x03FFFFFFu);  /* b epilogue */
    }

    /*
     * Fourth: selectors 17 and 35 (get device state / extended device state)
     * return a small DER SET as struct output, not a scalar. With no SEP they
     * fail (sep_endpoint() failed, ret 0xE00002D8) and the daemons retry
     * forever. iOS 26's _decode_extended_state (der_dict_iterate) parses a DER
     *   SET OF SEQUENCE { UTF8String key, INTEGER value }
     * (the encoder _encode_list_dict qsorts discrete pair objects, so each pair is
     * wrapped in its own SEQUENCE and the SET is canonical/sorted) -- NOT a flat
     * SET of alternating strings and integers. iOS 16's SweetKeyStore used the flat
     * form; on iOS 26 it fails to decode, and aks_get_device_state then leaves the
     * caller's struct zeroed yet still returns success, so SpringBoard reads lock
     * state 0 and shows "Enter Passcode" (no error is logged -- matches the serial).
     * The device lock state is struct offset 4, filled by der_key_state_lock_state
     * ("sls", the 2nd field after ss). SpringBoardFoundation:
     * SBFMobileKeyBagState.lockState = remap[offset4], remap={0,2,1,3,4,5,6,7}, and
     * -[SBFMobileKeyBag _queue_setHasPasscodeIfNecessary:] sets
     * hasPasscode = (lockState != 3). So sls=3 => lockState 3 => no passcode =>
     * the clock lock screen. Provide ss=4 (unlocked since boot) and sls=3, sorted:
     *   SET { SEQ { "ss", 4 }, SEQ { "sls", 3 } }
     *
     * sel 35 (@0xfffffe0007f354a8) sets w27=0 then falls through into the sel
     * 17 body at 0xfffffe0007f354b0, so a single rewrite at 0xfffffe0007f354b4
     * (just past sel 17's bti landing pad) covers both. The framework places a
     * small struct output inline: ptr at args+0x58 (X19+0x58), capacity/size
     * at args+0x60. Build the DER straight into that buffer with immediate
     * stores (no SEP, no cave, no external call), set the size, return 0, and
     * branch to sub_7F33034's own retab epilogue (PAC preserved).
     * Validated by the body head (str xzr,[sp,...]); mismatched bytes skip it.
     */
    {
        static const u8 der[24] = {
            0x31, 0x13,                                        /* SET, len 0x13 */
            0x30,0x07, 0x0c,0x02,'s','s',      0x02,0x01,0x04, /* SEQ { "ss",  4 } */
            0x30,0x08, 0x0c,0x03,'s','l','s',  0x02,0x01,0x03, /* SEQ { "sls", 3 } */
            0x00,0x00,0x00,                                    /* pad to 8-byte multiple */
        };
        const int der_len = 21;  /* real DER length (bytes before the pad) */
        const u64 body_va = 0xfffffe0007f354b4ULL;
        const u64 epi_va  = 0xfffffe0007f3a940ULL;
        const u64 next_va = 0xfffffe0007f357d8ULL;  /* sel 18 handler: capacity bound */
        u32 code[160];
        int n = 0, cbz_idx, ret_idx, i;
        u32 *b;
        i64 off;

        if (body_va < exec->vmaddr || body_va >= exec->vmaddr + exec->vmsize) {
            fprintf(stderr, "aks-stub: sel 17/35 body out of range\n");
            return;
        }
        b = (u32 *)&bkc_macho[exec->fileoff + (body_va - exec->vmaddr)];
        if (b[0] != 0xf9006bffu) {  /* str xzr, [sp, #...] at body head */
            fprintf(stderr, "aks-stub: sel 17/35 body head mismatch "
                    "(0x%X); not patched\n", b[0]);
            return;
        }

        code[n++] = 0xf9402e60u;            /* ldr  x0, [x19, #0x58] (structOutput) */
        cbz_idx = n;
        code[n++] = 0;                      /* cbz  x0, ret  (patched below) */
        for (i = 0; i + 8 <= (int)sizeof(der); i += 8) {  /* qwords of the padded blob */
            u64 v = 0;
            int k;
            for (k = 0; k < 8; k++) {
                v |= (u64)der[i + k] << (8 * k);
            }
            n += emit_mov_x8(&code[n], v);
            code[n++] = 0xf9000008u | ((u32)(i / 8) << 10);  /* str x8, [x0, #i] */
        }
        ret_idx = n;
        code[n++] = 0x52800008u | ((u32)der_len << 5);  /* movz w8, #der_len */
        code[n++] = 0xb9006268u;            /* str  w8, [x19, #0x60] (structOutputSize) */
        code[n++] = 0x52800018u;            /* movz w24, #0 */
        off = (i64)(epi_va - (body_va + 4 * n)) >> 2;
        code[n++] = 0x14000000u | ((u32)off & 0x03FFFFFFu);  /* b epilogue */

        /* cbz x0, ret_idx : skip the copy if the buffer is a descriptor (null) */
        code[cbz_idx] = 0xB4000000u | ((u32)((ret_idx - cbz_idx) & 0x7FFFF) << 5); /* x0 */

        if ((u64)(body_va + 4 * n) > next_va) {
            fprintf(stderr, "aks-stub: sel 17/35 code too large; not patched\n");
            return;
        }
        for (i = 0; i < n; i++) {
            b[i] = code[i];
        }
    }
}

/*
 * aks-crypt: route the keystore's wrap/unwrap through an HVC so QEMU can do
 * real, reversible AES over a machine-local key (vresearch_akscrypt.c), instead
 * of the aks-stub's blind success which would hand out garbage keys.
 *
 * Without a SEP the class-key wrap (IOConnectCallMethod sel 10) and unwrap
 * (sel 11) return kIOReturnNotReady, so usermanagerd can't build the mobile
 * persona's session bag and the UI never comes up ("No persona found",
 * "ks_crypt: e00002d8"). The user-client dispatcher (sub_7F33034) routes sel 10
 * to the handler at 0xfffffe0007f34858 and sel 11 to 0xfffffe0007f34ae8
 * (xnu-12377.100.591.502, vphone600). Both handlers map the struct-input
 * descriptor via getters, leaving the plaintext/wrapped pointer in a register
 * and its length in x0, then build their output into the stack buffer at
 * sp+0xd0 with the length at [x29-0x88] and, on a 0 return, copy that out to
 * the client (the common copyout at 0xfffffe000934977c). We overwrite the span
 * from just after the input is in hand up to and including the success branch,
 * with: marshal (in, inlen, out=sp+0xd0, cap=*[x29-0x88], class, keybag) into
 * the HVC registers, hvc #0, store the returned length to [x29-0x88], and
 * branch to the handler's own copyout. x19 (the args) and x29 are preserved, so
 * the copyout runs unchanged. Validated against anchor words; a different build
 * is left alone.
 *
 * sel 10 (wrap)   write span 0xfffffe0007f34914 .. 0xf34940, branch to 0xf38ffc
 * sel 11 (unwrap) write span 0xfffffe0007f34b90 .. 0xf34bbc, branch to 0xf34bec
 */
#define AKC_SEL10_VA   (( 0xfffffe0007f34914ULL ))   /* add x8, sp, #0xcc  */
#define AKC_SEL10_BL   (( 0xfffffe0007f34938ULL ))   /* bl sub_7F2BCB8     */
#define AKC_SEL10_COPY (( 0xfffffe0007f38ffcULL ))   /* success copyout    */
#define AKC_SEL11_VA   (( 0xfffffe0007f34b90ULL ))   /* movi v0.2d, #0     */
#define AKC_SEL11_BL   (( 0xfffffe0007f34be0ULL ))   /* bl sub_7F2BEA0     */
#define AKC_SEL11_COPY (( 0xfffffe0007f34becULL ))   /* success copyout    */

static u32 b_imm(u64 from, u64 to) {
    return 0x14000000u | ((u32)(((i64)(to - from)) >> 2) & 0x03FFFFFFu);
}

void patch_kc_aks_crypt(u8 *bkc_macho) {
    fse_t *fse = macho_find_fileset_entry(bkc_macho,
                     "com.apple.driver.AppleSEPKeyStore");
    seg_t *exec;
    u32 *p10, *p11;
    u64 b10, b11;

    if (!fse) {
        fprintf(stderr, "aks-crypt: couldn't find AppleSEPKeyStore kext\n");
        return;
    }
    exec = macho_find_seg(&bkc_macho[fse->fileoff], "__TEXT_EXEC");
    if (!exec) {
        fprintf(stderr, "aks-crypt: couldn't find AppleSEPKeyStore __TEXT_EXEC\n");
        return;
    }
    if (AKC_SEL10_VA < exec->vmaddr ||
        AKC_SEL10_COPY >= exec->vmaddr + exec->vmsize ||
        AKC_SEL11_VA < exec->vmaddr ||
        AKC_SEL11_BL  >= exec->vmaddr + exec->vmsize) {
        fprintf(stderr, "aks-crypt: targets out of range; not patched\n");
        return;
    }
    p10 = (u32 *)&bkc_macho[exec->fileoff + (AKC_SEL10_VA - exec->vmaddr)];
    p11 = (u32 *)&bkc_macho[exec->fileoff + (AKC_SEL11_VA - exec->vmaddr)];

    /* Anchor-validate both handlers: write-span head and the bl we replace. */
    if (p10[0] != 0x910333e8u ||
        p10[(AKC_SEL10_BL - AKC_SEL10_VA) / 4] != 0x97ffdce0u) {
        fprintf(stderr, "aks-crypt: sel 10 handler mismatch "
                "(0x%X 0x%X); not patched\n",
                p10[0], p10[(AKC_SEL10_BL - AKC_SEL10_VA) / 4]);
        return;
    }
    if (p11[0] != 0x6f00e400u ||
        p11[(AKC_SEL11_BL - AKC_SEL11_VA) / 4] != 0x97ffdcb0u) {
        fprintf(stderr, "aks-crypt: sel 11 handler mismatch "
                "(0x%X 0x%X); not patched\n",
                p11[0], p11[(AKC_SEL11_BL - AKC_SEL11_VA) / 4]);
        return;
    }

    /*
     * sel 10 (wrap). Registers in hand at 0xf34914: x22 = plaintext ptr,
     * x0 = plaintext len, x25 = key class, x26 = keybag handle. HVC id
     * 0xC1000010. The 12th word (at 0xf34940) replaces the original
     * "cbz w0, copyout" with an unconditional branch to copyout; QEMU
     * guarantees a valid (possibly 0) length, so no error test is needed.
     */
    p10[0]  = 0xAA1603E1u;  /* mov  x1, x22            (in)        */
    p10[1]  = 0xAA0003E2u;  /* mov  x2, x0             (in len)    */
    p10[2]  = 0x910343E3u;  /* add  x3, sp, #0xd0      (out)       */
    p10[3]  = 0xD10223A9u;  /* sub  x9, x29, #0x88     (&outlen)   */
    p10[4]  = 0xB9400124u;  /* ldr  w4, [x9]           (out cap)   */
    p10[5]  = 0xAA1903E5u;  /* mov  x5, x25            (class)     */
    p10[6]  = 0xAA1A03E6u;  /* mov  x6, x26            (keybag)    */
    p10[7]  = 0xD2800200u;  /* movz x0, #0x10                      */
    p10[8]  = 0xF2B82000u;  /* movk x0, #0xc100, lsl #16  (fn)     */
    p10[9]  = 0xD4000002u;  /* hvc  #0                             */
    p10[10] = 0xB9000120u;  /* str  w0, [x9]           (*outlen)   */
    b10 = AKC_SEL10_VA + 4 * 11;
    p10[11] = b_imm(b10, AKC_SEL10_COPY);              /* b copyout */

    /*
     * sel 11 (unwrap). Registers in hand at 0xf34b90: x23 = wrapped ptr,
     * x0 = wrapped len, x26 = key class (scalarInput[1]), x27 = keybag
     * (scalarInput[0]). HVC id 0xC1000011. The 12th word (0xf34bbc)
     * branches to the success copyout; the original error test is dropped.
     */
    p11[0]  = 0xAA1703E1u;  /* mov  x1, x23            (in)        */
    p11[1]  = 0xAA0003E2u;  /* mov  x2, x0             (in len)    */
    p11[2]  = 0x910343E3u;  /* add  x3, sp, #0xd0      (out)       */
    p11[3]  = 0xD10223A9u;  /* sub  x9, x29, #0x88     (&outlen)   */
    p11[4]  = 0xB9400124u;  /* ldr  w4, [x9]           (out cap)   */
    p11[5]  = 0xAA1A03E5u;  /* mov  x5, x26            (class)     */
    p11[6]  = 0xAA1B03E6u;  /* mov  x6, x27            (keybag)    */
    p11[7]  = 0xD2800220u;  /* movz x0, #0x11                      */
    p11[8]  = 0xF2B82000u;  /* movk x0, #0xc100, lsl #16  (fn)     */
    p11[9]  = 0xD4000002u;  /* hvc  #0                             */
    p11[10] = 0xB9000120u;  /* str  w0, [x9]           (*outlen)   */
    b11 = AKC_SEL11_VA + 4 * 11;
    p11[11] = b_imm(b11, AKC_SEL11_COPY);              /* b copyout */
}

/*
 * amfi-lc-off: stop AMFI from enforcing launch constraints, the way iOS 16
 * had no AMFI at all. On the vphone600 research kernel AMFI is present and
 * enforces launch constraints: SpringBoard (launch type 0/1) fails its launch
 * constraint and is killed ("AMFI: Launch Constraint Violation (enforcing)"),
 * so the UI never comes up. The boot-args are no help: amfi_enforce_launch_
 * constraints only ever RAISES enforcement (it is applied only when nonzero),
 * and cs_enforcement_disable panics the kernel on purpose.
 *
 * AMFI decides whether constraints are enforced in sub_7DE30A8, which returns
 * (dword_78806DC != 0):
 *   bti c; adrp x8,#..DC; ldr w8,[x8,#..DC]; cmp w8,#0; cset w0,ne; ret
 * The violation reporter computes enforcing = sub_7DE30A8() & ...; a 0 return
 * means every violation is logged "(not enforcing)" and the process is left
 * alone. Patch the cset at 0xfffffe0007de30b8 (cset w0, ne = 0x1a9f07e0) to
 * movz w0, #0 so the routine always returns 0. Validated against that opcode.
 */
#define AMFI_LC_ENFORCED_CSET  (( 0xfffffe0007de30b8ULL ))

void patch_kc_amfi_lc_off(u8 *bkc_macho) {
    fse_t *fse = macho_find_fileset_entry(bkc_macho,
                     "com.apple.driver.AppleMobileFileIntegrity");
    seg_t *exec;
    u32 *p;

    if (!fse) {
        fprintf(stderr, "amfi-lc-off: couldn't find AppleMobileFileIntegrity kext\n");
        return;
    }
    exec = macho_find_seg(&bkc_macho[fse->fileoff], "__TEXT_EXEC");
    if (!exec) {
        fprintf(stderr, "amfi-lc-off: couldn't find AMFI __TEXT_EXEC\n");
        return;
    }
    if (AMFI_LC_ENFORCED_CSET < exec->vmaddr ||
        AMFI_LC_ENFORCED_CSET >= exec->vmaddr + exec->vmsize) {
        fprintf(stderr, "amfi-lc-off: target not in __TEXT_EXEC; not patched\n");
        return;
    }
    p = (u32 *)&bkc_macho[exec->fileoff + (AMFI_LC_ENFORCED_CSET - exec->vmaddr)];
    if (p[0] != 0x1a9f07e0u) {       /* cset w0, ne */
        fprintf(stderr, "amfi-lc-off: enforcement cset mismatch "
                "(0x%X); not patched\n", p[0]);
        return;
    }
    p[0] = 0x52800000u;              /* movz w0, #0 */
}

/*
 * acm-stub: let SpringBoard run on a SEP-less machine, without faking any
 * credential or unlock result.
 *
 * AppleSEPCredentialManager has two "send a command and wait for the SEP
 * endpoint outside the ACM command gate" wrappers (sub_7F12C6C and
 * sub_7F12D54 in xnu-12377.* vphone600). Each logs
 * "waitForSEPEndpointOutsideACMCommandGate: waiting cmd=%d" and then calls the
 * endpoint object, which blocks forever when there is no SEP. SpringBoard's
 * main thread issues an ACM command early in FBSystemShellInitialize and hangs
 * there (a stackshot shows it ~865 s deep in these frames), so the lock screen
 * is never drawn.
 *
 * Each wrapper already returns a "no endpoint" error, 0xE00002C2, on its
 * x4==NULL path. Make both return that same error right after the prologue,
 * instead of calling the blocking endpoint method, so the command fails fast
 * and SpringBoard's run loop keeps going. This fabricates nothing: the device
 * stays locked and SEP-less, and ACM queries simply report "unavailable" - the
 * honest answer for a machine with no SEP. Same absent-hardware model as
 * aks-stub.
 *
 * Both are identified by their prologue (pacibsp; sub sp, sp, #0x70) and the
 * cbz x4 at the patch site; if either does not match (a different build),
 * nothing is patched.
 */
#define SUB_SP_SP_0x70      (( 0xD101C3FFu ))
#define MOVZ_W0_0x2C2       (( 0x52805840u ))   /* movz w0, #0x2c2            */
#define MOVK_W0_E000_LSL16  (( 0x72BC0000u ))   /* movk w0, #0xe000, lsl #16  */
#define ACM_SEND_A          (( 0xfffffe0007f12c6cULL ))
#define ACM_SEND_A_EPI      (( 0xfffffe0007f12d00ULL ))
#define ACM_SEND_B          (( 0xfffffe0007f12d54ULL ))
#define ACM_SEND_B_EPI      (( 0xfffffe0007f12e40ULL ))

void patch_kc_acm_stub(u8 *bkc_macho) {
    fse_t *fse = macho_find_fileset_entry(bkc_macho,
                     "com.apple.driver.AppleSEPCredentialManager");
    seg_t *exec;
    struct { u64 fn; u64 epi; u32 site_idx; u32 site_word; const char *tag; } w[2] = {
        { ACM_SEND_A, ACM_SEND_A_EPI, 6, 0xb4000484u, "A" },
        { ACM_SEND_B, ACM_SEND_B_EPI, 7, 0xb40004a4u, "B" },
    };
    int i;

    if (!fse) {
        fprintf(stderr, "acm-stub: couldn't find AppleSEPCredentialManager kext\n");
        return;
    }
    exec = macho_find_seg(&bkc_macho[fse->fileoff], "__TEXT_EXEC");
    if (!exec) {
        fprintf(stderr, "acm-stub: couldn't find ACM __TEXT_EXEC\n");
        return;
    }

    /* Verify both wrappers before writing either, so a build mismatch is a
     * no-op rather than a half-applied patch. */
    for (i = 0; i < 2; i++) {
        u32 *fn;
        if (w[i].fn < exec->vmaddr ||
            w[i].epi >= exec->vmaddr + exec->vmsize) {
            fprintf(stderr, "acm-stub: wrapper %s out of __TEXT_EXEC; not patched\n",
                    w[i].tag);
            return;
        }
        fn = (u32 *)&bkc_macho[exec->fileoff + (w[i].fn - exec->vmaddr)];
        if (fn[0] != PACIBSP || fn[1] != SUB_SP_SP_0x70 ||
            fn[w[i].site_idx] != w[i].site_word) {
            fprintf(stderr, "acm-stub: wrapper %s prologue/site mismatch "
                    "(0x%X 0x%X 0x%X); not patched\n",
                    w[i].tag, fn[0], fn[1], fn[w[i].site_idx]);
            return;
        }
    }

    for (i = 0; i < 2; i++) {
        u32 *fn = (u32 *)&bkc_macho[exec->fileoff + (w[i].fn - exec->vmaddr)];
        u64 site_va = w[i].fn + 4ULL * w[i].site_idx;
        fn[w[i].site_idx + 0] = MOVZ_W0_0x2C2;
        fn[w[i].site_idx + 1] = MOVK_W0_E000_LSL16;
        fn[w[i].site_idx + 2] = b_imm(site_va + 8, w[i].epi);
    }
}

/*
 * hfs-dp-stub: report data protection class D on HFS+ volumes mounted without
 * content protection, and accept class changes on them as no-ops.
 *
 * Our root and Data volumes are plain HFS+ (no MNT_CPROTECT, no per-file
 * keys). There, hfs_vnop_getattr reports va_dataprotect_class 0 ("no class"),
 * and cp_vnode_getclass / cp_vnode_setclass return ENOTSUP. iOS 26 code that
 * expects a protection class then fails: PosterBoard rejects its data store
 * because NSURLFileProtectionKey reads nil instead of
 * NSURLFileProtectionNone (no wallpaper), and installcoordinationd aborts in
 * -[IXGlobalConfiguration createDirectories] when F_SETPROTECTIONCLASS fails
 * (no system app installs). An unencrypted volume is class D in practice, so
 * say so: getattr and cp_vnode_getclass return 4 when the file has no cprotect
 * entry, cp_vnode_setclass returns 0 without storing anything. Volumes that do
 * have content protection take their old paths unchanged.
 *
 * Sites in com.apple.filesystems.hfs.kext (xnu-12377.100.591.502 vphone600):
 *   getattr: cbz x8 (no c_cpentry) at 0xfffffe00083ed09c now goes to a stub
 *            that sets w8 = 4 and rejoins at the str to va_dataprotect_class
 *   cp_vnode_getclass: movz w21, #ENOTSUP at 0xfffffe0008405498 becomes a
 *            branch to a stub that stores 4 to *class and returns 0
 *   cp_vnode_setclass: movz w22, #ENOTSUP at 0xfffffe00084055bc -> movz w22, #0
 * The stubs go over cp_vnode_getclass's "uninitialized cnode" panic call
 * setup (0xfffffe0008405540..54), reached only on a protected volume with a
 * cnode missing its cprotect entry, which would have panicked anyway. Every
 * word is checked first; a different build is left alone.
 */
#define HFS_GETATTR_CBZ     (( 0xfffffe00083ed09cULL ))
#define HFS_GETATTR_STR     (( 0xfffffe00083ed0a8ULL ))
#define HFS_GETCLASS_ENOTSUP (( 0xfffffe0008405498ULL ))
#define HFS_GETCLASS_RET    (( 0xfffffe0008405524ULL ))
#define HFS_SETCLASS_ENOTSUP (( 0xfffffe00084055bcULL ))
#define HFS_CAVE            (( 0xfffffe000840553cULL ))

static u32 cbz_x_imm(int rt, u64 from, u64 to) {
    return 0xB4000000u | (((u32)(((i64)(to - from)) >> 2) & 0x7FFFFu) << 5) | rt;
}

void patch_kc_hfs_dp_stub(u8 *bkc_macho) {
    static const struct { u64 va; u32 word; } expect[] = {
        { HFS_GETATTR_CBZ - 4,      0xF940A668u },  /* ldr x8, [x19, #0x148]  */
        { HFS_GETATTR_CBZ,          0xB4000068u },  /* cbz x8, +0xc           */
        { HFS_GETATTR_CBZ + 4,      0xB9400508u },  /* ldr w8, [x8, #4]       */
        { HFS_GETATTR_CBZ + 8,      0x12001108u },  /* and w8, w8, #0x1f      */
        { HFS_GETATTR_STR,          0xB9013B88u },  /* str w8, [x28, #0x138]  */
        { HFS_GETCLASS_ENOTSUP,     0x528005B5u },  /* movz w21, #0x2d        */
        { HFS_GETCLASS_ENOTSUP + 4, 0x14000022u },  /* b ret                  */
        { HFS_GETCLASS_RET,         0xAA1503E0u },  /* mov x0, x21            */
        { HFS_SETCLASS_ENOTSUP,     0x528005B6u },  /* movz w22, #0x2d        */
        { HFS_CAVE,                 0x528045A8u },  /* movz w8, #0x22d        */
        { HFS_CAVE + 0x04,          0xF0FF7869u },  /* adrp x9, ...           */
        { HFS_CAVE + 0x08,          0x91138D29u },  /* add x9, x9, ...        */
        { HFS_CAVE + 0x0c,          0xA900A3E9u },  /* stp x9, x8, [sp, #8]   */
        { HFS_CAVE + 0x10,          0xF90003F4u },  /* str x20, [sp]          */
        { HFS_CAVE + 0x14,          0xF0FF7860u },  /* adrp x0, ...           */
        { HFS_CAVE + 0x18,          0x9114C800u },  /* add x0, x0, ...        */
    };
    const u64 getclass_stub = HFS_CAVE + 0x04, getattr_stub = HFS_CAVE + 0x14;
    fse_t *fse = macho_find_fileset_entry(bkc_macho, "com.apple.filesystems.hfs.kext");
    seg_t *exec;
    size_t i;

#define HFS_WORD(va) ((u32 *)&bkc_macho[exec->fileoff + ((va) - exec->vmaddr)])
    if (!fse) {
        fprintf(stderr, "hfs-dp-stub: couldn't find the HFS kext\n");
        return;
    }
    exec = macho_find_seg(&bkc_macho[fse->fileoff], "__TEXT_EXEC");
    if (!exec) {
        fprintf(stderr, "hfs-dp-stub: couldn't find HFS __TEXT_EXEC\n");
        return;
    }
    for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        if (expect[i].va < exec->vmaddr || expect[i].va >= exec->vmaddr + exec->vmsize) {
            fprintf(stderr, "hfs-dp-stub: 0x%llx out of __TEXT_EXEC; not patched\n",
                    (unsigned long long)expect[i].va);
            return;
        }
        if (*HFS_WORD(expect[i].va) != expect[i].word) {
            fprintf(stderr, "hfs-dp-stub: 0x%llx is 0x%X, expected 0x%X; not patched\n",
                    (unsigned long long)expect[i].va, *HFS_WORD(expect[i].va),
                    expect[i].word);
            return;
        }
    }

    /* cp_vnode_getclass stub: *class = 4; return 0 */
    *HFS_WORD(getclass_stub + 0x0) = 0x52800088u;                   /* movz w8, #4      */
    *HFS_WORD(getclass_stub + 0x4) = 0xB9000268u;                   /* str w8, [x19]    */
    *HFS_WORD(getclass_stub + 0x8) = 0x52800015u;                   /* movz w21, #0     */
    *HFS_WORD(getclass_stub + 0xc) = b_imm(getclass_stub + 0xc, HFS_GETCLASS_RET);
    /* getattr stub: w8 = 4, then store it as va_dataprotect_class */
    *HFS_WORD(getattr_stub + 0x0) = 0x52800088u;                    /* movz w8, #4      */
    *HFS_WORD(getattr_stub + 0x4) = b_imm(getattr_stub + 0x4, HFS_GETATTR_STR);

    *HFS_WORD(HFS_GETATTR_CBZ) = cbz_x_imm(8, HFS_GETATTR_CBZ, getattr_stub);
    *HFS_WORD(HFS_GETCLASS_ENOTSUP) = b_imm(HFS_GETCLASS_ENOTSUP, getclass_stub);
    *HFS_WORD(HFS_SETCLASS_ENOTSUP) = 0x52800016u;                  /* movz w22, #0     */
#undef HFS_WORD
}

bool kc_uses_mte(u8 *bkc_macho) {
    seg_t *kc_exec_seg = macho_find_seg(bkc_macho, "__TEXT_EXEC");
    assert(kc_exec_seg);

    u32 *kc_text = (u32*)&bkc_macho[kc_exec_seg->fileoff];
    for (size_t i = 0; i < kc_exec_seg->vmsize / sizeof(u32); i++) {
        if (IS_LDG(kc_text[i])) return true;
    }

    return false;
}
