#include "qemu/compiler.h"
#include "xnu/patch.h"
#include "xnu/mach-o/loader.h"
#include "xnu/mach-o/macho_arm64.h"
#include "xnu/boot/xnuboot.h"
#include "xnu/macho.h"

#define PACIBSP                (( 0xD503237F ))
#define RETAB                  (( 0xD65F0FFF ))
#define BTI_C                  (( 0xD503245F ))
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
