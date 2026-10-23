/*
 * vresearch: Apple's VResearch1 platform, the paravirtual machine that
 * Virtualization.framework gives Private Cloud Compute research VMs
 * (vresearch101ap / vphone600ap device trees).
 *
 * XNU boots the darwin.c way: no iBoot, the loader in xnuboot_sptm.c places
 * SPTM + TXM + the boot kernelcache and fills /chosen/memory-map. The devices
 * are the vmapple ones (hw/vmapple/vmapple.c): same physical addresses and
 * interrupt numbers as the guest's device tree, where arm-io child address +
 * 0x10000000 = physical address.
 *
 * Unlike the darwin machine, the CPU starts at EL1: a VZ guest has no EL2,
 * so vresearch1's SPTM runs in GL1 and programs the _EL1 registers
 * (VBAR_EL1, SPRR_CONFIG_EL1, GXF_CONFIG_EL1, TCR_EL1). PSCI goes over HVC,
 * like vmapple; QEMU answers every other HVC with NOT_SUPPORTED.
 *
 * Use scripts/darwin/vresearch_dt_fixup.py on the device tree first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qobject/qlist.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/pvpanic.h"
#include "hw/misc/unimp.h"
#include "hw/pci/msi.h"
#include "hw/pci-host/gpex.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/vmapple/vmapple.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/gtimer.h"
#include "cpu.h"
#include "xnu/boot/xnuboot.h"
#include "xnu/apple_dtree.h"
#include "xnu/apple_regs.h"
#include "xnu/patch.h"
#include "xnu/macho.h"

#define CPACR_ENABLE_FPU         (( BIT(20) | BIT(21) ))

static const char *default_args =
    "serial=3 -v debug=0x14e cpus=1 rd=md0 wdt=-1 keepsyms=1 -noprogress";

#define TYPE_VRESEARCH_MACHINE MACHINE_TYPE_NAME("vresearch")
OBJECT_DECLARE_SIMPLE_TYPE(VResearchState, VRESEARCH_MACHINE)

enum {
    VR_CONFIG,
    VR_GIC_DIST,
    VR_GIC_REDIST,
    VR_GIC_MSI,
    VR_UART,
    VR_RTC,
    VR_GPIO,
    VR_PVPANIC,
    VR_BDOOR,
    VR_APV_GFX,
    VR_APV_IOSFC,
    VR_AES_1,
    VR_AES_2,
    VR_AVP_RTC,
    VR_SEP,
    VR_AVP_CTRR,
    VR_PCIE,
    VR_PCIE_ECAM,
    VR_PCIE_MMIO,
};

static const MemMapEntry memmap[] = {
    /*
     * vmapple's config page, but larger: AppleVirtualPlatformPowerSource rings
     * a doorbell at 0x4ffff8 (the physical address of a request block)
     */
    [VR_CONFIG] =       { 0x00400000, 0x00100000 },
    [VR_GIC_DIST] =     { 0x10000000, 0x00010000 },
    [VR_GIC_REDIST] =   { 0x10010000, 0x00400000 },
    [VR_GIC_MSI] =      { 0x1fff0000, 0x00010000 },
    [VR_UART] =         { 0x20010000, 0x00010000 },
    [VR_RTC] =          { 0x20050000, 0x00001000 },
    [VR_GPIO] =         { 0x20060000, 0x00001000 },
    [VR_PVPANIC] =      { 0x20070000, 0x00000002 },
    /* vmapple's backdoor window; AppleVirtualPlatform's SimpleVirtio is at +0x10000 */
    [VR_BDOOR] =        { 0x30000000, 0x00200000 },
    [VR_APV_GFX] =      { 0x30200000, 0x00010000 },
    [VR_APV_IOSFC] =    { 0x30210000, 0x00010000 },
    [VR_AES_1] =        { 0x30220000, 0x00004000 },
    [VR_AES_2] =        { 0x30230000, 0x00004000 },
    [VR_AVP_RTC] =      { 0x30240000, 0x00001000 },
    [VR_SEP] =          { 0x30250000, 0x00001000 },
    [VR_AVP_CTRR] =     { 0x30260000, 0x00010000 },
    [VR_PCIE_ECAM] =    { 0x40000000, 0x10000000 },
    /* the device tree's window is larger than vmapple's (0x1fff0000) */
    [VR_PCIE_MMIO] =    { 0x50000000, 0x2fff0000 },
};

