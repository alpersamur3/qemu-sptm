#include "qemu/osdep.h"
#include "exec/memattrs.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "qemu/bitops.h"
#include "xnu/boot/xnuboot.h"
#include "xnu/boot/args.h"
#include "xnu/boot/trustcache.h"
#include "xnu/apple_dtree.h"
#include "xnu/mach-o/loader.h"
#include "xnu/mach-o/macho_arm64.h"
#include "xnu/patch.h"
#include "xnu/macho.h"

// #define DEBUG_MEMORY_LAYOUT

// This will move the virtual base to place the boot kc *exactly* at the macho's virtual base.
// Only use this for debugging early kernel boot- it can cause SPTM/ TXM to freak out during launchd
// #define LINEUP_BOOTKC_NICELY

#define PAGE_SIZE ((DARWIN_PAGE_SIZE))

// SPTM overrides the virtual base for TXM/ BootKC:
// TXM actual virt addr = sptm.virtlo + 1 * SPTM_EXPECTED_STRIDE
// BKC actual virt addr = sptm.virtlo + 2 * SPTM_EXPECTED_STRIDE
#define SPTM_EXPECTED_STRIDE    0x10000000

#define GET_L2_PT_INDEX(a)      (( ((a)) & ( (BIT(36)-1)) ))
#define STRIP_L2_PT_INDEX(a)    (( ((a)) & (~(BIT(36)-1)) ))

static void alloc_ram(Object *cpuobj, hwaddr base, size_t len) {
    MemoryRegion *ram_main = get_system_memory();
    MemoryRegion *ram_subreg = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram_subreg, NULL, "dram", len, &error_fatal);
    memory_region_add_subregion(ram_main, base, ram_subreg);
}

static hwaddr vtop(macho_info_t *mi, hwaddr v) {
    return mi->physlo + (v - mi->virtlo);
}

static void set_adt_mmap(struct dtree_node *memory_map, const char *reg_name, hwaddr paddr, size_t sz) {
    u64 *map_prop = adt_get_prop_val(memory_map, reg_name);
    if (!map_prop) {
        fprintf(stderr, "dtree is missing /chosen/memory-map/%s\n", reg_name);
        exit(1);
    }
    assert(map_prop);
    map_prop[0] = paddr;
    map_prop[1] = sz;
#ifdef DEBUG_MEMORY_LAYOUT
    printf("%16s:\t0x%016llX (0x%016llX)\n", reg_name, map_prop[0], map_prop[1]);
#endif // DEBUG_MEMORY_LAYOUT
}

static trust_cache_offsets_t get_tcinfo(void) {
    assert (8 == sizeof(trust_cache_offsets_t));
    return (trust_cache_offsets_t) {
        .num_caches = 1,
        .offsets = {sizeof(trust_cache_offsets_t)},
    };
}

// Push something from the "blob" into the ADT map
static void push_adt_mmap(struct dtree_node *memory_map, const char *reg_name, hwaddr *paddr, hwaddr *last_map_entry) {
    assert(0 == ((*paddr) & (PAGE_SIZE-1)));
    set_adt_mmap(memory_map, reg_name, *last_map_entry, *paddr - *last_map_entry);
    *last_map_entry = *paddr;
}

// Push a macho segment into the "blob"
static void push_seg(hwaddr *paddr, macho_info_t *mi, const char *segname) {
    seg_t *s = macho_find_seg(mi->macho, segname);
    macho_load_seg_at(mi, s, *paddr);
    *paddr += s->vmsize;
}

static hwaddr get_seglen(macho_info_t *mi, const char *segname) {
    seg_t *s = macho_find_seg(mi->macho, segname);
    if (s) return s->vmsize;
    return 0;
}

