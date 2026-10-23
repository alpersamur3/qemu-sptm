/*
 * vresearch SEP mailbox (sep=on): the device behind /arm-io/sep
 * (iop-sep,vpiop) at 0x30250000, driven by the kernel's AppleA7IOP /
 * AppleVPIOP / AppleSEPManager.
 *
 * On a real vphone600 the SEP is a second ARM core: iBoot's SEP ROM
 * (AVPSEPBooter, which OpenSEP reimplements) runs on it and the AP boots the
 * SEP firmware over this mailbox, then SEPOS runs and answers endpoint
 * traffic (keystore/AKS, credential manager, ...). Here there is no SEP core;
 * this device answers the mailbox so the AP believes the SEP booted.
 *
 * The register layer and the boot protocol are Apple's, read out of the
 * kernel (AppleVPIOP accessors, AppleSEPBooter::bootSEP / _bootAction) and
 * cross-checked against owngoal-dev/OpenSEP and OpenAVB (MIT), which are
 * built and tested against this same mailbox:
 *
 *   0x10  control      bit0 enable/activate
 *   0x14  status       bit31 active, bit4 RX event (write-1-to-clear)
 *   0x18  int enable   bit4 RX-not-empty, bit5 TX-empty
 *   0x1c  RX status    bit17 empty            (0x100 slot: SEP -> AP)
 *   0x20  TX status    bit17 empty, bit16 full(0x200 slot: AP -> SEP)
 *   0x100 RX slot, 16 bytes, read low then high (ldp); the high read pops it
 *   0x200 TX slot, 16 bytes, written low then high (stp)
 *
 * A message is the low 64-bit word of a slot (the high word is zero):
 *   bits 0-7   endpoint (0xff bootstrap, 0xfd discovery, ...)
 *   bits 8-15  tag / sequence
 *   bits 16-23 opcode (request) / opcode+0x64 (response)
 *   bits 24-31 argument
 *   bits 32-63 payload
 *
 * The discovery endpoint 0xfd (AppleSEPDiscovery) carries the SEP's endpoint
 * advertisements, which the kernel's clients (AppleSEPKeyStore 'sks ',
 * AppleCredentialManager 'scrd', xART, ...) match by name:
 *   byte2 = 0  endpoint-name advertisement: byte3 = id, bytes4-7 = FourCC name
 *   byte2 = 1  OOL-size advertisement:      byte3 = id, bytes4-7 = sizes
 *
 * SEP ROM boot (endpoint 0xff = bootstrap), the opcodes the kernel sends and
 * the acks it waits for (response = request + 0x64, same as OpenSEP):
 *   0x02 status query     -> 0x66 payload = ROM status (1, then 2 after Tz0)
 *   0x03 generate nonce   -> 0x67 payload = 160 (NONCE_BIT_LEN)
 *   0x04 read nonce word  -> 0x68 payload = nonce word (arg = index 0..4)
 *   0x05 BootTz0          -> 0x69 "SEP accepted Tz0"
 *   0x06 BootIMG4         -> 0x6a "SEP accepted IMG4"
 *   0x07 LoadSEPART       -> 0x6b "SEP accepted SEPART"
 *   0x08 resume/boot      -> 0x6c "SEP resumed from RAM"
 *   0x24 BootTmmManifest  -> 0x88 "SEP accepted TMM manifest"
 *   0x25 BootPatch        -> 0x89 "SEP accepted patches"
 * The kernel checks only the opcode and (for 0x02) the status, so a flat
 * "request + 0x64" responder with the right status sequence boots the SEP.
 *
 * Runtime endpoints (keystore/AKS and friends) are not modelled yet: traffic
 * on endpoints other than 254 is logged so the protocol can be filled in.
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
#include "hw/arm/vresearch_sep.h"

OBJECT_DECLARE_SIMPLE_TYPE(VResearchSEPState, VRESEARCH_SEP)

/* register offsets */
#define SEP_CONTROL         0x10
#define SEP_STATUS          0x14
#define SEP_INT_ENABLE      0x18
#define SEP_RX_STATUS       0x1c
#define SEP_TX_STATUS       0x20
#define SEP_RX_SLOT         0x100
#define SEP_TX_SLOT         0x200

