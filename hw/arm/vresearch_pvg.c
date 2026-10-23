/*
 * vresearch PVG: a display-only model of Apple's paravirtualised GPU, the
 * device behind /arm-io/gfx (AppleParavirtGPU, AppleParavirtDisplay) and
 * /arm-io/iosurface-mapper (IOSurfaceRootParavirtMapperInterface).
 *
 * On a Mac, QEMU's apple-gfx hands this device to ParavirtualizedGraphics
 * .framework, which runs the guest's command FIFOs on Metal. Here nothing
 * draws: the FIFOs are parsed, every command completes at once (GPU work is
 * dropped), and the display handshake (shared state page, mode list, ack)
 * runs as the framework's does. Presents are acknowledged and logged with
 * their IOSurface descriptor, and display 0's last presented mapper surface
 * (opcode 8) is copied into the QEMU console ("pvg") when it refreshes.
 * Backings of task 0 (opcode 7) are not shown yet. That is enough to see how
 * far a guest gets without the GPU.
 *
 * The guest ABI follows reims-vgpu's description of it (docs/testing.md,
 * section 6, "Guest ABI reference"): pages are 16 KiB, "page" fields are
 * guest-physical page numbers, every command carries a 12-byte header
 * {u16 opcode, u16 barrierCount, u32 length, u32 stamp}.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "hw/arm/vresearch_pvg.h"

OBJECT_DECLARE_SIMPLE_TYPE(VResearchPVGState, VRESEARCH_PVG)

#define PVG_PAGE            0x4000ULL
#define PVG_VERSION_MAX     0xf5
#define PVG_FIFOS           64
#define PVG_TASKS           256
#define PVG_PORTS           8
#define PVG_CMD_HDR         12

/* root node page */
#define ROOT_TASK(t)        (4 * (t))
#define ROOT_FIFO_NODE(id)  (0x400 + 0x14 * ((id) - 1))
#define ROOT_DISPLAY(p)     (0x900 + 4 * (p))

/* child FIFO node */
#define NODE_WRITTEN        0x0
#define NODE_READ           0x4
#define NODE_FAULT          0x8
#define NODE_STAMP_INDEX    0xc
#define NODE_RUNNING        0xe
#define NODE_PAGE_LIST      0x10

/* shared display state page */
#define DSP_SERIAL          0x000
#define DSP_NAME            0x004
#define DSP_PORT            0x012
#define DSP_MM_W            0x014
#define DSP_MM_H            0x016
#define DSP_GLYPH_MAX_W     0x018
#define DSP_GLYPH_MAX_H     0x01a
#define DSP_1024            0x01c
#define DSP_CURSOR_CBS      0x020
#define DSP_MM_WF           0x024
#define DSP_MM_HF           0x028
#define DSP_COLORSPACE      0x02c
#define DSP_NITS            0x070
#define DSP_RAISED          0x100
#define DSP_ENABLED         0x104
#define DSP_EXPECTED_ACK    0x200
#define DSP_MODE_COUNT      0x208
#define DSP_MODES           0x210
#define DSP_MATRICES        0xa10
#define DSP_UUID            0xa74
#define DSP_PRESENT_COUNT   0xf98
#define DSP_PRESENTED_REF   0xfa0
#define DSP_ACK_PENDING     0xfa9
#define DSP_PRESENTED_MAP   0xfb0
#define DSP_ASLEEP          0xfb8
#define DSP_LAST_ACK        0xfbc
#define DSP_STATE           0xfc0

#define DSP_IRQ_PRESENTED   2
#define DSP_IRQ_CONNECT     4
#define DSP_IRQ_UPDATE      12

/* IOSurface mapper */
#define IOSFC_DESC_V2       0x400
#define IOSFC_IDS           8192
#define IOSFC_SUCCESS       0
#define IOSFC_BAD_ARGUMENT  0xe00002c2
#define IOSFC_UNSUPPORTED   0xe00002c7

/* v2 descriptor base (reims-vgpu iosurface/descriptor.rs) */
#define IOSFC_FORMAT        0x04
#define IOSFC_OFFSET        0x08
#define IOSFC_ALLOC         0x18
#define IOSFC_WIDTH         0x20
#define IOSFC_HEIGHT        0x28
#define IOSFC_BPR           0x30
#define IOSFC_BPE           0x38
#define IOSFC_FLAGS         0x41    /* readOnly:1, hostOnly:1 */
#define IOSFC_HOST_ONLY     0x02
#define IOSFC_BGRA          0x42475241
#define IOSFC_RGBA          0x52474241

enum { CMD_OK, CMD_FAULT, CMD_NO_STAMP };

typedef struct PVGFifo {
    bool defined;
    bool faulted;
    uint32_t id;
    uint32_t stamp_index;
    uint32_t held_stamp;        /* the faulted command's stamp */
    uint32_t read;              /* bytes consumed (child: local count) */
    uint32_t size;              /* ring bytes */
    uint32_t npages;            /* child: ring pages */
    uint64_t *pages;
    uint64_t ring;              /* root: ring start (contiguous) */
} PVGFifo;

typedef struct PVGTask {
    bool defined;
    uint32_t root_page;
    uint32_t pt_root;
    uint32_t depth;
    uint64_t length;
} PVGTask;

