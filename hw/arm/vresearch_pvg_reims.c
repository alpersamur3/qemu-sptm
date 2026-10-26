/*
 * vresearch PVG on reims-vgpu: Apple's paravirtualised GPU (/arm-io/gfx,
 * AppleParavirtGPU) and its IOSurface mapper (/arm-io/iosurface-mapper) with
 * the reims-vgpu library behind them (pvg=reims), which runs the guest's
 * command FIFOs on the host GPU through Vulkan. The display-only model in
 * vresearch_pvg.c (pvg=on) parses the FIFOs itself and drops the GPU work;
 * this device forwards every register access to the library and keeps only
 * the glue:
 *
 * - The library's GPU registers (RVG_DEVICE_MMIO_SIZE, 0x4000) and the
 *   IOSurface device's (0x1000-0x1030) decode offsets exactly as the model
 *   does, so the raw offsets of the two 0x10000 windows are forwarded.
 * - The guest's RAM reaches the library as a memory map built from the
 *   system memory's flat view at machine-init-done, each range with the
 *   pagefile section behind it (qemu_win_ram_section(), the Windows memory
 *   file ABI: fd = the section handle, offset = the range's byte offset in
 *   it) so that the library can alias runs of guest pages for the root page,
 *   the FIFO rings and the surfaces.
 * - TCG takes the BQL at every MMIO access and a root FIFO write waits for
 *   the library's FIFO thread, so no library callback may take the BQL or
 *   wait for a bottom half: raise_interrupt only schedules one. The GPU line
 *   is driven high from that bottom half and low when the guest reads the
 *   display (0x1014) or stamp (0x1018) mask, which clears the event in the
 *   library, so a level-triggered SPI never loses it; the IOSurface line is
 *   pulsed per command from a bottom half.
 * - Frames are read back on a thread of their own: new_frame only sets an
 *   event, and the frame thread encodes the display's current frame into a
 *   frame target, waits for it and reads its pixels into the console's
 *   surface, so neither the library's display thread nor a vCPU ever waits
 *   on the GPU (the guest's next present waits for a frame in flight). New
 *   frames coalesce: an encode takes whatever the guest presented last.
 * - The library device is never released: its teardown waits for FIFO work
 *   that never completes once the vCPUs stop. Migration is blocked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "migration/blocker.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "ui/console.h"
#include "hw/arm/vresearch_pvg.h"
#include "reims_vgpu.h"

OBJECT_DECLARE_SIMPLE_TYPE(VResearchPVGReimsState, VRESEARCH_PVG_REIMS)

/* The guest's page (the library's APV page, a guest-arm64 build) */
#define PVG_PAGE            0x4000ULL

/* The config versions this file was written against */
QEMU_BUILD_BUG_ON(sizeof(RvgDeviceConfig) != 64);
QEMU_BUILD_BUG_ON(sizeof(RvgHostCallbacks) != 88);
QEMU_BUILD_BUG_ON(sizeof(RvgMemoryRangeFile) != 16);

struct VResearchPVGReimsState {
    SysBusDevice parent_obj;
    MemoryRegion iomem_gfx;
    MemoryRegion iomem_iosfc;
    qemu_irq irq_gfx;
    qemu_irq irq_iosfc;

    uint32_t width, height;     /* the display's one mode */

    Notifier init_done;
    Error *mig_blocker;

    /* the library's objects; NULL until machine-init-done */
    RvgDevice *dev;
    RvgIOSurfaceDevice *iosfc;
    RvgDisplay *display;

    /* a GPU raise waits for its bottom half (written on library threads) */
    int gfx_bh_pending;
    uint64_t gfx_raises;
    uint64_t iosfc_raises;

    /* the display thread's reports */
    uint32_t mode_w, mode_h, mode_fmt;
    uint64_t frames;

    /* the console: the frame thread's last read-back, black until then */
    QemuConsole *con;
    uint32_t *frame;