/* SPI numbers: the device tree's INTIDs - 32 */
static const int irqmap[] = {
    [VR_UART] = 1,
    [VR_RTC] = 2,
    [VR_GPIO] = 0x5,
    [VR_APV_IOSFC] = 0x10,
    [VR_APV_GFX] = 0x11,
    [VR_AES_1] = 0x12,
    [VR_AVP_RTC] = 0x13,
    [VR_PCIE] = 0x20,
};

#define NUM_IRQS        256
#define GPEX_NUM_IRQS   16

/* MSI frame: GICv2m-like, SPIs INTID 128-191 */
#define MSI_TYPER       0x008
#define MSI_SETSPI      0x040
#define MSI_IIDR        0xfcc
#define MSI_TYPER_VALID BIT(31)
#define MSI_BASE_INTID  128
#define MSI_NUM_SPIS    64

struct VResearchState {
    MachineState           parent_obj;
    ARMCPU                *cpu;
    DeviceState           *gic;
    MemoryRegion           msi_frame;
    MemoryRegion           ecam_alias;
    struct xnu_boot_info   bootinfo;
    GMappedFile           *bootkc_m, *dtree_m, *sptm_m, *txm_m, *tc_m, *ramdisk_m;
};

#define VRESEARCH_ARG(a) \
static char *a##_get(Object *obj, Error **errp) { \
    return g_strdup(VRESEARCH_MACHINE(obj)->bootinfo.a); \
} \
static void a##_set(Object *obj, const char *val, Error **errp) { \
    VResearchState *s = VRESEARCH_MACHINE(obj); \
    g_free(s->bootinfo.a); \
    s->bootinfo.a = g_strdup(val); \
}

VRESEARCH_ARG(bootkc)
VRESEARCH_ARG(args)
VRESEARCH_ARG(dtree)
VRESEARCH_ARG(sptm)
VRESEARCH_ARG(txm)
VRESEARCH_ARG(tc)
VRESEARCH_ARG(ramdisk)

/*
 * A private, writable mapping (the loader patches the kernelcache and the
 * device tree in place). GMappedFile also works on Windows, unlike mmap.
 */
static GMappedFile *map_file(const char *path, const char *what, mmap_file_t *f)
{
    g_autoptr(GError) err = NULL;
    GMappedFile *m;

    if (!path) {
        error_report("vresearch: no %s given (-%s)", what, what);
        exit(1);
    }
    m = g_mapped_file_new(path, TRUE, &err);
    if (!m) {
        error_report("vresearch: can't open %s '%s': %s", what, path, err->message);
        exit(1);
    }
    f->buf = g_mapped_file_get_contents(m);
    f->len = g_mapped_file_get_length(m);
    if (!f->buf || !f->len) {
        error_report("vresearch: %s '%s' is empty", what, path);
        exit(1);
    }
    /* is_im4p() looks at the first 0x34 bytes; a trust cache can be smaller */
    if (f->len >= 0x34 && is_im4p(f->buf)) {
        error_report("vresearch: %s '%s' is an im4p, unwrap it first", what, path);
        exit(1);
    }
    return m;
}

static void vresearch_reset(void *opaque)
{
    VResearchState *s = opaque;
    CPUARMState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));

    /* no EL2/EL3, so the reset state is EL1h */
    assert(1 == arm_highest_el(env));
    env->xregs[0] = s->bootinfo.init_x0;
    env->pc = s->bootinfo.init_pc;

    /* SPTM needs the FPU enabled */
    env->cp15.cpacr_el1 |= CPACR_ENABLE_FPU;
    arm_rebuild_hflags(env);
}

static void setup_mte(Object *cpuobj, MachineState *ms, struct xnu_boot_info *info)
{
    MemoryRegion *tag_sysmem, *tagram;

    if (!object_property_find(cpuobj, "tag-memory")) {
        error_report("vresearch: cpu has no tag-memory property; MTE can't be enabled");
        exit(1);
    }

    tag_sysmem = g_new(MemoryRegion, 1);
    memory_region_init(tag_sysmem, OBJECT(ms), "tag-memory", UINT64_MAX / 32);
    object_property_set_link(cpuobj, "tag-memory", OBJECT(tag_sysmem), &error_abort);

    tagram = g_new(MemoryRegion, 1);
    memory_region_init_ram(tagram, NULL, "mte_tags", info->dram_size / 32, &error_fatal);
    memory_region_add_subregion(tag_sysmem, info->dram_base / 32, tagram);
}

