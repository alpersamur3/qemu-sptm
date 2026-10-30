#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/apple_amcc.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "xnu/apple_dtree.h"
#include "migration/vmstate.h"
#include "trace.h"

DeviceState *amcc_create(struct dtree_node *amcc_node) {
    DeviceState *d;
    SysBusDevice *sbd;
    AMCCState *s;

    uint64_t *amcc_base = (uint64_t*)adt_get_prop_val(amcc_node, "aperture-phys-addr");
    assert(amcc_base);

    d = qdev_new(TYPE_AMCC);
    sbd = SYS_BUS_DEVICE(d);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, *amcc_base);

    s = AMCC(d);

    for (size_t bank_id = 0; bank_id < AMCC_NUM_BANKS; bank_id++) {
        char bank_label = 'a' + bank_id;
        char propname[128];
        snprintf(propname, sizeof(propname), "amcc-ctrr-%c", bank_label);

        struct dtree_node *ctrr_node = adt_find_node(amcc_node, propname);
        if (!ctrr_node) {
            fprintf(stderr, "warning: no %s node found\n", propname);
            continue;
        }

        uint32_t *lwr_reg = (uint32_t*)adt_get_prop_val(ctrr_node, "lower-limit-reg-offset");
        uint32_t *upr_reg = (uint32_t*)adt_get_prop_val(ctrr_node, "upper-limit-reg-offset");

        if (lwr_reg) s->lwr_reg[bank_id] = *lwr_reg;
        if (upr_reg) s->upr_reg[bank_id] = *upr_reg;

        if (lwr_reg) assert(0 != s->lwr_reg[bank_id]);
        if (upr_reg) assert(0 != s->upr_reg[bank_id]);

        assert(!(!lwr_reg ^ !upr_reg));
    }

    return d;
}

static uint64_t amcc_read(void *opaque, hwaddr offset, unsigned size) {
    AMCCState *s = (AMCCState*)opaque;
    if (0 == offset) return 0;

    for (size_t bank_id = 0; bank_id < AMCC_NUM_BANKS; bank_id++) {
        if (offset == s->lwr_reg[bank_id]) return s->ctrr_lwr[bank_id];
        if (offset == s->upr_reg[bank_id]) return s->ctrr_upr[bank_id];
    }

    return 0;
}

static void amcc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
    AMCCState *s = (AMCCState*)opaque;
    if (0 == offset) return;

    for (size_t bank_id = 0; bank_id < AMCC_NUM_BANKS; bank_id++) {
        if (offset == s->lwr_reg[bank_id]) s->ctrr_lwr[bank_id] = value;
        if (offset == s->upr_reg[bank_id]) s->ctrr_upr[bank_id] = value;
    }
}

static const MemoryRegionOps amcc_ops = {
    .read  = amcc_read,
    .write = amcc_write,
};

static void amcc_init(Object *obj) {
    AMCCState *s = AMCC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    for (size_t i = 0; i < AMCC_NUM_BANKS; i++) {
        s->ctrr_lwr[i] = 0;
        s->ctrr_upr[i] = 0;
        s->lwr_reg[i] = 0;
        s->upr_reg[i] = 0;
    }

    memory_region_init_io(&s->iomem, obj, &amcc_ops, s, TYPE_AMCC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

/* A saved state keeps the CTRR ranges the guest programmed (the register
 * offsets come from the device tree). */
static const VMStateDescription vmstate_amcc = {
    .name = TYPE_AMCC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(ctrr_lwr, AMCCState, AMCC_NUM_BANKS),
        VMSTATE_UINT64_ARRAY(ctrr_upr, AMCCState, AMCC_NUM_BANKS),
        VMSTATE_END_OF_LIST()
    },
};

static void amcc_class_init(ObjectClass *oc, const void *data) {
    DEVICE_CLASS(oc)->vmsd = &vmstate_amcc;
}

static const TypeInfo amcc_info = {
    .name           =  TYPE_AMCC,
    .parent         =  TYPE_SYS_BUS_DEVICE,
    .instance_size  =  sizeof(AMCCState),
    .instance_init  =  amcc_init,
    .class_init     =  amcc_class_init,
};

static void amcc_register_types(void) {
    type_register_static(&amcc_info);
}

type_init(amcc_register_types)
