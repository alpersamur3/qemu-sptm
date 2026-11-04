/*
 * native-el0: run the guest's EL0 (user-mode) code directly on an AArch64
 * Linux host while EL1 and above stay under TCG.  See native-el0.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_ARM_NATIVE_EL0_H
#define TARGET_ARM_NATIVE_EL0_H

#include "hw/core/cpu.h"

/* TCGCPUOps hooks (see accel/tcg/cpu-exec.c) */
bool native_el0_try(CPUState *cs);
void native_el0_kick(CPUState *cs);

/* TLB maintenance hooks (see tlb-insns.c): keep the host mirror coherent */
void native_el0_tlbi_all(void);
void native_el0_tlbi_asid(uint16_t asid);
void native_el0_tlbi_page(uint64_t va, int asid);     /* asid < 0: every address space */
void native_el0_tlbi_range(uint64_t base, uint64_t len, int asid);

/*
 * A broadcast page TLBI by ASID (tlb-insns.c): which vCPUs can hold the entry?
 * QEMU's TLB has no ASID tag and is emptied at every ASID change, so only those
 * in that ASID right now.  ASOFT_TLBI_FILTER=0: always TLBI_SCOPE_ALL.
 */
enum { TLBI_SCOPE_ALL, TLBI_SCOPE_SELF, TLBI_SCOPE_NONE };
int native_el0_tlbi_scope(CPUState *cs, uint64_t va, uint16_t asid);

extern bool native_el0_enabled;

#endif