static qemu_irq spi(VResearchState *s, int n)
{
    return qdev_get_gpio_in(s->gic, n);
}

/*
 * AppleVirtualPlatformPCIEMSIController reads a GICv2m MSI_TYPER with a valid
 * bit on top (bit 31; [28:16] base INTID, [12:0] count) and points MSIs at
 * frame + 0x40, where writing an INTID raises that SPI.
 */
static uint64_t msi_frame_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case MSI_TYPER:
        return MSI_TYPER_VALID | (MSI_BASE_INTID << 16) | MSI_NUM_SPIS;
    case MSI_IIDR:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "gic-msi: read at 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void msi_frame_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    VResearchState *s = opaque;

    if (addr == MSI_SETSPI && val >= MSI_BASE_INTID &&
        val < MSI_BASE_INTID + MSI_NUM_SPIS) {
        qemu_irq_pulse(spi(s, val - GIC_INTERNAL));
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "gic-msi: write 0x%" PRIx64 " at 0x%"
                  HWADDR_PRIx "\n", val, addr);
}

static const MemoryRegionOps msi_frame_ops = {
    .read = msi_frame_read,
    .write = msi_frame_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void create_gic(VResearchState *s)
{
    DeviceState *cpudev = DEVICE(s->cpu);
    SysBusDevice *gicbusdev;
    QList *redist_region_count;

    s->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", 1);
    qdev_prop_set_uint32(s->gic, "num-irq", NUM_IRQS + GIC_INTERNAL);

    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, 1);
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);

    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, memmap[VR_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, memmap[VR_GIC_REDIST].base);

    /* PCIe's MSI frame (the gic's reg #2, msi-frame-index = 2) */
    memory_region_init_io(&s->msi_frame, OBJECT(s), &msi_frame_ops, s,
                          "gic-msi", memmap[VR_GIC_MSI].size);
    memory_region_add_subregion(get_system_memory(), memmap[VR_GIC_MSI].base,
                                &s->msi_frame);
    msi_nonbroken = true;

    /*
     * PPIs are INTID 27 (virtual timer) and 30 (physical timer). vmapple only
     * wires the former; an EL1 kernel could use either.
     */
    qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
                          qdev_get_gpio_in(s->gic, NUM_IRQS + 27));
    qdev_connect_gpio_out(cpudev, GTIMER_PHYS,
                          qdev_get_gpio_in(s->gic, NUM_IRQS + 30));

    sysbus_connect_irq(gicbusdev, 0, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
    sysbus_connect_irq(gicbusdev, 1, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
}

static void create_pcie(VResearchState *s)
{
    hwaddr base_mmio = memmap[VR_PCIE_MMIO].base;
    hwaddr size_mmio = memmap[VR_PCIE_MMIO].size;
    MemoryRegion *mmio_alias, *mmio_reg, *ecam_reg;
    DeviceState *dev;
    PCIHostState *pci;
    int irq = irqmap[VR_PCIE];
    int i;

    dev = qdev_new(TYPE_GPEX_HOST);
    qdev_prop_set_uint32(dev, "num-irqs", GPEX_NUM_IRQS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(&s->ecam_alias, OBJECT(dev), "pcie-ecam",
                             ecam_reg, 0, memmap[VR_PCIE_ECAM].size);
    memory_region_add_subregion(get_system_memory(), memmap[VR_PCIE_ECAM].base,
                                &s->ecam_alias);

    /* PCI addresses = CPU addresses in the MMIO window */
    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pcie-mmio",
                             mmio_reg, base_mmio, size_mmio);
    memory_region_add_subregion(get_system_memory(), base_mmio, mmio_alias);

    for (i = 0; i < GPEX_NUM_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, spi(s, irq + i));
        gpex_set_irq_num(GPEX_HOST(dev), i, irq + i);
    }

    pci = PCI_HOST_BRIDGE(dev);
    while ((dev = qemu_create_nic_device("virtio-net-pci", true, NULL))) {
        qdev_realize_and_unref(dev, BUS(pci->bus), &error_fatal);
    }
}

/*
 * /product/util: a 32 MB block split by its 'regions' (id, offset, size)
 * triplets, presumably memory shared with the host. iBoot fills its reg; here
 * the fixup script does, and this backs it with zeroed RAM.
 */
