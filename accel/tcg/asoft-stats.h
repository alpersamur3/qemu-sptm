/*
 * Event counters for the vresearch TCG speed-up work: one row per vCPU,
 * plain increments (each row is written by its own thread), summed by
 * "info jit".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ACCEL_TCG_ASOFT_STATS_H
#define ACCEL_TCG_ASOFT_STATS_H

#include "hw/core/cpu.h"

enum {
    ASTAT_JC_LOOKUP,      /* tb_lookup() */
    ASTAT_JC_MISS,        /* ... that went to the hash table */
    ASTAT_JC_WIPE_TLB,    /* jump cache wiped by a TLB flush by mmuidx */
    ASTAT_JC_WIPE_RANGE,  /* ... by a large range flush */
    ASTAT_JC_WIPE_INVAL,  /* ... on every vCPU by a TB invalidation (CF_PCREL) */
    ASTAT_JC_WIPE_LOWER,  /* an address space's cache dropped (TLBI ASIDE1) */
    ASTAT_JC_SWITCH,      /* the lower half's cache switched (ASID change) */
    ASTAT_JC_SWITCH_NEW,  /* ... to a cleared one */
    ASTAT_JC_CLEAR_PAGE,  /* one page's entries dropped (epoch bump) */
    ASTAT_TLB_FILL,       /* tlb_set_page_full() */
    ASTAT_TLB_FILL_LOWER, /* ... for a lower-half address */
    ASTAT_PROBE_CODE,     /* get_page_addr_code_hostp() */
    ASTAT_PROBE_BTI,      /* is_guarded_page() */
    ASTAT_FLUSH_MAP0,     /* TLB flush by mmuidx, by the low 3 bits of the set */
    ASTAT_FLUSH_MAP7 = ASTAT_FLUSH_MAP0 + 7,
    ASTAT_N
};

extern uint64_t asoft_stats[16][ASTAT_N];

#define ASTAT(cpu, e) (asoft_stats[(cpu)->cpu_index & 15][e]++)
#define ASTAT_ANY(e) \
    (asoft_stats[current_cpu ? current_cpu->cpu_index & 15 : 15][e]++)

#endif