    /* the frame path: new_frame sets the event, the thread reads back */
    QemuEvent frame_event;
    QemuThread frame_thread;
    RvgFrameTarget *target;
    uint64_t shown;
    unsigned frame_errors;      /* the frame thread's own; logged up to 16 */

    unsigned irq_reads;
    unsigned log_budget;
};

/* --- logging (any thread, possibly under the library's locks: no BQL) --- */

static void reims_log(void *ctx, RvgLogLevel level, const char *target,
                      size_t target_len, const char *message, size_t message_len)
{
    static const char *const names[] = {
        "off", "error", "warn", "info", "debug", "trace",
    };
    const char *name = level < ARRAY_SIZE(names) ? names[level] : "?";

    fprintf(stderr, "reims[%s] %.*s: %.*s\n", name, (int)MIN(target_len, 256),
            target, (int)MIN(message_len, 4096), message);
    fflush(stderr);
}

/* --- interrupts --- */

/*
 * Runs on the main loop with the BQL. The line stays high until the guest
 * reads 0x1014 or 0x1018 (gfx_read); a raise that arrives meanwhile queues
 * this again and re-raises it after that read.
 */
static void gfx_irq_bh(void *opaque)
{
    VResearchPVGReimsState *s = opaque;

    qatomic_set(&s->gfx_bh_pending, 0);
    qemu_set_irq(s->irq_gfx, 1);
}

/*
 * A library thread (a FIFO's or the reporting thread), or the vCPU inside
 * an MMIO access that called back, with the BQL held by a vCPU that may be
 * waiting for this very thread: schedule and return.
 */
static void gfx_raise_interrupt(void *ctx, uint32_t vector)
{
    VResearchPVGReimsState *s = ctx;

    qatomic_inc(&s->gfx_raises);
    if (qatomic_xchg(&s->gfx_bh_pending, 1) == 0) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), gfx_irq_bh, s);
    }
}

static void iosfc_irq_bh(void *opaque)
{
    VResearchPVGReimsState *s = opaque;

    qemu_irq_pulse(s->irq_iosfc);
}

/* The IOSurface device's worker thread: one raise a command. */
static void iosfc_raise_interrupt(void *ctx, uint32_t vector)
{
    VResearchPVGReimsState *s = ctx;

    qatomic_inc(&s->iosfc_raises);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), iosfc_irq_bh, s);
}

/* --- display callbacks (the library's display thread; may block) --- */

static void display_mode_change(void *ctx, uint16_t width, uint16_t height,
                                uint32_t pixel_format)
{
    VResearchPVGReimsState *s = ctx;

    qatomic_set(&s->mode_w, width);
    qatomic_set(&s->mode_h, height);
    qatomic_set(&s->mode_fmt, pixel_format);
    fprintf(stderr, "vresearch-pvg-reims: display mode %ux%u, pixel format "
            "0x%08x\n", width, height, pixel_format);
    fflush(stderr);
}

static void display_new_frame(void *ctx)
{
    VResearchPVGReimsState *s = ctx;
    uint64_t n = qatomic_fetch_inc(&s->frames) + 1;

    if (n <= 8 || n % 256 == 0) {
        fprintf(stderr, "vresearch-pvg-reims: new frame %" PRIu64 "\n", n);
        fflush(stderr);
    }
    /* the frame thread reads it back; no waiting here */
    qemu_event_set(&s->frame_event);
}

/* --- the frame path (its own thread; no BQL, no library callback) --- */

/*
 * One read-back: encode the display's current frame into the target, wait
 * for the GPU, copy the target into the console's surface. An encode that
 * Metal refuses is skipped; a committed frame is always released after its
 * wait, so none stays in flight to hold the guest's presents up.
 */
static void frame_error(VResearchPVGReimsState *s, const char *what, RvgStatus st)
{
    if (s->frame_errors < 16) {
        s->frame_errors++;
        fprintf(stderr, "vresearch-pvg-reims: frame %s: %s (%u)\n", what,
                rvg_status_message(st), st);
        fflush(stderr);
    }
}