static void create_util(struct dtree_node *dt_root)
{
    struct dtree_node *util = adt_find_node(dt_root, "product/util");
    struct adt_io_reg *reg;
    MemoryRegion *mr;

    if (!util || adt_get_prop_len(util, "reg") < sizeof(*reg)) {
        return;
    }
    reg = adt_get_prop_val(util, "reg");
    mr = g_new(MemoryRegion, 1);
    memory_region_init_ram(mr, NULL, "util", reg->len, &error_fatal);
    memory_region_add_subregion(get_system_memory(), reg->base, mr);
}

static void create_devices(VResearchState *s)
{
    SysBusDevice *sbd;

    create_gic(s);

    pl011_create(memmap[VR_UART].base, spi(s, irqmap[VR_UART]), serial_hd(0));
    sysbus_create_simple("pl031", memmap[VR_RTC].base, spi(s, irqmap[VR_RTC]));
    sysbus_create_simple("pl061", memmap[VR_GPIO].base, spi(s, irqmap[VR_GPIO]));

    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_PVPANIC_MMIO_DEVICE));
    sysbus_mmio_map(sbd, 0, memmap[VR_PVPANIC].base);
    sysbus_realize_and_unref(sbd, &error_fatal);

    /*
     * Needs a crypto backend (nettle, gcrypt or gnutls): QEMU has no built-in
     * ciphers any more, every AES command then fails, and the kernel waits
     * for it forever before launchd.
     */
    sbd = SYS_BUS_DEVICE(qdev_new(TYPE_APPLE_AES));
    sysbus_mmio_map(sbd, 0, memmap[VR_AES_1].base);
    sysbus_mmio_map(sbd, 1, memmap[VR_AES_2].base);
    sysbus_connect_irq(sbd, 0, spi(s, irqmap[VR_AES_1]));
    sysbus_realize_and_unref(sbd, &error_fatal);

    /*
     * Not modelled yet. Reads return 0, so the PVG driver sees no host GPU,
     * avp,ctrr has no 'ctrr' magic and the kernel skips it, SimpleVirtio's
     * host interface (fixed at 0x30010000 in the kernel) has no version 1.
     */
    create_unimplemented_device("vz-backdoor", memmap[VR_BDOOR].base,
                                memmap[VR_BDOOR].size);
    create_unimplemented_device("vz-config", memmap[VR_CONFIG].base,
                                memmap[VR_CONFIG].size);
    create_unimplemented_device("apv-gfx", memmap[VR_APV_GFX].base,
                                memmap[VR_APV_GFX].size);
    create_unimplemented_device("apv-iosfc", memmap[VR_APV_IOSFC].base,
                                memmap[VR_APV_IOSFC].size);
    create_unimplemented_device("avp-rtc", memmap[VR_AVP_RTC].base,
                                memmap[VR_AVP_RTC].size);
    create_unimplemented_device("sep-mailbox", memmap[VR_SEP].base,
                                memmap[VR_SEP].size);
    create_unimplemented_device("avp-ctrr", memmap[VR_AVP_CTRR].base,
                                memmap[VR_AVP_CTRR].size);

    create_pcie(s);
}