/* status / control bits */
#define SEP_CONTROL_ENABLE  (1u << 0)
#define SEP_STATUS_ACTIVE   (1u << 31)
#define SEP_STATUS_RX_EVENT (1u << 4)
#define SEP_EMPTY           (1u << 17)
#define SEP_FULL            (1u << 16)
#define SEP_IE_RX           (1u << 4)
#define SEP_IE_TX           (1u << 5)

/* message fields */
#define MSG_ENDPOINT(m)     ((uint8_t)(m))
#define MSG_TAG(m)          ((uint8_t)((m) >> 8))
#define MSG_OPCODE(m)       ((uint8_t)((m) >> 16))
#define MSG_ARG(m)          ((uint8_t)((m) >> 24))
#define MSG_PAYLOAD(m)      ((uint32_t)((m) >> 32))

#define MSG(ep, tag, op, arg, payload) \
    ((uint64_t)(uint8_t)(ep) | (uint64_t)(uint8_t)(tag) << 8 | \
     (uint64_t)(uint8_t)(op) << 16 | (uint64_t)(uint8_t)(arg) << 24 | \
     (uint64_t)(uint32_t)(payload) << 32)

#define EP_BOOTSTRAP        0xff
#define EP_DISCOVERY        0xfd

#define SEP_RX_DEPTH        32

struct VResearchSEPState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq_a;             /* INTID 0x35 */
    qemu_irq irq_b;             /* INTID 0x34 */

    uint32_t control;
    uint32_t int_enable;

    /* SEP -> AP message ring */
    uint64_t rx[SEP_RX_DEPTH];
    unsigned rx_head, rx_count;
    bool rx_half;               /* low word of the head slot already read */

    /* AP -> SEP staging (stp writes low then high) */
    uint64_t tx_low;

    /* boot state */
    uint32_t rom_status;        /* 1, then 2 after Tz0 */
    bool booted;
    bool startup_sent;          /* the SEP's unsolicited startup event */
    bool adverts_sent;          /* the discovery endpoint advertisements */
    bool irq_level;             /* last asserted IRQ level */

    unsigned log_budget;
};

/*
 * The SEP ROM's startup event, sent unsolicited once the AP activates the
 * mailbox (OpenSEP sep_mailbox_send_startup_event): endpoint 0xff (bootstrap),
 * opcode 0xd2, payload = phase. The kernel's bootstrap handler clears its wait
 * on it and reads the phase as the ROM status.
 */
#define SEP_STARTUP_EVENT(phase)    MSG(EP_BOOTSTRAP, 0, 0xd2, 0, (phase))

/*
 * The runtime endpoints the SEP advertises on the discovery channel. The
 * names are the SEPOS apps (sks keystore, scrd credential, xART/ARTM
 * anti-replay, sars, skg); the ids are ours to assign, matched by name on the
 * kernel side. ('xART' etc. are 4-char FourCCs, little-endian in the message.)
 */
#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static const struct { uint8_t id; uint32_t name; } sep_endpoints[] = {
    { 0x08, FOURCC('s', 'k', 's', ' ') },   /* AppleSEPKeyStore */
    { 0x09, FOURCC('s', 'c', 'r', 'd') },   /* AppleCredentialManager */
    { 0x0a, FOURCC('x', 'A', 'R', 'T') },   /* anti-replay */
    { 0x0b, FOURCC('A', 'R', 'T', 'M') },
    { 0x0c, FOURCC('s', 'a', 'r', 's') },
    { 0x0d, FOURCC('s', 'k', 'g', ' ') },
    { 0x0e, FOURCC('S', 'E', 'P', 'D') },
};

