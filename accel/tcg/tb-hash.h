/*
 * internal execution defines for qemu
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EXEC_TB_HASH_H
#define EXEC_TB_HASH_H

#include "exec/vaddr.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "qemu/xxhash.h"
#include "tb-jmp-cache.h"

#ifdef CONFIG_SOFTMMU

/*
 * Sets of TB_JMP_CACHE_WAYS entries, the set chosen by all the pc's bits.
 * (Upstream splits the index into a page group and a slot within the page,
 * so that a page's entries can be dropped without a scan, but that caps a
 * page at 64 cached TBs: ARM64 TBs average 18 bytes, a hot 4K page holds
 * hundreds.)  tb_jmp_cache_clear_page() scans instead.
 *
 * The result is the index of the set's first entry.
 */
#define TB_JMP_CACHE_WAYS 2

static inline unsigned int tb_jmp_cache_hash_func(vaddr pc)
{
    unsigned int set = ((pc >> 2) * 0x9E3779B97F4A7C15ull)
                       >> (64 - (TB_JMP_CACHE_BITS - 1));

    return set * TB_JMP_CACHE_WAYS;
}

#else

/* In user-mode we can get better hashing because we do not have a TLB */
#define TB_JMP_CACHE_WAYS 1

static inline unsigned int tb_jmp_cache_hash_func(vaddr pc)
{
    return (pc ^ (pc >> TB_JMP_CACHE_BITS)) & (TB_JMP_CACHE_SIZE - 1);
}

#endif /* CONFIG_SOFTMMU */

static inline
uint32_t tb_hash_func(tb_page_addr_t phys_pc, vaddr pc,
                      uint32_t flags, uint64_t flags2, uint32_t cf_mask)
{
    return qemu_xxhash8(phys_pc, pc, flags2, flags, cf_mask);
}

#endif