static void vresearch_init(MachineState *ms)
{
    VResearchState *s = VRESEARCH_MACHINE(ms);
    struct xnu_boot_info *info = &s->bootinfo;
    struct dtree_node *dt_root, *chosen, *cpu0;
    u64 *dram_base, *dram_size;
    u32 *freq;
    Object *cpuobj;
    ARMCPU *cpu;

    if (!info->args) {
        info->args = g_strdup(default_args);
    }
    if (!info->sptm || !info->txm) {
        error_report("vresearch: needs -sptm and -txm");
        exit(1);
    }
    s->bootkc_m = map_file(info->bootkc, "bootkc", &info->bootkc_f);
    s->dtree_m = map_file(info->dtree, "dtree", &info->dtree_f);
    s->sptm_m = map_file(info->sptm, "sptm", &info->sptm_f);
    s->txm_m = map_file(info->txm, "txm", &info->txm_f);
    s->tc_m = map_file(info->tc, "tc", &info->tc_f);
    s->ramdisk_m = map_file(info->ramdisk, "ramdisk", &info->ramdisk_f);
    dt_root = info->dtree_f.buf;

    chosen = adt_find_node(dt_root, "chosen");
    cpu0 = adt_find_node(dt_root, "cpus/cpu0");
    dram_base = chosen ? adt_get_prop_val(chosen, "dram-base") : NULL;
    dram_size = chosen ? adt_get_prop_val(chosen, "dram-size") : NULL;
    freq = cpu0 ? adt_get_prop_val(cpu0, "timebase-frequency") : NULL;
    if (!dram_base || !dram_size || !freq || !adt_find_node(dt_root, "arm-io/gic")) {
        error_report("vresearch: '%s' isn't a fixed-up VResearch1 device tree "
                     "(run vresearch_dt_fixup.py)", info->dtree);
        exit(1);
    }
    info->dram_base = *dram_base;
    info->dram_size = *dram_size;
    info->has_mte = kc_uses_mte(info->bootkc_f.buf);

    cpuobj = object_new(ms->cpu_type);
    cpu = ARM_CPU(cpuobj);
    s->cpu = cpu;

    /* No EL2/EL3: the kernel and SPTM run at EL1 / GL1, as in a VZ guest */
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    if (object_property_find(cpuobj, "has_el2")) {
        object_property_set_bool(cpuobj, "has_el2", false, &error_fatal);
    }
    object_property_set_int(cpuobj, "psci-conduit", QEMU_PSCI_CONDUIT_HVC,
                            &error_fatal);
    object_property_set_int(cpuobj, "mp-affinity", 0, &error_fatal);

    /* As in darwin.c: CBAR and PMU MSRs collide with Apple's */
    unset_feature(&cpu->env, ARM_FEATURE_CBAR);
    unset_feature(&cpu->env, ARM_FEATURE_CBAR_RO);
    unset_feature(&cpu->env, ARM_FEATURE_PMU);

    /* 24 MHz in the vphone600ap / vresearch101ap device trees */
    cpu->gt_cntfrq_hz = *freq;

    arm_load_xnu(cpu, ms, info);
    if (info->has_mte) {
        setup_mte(cpuobj, ms, info);
    }
    apple_regs_init(cpu, NULL, dt_root, info);

    /* As in darwin.c, for kernels that check SME's max vector length */
    cpu->sme_vq.supported = 0x0b;

    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    create_devices(s);
    create_util(dt_root);

    qemu_register_reset(vresearch_reset, s);

    g_clear_pointer(&s->bootkc_m, g_mapped_file_unref);
    g_clear_pointer(&s->dtree_m, g_mapped_file_unref);
    g_clear_pointer(&s->sptm_m, g_mapped_file_unref);
    g_clear_pointer(&s->txm_m, g_mapped_file_unref);
    g_clear_pointer(&s->tc_m, g_mapped_file_unref);
    g_clear_pointer(&s->ramdisk_m, g_mapped_file_unref);
}

static GlobalProperty vresearch_compat_defaults[] = {
    { TYPE_VIRTIO_PCI, "disable-legacy", "on" },
    /* no option ROM: nothing would run it, and a Windows build may not ship efi-virtio.rom */
    { "virtio-net-pci", "romfile", "" },
};

static void vresearch_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple VResearch1 research VM (vphone600ap, vresearch101ap)";
    mc->init = vresearch_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("max");
    mc->block_default_type = IF_VIRTIO;
    mc->no_cdrom = 1;
    mc->pci_allow_0_address = true;
    mc->minimum_page_bits = 12;

    compat_props_add(mc->compat_props, vresearch_compat_defaults,
                     G_N_ELEMENTS(vresearch_compat_defaults));

    object_class_property_add_str(oc, "bootkc", bootkc_get, bootkc_set);
    object_class_property_add_str(oc, "args", args_get, args_set);
    object_class_property_add_str(oc, "dtree", dtree_get, dtree_set);
    object_class_property_add_str(oc, "sptm", sptm_get, sptm_set);
    object_class_property_add_str(oc, "txm", txm_get, txm_set);
    object_class_property_add_str(oc, "tc", tc_get, tc_set);
    object_class_property_add_str(oc, "ramdisk", ramdisk_get, ramdisk_set);
}

static const TypeInfo vresearch_machine_info = {
    .name           = TYPE_VRESEARCH_MACHINE,
    .parent         = TYPE_MACHINE,
    .instance_size  = sizeof(VResearchState),
    .class_init     = vresearch_machine_class_init,
    .interfaces     = arm_machine_interfaces,
};

static void vresearch_register_types(void)
{
    type_register_static(&vresearch_machine_info);
}

type_init(vresearch_register_types)