struct VResearchPVGState {
    SysBusDevice parent_obj;
    MemoryRegion iomem_gfx;
    MemoryRegion iomem_iosfc;
    qemu_irq irq_gfx;
    qemu_irq irq_iosfc;

    uint32_t width, height;     /* the display's one mode */

    /* gfx registers */
    uint32_t version;
    uint32_t root_page;
    uint32_t fifo_enabled, fifo_length, fifo_start, fifo_base;
    uint32_t fifo_written;
    uint32_t display_irq, stamp_irq;
    uint64_t fault_mask;
    uint32_t root_fault_offset;
    bool running;

    PVGFifo root;
    PVGFifo child[PVG_FIFOS];
    PVGTask task[PVG_TASKS];
    uint64_t display_page[PVG_PORTS];

    /* IOSurface mapper */
    uint64_t ring_base, ring_size, desc_base, desc_enable;
    uint64_t ring_write, ring_read;
    uint64_t mapped[IOSFC_IDS / 64];

    /* the presented frame */
    QemuConsole *con;
    uint32_t *frame;
    uint64_t presents;
    uint64_t shown_id;          /* port 0's mapping ID, 0 for none */
    uint64_t scanned;           /* presents at the last scanout */
    unsigned log_budget;
    unsigned irq_reads;
};

/* --- guest memory --- */

