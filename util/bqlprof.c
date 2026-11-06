/*
 * ASOFT_BQLPROF=1: who holds the BQL, and for how long (include/qemu/bqlprof.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "qemu/bqlprof.h"
#include <pthread.h>

bool bqlprof_on;

enum { KIND_SITE, KIND_NAME, KIND_CODE };

typedef struct Site {
    const void *ptr;        /* published last (release), NULL = free */
    int line;
    int kind;
    uint64_t hold, holds, wait, waits;
    uint64_t last_hold, last_holds, last_wait, last_waits;
} Site;

#define NSITES 4096
static Site sites[NSITES];
static Site overflow_site = { "(table full)", 0, KIND_NAME };
static pthread_mutex_t insert_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t reporter_once = PTHREAD_ONCE_INIT;
static uint64_t tick_hz;

static __thread bool t_held;
static __thread uint64_t t_hold0, t_wait0;
static __thread Site *t_site;

static inline uint64_t ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;

    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return get_clock();
#endif
}

static void __attribute__((constructor)) bqlprof_init(void)
{
    const char *e = getenv("ASOFT_BQLPROF");

#if defined(__aarch64__)
    asm volatile("mrs %0, cntfrq_el0" : "=r"(tick_hz));
#else
    tick_hz = 1000000000ULL;
#endif
    bqlprof_on = e && *e && strcmp(e, "0") != 0 && tick_hz;
}

static Site *find(const void *ptr, int line, int kind)
{
    uint64_t h = ((uint64_t)(uintptr_t)ptr * 0x9E3779B97F4A7C15ULL) ^
                 ((uint64_t)(uint32_t)line * 0xC2B2AE3D27D4EB4FULL) ^ kind;
    unsigned i = (h >> 20) & (NSITES - 1);

    for (unsigned n = 0; n < NSITES; n++, i = (i + 1) & (NSITES - 1)) {
        const void *p = qatomic_load_acquire(&sites[i].ptr);

        if (!p) {
            pthread_mutex_lock(&insert_lock);
            p = sites[i].ptr;
            if (!p) {
                sites[i].line = line;
                sites[i].kind = kind;
                qatomic_store_release(&sites[i].ptr, ptr);
                pthread_mutex_unlock(&insert_lock);
                return &sites[i];
            }
            pthread_mutex_unlock(&insert_lock);
        }
        if (p == ptr && sites[i].line == line && sites[i].kind == kind) {
            return &sites[i];
        }
    }
    return &overflow_site;
}

static void charge(uint64_t now)
{
    if (t_site && now > t_hold0) {
        qatomic_add(&t_site->hold, now - t_hold0);
    }
    t_hold0 = now;
}

static void name_of(const Site *s, char *buf, size_t len)
{
    switch (s->kind) {
    case KIND_SITE: {
        const char *f = s->ptr, *slash = strrchr(f, '/');

        snprintf(buf, len, "%s:%d", slash ? slash + 1 : f, s->line);
        break;
    }
    case KIND_NAME:
        if (s->line) {
            snprintf(buf, len, "%s 0x%x", (const char *)s->ptr, s->line);
        } else {
            snprintf(buf, len, "%s", (const char *)s->ptr);
        }
        break;
    default:
        snprintf(buf, len, "fn%+" PRId64,
                 (int64_t)((uintptr_t)s->ptr - (uintptr_t)&bqlprof_retag));
        break;
    }
}

typedef struct Row {
    Site *s;
    uint64_t hold, holds, wait, waits;
} Row;

static int by_hold(const void *a, const void *b)
{
    const Row *x = a, *y = b;

    return x->hold < y->hold ? 1 : x->hold > y->hold ? -1 : 0;
}

static int by_wait(const void *a, const void *b)
{
    const Row *x = a, *y = b;

    return x->wait < y->wait ? 1 : x->wait > y->wait ? -1 : 0;
}

#define PERIOD_S 30
#define TOP_HOLD 18
#define TOP_WAIT 10