static void frame_read_back(VResearchPVGReimsState *s)
{
    size_t stride = (size_t)s->width * 4;
    RvgFrame *frame = NULL;
    RvgStatus st;
    uint64_t n;

    st = rvg_display_encode_frame(s->display, s->target, NULL, &frame);
    if (st != RVG_OK || !frame) {
        frame_error(s, "encode refused", st);
        return;
    }
    st = rvg_frame_commit(frame);
    if (st == RVG_OK) {
        st = rvg_frame_wait(frame);
    }
    rvg_frame_release(frame);
    if (st != RVG_OK) {
        frame_error(s, "failed on the GPU", st);
        return;
    }
    st = rvg_frame_target_read_pixels(s->target, s->frame, stride,
                                      stride * s->height);
    if (st != RVG_OK) {
        frame_error(s, "read-back refused", st);
        return;
    }
    n = qatomic_fetch_inc(&s->shown) + 1;
    if (n <= 8 || n % 256 == 0) {
        uint64_t guest = 0, host = 0;

        rvg_display_present_counts(s->display, &guest, &host);
        fprintf(stderr, "vresearch-pvg-reims: frame %" PRIu64 " shown "
                "(presents: guest %" PRIu64 ", host %" PRIu64 ")\n", n, guest, host);
        fflush(stderr);
    }
}

/*
 * Waits for new_frame and reads each frame back. A new_frame that arrives
 * while one is read back leaves the event set, so the next encode takes the
 * guest's latest frame. Runs for the machine's life, like the library
 * device, which is never released.
 */
static void *frame_thread_fn(void *opaque)
{
    VResearchPVGReimsState *s = opaque;

    for (;;) {
        qemu_event_wait(&s->frame_event);
        qemu_event_reset(&s->frame_event);
        frame_read_back(s);
    }
    return NULL;
}

/* --- the console --- */

static bool fb_update(void *opaque)
{
    VResearchPVGReimsState *s = opaque;

    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps pvg_reims_gfx_ops = {
    .gfx_update = fb_update,
};

/* --- GPU registers --- */

static uint64_t gfx_read(void *opaque, hwaddr addr, unsigned size)
{
    VResearchPVGReimsState *s = opaque;
    uint32_t reg = 0x1000 + 4 * (uint16_t)((addr - 0x1000) / 4);
    uint32_t v;

    if (!s->dev || addr >= RVG_DEVICE_MMIO_SIZE) {
        return 0;
    }
    v = rvg_device_mmio_read(s->dev, addr);
    if (reg == 0x1014 || reg == 0x1018) {
        /*
         * The guest took the display (0x1014) or stamp (0x1018) events, which
         * the read cleared in the library: lower the line. A raise since the
         * read, or one still waiting for its bottom half, raises it again
         * once this access has released the BQL.
         */
        qemu_set_irq(s->irq_gfx, 0);
        if (s->irq_reads < 16) {
            s->irq_reads++;
            qemu_log_mask(LOG_UNIMP, "pvg: read 0x%x = 0x%x\n", reg, v);
        }
    }
    return v;
}

static void gfx_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VResearchPVGReimsState *s = opaque;
    uint32_t reg = 0x1000 + 4 * (uint16_t)((addr - 0x1000) / 4);

    if (!s->dev || addr >= RVG_DEVICE_MMIO_SIZE) {
        return;
    }
    switch (reg) {
    case 0x1000:    /* root FIFO enable */
    case 0x1004:    /* root FIFO length */
    case 0x1010:    /* root FIFO start */
    case 0x101c:    /* root page */
    case 0x1030:    /* root FIFO base page */
    case 0x1034:    /* binary version */
        qemu_log_mask(LOG_UNIMP, "pvg: write 0x%x <- 0x%" PRIx64 "\n", reg, val);
        break;
    default:
        break;
    }
    rvg_device_mmio_write(s->dev, addr, (uint32_t)val);
}