static uint32_t ld32(uint64_t gpa)
{
    return address_space_ldl_le(&address_space_memory, gpa,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint64_t ld64(uint64_t gpa)
{
    return address_space_ldq_le(&address_space_memory, gpa,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void st8(uint64_t gpa, uint8_t v)
{
    address_space_stb(&address_space_memory, gpa, v, MEMTXATTRS_UNSPECIFIED, NULL);
}

static void st16(uint64_t gpa, uint16_t v)
{
    address_space_stw_le(&address_space_memory, gpa, v, MEMTXATTRS_UNSPECIFIED, NULL);
}

static void st32(uint64_t gpa, uint32_t v)
{
    address_space_stl_le(&address_space_memory, gpa, v, MEMTXATTRS_UNSPECIFIED, NULL);
}

static void st64(uint64_t gpa, uint64_t v)
{
    address_space_stq_le(&address_space_memory, gpa, v, MEMTXATTRS_UNSPECIFIED, NULL);
}

static void stf(uint64_t gpa, float f)
{
    uint32_t v;

    memcpy(&v, &f, 4);
    st32(gpa, v);
}

static void rd(uint64_t gpa, void *buf, size_t len)
{
    address_space_read(&address_space_memory, gpa, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static uint32_t p32(const uint8_t *p, size_t off)
{
    return ldl_le_p(p + off);
}

static uint64_t p64(const uint8_t *p, size_t off)
{
    return ldq_le_p(p + off);
}

static void hexdump(const char *what, uint64_t gpa, size_t len)
{
    uint8_t buf[0x400];
    size_t i;

    len = MIN(len, sizeof(buf));
    rd(gpa, buf, len);
    qemu_log_mask(LOG_UNIMP, "pvg: %s @0x%" PRIx64 ":", what, gpa);
    for (i = 0; i < len; i += 4) {
        qemu_log_mask(LOG_UNIMP, "%s%08x", i % 32 ? " " : "\n  ", ldl_le_p(buf + i));
    }
    qemu_log_mask(LOG_UNIMP, "\n");
}

/* --- interrupts and stamps --- */

/*
 * QEMU's apple-gfx pulses the line for each event; here it stays up while a
 * display or stamp bit waits to be read (0x1014, 0x1018 clear them), which
 * reaches the guest whether it configured the SPI as edge or level.
 */
static void update_gfx_irq(VResearchPVGState *s)
{
    qemu_set_irq(s->irq_gfx, (s->display_irq | s->stamp_irq) != 0);
}

static void raise_gfx(VResearchPVGState *s)
{
    if (!(s->display_irq | s->stamp_irq)) {
        qemu_irq_pulse(s->irq_gfx);     /* a fault: nothing to read-to-clear */
    }
    update_gfx_irq(s);
}

static uint64_t stamp_slot(VResearchPVGState *s, uint32_t index)
{
    return (uint64_t)s->fifo_base * PVG_PAGE + 4 * index;
}

static void publish(VResearchPVGState *s, PVGFifo *f, uint32_t stamp)
{
    st32(stamp_slot(s, f->stamp_index), stamp);
    s->stamp_irq |= 1u << (f->stamp_index & 31);
    raise_gfx(s);
}

static void display_raise(VResearchPVGState *s, uint32_t port, uint32_t bits)
{
    uint64_t page = s->display_page[port];

    /*
     * The framework pokes the port only for bits the guest enabled (+0x104),
     * but AppleParavirtDisplay connects with nothing enabled yet and then
     * waits for the connect: poke it for every raise.
     */
    st32(page + DSP_RAISED, ld32(page + DSP_RAISED) | bits);
    s->display_irq |= 1u << port;
    raise_gfx(s);
}

/* --- displays --- */

static void display_connect(VResearchPVGState *s, uint32_t port, uint64_t page)
{
    static const float srgb[8] = { 0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290 };
    char uuid[40];
    int i, m, r;

    s->display_page[port] = page;
    if (s->root_page) {
        st32((uint64_t)s->root_page * PVG_PAGE + ROOT_DISPLAY(port), page / PVG_PAGE);
    }
    if (!page) {
        return;
    }
    st32(page + DSP_SERIAL, 0x1000 + port);
    for (i = 0; i < 14; i++) {
        st8(page + DSP_NAME + i, "Apple Virtual"[i]);
    }
    st16(page + DSP_PORT, port);
    st16(page + DSP_MM_W, 71);
    st16(page + DSP_MM_H, 154);
    st16(page + DSP_GLYPH_MAX_W, 0);
    st16(page + DSP_GLYPH_MAX_H, 0);
    st32(page + DSP_1024, 1024);
    st32(page + DSP_CURSOR_CBS, 0);
    stf(page + DSP_MM_WF, 71.5);
    stf(page + DSP_MM_HF, 154.9);
    for (i = 0; i < 8; i++) {
        stf(page + DSP_COLORSPACE + 4 * i, srgb[i]);
    }
    stf(page + DSP_NITS, 0);
    stf(page + DSP_NITS + 4, 500);
    stf(page + DSP_NITS + 8, 500);
    for (m = 0; m < 2; m++) {
        for (r = 0; r < 3; r++) {
            for (i = 0; i < 4; i++) {
                stf(page + DSP_MATRICES + 48 * m + 16 * r + 4 * i, r == i);
            }
        }
    }
    snprintf(uuid, sizeof(uuid), "A5F70000-0000-4000-8000-%012x", port);
    for (i = 0; i < 37; i++) {
        st8(page + DSP_UUID + i, uuid[i]);
    }
    /* one mode, 60 Hz in 16.16 */
    st16(page + DSP_MODE_COUNT, 1);
    st16(page + DSP_MODES, s->width);
    st16(page + DSP_MODES + 2, s->height);
    st32(page + DSP_MODES + 4, 60 << 16);
    st32(page + DSP_MODES + 8, 0);
    /* state 0 -> 1: offer the mode list, wait for the ack */
    st32(page + DSP_EXPECTED_ACK, ld32(page + DSP_EXPECTED_ACK) + 1);
    st8(page + DSP_ACK_PENDING, 1);
    st32(page + DSP_STATE, 1);
    qemu_log_mask(LOG_UNIMP, "pvg: display %u connected, page 0x%" PRIx64
                  ", %ux%u, enabled irqs 0x%x\n", port, page, s->width, s->height,
                  ld32(page + DSP_ENABLED));
    display_raise(s, port, DSP_IRQ_CONNECT);
}

static void display_ack(VResearchPVGState *s, uint32_t port, uint32_t ack)
{
    uint64_t page = s->display_page[port];

    if (ack != ld32(page + DSP_EXPECTED_ACK)) {
        qemu_log_mask(LOG_GUEST_ERROR, "pvg: display %u: ack %u, expected %u\n",
                      port, ack, ld32(page + DSP_EXPECTED_ACK));
    }
    st8(page + DSP_ACK_PENDING, 0);
    st32(page + DSP_LAST_ACK, ack);
    st32(page + DSP_STATE, 2);
}

/*
 * The descriptor of IOSurface mapping `id`: the descriptor table (0x1010)
 * is a page of u32 {valid:1, isTable:1, page:30}, one per page of 16 v2
 * descriptors.
 */
static uint64_t iosfc_desc(VResearchPVGState *s, uint32_t id)
{
    uint32_t per_page = PVG_PAGE / IOSFC_DESC_V2;
    uint32_t e;

    if (!s->desc_base || !id || id >= IOSFC_IDS) {
        return 0;
    }
    e = ld32(s->desc_base + 4 * (id / per_page));
    if (!(e & 1)) {
        return 0;
    }
    return (uint64_t)(e >> 2) * PVG_PAGE + (id % per_page) * IOSFC_DESC_V2;
}

static void present_mapping(VResearchPVGState *s, uint32_t port, uint64_t id)
{
    uint64_t desc = iosfc_desc(s, id);

    if (s->log_budget) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP, "pvg: present port %u mapping %" PRIu64 "\n", port, id);
        if (desc) {
            hexdump("surface descriptor", desc, 0x200);
        }
    }
}

static void present_done(VResearchPVGState *s, uint32_t port)
{
    uint64_t page = s->display_page[port];

    if (!page) {
        return;
    }
    s->presents++;
    st64(page + DSP_PRESENT_COUNT, ld64(page + DSP_PRESENT_COUNT) + 1);
    display_raise(s, port, DSP_IRQ_PRESENTED);
}

/*
 * The guest pages of a guest-backed v2 surface: word0 of its descriptor is
 * the page-table root {valid:1, isTable:1, page:30}. A surface of at most one
 * page of entries uses the root as its only level, a larger one the root's
 * entries as second-level tables (reims-vgpu iosurface/walk.rs).
 */
static uint64_t *surface_pages(uint32_t root, uint64_t npages)
{
    uint64_t per = PVG_PAGE / 4, base = (uint64_t)(root >> 2) * PVG_PAGE;
    uint64_t *pages, i, n = 0;

    if (!(root & 1) || !npages || npages > per * per) {
        return NULL;
    }
    pages = g_new(uint64_t, npages);
    if (npages <= per) {
        for (i = 0; i < npages; i++) {
            uint32_t e = ld32(base + 4 * i);

            if ((e & 3) != 1) {
                goto bad;
            }
            pages[i] = (uint64_t)(e >> 2) * PVG_PAGE;
        }
        return pages;
    }
    for (i = 0; n < npages; i++) {
        uint32_t t = ld32(base + 4 * i);
        uint64_t j, table = (uint64_t)(t >> 2) * PVG_PAGE;

        if ((t & 3) != 3) {
            goto bad;
        }
        for (j = 0; j < per && n < npages; j++, n++) {
            uint32_t e = ld32(table + 4 * j);

            if ((e & 3) != 1) {
                goto bad;
            }
            pages[n] = (uint64_t)(e >> 2) * PVG_PAGE;
        }
    }
    return pages;
bad:
    g_free(pages);
    return NULL;
}

/*
 * Copy the surface port 0 last presented (opcode 8) into the console's
 * frame, clipped to the display mode. 'BGRA' is the console's x8r8g8b8 as it
 * is; 'RGBA' swaps red and blue. Other formats ('RGhA', half floats) and
 * host-only surfaces are logged and left out.
 */
static void scanout(VResearchPVGState *s)
{
    uint64_t desc = iosfc_desc(s, s->shown_id);
    uint64_t off, alloc, w, h, bpr, npages, y, *pages;
    uint32_t fmt;
    uint8_t d[0x80];

    if (!desc) {
        return;
    }
    rd(desc, d, sizeof(d));
    fmt = p32(d, IOSFC_FORMAT);
    off = p64(d, IOSFC_OFFSET);
    alloc = p64(d, IOSFC_ALLOC);
    w = MIN(p64(d, IOSFC_WIDTH), s->width);
    h = MIN(p64(d, IOSFC_HEIGHT), s->height);
    bpr = p64(d, IOSFC_BPR);
    if ((fmt != IOSFC_BGRA && fmt != IOSFC_RGBA) || lduw_le_p(d + IOSFC_BPE) != 4 ||
        (d[IOSFC_FLAGS] & IOSFC_HOST_ONLY) || bpr < 4 * w) {
        if (s->log_budget) {
            s->log_budget--;
            qemu_log_mask(LOG_UNIMP, "pvg: surface %" PRIu64 " not shown: format 0x%08x, "
                          "%u bytes a pixel, flags 0x%x, row %" PRIu64 "\n", s->shown_id, fmt,
                          lduw_le_p(d + IOSFC_BPE), d[IOSFC_FLAGS], bpr);
        }
        return;
    }
    npages = DIV_ROUND_UP(alloc, PVG_PAGE);
    pages = surface_pages(p32(d, 0), npages);
    if (!pages) {
        if (s->log_budget) {
            s->log_budget--;
            qemu_log_mask(LOG_UNIMP, "pvg: surface %" PRIu64 ": bad page table\n",
                          s->shown_id);
        }
        return;
    }
    for (y = 0; y < h && off + y * bpr + 4 * w <= npages * PVG_PAGE; y++) {
        uint8_t *dst = (uint8_t *)(s->frame + y * s->width);
        uint64_t src = off + y * bpr, left = 4 * w;

        while (left) {
            uint64_t in = src % PVG_PAGE, n = MIN(left, PVG_PAGE - in);

            rd(pages[src / PVG_PAGE] + in, dst, n);
            dst += n;
            src += n;
            left -= n;
        }
        if (fmt == IOSFC_RGBA) {
            uint32_t *px = s->frame + y * s->width;
            uint64_t x;

            for (x = 0; x < w; x++) {
                px[x] = (px[x] & 0xff00ff00) | ((px[x] & 0xff) << 16) | ((px[x] >> 16) & 0xff);
            }
        }
    }
    g_free(pages);
}

static bool fb_update(void *opaque)
{
    VResearchPVGState *s = opaque;

    if (s->shown_id && s->scanned != s->presents) {
        s->scanned = s->presents;
        scanout(s);
    }
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps pvg_gfx_ops = {
    .gfx_update = fb_update,
};

/* --- commands --- */

static const uint32_t device_info[][2] = {
    {1, 4}, {2, 0}, {3, 1024}, {4, 1024}, {5, 1024}, {6, 32768}, {7, 1}, {8, 1},
    {9, 1}, {10, 8}, {11, 511}, {12, 1}, {13, 16}, {14, 1}, {15, 256}, {16, 1},
    {17, 1}, {18, 0x20007}, {19, 1}, {20, 2}, {21, 1}, {22, 2}, {23, 1}, {24, 1},
    {25, 1}, {26, 0}, {27, 1}, {28, 7}, {33, 0xbdf}, {36, 0xffffffff}, {29, 1024},
    {30, 31}, {31, 65536}, {32, 16}, {34, 8}, {41, 7}, {35, 2048}, {37, 1009},
    {42, 0}, {44, 63}, {40, 16},
};

static void get_device_info(uint32_t max_key, uint32_t max_count, uint64_t dst)
{
    uint32_t n = 0;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(device_info) && n < max_count; i++) {
        if (device_info[i][0] < max_key) {
            st32(dst + 8 * n, device_info[i][0]);
            st32(dst + 8 * n + 4, device_info[i][1]);
            n++;
        }
    }
    if (n < max_count) {
        st32(dst + 8 * n, 0);
        st32(dst + 8 * n + 4, 0);
    }
}

static void child_define(VResearchPVGState *s, uint32_t id)
{
    uint64_t node = (uint64_t)s->root_page * PVG_PAGE + ROOT_FIFO_NODE(id);
    PVGFifo *f = &s->child[id];
    uint64_t list = (uint64_t)ld32(node + NODE_PAGE_LIST) * PVG_PAGE;
    uint32_t n;

    g_free(f->pages);
    memset(f, 0, sizeof(*f));
    f->id = id;
    f->stamp_index = ld32(node + NODE_STAMP_INDEX) & 0xffff;
    st32(node + NODE_READ, 0);
    st32(node + NODE_FAULT, 0xffffffff);
    f->pages = g_new0(uint64_t, 4096);
    for (n = 0; list && n < 4096; n++) {
        uint32_t pg = ld32(list + 4 * n);

        if (!pg) {
            break;
        }
        f->pages[n] = (uint64_t)pg * PVG_PAGE;
    }
    f->npages = n;
    f->size = n * PVG_PAGE;
    f->defined = n > 0;
    st16(node + NODE_RUNNING, 1);
    qemu_log_mask(LOG_UNIMP, "pvg: child FIFO %u: %u pages, stamp index %u\n",
                  id, n, f->stamp_index);
}

static void child_delete(VResearchPVGState *s, uint32_t id)
{
    uint64_t node = (uint64_t)s->root_page * PVG_PAGE + ROOT_FIFO_NODE(id);
    PVGFifo *f = &s->child[id];

    g_free(f->pages);
    memset(f, 0, sizeof(*f));
    st16(node + NODE_RUNNING, 0);
    /* what the framework does too: the next node's page list goes */
    if (id + 1 < PVG_FIFOS) {
        st32(node + 0x14 + NODE_PAGE_LIST, 0);
    }
}

static int run_command(VResearchPVGState *s, PVGFifo *f, uint16_t op,
                       const uint8_t *p, uint32_t len)
{
    uint32_t t;

#define NEED(n) do { if (len < (n)) { goto short_payload; } } while (0)
    switch (op) {
    case 0:     /* Debug */
        if (len) {
            qemu_log_mask(LOG_UNIMP, "pvg: guest: %.*s\n", (int)len, p);
        }
        return CMD_OK;
    case 1:     /* DisplaySetSharedStatePage */
        NEED(8);
        if (p32(p, 0) >= PVG_PORTS) {
            return CMD_FAULT;
        }
        display_connect(s, p32(p, 0), (uint64_t)p32(p, 4) * PVG_PAGE);
        return CMD_OK;
    case 2:     /* DisplayAck */
        NEED(8);
        if (p32(p, 0) >= PVG_PORTS || !s->display_page[p32(p, 0)]) {
            return CMD_FAULT;
        }
        display_ack(s, p32(p, 0), p32(p, 4));
        return CMD_OK;
    case 7:     /* DisplayTransaction3: a backing of task 0 */
        NEED(36);
        if (p32(p, 0) < PVG_PORTS) {
            if (s->log_budget) {
                s->log_budget--;
                qemu_log_mask(LOG_UNIMP, "pvg: present port %u backing ref %u\n",
                              p32(p, 0), p32(p, 8));
            }
            present_done(s, p32(p, 0));
        }
        return CMD_OK;
    case 8:     /* DisplaySwapMapping: an IOSurface mapper surface */
        NEED(40);
        if (p32(p, 0) < PVG_PORTS) {
            present_mapping(s, p32(p, 0), p64(p, 8));
            st64(s->display_page[p32(p, 0)] + DSP_PRESENTED_MAP, p64(p, 8));
            if (p32(p, 0) == 0) {
                s->shown_id = p64(p, 8);
            }
            present_done(s, p32(p, 0));
        }
        return CMD_OK;
    case 9:     /* DisplaySleepState */
        NEED(8);
        if (p32(p, 0) < PVG_PORTS && s->display_page[p32(p, 0)]) {
            st8(s->display_page[p32(p, 0)] + DSP_ASLEEP, p[4]);
        }
        return CMD_OK;
    case 4: case 5: case 10: case 11: case 12:
        return CMD_OK;
    case 30:    /* NOP */
        return CMD_OK;
    case 32:    /* DeleteTask */
        NEED(4);
        t = p32(p, 0);
        if (t >= PVG_TASKS) {
            return CMD_FAULT;
        }
        s->task[t].defined = false;
        st32((uint64_t)s->root_page * PVG_PAGE + ROOT_TASK(t), 0);
        return CMD_OK;
    case 48:    /* DefineChildFIFO */
    case 49:    /* DeleteChildFIFO */
        NEED(4);
        t = p32(p, 0);
        if (f != &s->root || !t || t >= PVG_FIFOS) {
            return CMD_FAULT;
        }
        if (op == 48) {
            child_define(s, t);
        } else {
            child_delete(s, t);
        }
        return CMD_OK;
    case 56: {  /* DefineTask2 */
        uint64_t root;

        NEED(16);
        t = p32(p, 0) >> 1;
        if (t >= PVG_TASKS || !p32(p, 12)) {
            return CMD_FAULT;
        }
        root = (uint64_t)p32(p, 12) * PVG_PAGE;
        s->task[t] = (PVGTask) {
            .defined = true, .root_page = p32(p, 12), .pt_root = ld32(root),
            .depth = ld32(root + 4), .length = p64(p, 4),
        };
        st32((uint64_t)s->root_page * PVG_PAGE + ROOT_TASK(t), p32(p, 12));
        st64(root + 0x14, p64(p, 4));
        qemu_log_mask(LOG_UNIMP, "pvg: task %u: length 0x%" PRIx64 ", depth %u\n",
                      t, p64(p, 4), s->task[t].depth);
        return CMD_OK;
    }
    case 58:    /* GetDeviceInfo */
        NEED(12);
        get_device_info(p32(p, 0), p32(p, 4), (uint64_t)p32(p, 8) * PVG_PAGE);
        return CMD_OK;
    case 61:    /* Delay */
        return CMD_OK;
    case 34: case 37: case 40: case 51: case 52: case 53: case 54: case 57:
    case 59: case 60: case 62: case 63: case 64: case 65: case 66: case 68:
        return CMD_OK;
    case 55: case 67: case 69:  /* GPU work: dropped */
        if (s->log_budget) {
            s->log_budget--;
            qemu_log_mask(LOG_UNIMP, "pvg: exec %u dropped (task %u)\n", op,
                          len >= 4 ? p32(p, 0) : 0);
        }
        return CMD_OK;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "pvg: FIFO %u: unknown opcode %u\n", f->id, op);
        return CMD_FAULT;
    }
short_payload:
    qemu_log_mask(LOG_GUEST_ERROR, "pvg: FIFO %u: opcode %u: payload %u too short\n",
                  f->id, op, len);
    return CMD_FAULT;
#undef NEED
}

/* --- FIFOs --- */

static void ring_read(PVGFifo *f, uint32_t count, void *buf, uint32_t len)
{
    uint8_t *b = buf;

    while (len) {
        uint32_t off = count % f->size;
        uint32_t n;
        uint64_t gpa;

        if (f->pages) {
            n = MIN(len, PVG_PAGE - off % PVG_PAGE);
            gpa = f->pages[off / PVG_PAGE] + off % PVG_PAGE;
        } else {
            n = MIN(len, f->size - off);
            gpa = f->ring + off;
        }
        rd(gpa, b, n);
        b += n;
        count += n;
        len -= n;
    }
}

static uint32_t fifo_written(VResearchPVGState *s, PVGFifo *f)
{
    if (f == &s->root) {
        return s->fifo_written;
    }
    return ld32((uint64_t)s->root_page * PVG_PAGE + ROOT_FIFO_NODE(f->id) + NODE_WRITTEN);
}

static void fifo_advance(VResearchPVGState *s, PVGFifo *f, uint32_t len)
{
    f->read += len;
    if (f != &s->root) {
        st32((uint64_t)s->root_page * PVG_PAGE + ROOT_FIFO_NODE(f->id) + NODE_READ,
             f->read);
    }
}

static void fifo_fault(VResearchPVGState *s, PVGFifo *f, uint32_t offset, uint32_t stamp)
{
    f->faulted = true;
    f->held_stamp = stamp;
    s->fault_mask |= 1ULL << f->stamp_index;
    if (f == &s->root) {
        s->root_fault_offset = offset;
    } else {
        st32((uint64_t)s->root_page * PVG_PAGE + ROOT_FIFO_NODE(f->id) + NODE_FAULT,
             offset);
    }
    raise_gfx(s);
}

static void fifo_resume(VResearchPVGState *s, PVGFifo *f)
{
    if (f->faulted) {
        f->faulted = false;
        if (f == &s->root) {
            s->root_fault_offset = 0xffffffff;
        }
        publish(s, f, f->held_stamp);
    }
}

/* Runs a FIFO until it is empty, blocked or faulted; true if it ran something */
static bool fifo_run(VResearchPVGState *s, PVGFifo *f)
{
    bool ran = false;

    while (f->defined && !f->faulted) {
        uint8_t hdr[PVG_CMD_HDR];
        uint32_t avail = fifo_written(s, f) - f->read;
        uint32_t len, nbar, off = f->read % f->size;
        uint16_t op;
        uint8_t *cmd;
        int r;
        unsigned i;

        if (avail < PVG_CMD_HDR) {
            break;
        }
        ring_read(f, f->read, hdr, PVG_CMD_HDR);
        op = lduw_le_p(hdr);
        nbar = lduw_le_p(hdr + 2);
        len = ldl_le_p(hdr + 4);
        if (len < PVG_CMD_HDR || len > f->size || nbar > 64 ||
            len < PVG_CMD_HDR + 8 * nbar) {
            qemu_log_mask(LOG_GUEST_ERROR, "pvg: FIFO %u: bad command (op %u, "
                          "length %u, %u barriers) at 0x%x\n", f->id, op, len, nbar, off);
            fifo_advance(s, f, PVG_CMD_HDR);
            fifo_fault(s, f, off, ldl_le_p(hdr + 8));
            break;
        }
        if (avail < len) {
            break;
        }
        cmd = g_malloc(len);
        ring_read(f, f->read, cmd, len);
        for (i = 0; i < nbar; i++) {
            uint32_t idx = p32(cmd, PVG_CMD_HDR + 8 * i);
            uint32_t val = p32(cmd, PVG_CMD_HDR + 8 * i + 4);

            if (idx >= PVG_FIFOS || (int32_t)(ld32(stamp_slot(s, idx)) - val) < 0) {
                break;
            }
        }
        if (i < nbar) {
            g_free(cmd);
            break;              /* blocked on a barrier: run again after a stamp */
        }
        fifo_advance(s, f, len);
        if (f == &s->root) {
            /* 0x100c is advanced before the command runs */
        }
        r = run_command(s, f, op, cmd + PVG_CMD_HDR + 8 * nbar,
                        len - PVG_CMD_HDR - 8 * nbar);
        if (r == CMD_OK) {
            publish(s, f, ldl_le_p(hdr + 8));
        } else if (r == CMD_FAULT) {
            fifo_fault(s, f, off, ldl_le_p(hdr + 8));
        }
        g_free(cmd);
        ran = true;
    }
    return ran;
}

static void run_all(VResearchPVGState *s)
{
    bool progress;
    int i;

    if (s->running) {
        return;
    }
    s->running = true;
    do {
        progress = fifo_run(s, &s->root);
        for (i = 1; i < PVG_FIFOS; i++) {
            progress |= fifo_run(s, &s->child[i]);
        }
    } while (progress);
    s->running = false;
}

static void root_fifo_enable(VResearchPVGState *s, uint32_t val)
{
    PVGFifo *f = &s->root;

    if (!val) {
        int i;

        s->fifo_enabled = 0;
        f->defined = false;
        for (i = 1; i < PVG_FIFOS; i++) {
            g_free(s->child[i].pages);
            memset(&s->child[i], 0, sizeof(s->child[i]));
        }
        return;
    }
    if (s->fifo_enabled) {
        return;
    }
    if (!s->root_page || !s->fifo_length || s->fifo_length % PVG_PAGE ||
        s->fifo_length > 0x20000 || !s->fifo_start || s->fifo_start % 0x400 ||
        s->fifo_start >= s->fifo_length) {
        qemu_log_mask(LOG_GUEST_ERROR, "pvg: root FIFO refused (length 0x%x, "
                      "start 0x%x)\n", s->fifo_length, s->fifo_start);
        return;
    }
    memset(f, 0, sizeof(*f));
    f->defined = true;
    f->ring = (uint64_t)s->fifo_base * PVG_PAGE + s->fifo_start;
    f->size = s->fifo_length - s->fifo_start;
    s->fifo_written = 0;
    s->fifo_enabled = val;
    s->root_fault_offset = 0xffffffff;
    qemu_log_mask(LOG_UNIMP, "pvg: root FIFO at page 0x%x, ring 0x%x bytes\n",
                  s->fifo_base, f->size);
}

static uint64_t gfx_read(void *opaque, hwaddr addr, unsigned size)
{
    VResearchPVGState *s = opaque;
    uint32_t reg = 0x1000 + 4 * (uint16_t)((addr - 0x1000) / 4);
    uint32_t v = 0;

    switch (reg) {
    case 0x1000: v = s->fifo_enabled; break;
    case 0x100c: v = s->root.read; break;
    case 0x1014:
    case 0x1018:
        v = reg == 0x1014 ? s->display_irq : s->stamp_irq;
        if (s->irq_reads < 16) {
            s->irq_reads++;
            qemu_log_mask(LOG_UNIMP, "pvg: read 0x%x = 0x%x\n", reg, v);
        }
        if (reg == 0x1014) {
            s->display_irq = 0;
        } else {
            s->stamp_irq = 0;
        }
        update_gfx_irq(s);
        break;
    case 0x101c: v = s->root_page; break;
    case 0x102c: v = s->fault_mask != 0; break;
    case 0x1034: v = s->version; break;
    case 0x103c: v = s->fifo_enabled ? s->root_fault_offset : 0; break;
    case 0x122c: v = 1; break;
    default:
        break;
    }
    return v;
}

static void gfx_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VResearchPVGState *s = opaque;
    uint32_t reg = 0x1000 + 4 * (uint16_t)((addr - 0x1000) / 4);

    switch (reg) {
    case 0x1000: root_fifo_enable(s, val); break;
    case 0x1004: if (!s->fifo_enabled) { s->fifo_length = val; } break;
    case 0x1008: s->fifo_written = val; run_all(s); break;
    case 0x1010: if (!s->fifo_enabled) { s->fifo_start = val; } break;
    case 0x101c: s->root_page = val; break;
    case 0x1020: run_all(s); break;
    case 0x1024: fifo_resume(s, &s->root); run_all(s); break;
    case 0x1028:
        s->fault_mask &= ~(1ULL << (val & 63));
        if (val && val < PVG_FIFOS) {
            fifo_resume(s, &s->child[val]);
        }
        run_all(s);
        break;
    case 0x1030: if (!s->fifo_enabled) { s->fifo_base = val; } break;
    case 0x1034:
        if (!(val & 0x80000000)) {
            s->version = MIN((uint32_t)val, PVG_VERSION_MAX);
            qemu_log_mask(LOG_UNIMP, "pvg: guest asks for version %u, gets %u\n",
                          (uint32_t)val, s->version);
        }
        break;
    case 0x1038:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "pvg: write 0x%x <- 0x%" PRIx64 "\n", reg, val);
        break;
    }
}

static const MemoryRegionOps gfx_ops = {
    .read = gfx_read,
    .write = gfx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* --- IOSurface mapper --- */

static void iosfc_run(VResearchPVGState *s)
{
    while (s->ring_base && s->ring_size && s->ring_read != s->ring_write) {
        uint64_t e = s->ring_base + 16 * (s->ring_read % s->ring_size);
        uint32_t cmd = ld32(e), id = ld32(e + 4);
        uint32_t status = IOSFC_SUCCESS;
        uint64_t desc = iosfc_desc(s, id);

        if (!s->desc_base) {
            break;
        }
        switch (cmd) {
        case 1:     /* map */
        case 3:     /* test */
            if (!desc) {
                status = IOSFC_BAD_ARGUMENT;
            } else if (cmd == 1) {
                if (s->log_budget && !(s->mapped[id / 64] & (1ULL << (id % 64)))) {
                    s->log_budget--;
                    qemu_log_mask(LOG_UNIMP, "pvg: iosfc map %u\n", id);
                    hexdump("descriptor", desc, IOSFC_DESC_V2);
                }
                s->mapped[id / 64] |= 1ULL << (id % 64);
            }
            break;
        case 2:     /* unmap */
            if (!id || id >= IOSFC_IDS) {
                status = IOSFC_BAD_ARGUMENT;
            } else {
                s->mapped[id / 64] &= ~(1ULL << (id % 64));
            }
            break;
        default:
            status = IOSFC_UNSUPPORTED;
            break;
        }
        st32(e + 8, status);
        s->ring_read++;
        qemu_irq_pulse(s->irq_iosfc);
    }
}

static uint64_t iosfc_read(void *opaque, hwaddr addr, unsigned size)
{
    VResearchPVGState *s = opaque;

    switch (addr) {
    case 0x1000: return s->ring_base;
    case 0x1008: return s->ring_size;
    case 0x1010: return s->desc_base;
    case 0x1018: return s->ring_write;
    case 0x1020: return s->ring_read;
    case 0x1028: return 1;      /* v2 descriptors */
    case 0x1030: return s->desc_enable;
    default: return UINT64_MAX;
    }
}

static void iosfc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VResearchPVGState *s = opaque;

    switch (addr) {
    case 0x1000: s->ring_base = val; break;
    case 0x1008: s->ring_size = val; break;
    case 0x1010:
        s->desc_base = val;
        memset(s->mapped, 0, sizeof(s->mapped));
        break;
    case 0x1018: s->ring_write = val; iosfc_run(s); break;
    case 0x1020: s->ring_read = val; break;
    case 0x1030: s->desc_enable = val; s->desc_base = 0; break;
    default: break;
    }
}

static const MemoryRegionOps iosfc_ops = {
    .read = iosfc_read,
    .write = iosfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

/* --- device --- */

static void pvg_reset(DeviceState *dev)
{
    VResearchPVGState *s = VRESEARCH_PVG(dev);
    int i;

    for (i = 0; i < PVG_FIFOS; i++) {
        g_free(s->child[i].pages);
    }
    s->version = s->root_page = s->fifo_enabled = 0;
    s->fifo_length = s->fifo_start = s->fifo_base = s->fifo_written = 0;
    s->display_irq = s->stamp_irq = 0;
    s->fault_mask = 0;
    memset(&s->root, 0, sizeof(s->root));
    memset(s->child, 0, sizeof(s->child));
    memset(s->task, 0, sizeof(s->task));
    memset(s->display_page, 0, sizeof(s->display_page));
    s->ring_base = s->ring_size = s->desc_base = s->desc_enable = 0;
    s->ring_write = s->ring_read = 0;
    memset(s->mapped, 0, sizeof(s->mapped));
    s->shown_id = s->scanned = 0;
    s->log_budget = 64;
    s->irq_reads = 0;
}

static void pvg_realize(DeviceState *dev, Error **errp)
{
    VResearchPVGState *s = VRESEARCH_PVG(dev);
    DisplaySurface *surface;

    if (!s->width || !s->height || s->width > 8192 || s->height > 8192) {
        error_setg(errp, "vresearch-pvg: bad display size %ux%u", s->width, s->height);
        return;
    }
    s->frame = g_new0(uint32_t, (size_t)s->width * s->height);
    s->con = qemu_graphic_console_create(dev, 0, &pvg_gfx_ops, s);
    surface = qemu_create_displaysurface_from(s->width, s->height, PIXMAN_x8r8g8b8,
                                              s->width * 4, (uint8_t *)s->frame);
    qemu_console_set_surface(s->con, surface);
}

static void pvg_init(Object *obj)
{
    VResearchPVGState *s = VRESEARCH_PVG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem_gfx, obj, &gfx_ops, s, "pvg-gfx", 0x10000);
    memory_region_init_io(&s->iomem_iosfc, obj, &iosfc_ops, s, "pvg-iosfc", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem_gfx);
    sysbus_init_mmio(sbd, &s->iomem_iosfc);
    sysbus_init_irq(sbd, &s->irq_gfx);
    sysbus_init_irq(sbd, &s->irq_iosfc);
}

static const Property pvg_properties[] = {
    DEFINE_PROP_UINT32("width", VResearchPVGState, width, 1179),
    DEFINE_PROP_UINT32("height", VResearchPVGState, height, 2556),
};

static void pvg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pvg_realize;
    device_class_set_legacy_reset(dc, pvg_reset);
    device_class_set_props(dc, pvg_properties);
    dc->user_creatable = false;
}

static const TypeInfo pvg_info = {
    .name = TYPE_VRESEARCH_PVG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VResearchPVGState),
    .instance_init = pvg_init,
    .class_init = pvg_class_init,
};

static void pvg_register_types(void)
{
    type_register_static(&pvg_info);
}

type_init(pvg_register_types)