static void sep_update_irq(VResearchSEPState *s)
{
    bool rx = s->rx_count != 0 && (s->int_enable & SEP_IE_RX);

    if (rx != s->irq_level && s->log_budget) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP, "vresearch-sep: IRQ %s (rx_count %u ie %#x)\n",
                      rx ? "HIGH" : "low", s->rx_count, s->int_enable);
    }
    s->irq_level = rx;
    /*
     * The mailbox has two interrupts (DT INTID 0x35 and 0x34). The kernel
     * enables only the one it waits on; raise both on a pending message so
     * whichever it uses wakes it, and the handler reads the slot either way.
     */
    qemu_set_irq(s->irq_a, rx);
    qemu_set_irq(s->irq_b, rx);
}

static void sep_push(VResearchSEPState *s, uint64_t msg)
{
    if (s->rx_count >= SEP_RX_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR, "vresearch-sep: RX ring overflow\n");
        return;
    }
    s->rx[(s->rx_head + s->rx_count) % SEP_RX_DEPTH] = msg;
    s->rx_count++;
    sep_update_irq(s);
}

/* arg selects the nonce word; a fixed deterministic value is fine here */
static uint32_t sep_nonce_word(uint8_t index)
{
    return 0x4e4f4e00u | index;         /* "NON" + index */
}

/* The SEP ROM bootstrap responder: response opcode = request opcode + 0x64. */
static void sep_bootstrap(VResearchSEPState *s, uint64_t req)
{
    uint8_t op = MSG_OPCODE(req);
    uint8_t rop = op + 0x64;
    uint8_t rarg = MSG_ARG(req);
    uint32_t payload = 0;
    uint64_t resp;

    switch (op) {
    case 0x02:                          /* status query */
        payload = s->rom_status;
        break;
    case 0x03:                          /* generate ROM nonce */
        payload = 160;                  /* NONCE_BIT_LEN */
        break;
    case 0x04:                          /* read nonce word */
        payload = sep_nonce_word(MSG_ARG(req));
        break;
    case 0x05:                          /* BootTz0 */
        s->rom_status = 2;
        break;
    case 0x06:                          /* BootIMG4 */
        s->booted = true;
        break;
    case 0x07:                          /* LoadSEPART */
    case 0x08:                          /* resume / boot without firmware */
    case 0x24:                          /* BootTmmManifest */
    case 0x25:                          /* BootPatch */
        break;
    default:
        break;
    }

    /* same endpoint (0xff) and tag echoed back, response opcode in byte2 */
    resp = MSG(EP_BOOTSTRAP, MSG_TAG(req), rop, rarg, payload);

    if (s->log_budget) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP,
                      "vresearch-sep: boot op %#x arg %#x payload %#x -> %#x\n",
                      op, MSG_ARG(req), MSG_PAYLOAD(req), rop);
    }
    sep_push(s, resp);
}

/* Advertise the SEP's runtime endpoints on the discovery channel (0xfd). */
static void sep_advertise(VResearchSEPState *s)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sep_endpoints); i++) {
        sep_push(s, MSG(EP_DISCOVERY, 0, 0 /* name advert */,
                        sep_endpoints[i].id, sep_endpoints[i].name));
    }
    if (s->log_budget) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP, "vresearch-sep: advertised %zu endpoints\n",
                      ARRAY_SIZE(sep_endpoints));
    }
}

static void sep_process_tx(VResearchSEPState *s, uint64_t msg)
{
    uint8_t ep = MSG_ENDPOINT(msg);

    if (ep == EP_BOOTSTRAP) {
        sep_bootstrap(s, msg);
        return;
    }

    /* Runtime endpoints (keystore/AKS, credential manager, ...) not modelled
     * yet: log so the protocol can be filled in. */
    if (s->log_budget) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP,
                      "vresearch-sep: ep %u op %#x arg %#x payload %#x "
                      "(msg %#018" PRIx64 ") unanswered\n",
                      ep, MSG_OPCODE(msg), MSG_ARG(msg), MSG_PAYLOAD(msg), msg);
    }
}