static void report(unsigned t)
{
    static Row rows[NSITES];
    unsigned n = 0;
    uint64_t hold = 0, wait = 0;
    double ms = 1000.0 / tick_hz;
    char name[160];

    for (unsigned i = 0; i < NSITES; i++) {
        Site *s = &sites[i];
        Row r;

        if (!qatomic_load_acquire(&s->ptr)) {
            continue;
        }
        r.s = s;
        r.hold = qatomic_read(&s->hold) - s->last_hold;
        r.holds = qatomic_read(&s->holds) - s->last_holds;
        r.wait = qatomic_read(&s->wait) - s->last_wait;
        r.waits = qatomic_read(&s->waits) - s->last_waits;
        s->last_hold += r.hold;
        s->last_holds += r.holds;
        s->last_wait += r.wait;
        s->last_waits += r.waits;
        if (r.hold || r.wait || r.holds) {
            rows[n++] = r;
            hold += r.hold;
            wait += r.wait;
        }
    }
    fprintf(stderr, "bql t=%us held %.0f ms (%.1f%% of %d s), threads waited %.0f ms; "
            "site hold-ms/count, wait-ms/count\n", t, hold * ms,
            hold * ms / (PERIOD_S * 10.0), PERIOD_S, wait * ms);
    qsort(rows, n, sizeof(Row), by_hold);
    for (unsigned i = 0; i < n && i < TOP_HOLD && rows[i].hold; i++) {
        name_of(rows[i].s, name, sizeof(name));
        fprintf(stderr, "bql t=%us hold %8.1f ms %8" PRIu64 "x  wait %8.1f ms  %s\n", t,
                rows[i].hold * ms, rows[i].holds, rows[i].wait * ms, name);
    }
    qsort(rows, n, sizeof(Row), by_wait);
    for (unsigned i = 0; i < n && i < TOP_WAIT && rows[i].wait; i++) {
        name_of(rows[i].s, name, sizeof(name));
        fprintf(stderr, "bql t=%us wait %8.1f ms %8" PRIu64 "x  hold %8.1f ms  %s\n", t,
                rows[i].wait * ms, rows[i].waits, rows[i].hold * ms, name);
    }
    fflush(stderr);
}

static void *reporter(void *opaque)
{
    for (unsigned t = PERIOD_S;; t += PERIOD_S) {
        g_usleep(PERIOD_S * G_USEC_PER_SEC);
        report(t);
    }
    return NULL;
}

static void start_reporter(void)
{
    pthread_t th;

    fprintf(stderr, "bqlprof: on, %" PRIu64 " ticks/s, fn offsets from bqlprof_retag %p\n",
            tick_hz, (void *)&bqlprof_retag);
    if (pthread_create(&th, NULL, reporter, NULL) == 0) {
        pthread_detach(th);
    }
}

void bqlprof_pre_lock(void)
{
    if (bqlprof_on) {
        t_wait0 = ticks();
    }
}

void bqlprof_locked(const char *file, int line)
{
    uint64_t now;
    Site *s;

    if (!bqlprof_on) {
        return;
    }
    pthread_once(&reporter_once, start_reporter);
    now = ticks();
    s = find(file, line, KIND_SITE);
    if (t_wait0) {
        if (now > t_wait0) {
            qatomic_add(&s->wait, now - t_wait0);
        }
        qatomic_inc(&s->waits);
        t_wait0 = 0;
    }
    qatomic_inc(&s->holds);
    t_held = true;
    t_site = s;
    t_hold0 = now;
}

void bqlprof_unlocking(void)
{
    if (!bqlprof_on || !t_held) {
        return;
    }
    charge(ticks());
    t_held = false;
    t_site = NULL;
    t_wait0 = 0;    /* a condition wait's relock is not a wait for the lock */
}

BqlProfTag bqlprof_retag(const void *ptr, int line)
{
    BqlProfTag prev = { NULL, 0 };
    Site *s;

    if (!t_held || !ptr) {
        return prev;
    }
    charge(ticks());
    prev.ptr = t_site;
    s = find(ptr, line < 0 ? 0 : line, line < 0 ? KIND_CODE : KIND_NAME);
    qatomic_inc(&s->holds);
    t_site = s;
    return prev;
}

void bqlprof_restore(BqlProfTag prev)
{
    if (!t_held) {
        return;
    }
    charge(ticks());
    t_site = (Site *)prev.ptr;
}
