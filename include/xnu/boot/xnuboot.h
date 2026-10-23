#ifndef XNUBOOT_H
#define XNUBOOT_H

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "target/arm/cpu-qom.h"
#include "qemu/notify.h"

#define ONE_KB        BIT(10)
#define ONE_MB        BIT(20)
#define ONE_GB        BIT(30)

#define DARWIN_PAGE_SIZE_KB           (( 16 ))
#define DARWIN_HUGE_PAGE_SIZE_MB      (( 32 ))

#define DARWIN_PAGE_SIZE              (( DARWIN_PAGE_SIZE_KB * ONE_KB ))
#define DARWIN_HUGE_PAGE_SIZE         (( DARWIN_HUGE_PAGE_SIZE_MB * ONE_MB ))

#define ROUND_DOWN_POW2(x,sz)      ((       ((x))        & (~(((sz)) - 1)) ))
#define ROUND_UP_POW2(x,sz)        (( (((x)) + (((sz))-1)) & (~(((sz)) - 1)) ))
#define ROUND_NEXT_PAGE(x)         (( ROUND_UP_POW2( ((x)), ( (( DARWIN_PAGE_SIZE )) )) ))
#define XNU_STRIP_VMA(x)           (( ((x)) & ((BIT_ULL(35)-1)) ))

typedef uint64_t usize;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t  i8;

typedef struct {
    void   *buf;
    size_t  len;
} mmap_file_t;

struct xnu_boot_info {
    // Arguments to a -M darwin machine
    char *bootkc, *args, *dtree, *sptm, *txm, *tc, *ramdisk;
    mmap_file_t bootkc_f, dtree_f, sptm_f, txm_f, tc_f, ramdisk_f;

    uint64_t dram_base, dram_size;
    bool has_mte;

    // Feedback from the kernel loader back to the darwin machine to boot during reset
    hwaddr  init_pc, init_x0;
};

#define MACHINE_CLASS_ARG(a) \
static char *a##_darwin_class_get(Object *obj, Error **errp) { \
    DarwinState *s = DARWIN_MACHINE(obj); \
    return g_strdup(s->bootinfo.a); \
} \
static void a##_darwin_class_set(Object *obj, const char *path, Error **errp) { \
    DarwinState *s = DARWIN_MACHINE(obj); \
    g_free(s->bootinfo.a); \
    s->bootinfo.a = g_strdup(path); \
}

void arm_load_xnu(ARMCPU *cpu, MachineState *ms, struct xnu_boot_info *info);

#endif /* XNUBOOT_H */
