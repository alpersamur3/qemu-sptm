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
     * Fourth: selectors 17 and 35 (get device state / extended device state)
     * return a small DER SET as struct output, not a scalar. With no SEP they
     * fail (sep_endpoint() failed, ret 0xE00002D8) and the daemons retry
     * forever. Answer with the same synthetic DER iOS 16's SweetKeyStore gave:
     *   SET { "ss":4, "sls":0, "sb":0, "sfa":0, "sgs":0, "sas":0,
     *         "sgpe":0, "srcd":0 }   (ss=4 => unlocked since boot)
     * encoded as 0x31 len, then per key 0x0c len <name> 0x02 0x01 <value>.
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
        static const u8 der[66] = {
            0x31, 0x40,
            0x0c,0x02,'s','s',          0x02,0x01,0x04,
            0x0c,0x03,'s','l','s',      0x02,0x01,0x00,
            0x0c,0x02,'s','b',          0x02,0x01,0x00,
            0x0c,0x03,'s','f','a',      0x02,0x01,0x00,
            0x0c,0x03,'s','g','s',      0x02,0x01,0x00,
            0x0c,0x03,'s','a','s',      0x02,0x01,0x00,
            0x0c,0x04,'s','g','p','e',  0x02,0x01,0x00,
            0x0c,0x04,'s','r','c','d',  0x02,0x01,0x00,
        };
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
        for (i = 0; i + 8 <= 64; i += 8) {  /* 8 full qwords of the 66-byte blob */
            u64 v = 0;
            int k;
            for (k = 0; k < 8; k++) {
                v |= (u64)der[i + k] << (8 * k);
            }
            n += emit_mov_x8(&code[n], v);
            code[n++] = 0xf9000008u | ((u32)(i / 8) << 10);  /* str x8, [x0, #i] */
        }
        code[n++] = 0x52800008u | ((u32)(der[64] | (der[65] << 8)) << 5); /* movz w8, #last2 */
        code[n++] = 0x79008008u;            /* strh w8, [x0, #64]  (64/2=32<<10) */
        ret_idx = n;
        code[n++] = 0x52800848u;            /* movz w8, #66  (0x42) */
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

bool kc_uses_mte(u8 *bkc_macho) {
    seg_t *kc_exec_seg = macho_find_seg(bkc_macho, "__TEXT_EXEC");
    assert(kc_exec_seg);

    u32 *kc_text = (u32*)&bkc_macho[kc_exec_seg->fileoff];
    for (size_t i = 0; i < kc_exec_seg->vmsize / sizeof(u32); i++) {
        if (IS_LDG(kc_text[i])) return true;
    }

    return false;
}
