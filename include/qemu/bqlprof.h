/*
 * ASOFT_BQLPROF=1: who holds the BQL, and for how long (measurement only).
 *
 * Every BQL acquisition starts a hold under its file:line; code that runs
 * with the BQL can retag the hold (an MMIO region, an IO system register, a
 * bottom half, a timer or fd callback), and the time until the next retag or
 * the release is charged to the tag. The time a thread waited for the lock is
 * charged to the acquiring file:line. A thread of its own prints the last
 * 30 s every 30 s ("bql t=..."). Off (one branch per lock) unless the
 * variable is set when the first BQL is taken.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_BQLPROF_H
#define QEMU_BQLPROF_H

typedef struct BqlProfTag {
    const void *ptr;
    int line;           /* >= 0: ptr is a string; -1: ptr is a code address */
} BqlProfTag;

extern bool bqlprof_on;

void bqlprof_pre_lock(void);
void bqlprof_locked(const char *file, int line);
void bqlprof_unlocking(void);
BqlProfTag bqlprof_retag(const void *ptr, int line);
void bqlprof_restore(BqlProfTag prev);

/* retag the current hold, if this thread holds the BQL and profiling is on */
static inline BqlProfTag bqlprof_tag(const void *ptr, int line)
{
    if (!bqlprof_on) {
        BqlProfTag none = { NULL, 0 };
        return none;
    }
    return bqlprof_retag(ptr, line);
}

static inline void bqlprof_untag(BqlProfTag prev)
{
    if (bqlprof_on && prev.ptr) {
        bqlprof_restore(prev);
    }
}

#endif