static uint64_t sep_read(void *opaque, hwaddr addr, unsigned size)
{
    VResearchSEPState *s = opaque;
    uint64_t head = s->rx_count ? s->rx[s->rx_head] : 0;

    switch (addr) {
    case SEP_CONTROL:
        return s->control;
    case SEP_STATUS:
        return (s->control & SEP_CONTROL_ENABLE ? SEP_STATUS_ACTIVE : 0) |
               (s->rx_count ? SEP_STATUS_RX_EVENT : 0);
    case SEP_INT_ENABLE:
        return s->int_enable;
    case SEP_RX_STATUS:
        return s->rx_count ? 0 : SEP_EMPTY;
    case SEP_TX_STATUS:
        return SEP_EMPTY;               /* AP -> SEP is consumed at once */
    case SEP_RX_SLOT:                   /* low 64 bits (first half of ldp) */
        s->rx_half = true;
        return head;
    case SEP_RX_SLOT + 8:               /* high 64 bits; reading it pops */
        if (s->rx_count) {
            if (s->log_budget) {
                s->log_budget--;
                qemu_log_mask(LOG_UNIMP, "vresearch-sep: R slot <- %#018"
                              PRIx64 "\n", head);
            }
            s->rx_head = (s->rx_head + 1) % SEP_RX_DEPTH;
            s->rx_count--;
        }
        s->rx_half = false;
        sep_update_irq(s);
        return 0;                       /* the high word is always zero */
    default:
        return 0;
    }
}

static void sep_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    VResearchSEPState *s = opaque;

    if (s->log_budget) {
        s->log_budget--;
        qemu_log_mask(LOG_UNIMP, "vresearch-sep: W %#04x = %#" PRIx64 "\n",
                      (unsigned)addr, val);
    }

    switch (addr) {
    case SEP_CONTROL:
        s->control = val;
        break;
    case SEP_STATUS:
        /* the AP sets bit31 to acquire; the SEP then announces itself */
        if ((val & SEP_STATUS_ACTIVE) && !s->startup_sent) {
            s->startup_sent = true;
            sep_push(s, SEP_STARTUP_EVENT(1));
        } else if ((val & SEP_STATUS_RX_EVENT) && s->startup_sent &&
                   !s->adverts_sent) {
            /* the AP has acked the startup; advertise the runtime endpoints */
            s->adverts_sent = true;
            sep_advertise(s);
        }
        break;                          /* write-1-to-clear event bits */
    case SEP_INT_ENABLE:
        s->int_enable = val;
        sep_update_irq(s);
        break;
    case SEP_TX_SLOT:                   /* low 64 bits (first half of stp) */
        s->tx_low = val;
        break;
    case SEP_TX_SLOT + 8:               /* high 64 bits; completes the message */
        sep_process_tx(s, s->tx_low);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sep_ops = {
    .read = sep_read,
    .write = sep_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void sep_reset(DeviceState *dev)
{
    VResearchSEPState *s = VRESEARCH_SEP(dev);

    s->control = 0;
    s->int_enable = 0;
    s->rx_head = s->rx_count = 0;
    s->rx_half = false;
    s->tx_low = 0;
    s->rom_status = 1;
    s->booted = false;
    s->startup_sent = false;
    s->adverts_sent = false;
    s->irq_level = false;
    s->log_budget = 256;
    sep_update_irq(s);
}

static void sep_init(Object *obj)
{
    VResearchSEPState *s = VRESEARCH_SEP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sep_ops, s, "sep-mailbox", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_a);
    sysbus_init_irq(sbd, &s->irq_b);
}

static void sep_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, sep_reset);
    dc->user_creatable = false;
}

static const TypeInfo sep_info = {
    .name = TYPE_VRESEARCH_SEP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VResearchSEPState),
    .instance_init = sep_init,
    .class_init = sep_class_init,
};

static void sep_register_types(void)
{
    type_register_static(&sep_info);
}

type_init(sep_register_types)