static const MemoryRegionOps gfx_ops = {
    .read = gfx_read,
    .write = gfx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* --- IOSurface mapper registers --- */

static uint64_t iosfc_read(void *opaque, hwaddr addr, unsigned size)
{
    VResearchPVGReimsState *s = opaque;

    if (!s->iosfc) {
        return 0;
    }
    return rvg_iosurface_device_mmio_read(s->iosfc, addr);
}

static void iosfc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VResearchPVGReimsState *s = opaque;

    if (!s->iosfc) {
        return;
    }
    if (s->log_budget && addr != 0x1018 && addr != 0x1020) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP, "pvg: iosfc write 0x%" HWADDR_PRIx " <- 0x%"
                      PRIx64 "\n", addr, val);
    }
    rvg_iosurface_device_mmio_write(s->iosfc, addr, val);
}

static const MemoryRegionOps iosfc_ops = {
    .read = iosfc_read,
    .write = iosfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

/* --- the memory map --- */

typedef struct MapBuilder {
    GArray *ranges;         /* RvgMemoryRange */
    GArray *files;          /* RvgMemoryRangeFile */
    unsigned skipped;
    unsigned without_file;
} MapBuilder;

/*
 * Every range of plain RAM the guest sees, in flat-view order, with the
 * section behind its host memory. The library wants each range to be whole
 * guest pages (process isolation), and a file offset in host pages.
 */
static bool map_add_range(Int128 start, Int128 len, const MemoryRegion *mr,
                          hwaddr offset_in_region, void *opaque)
{
    MapBuilder *b = opaque;
    uint64_t gpa, size, offset = 0;
    uint8_t *host;
    HANDLE section;
    RvgMemoryRange range;
    RvgMemoryRangeFile file;

    if (!memory_region_is_ram(mr) || memory_region_is_rom(mr) ||
        memory_region_is_romd(mr) || memory_region_is_ram_device(mr)) {
        return false;
    }
    gpa = int128_get64(start);
    size = int128_get64(len);
    host = (uint8_t *)memory_region_get_ram_ptr(mr) + offset_in_region;
    section = qemu_win_ram_section(host, &offset);
    if (gpa % PVG_PAGE || size % PVG_PAGE || offset % qemu_real_host_page_size()) {
        warn_report("vresearch-pvg-reims: RAM '%s' at 0x%" PRIx64 " (%" PRIu64
                    " bytes) left out of the GPU's memory map: not whole 16 KiB pages",
                    memory_region_name(mr), gpa, size);
        b->skipped++;
        return false;
    }
    if (!section) {
        warn_report("vresearch-pvg-reims: RAM '%s' at 0x%" PRIx64 " has no section "
                    "behind it: the GPU can't alias its pages", memory_region_name(mr),
                    gpa);
        b->without_file++;
    }
    range = (RvgMemoryRange) {
        .physical_address = gpa, .physical_length = size, .host_address = host,
    };
    file = (RvgMemoryRangeFile) {
        .fd = section ? (int32_t)(intptr_t)section : -1,
        .flags = 0,
        .offset = offset,
    };
    g_array_append_val(b->ranges, range);
    g_array_append_val(b->files, file);
    info_report("vresearch-pvg-reims: GPU memory map: '%s' guest 0x%" PRIx64
                " + 0x%" PRIx64 " at host %p, section %p offset 0x%" PRIx64,
                memory_region_name(mr), gpa, size, host, (void *)section, offset);
    return false;
}

static void build_memory_map(MapBuilder *b)
{
    b->ranges = g_array_new(false, false, sizeof(RvgMemoryRange));
    b->files = g_array_new(false, false, sizeof(RvgMemoryRangeFile));
    RCU_READ_LOCK_GUARD();
    flatview_for_each_range(address_space_to_flatview(&address_space_memory),
                            map_add_range, b);
}

/* --- the library objects: machine-init-done, after all RAM exists --- */

static void pvg_reims_machine_done(Notifier *notifier, void *data)
{
    VResearchPVGReimsState *s = container_of(notifier, VResearchPVGReimsState,
                                             init_done);
    MapBuilder b = { 0 };
    RvgMemoryMap map;
    RvgHostCallbacks cb = {
        .struct_size = sizeof(cb),
        .ctx = s,
        .raise_interrupt = gfx_raise_interrupt,
    };
    RvgDeviceConfig cfg = {
        .struct_size = sizeof(cfg),
        .display_port_count = 1,
        .metal_registry_id = 0,
        .feature_level = 0,
        .display_port_connection_types = 0,
        .use_iosurface_mapper = true,
        .process_isolation = true,
        .disable_argument_buffers = false,
        .process_isolation_follows_guest = false,
    };
    RvgIOSurfaceCallbacks icb = {
        .struct_size = sizeof(icb),
        .ctx = s,
        .raise_interrupt = iosfc_raise_interrupt,
    };
    RvgIOSurfaceDeviceConfig icfg = {
        .struct_size = sizeof(icfg),
        .callbacks = &icb,
    };
    RvgDisplayCallbacks dcb = {
        .struct_size = sizeof(dcb),
        .ctx = s,
        .mode_change = display_mode_change,
        .new_frame = display_new_frame,
    };
    RvgDisplayConfig dcfg = {
        .struct_size = sizeof(dcfg),
        .serial_number = 0x1000,
        .port = 0,
        .name = NULL,           /* "Apple Virtual" */
        .width_millimeters = 71.5,
        .height_millimeters = 154.9,
        .callbacks = &dcb,
        .window = NULL,
    };
    RvgDisplayMode mode = { s->width, s->height, 0, 60.0 };
    /* BGRA8, unconverted: the bytes of the console's x8r8g8b8 surface */
    RvgFrameTargetConfig tcfg = {
        .struct_size = sizeof(tcfg),
        .width = s->width,
        .height = s->height,
        .format = RVG_PIXEL_FORMAT_BGRA8_UNORM,
    };
    RvgStatus st;

    build_memory_map(&b);
    if (!b.ranges->len) {
        error_report("vresearch-pvg-reims: no RAM for the GPU's memory map");
        exit(1);
    }
    map.ranges = (const RvgMemoryRange *)b.ranges->data;
    map.range_count = b.ranges->len;
    cfg.memory_map = &map;
    cfg.callbacks = &cb;
    cfg.memory_files = (const RvgMemoryRangeFile *)b.files->data;
    icfg.memory_map = &map;

    /* the GPU device first: the IOSurface device maps through its map */
    st = rvg_device_create(&cfg, &s->dev);
    if (st != RVG_OK || !s->dev) {
        error_report("vresearch-pvg-reims: rvg_device_create failed: %s (%u)",
                     rvg_status_message(st), st);
        exit(1);
    }
    st = rvg_iosurface_device_create(&icfg, &s->iosfc);
    if (st != RVG_OK || !s->iosfc) {
        error_report("vresearch-pvg-reims: rvg_iosurface_device_create failed: %s (%u)",
                     rvg_status_message(st), st);
        exit(1);
    }
    st = rvg_display_create(s->dev, &dcfg, &s->display);
    if (st != RVG_OK || !s->display) {
        error_report("vresearch-pvg-reims: rvg_display_create failed: %s (%u)",
                     rvg_status_message(st), st);
        exit(1);
    }
    rvg_display_set_modes(s->display, &mode, 1);

    /* the console's frames: a target of the console's size and layout */
    st = rvg_frame_target_create(s->dev, &tcfg, &s->target);
    if (st != RVG_OK || !s->target) {
        warn_report("vresearch-pvg-reims: rvg_frame_target_create failed: %s (%u); "
                    "the guest's frames are not shown", rvg_status_message(st), st);
        s->target = NULL;
    } else {
        qemu_thread_create(&s->frame_thread, "pvg-reims-frames", frame_thread_fn,
                           s, QEMU_THREAD_DETACHED);
    }

    info_report("vresearch-pvg-reims: GPU device, IOSurface device (features 0x%"
                PRIx64 ") and display %ux%u created over %u RAM range(s)%s",
                rvg_iosurface_device_mmio_read(s->iosfc, 0x1028), s->width, s->height,
                b.ranges->len, b.without_file ? " (some without a section!)" : "");

    /* the library copied what it keeps */
    g_array_free(b.ranges, true);
    g_array_free(b.files, true);
}

/* --- device --- */

static void pvg_reims_reset_hold(Object *obj, ResetType type)
{
    VResearchPVGReimsState *s = VRESEARCH_PVG_REIMS(obj);
    /*
     * The library has no reset of the IOSurface device: write its registers
     * as a guest would, ring base (unmaps the ring), descriptor enable and
     * descriptor table base (tear the surfaces down), ring size, read and
     * write counters, in that order.
     */
    static const uint64_t iosfc_regs[] = {
        0x1000, 0x1030, 0x1010, 0x1008, 0x1020, 0x1018,
    };
    size_t i;

    if (s->dev) {
        rvg_device_reset(s->dev);
    }
    if (s->iosfc) {
        for (i = 0; i < ARRAY_SIZE(iosfc_regs); i++) {
            rvg_iosurface_device_mmio_write(s->iosfc, iosfc_regs[i], 0);
        }
    }
    qemu_set_irq(s->irq_gfx, 0);
    s->irq_reads = 0;
    s->log_budget = 64;
}

static void pvg_reims_realize(DeviceState *dev, Error **errp)
{
    VResearchPVGReimsState *s = VRESEARCH_PVG_REIMS(dev);
    DisplaySurface *surface;

    if (!s->width || !s->height || s->width > 8192 || s->height > 8192) {
        error_setg(errp, "vresearch-pvg-reims: bad display size %ux%u",
                   s->width, s->height);
        return;
    }
    error_setg(&s->mig_blocker, "vresearch-pvg-reims: the reims-vgpu GPU's "
               "state is not migratable");
    if (migrate_add_blocker(&s->mig_blocker, errp) < 0) {
        return;
    }

    /* Before the first library call; REIMS_VGPU_LOG's filter overrides the level. */
    rvg_log_set_handler(reims_log, s, RVG_LOG_INFO);

    s->frame = g_new0(uint32_t, (size_t)s->width * s->height);
    /* before the library exists: its display thread may call new_frame early */
    qemu_event_init(&s->frame_event, false);
    s->con = qemu_graphic_console_create(dev, 0, &pvg_reims_gfx_ops, s);
    surface = qemu_create_displaysurface_from(s->width, s->height, PIXMAN_x8r8g8b8,
                                              s->width * 4, (uint8_t *)s->frame);
    qemu_console_set_surface(s->con, surface);

    /* the guest's RAM is complete only once the machine is */
    s->init_done.notify = pvg_reims_machine_done;
    qemu_add_machine_init_done_notifier(&s->init_done);
}

static void pvg_reims_init(Object *obj)
{
    VResearchPVGReimsState *s = VRESEARCH_PVG_REIMS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem_gfx, obj, &gfx_ops, s, "pvg-gfx", 0x10000);
    memory_region_init_io(&s->iomem_iosfc, obj, &iosfc_ops, s, "pvg-iosfc", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem_gfx);
    sysbus_init_mmio(sbd, &s->iomem_iosfc);
    sysbus_init_irq(sbd, &s->irq_gfx);
    sysbus_init_irq(sbd, &s->irq_iosfc);
}

static const Property pvg_reims_properties[] = {
    DEFINE_PROP_UINT32("width", VResearchPVGReimsState, width, 1179),
    DEFINE_PROP_UINT32("height", VResearchPVGReimsState, height, 2556),
};

static void pvg_reims_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pvg_reims_realize;
    rc->phases.hold = pvg_reims_reset_hold;
    device_class_set_props(dc, pvg_reims_properties);
    dc->user_creatable = false;
}

static const TypeInfo pvg_reims_info = {
    .name = TYPE_VRESEARCH_PVG_REIMS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VResearchPVGReimsState),
    .instance_init = pvg_reims_init,
    .class_init = pvg_reims_class_init,
};

static void pvg_reims_register_types(void)
{
    type_register_static(&pvg_reims_info);
}

type_init(pvg_reims_register_types)