static void arm_load_xnu_sptm(ARMCPU *cpu, MachineState *ms, struct xnu_boot_info *info) {
    u8 *dtree = (u8*)info->dtree_f.buf;
    u8 *macho_sptm = (u8*)info->sptm_f.buf;
    u8 *macho_bkc = (u8*)info->bootkc_f.buf;
    u8 *macho_txm = (u8*)info->txm_f.buf;

    alloc_ram(OBJECT(cpu), info->dram_base, info->dram_size);

    macho_info_t bkc_mi  = macho_get_info(macho_bkc);
    macho_info_t txm_mi  = macho_get_info(macho_txm);
    macho_info_t sptm_mi = macho_get_info(macho_sptm);

    hwaddr args_base_phys  = 0;
    hwaddr dtree_base_phys = 0;
    hwaddr tc_base_phys = 0;
    hwaddr rd_base_phys = 0;

    hwaddr tc_size_rounded = ROUND_NEXT_PAGE(sizeof(trust_cache_offsets_t) + info->tc_f.len);
    hwaddr dtree_size_rounded = ROUND_NEXT_PAGE(info->dtree_f.len);

    macho_verify_header(macho_bkc, true);
    macho_verify_header(macho_txm, false);
    macho_verify_header(macho_sptm, false);
    patch_kc(macho_bkc);

    struct dtree_node *map = adt_find_node((struct dtree_node*)dtree, "chosen/memory-map");
    hwaddr blob_head = info->dram_base;
    hwaddr blob_tail = blob_head;

    // PUSH_SEG(m,s): push a segment into the blob
    // m: which macho_info_t to search for the segment in
    // s: which segment to look for
#define PUSH_SEG(m,s) push_seg(&blob_head, &m##_mi, s)

    // SEGLEN(m,s): get length of a segment
    // m: which macho_info_t to search for the segment in
    // s: which segment to look for
    // Returns the number of bytes PUSH_SEG will consume when pushing this
    // segment without actually pushing it to the blob
#define SEGLEN(m,s) get_seglen(&m##_mi, s)

    // END_ENTRY(n): mark the end of an ADT memory map entry
    // n: name of the ADT entry we are ending
#define END_ENTRY(n) push_adt_mmap(map, n, &blob_head, &blob_tail)

    // SKIP(n): skip n bytes in the blob
#define SKIP(n) \
    blob_head += n; \
    blob_tail = blob_head;

    // Ideally, the boot KC is loaded *exactly* where the macho wants it,
    // because that's the file we're going to be doing the most debugging in,
    // and relocating fileset KCs is annoying.
    //
    // To make that happen, we need SPTM to be loaded 2*SPTM_EXPECTED_STRIDE
    // before BKC's virtlo.
    //
    // We can make this happen by:
    //  1. Setting the L2 page table index (bits [35:0]) to match exactly
    //  2. Setting the upper bits ([63:36]) in bargs.virtBase to
    //     2*SPTM_EXPECTED_STRIDE before where bkc wants to be loaded.
    //
    // Since we can't arbitrarily place SPTM-ro (the start of SPTM) wherever we
    // want, we need to shift the entire blob forward by enough to put it where
    // we want.
    //
    // Specifically, we need:
    // GET_L2_PT_INDEX(SPTM-ro) == GET_L2_PT_INDEX(bkc_mi.virtlo)
    //
    // First, calculate where SPTM-ro ends up if we do nothing,
    // then skip however many bytes we need to make it end up where we want
    //
    // Sliding the blob forward in memory also seems to prevent random TXM
    // panics during early launchd.
    hwaddr bytes_before_sptm =
        SEGLEN(txm, "__TEXT") +
        SEGLEN(txm, "__DATA_CONST") +
        SEGLEN(txm, "__TEXT_EXEC") +
        SEGLEN(txm, "__TEXT_BOOT_EXEC") +
        SEGLEN(bkc, "__TEXT_EXEC") +
        SEGLEN(bkc, "__TEXT_BOOT_EXEC") +
        SEGLEN(bkc, "__TEXT") +
        SEGLEN(bkc, "__PRELINK_TEXT") +
        SEGLEN(bkc, "__DATA_CONST") +
        SEGLEN(bkc, "__DATA_SPTM") +
        dtree_size_rounded +
        tc_size_rounded +
        DARWIN_PAGE_SIZE; // extra page we skip in front of the device tree

    assert(GET_L2_PT_INDEX(bkc_mi.virtlo) > bytes_before_sptm);
    SKIP(GET_L2_PT_INDEX(bkc_mi.virtlo) - bytes_before_sptm);

    // To find struct layout in SPTM, search for function that uses the string
    // "%s: region '%s' [%p-%p] not immediately after region '%s' [ending at %p]"
    // This string is used in a call to panic at the bottom of a loop that
    // iterates over structs that tell you exactly which sections SPTM wants in
    // what order.

    // TXM-ro
    {
        PUSH_SEG(txm, "__TEXT");
        PUSH_SEG(txm, "__DATA_CONST");
        END_ENTRY("TXM-ro");
    }

    // TXM-rx
    {
        PUSH_SEG(txm, "__TEXT_EXEC");
        END_ENTRY("TXM-rx");
    }

    // TXM-bx
    {
        PUSH_SEG(txm, "__TEXT_BOOT_EXEC");
        END_ENTRY("TXM-bx");
    }

    // TrustCache
    {
        tc_base_phys = blob_head;
        blob_head += tc_size_rounded;
        END_ENTRY("TrustCache");
    }

    // AuxKC-ro, AuxKC-rx (ignored)

    // BootKC-rx
    {
        PUSH_SEG(bkc, "__TEXT_EXEC");
        END_ENTRY("BootKC-rx");
    }

    // BootKC-bx
    {
        PUSH_SEG(bkc, "__TEXT_BOOT_EXEC");
        END_ENTRY("BootKC-bx");
    }

    // BootKC-ro
    {
        PUSH_SEG(bkc, "__TEXT");
        PUSH_SEG(bkc, "__PRELINK_TEXT");
        PUSH_SEG(bkc, "__DATA_CONST");
        END_ENTRY("BootKC-ro");
    }

    // BootKC-rs
    {
        PUSH_SEG(bkc, "__DATA_SPTM");
        END_ENTRY("BootKC-rs");
    }

    // CL4-rx, CL4-ro (ignored)

    // DeviceTree
    {
        // SPTM reads a few bytes before the device tree during early boot, and
        // will panic if you don't provide an extra readable page here.
        blob_head += PAGE_SIZE;
        dtree_base_phys = blob_head;
        blob_head += dtree_size_rounded;
        END_ENTRY("DeviceTree");
    }

    // SPTM gets loaded in its entirety right here
    macho_load(&sptm_mi, blob_head);

    // SPTM-ro
    {
        seg_t *sptm_text = macho_find_seg(sptm_mi.macho, "__TEXT");
        seg_t *sptm_data_const = macho_find_seg(sptm_mi.macho, "__DATA_CONST");
        seg_t *sptm_late_const = macho_find_seg(sptm_mi.macho, "__LATE_CONST");

        hwaddr sptm_ro_start = vtop(&sptm_mi, sptm_text->vmaddr);
        size_t sptm_ro_size = sptm_text->vmsize + sptm_data_const->vmsize + sptm_late_const->vmsize;
        set_adt_mmap(map, "SPTM-ro", sptm_ro_start, sptm_ro_size);
    }

    // SPTM-rx
    {
        seg_t *sptm_text_exec = macho_find_seg(sptm_mi.macho, "__TEXT_EXEC");
        seg_t *sptm_last = macho_find_seg(sptm_mi.macho, "__LAST");

        hwaddr sptm_rx_start = vtop(&sptm_mi, sptm_text_exec->vmaddr);
        size_t sptm_rx_size = sptm_text_exec->vmsize + sptm_last->vmsize;
        set_adt_mmap(map, "SPTM-rx", sptm_rx_start, sptm_rx_size);
    }

    // SPTM-rw
    {
        seg_t *sptm_data = macho_find_seg(sptm_mi.macho, "__DATA");
        seg_t *sptm_bootdata = macho_find_seg(sptm_mi.macho, "__BOOTDATA");

        hwaddr sptm_rw_start = vtop(&sptm_mi, sptm_data->vmaddr);
        size_t sptm_rw_size = sptm_data->vmsize + sptm_bootdata->vmsize;
        set_adt_mmap(map, "SPTM-rw", sptm_rw_start, sptm_rw_size);
    }

    // SPTM-le
    {
        seg_t *sptm_linkedit = macho_find_seg(sptm_mi.macho, "__LINKEDIT");

        set_adt_mmap(map, "SPTM-le", vtop(&sptm_mi, sptm_linkedit->vmaddr), sptm_linkedit->vmsize);
    }

    SKIP(sptm_mi.virthi - sptm_mi.virtlo);

    // TXM-rw
    {
        PUSH_SEG(txm, "__DATA");
        END_ENTRY("TXM-rw");
    }

    // TXM-le
    {
        PUSH_SEG(txm, "__LINKEDIT");
        END_ENTRY("TXM-le");
    }

    // BootKC-rw
    {
        PUSH_SEG(bkc, "__PRELINK_INFO");
        PUSH_SEG(bkc, "__DATA");
        END_ENTRY("BootKC-rw");
    }

    // BootKC-le
    {
        PUSH_SEG(bkc, "__LINKEDIT");
        END_ENTRY("BootKC-le");
    }

    // CL4-dummypage
    {
        blob_head += DARWIN_PAGE_SIZE;
        END_ENTRY("CL4-dummypage");
    }

    // BootArgs
    {
        args_base_phys = blob_head;
        blob_head += sizeof(boot_args);
        blob_head = ROUND_NEXT_PAGE(blob_head);
        END_ENTRY("BootArgs");
    }

    // RAMDisk
    {
        rd_base_phys = blob_head;
        blob_head += info->ramdisk_f.len;
        blob_head = ROUND_NEXT_PAGE(blob_head);
        END_ENTRY("RAMDisk");
    }

#undef PUSH_SEG
#undef END_ENTRY
#undef SKIP

    set_adt_mmap(map, "TXM-virt",  txm_mi.virtlo, 0);
    set_adt_mmap(map, "TXM-entry", txm_mi.entrypoint, 0);
    set_adt_mmap(map, "BootKC-virt",  bkc_mi.virtlo, 0);
    set_adt_mmap(map, "BootKC-entry", bkc_mi.entrypoint, 0);
    set_adt_mmap(map, "SPTM-virt",  sptm_mi.virtlo, 0);
    set_adt_mmap(map, "SPTM-entry", sptm_mi.entrypoint, 0);

    // SPTM places TXM 1x SPTM_EXPECTED_STRIDE away, and BKC 2x
    // SPTM_EXPECTED_STRIDE away in virtual memory. It completely ignores the
    // -virt dtree entries when choosing where to load TXM/ BKC. It happens
    // that TXM and the BKC machos have their virtlo 1x and 2x
    // SPTM_EXPECTED_STRIDE before SPTM's virtlo respectively. I'm not sure
    // whether SPTM is hardcoded to place TXM/ BKC this many bytes after it, or
    // whether it calculates this based on the macho header of TXM/ BKC, so for
    // now just assert the machos match what we expect in case this ever
    // changes later on.
    assert(1 * SPTM_EXPECTED_STRIDE == GET_L2_PT_INDEX(sptm_mi.virtlo - txm_mi.virtlo));
    assert(2 * SPTM_EXPECTED_STRIDE == GET_L2_PT_INDEX(sptm_mi.virtlo - bkc_mi.virtlo));

    boot_args args = {0};
    args.Revision = kBootArgsRevision2;
    args.Version = kBootArgsVersion2;

#ifdef LINEUP_BOOTKC_NICELY
    args.virtBase = STRIP_L2_PT_INDEX(bkc_mi.virtlo) - (2 * SPTM_EXPECTED_STRIDE);
#else // LINEUP_BOOTKC_NICELY
    args.virtBase = STRIP_L2_PT_INDEX(bkc_mi.virtlo);
#endif // ! LINEUP_BOOTKC_NICELY

    args.physBase = info->dram_base;
    args.memSizeActual = info->dram_size;
    if (info->has_mte) {
        args.memSize = (31 * info->dram_size) / 32;
    } else {
        args.memSize = info->dram_size;
    }
    args.topOfKernelData = blob_head;
    args.deviceTreeP = dtree_base_phys - args.physBase + args.virtBase;
    args.deviceTreeLength = info->dtree_f.len;
    g_strlcpy(args.CommandLine, info->args, BOOT_LINE_LENGTH);

#ifdef DEBUG_MEMORY_LAYOUT
    uint64_t sptm_load = args.virtBase - args.physBase + sptm_mi.physlo;
    uint64_t txm_load = sptm_load + 1 * SPTM_EXPECTED_STRIDE;
    uint64_t bkc_load = sptm_load + 2 * SPTM_EXPECTED_STRIDE;

    printf("==========\n");
    printf("SPTM base: 0x%016llX\n", sptm_load);
    printf("TXM base:  0x%016llX\n", txm_load);
    printf("BKC base:  0x%016llX\n", bkc_load);
    printf("==========\n");
#endif // DEBUG_MEMORY_LAYOUT

    address_space_write(
        &address_space_memory,
        args_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        &args,
        sizeof(args)
    );

    address_space_write(
        &address_space_memory,
        dtree_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        info->dtree_f.buf,
        info->dtree_f.len
    );

    trust_cache_offsets_t tc_info = get_tcinfo();

    address_space_write(
        &address_space_memory,
        tc_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        &tc_info,
        sizeof(tc_info)
    );

    address_space_write(
        &address_space_memory,
        tc_base_phys + sizeof(tc_info),
        MEMTXATTRS_UNSPECIFIED,
        info->tc_f.buf,
        info->tc_f.len
    );

    address_space_write(
        &address_space_memory,
        rd_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        info->ramdisk_f.buf,
        info->ramdisk_f.len
    );

    info->init_pc = vtop(&sptm_mi, sptm_mi.entrypoint);
    info->init_x0 = args_base_phys;
}

