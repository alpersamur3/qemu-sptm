/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"

#define TB_JMP_CACHE_BITS 12
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * The guest's address spaces (XNU: an ASID per process) each keep a cache
 * of their lower-half pcs, up to TB_JMP_USER_CACHES of them, so that a
 * process finds its entries again when it is scheduled; the upper half
 * (the kernel, global in XNU) has one.  Dropping a page's entries bumps
 * the page's epoch instead of touching the arrays: an entry carries the
 * epoch it was made under.
 */
#define TB_JMP_USER_CACHES 8
#define TB_JMP_EPOCH_BITS 12
#define TB_JMP_EPOCH_SIZE (1 << TB_JMP_EPOCH_BITS)

/* an entry's tb word: the pointer below bit 48, the page's epoch above */
#define TB_JMP_PTR_MASK ((1ULL << 48) - 1)
#define TB_JMP_EPOCH_SHIFT 48

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpArray {
    struct {
        uintptr_t tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpArray;

typedef struct CPUJumpCache {
    struct rcu_head rcu;
    CPUJumpArray *cur;                      /* the lower half's cache in use */
    uint32_t cur_tag;
    uint64_t use_clock;
    uint32_t user_tag[TB_JMP_USER_CACHES];  /* address space id + 1; 0 = free */
    uint64_t user_use[TB_JMP_USER_CACHES];  /* use_clock when last selected */
    uint16_t page_epoch[TB_JMP_EPOCH_SIZE];
    CPUJumpArray kernel;
    CPUJumpArray user[TB_JMP_USER_CACHES];
} CPUJumpCache;

static inline CPUJumpArray *tb_jmp_cache_array(CPUJumpCache *jc, vaddr pc)
{
    return (pc >> 63) ? &jc->kernel : jc->cur;
}

static inline unsigned int tb_jmp_cache_page_slot(vaddr pc)
{
    return (pc >> TARGET_PAGE_BITS) & (TB_JMP_EPOCH_SIZE - 1);
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