static void arm_load_xnu_nosptm(ARMCPU *cpu, MachineState *ms, struct xnu_boot_info *info) {
    u8 *macho_bkc = (u8*)info->bootkc_f.buf;

    alloc_ram(OBJECT(cpu), info->dram_base, info->dram_size);

    macho_info_t bkc_mi  = macho_get_info(macho_bkc);

    // iOS wants the trustcache to be both beneath the kernel and
    // not in the first huge page of DRAM
    hwaddr tc_base_phys = info->dram_base + DARWIN_HUGE_PAGE_SIZE;
    hwaddr xnu_load_phys = info->dram_base + XNU_STRIP_VMA(bkc_mi.virtlo);
    hwaddr args_base_phys  = info->dram_base + XNU_STRIP_VMA(bkc_mi.virthi);
    hwaddr dtree_base_phys = ROUND_NEXT_PAGE(args_base_phys + sizeof(boot_args));
    hwaddr rd_base_phys = ROUND_NEXT_PAGE(dtree_base_phys + info->dtree_f.len);

    assert(xnu_load_phys >= tc_base_phys + info->tc_f.len);
    assert(args_base_phys >= xnu_load_phys + bkc_mi.virthi - bkc_mi.virtlo);
    assert(dtree_base_phys >= args_base_phys + sizeof(boot_args));
    assert(rd_base_phys >= dtree_base_phys + info->dtree_f.len);
    assert(info->dram_base + info->dram_size > tc_base_phys + info->tc_f.len);

    // We could support loading pre-SPTM KCs with MTE, but they don't exist, so
    // since we aren't loading SPTM, confirm we aren't expecting MTE to work.
    assert(!info->has_mte);

    macho_verify_header(macho_bkc, true);
    patch_kc(macho_bkc);
    macho_load(&bkc_mi, xnu_load_phys);

    struct dtree_node *map = adt_find_node((struct dtree_node*)info->dtree_f.buf, "chosen/memory-map");
    set_adt_mmap(map, "TrustCache", tc_base_phys, info->tc_f.len + sizeof(trust_cache_offsets_t));
    set_adt_mmap(map, "RAMDisk", rd_base_phys, info->ramdisk_f.len);

    boot_args args = {0};
    args.Revision = kBootArgsRevision2;
    args.Version = kBootArgsVersion2;
    args.virtBase = STRIP_L2_PT_INDEX(bkc_mi.virtlo);
    args.physBase = info->dram_base;
    args.memSize = info->dram_size;
    args.topOfKernelData = rd_base_phys + ROUND_NEXT_PAGE(info->ramdisk_f.len);
    args.deviceTreeP = dtree_base_phys - args.physBase + args.virtBase;
    args.deviceTreeLength = info->dtree_f.len;
    g_strlcpy(args.CommandLine, info->args, BOOT_LINE_LENGTH);

    assert(args.topOfKernelData > bkc_mi.physlo + (bkc_mi.virthi - bkc_mi.virtlo));

#ifdef DEBUG_MEMORY_LAYOUT
    printf("xnu:   0x%lX\n", xnu_load_phys);
    printf("rd:    0x%lX\n", rd_base_phys);
    printf("tc:    0x%lX\n", tc_base_phys);
    printf("rdlen: 0x%lX\n", info->ramdisk_f.len);
    printf("ktop:  0x%lX\n", args.topOfKernelData);
    printf("vbase: 0x%lX\n", args.virtBase);
    printf("pbase: 0x%lX\n", args.physBase);
    printf("dtp:   0x%llX\n", args.deviceTreeP);
#endif // DEBUG_MEMORY_LAYOUT

    address_space_write(
        &address_space_memory,
        args_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        &args,
        sizeof(args)
    );

    address_space_write(
        &address_space_memory,
        dtree_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        info->dtree_f.buf,
        info->dtree_f.len
    );

    trust_cache_offsets_t tc_info = get_tcinfo();

    address_space_write(
        &address_space_memory,
        tc_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        &tc_info,
        sizeof(tc_info)
    );

    address_space_write(
        &address_space_memory,
        tc_base_phys + sizeof(tc_info),
        MEMTXATTRS_UNSPECIFIED,
        info->tc_f.buf,
        info->tc_f.len
    );

    address_space_write(
        &address_space_memory,
        rd_base_phys,
        MEMTXATTRS_UNSPECIFIED,
        info->ramdisk_f.buf,
        info->ramdisk_f.len
    );

    info->init_pc = vtop(&bkc_mi, bkc_mi.entrypoint);
    info->init_x0 = args_base_phys;
}

void arm_load_xnu(ARMCPU *cpu, MachineState *ms, struct xnu_boot_info *info) {
    if (info->sptm) arm_load_xnu_sptm(cpu, ms, info);
    else arm_load_xnu_nosptm(cpu, ms, info);
}
