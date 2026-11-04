/*
 * native-el0: run the guest's EL0 code directly on an AArch64 Linux host.
 *
 * The host and the guest share the instruction set, so user-mode guest code
 * needs no translation: it needs an address space and a way to trap back.
 *
 *  - Address space ("mirror"): the host virtual range [4 GiB, 64 GiB) is
 *    reserved at start-up.  Guest user pages are mapped into it at their own
 *    guest virtual address, lazily, when the native code faults on them: the
 *    handler walks the guest's own page tables (get_phys_addr) and maps the
 *    guest RAM file (memfd) with the page's EL0 permissions.  TLB maintenance
 *    instructions executed by the guest kernel drop the affected mirror pages,
 *    so the mirror is a software TLB with the same coherence rules as a
 *    hardware one.
 *
 *  - Two ways to own a mirror:
 *
 *    In-process (ASOFT_NATIVE_EL0=1): QEMU's own address space holds one
 *    mirror, so one guest address space (ASID) runs natively at a time; the
 *    vCPU thread enters guest code by sending itself a signal whose frame it
 *    rewrites, and traps come back as signals on the same thread.
 *
 *    Stub processes (ASOFT_NATIVE_EL0_STUBS=N): N helper processes, forked
 *    from a factory that QEMU forks before it has threads, each hold one
 *    mirror, so N address spaces are native at once.  A stub has a thread per
 *    vCPU; the QEMU vCPU thread drives it through a shared-memory slot
 *    (register frame, command, status) and futexes.  The stub thread enters
 *    guest code with rt_sigreturn on a frame it builds itself (no signal
 *    round trip); traps are reported back as the full signal frame and the
 *    QEMU side decides exactly as in the in-process mode.  TLB maintenance
 *    reaches a stub through a per-stub ring of invalidation ranges, applied
 *    by its threads before every entry and, while they run guest code, from
 *    a signal handler.
 *
 *  - Traps: a seccomp filter turns every system call made from below 64 GiB
 *    into SIGSYS, faults arrive as SIGSEGV/SIGBUS, BRK as SIGTRAP, UNDEF as
 *    SIGILL.  The decision is the same in both modes: resume natively (a
 *    mirror miss that could be filled, an MRS that could be emulated) or
 *    raise the architectural exception through QEMU's normal path
 *    (raise_exception -> cpu_loop_exit, which longjmps back into cpu_exec).
 *    Another vCPU or the I/O thread preempts native code with a real-time
 *    signal (a "kick").
 *
 * Diagnostics: ASOFT_NATIVE_EL0_STATS=1, ASOFT_NATIVE_EL0_TRACE=<n> (first n
 * traps on stderr), ASOFT_EL_SAMPLER=<s> (1 kHz where-is-each-vCPU sampler,
 * reports every s seconds, works without the engine too).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "cpu-features.h"
#include "syndrome.h"
#include "native-el0.h"

bool native_el0_enabled;

#if defined(__ANDROID__) && defined(__aarch64__)
/* Bionic refuses an executable whose ELF TLS segment is aligned below 64 */
__thread char asoft_tls_align[1] __attribute__((aligned(64), used));
#endif

#ifndef CONFIG_USER_ONLY
static int tlbi_filter = -1;
static uint64_t tlbi_scope_n[3];

int native_el0_tlbi_scope(CPUState *cs, uint64_t va, uint16_t asid)
{
    CPUARMState *env = cpu_env(cs);

    if (unlikely(tlbi_filter < 0)) {
        const char *e = getenv("ASOFT_TLBI_FILTER");
        tlbi_filter = !e || *e != '0';
    }
    /*
     * Not with x-keep-kernel-tlb: its per-ASID jump caches outlive an ASID
     * change, so another vCPU has to hear about the page.  Not for ASID 0 or
     * the upper half: global entries match any ASID.
     */
    if (!tlbi_filter || env_archcpu(env)->keep_kernel_tlb || !asid ||
        (int64_t)va < 0 || arm_is_el2_enabled(env)) {
        return TLBI_SCOPE_ALL;
    }
    /*
     * Against vmsa_ttbr_write(): the guest's PTE store is ordered before these
     * reads, so a vCPU we see in another ASID walks the new PTE once it comes
     * to this one.
     */
    smp_mb();
    bool self = false;
    CPUState *other;
    CPU_FOREACH(other) {
        CPUARMState *e = cpu_env(other);
        uint64_t ttbr = (qatomic_read(&e->cp15.tcr_el[1]) & (1ULL << 22))
                        ? qatomic_read(&e->cp15.ttbr1_el[1])
                        : qatomic_read(&e->cp15.ttbr0_el[1]);
        if (extract64(ttbr, 48, 16) != asid) {
            continue;
        }
        if (other != cs) {
            qatomic_inc(&tlbi_scope_n[TLBI_SCOPE_ALL]);
            return TLBI_SCOPE_ALL;
        }
        self = true;
    }
    int scope = self ? TLBI_SCOPE_SELF : TLBI_SCOPE_NONE;
    qatomic_inc(&tlbi_scope_n[scope]);
    return scope;
}
#endif

#if defined(__linux__) && defined(__aarch64__) && !defined(CONFIG_USER_ONLY)

#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <asm/sigcontext.h>
#include <arm_neon.h>

#include "qemu/atomic.h"
#include "qemu/bitops.h"
#include "qemu/rcu.h"
#include "qemu/int128.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/icount.h"
#include "exec/translation-block.h"
#include "exec/page-protection.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/ramblock.h"

/* The host window that mirrors guest user space, and a guard above it so
 * that nothing of QEMU's own lands where a wild guest pointer could reach. */
#define WIN_LO      0x100000000ULL      /* 4 GiB: XNU's user space starts here */
#define WIN_HI      0x1000000000ULL     /* 64 GiB: above XNU's MACH_VM_MAX_ADDRESS */
#define GUARD_HI    0x5000000000ULL     /* 320 GiB: Bionic maps libraries above */
#define MAX_FILL    (64ULL << 20)       /* largest single mirror mapping */
#define SLAB_SHIFT  20                  /* block-extent table granularity, 1 MiB */
#define NSLABS      ((WIN_HI - WIN_LO) >> SLAB_SHIFT)

#define ALT_SIZE    (1 << 20)           /* per-thread alternate signal stack */
#define MAX_CTX     64

/* stub mode */
#define STUB_MAX        32              /* ASOFT_NATIVE_EL0_STUBS=N, N <= this */
#define STUB_VCPUS      8               /* threads per stub = vCPUs served */
#define INVAL_RING      1024            /* pending invalidation ranges per stub */
#define STUB_FRAME_OFF  (64 << 10)      /* entry frame inside a stub thread's alt stack */
#define RT_FRAME_SIZE   ((sizeof(siginfo_t) + sizeof(ucontext_t) + 15) & ~(size_t)15)

struct StubSlot;

typedef struct NativeCtx {
    CPUState *cs;
    uint64_t host_tpidr;        /* Bionic's TLS pointer for this thread */
    uint64_t guest_tpidr;
    void *altbase;              /* ALT_SIZE-aligned; [0] holds this pointer */
    int in_guest;               /* guest state is live natively (or a stub runs it) */
    int kick_pending;
    int tcg_until_el1;          /* something native cannot do: TCG until EL1 */
    int guard_active;           /* a host read of mirror memory is in progress */
    sigjmp_buf guard;
    /* stub mode: the stub this vCPU drives right now */
    int stub;
    struct StubSlot *slot;
    int fill_pending;           /* handle_trap queued a fill in slot->fill */
    /* statistics */
    uint64_t enters, svcs, fills, fill_fails, faults, ills, emul_mrs, brks,
             kicks, unsupported;
    /* time accounting, CNTVCT ticks: native = guest code running on the
     * CPU (stub mode: command posted to report received), handler = inside
     * the trap decision before going back natively or to TCG, entry =
     * pthread_kill(sig_enter) to the first guest instruction (in-process) */
    uint64_t try_tick, enter_tick, trap_tick;
    uint64_t native_ticks, handler_ticks, entry_ticks;
    /* heat: native_ticks when the stub was acquired; a TCG slice for want of one */
    uint64_t native_mark, deny_tick;
    uint16_t deny_asid;
} NativeCtx;

static NativeCtx *ctx_by_index[MAX_CTX];
static void *altbases[MAX_CTX];
static int sig_kick, sig_enter, sig_inval;
static int trace_left;
static bool stats_on;
static bool pac_off;        /* ASOFT_NATIVE_EL0_PAC=off: EL0 keys disabled natively */
static bool tpidrro_mode;   /* ASOFT_NATIVE_EL0_TPIDRRO=1: code pages read TPIDR_EL0 instead */
static uint64_t tpidrro_patched;
static bool perm_fallback;  /* ASOFT_NATIVE_EL0_PERMFALLBACK=1 */
static bool patch_log;      /* ASOFT_NATIVE_EL0_PATCHLOG=1: every rewritten word with its neighbours */
static int nstubs;          /* ASOFT_NATIVE_EL0_STUBS=N: stub mode when > 0 */
static uint64_t stub_spin_ticks;    /* ASOFT_NATIVE_EL0_STUB_SPIN_US: a stub thread spins this long for a command */
static uint64_t qemu_spin_ticks;    /* ASOFT_NATIVE_EL0_SPIN_US: a vCPU thread spins this long for a report */
static cpu_set_t stub_cpuset;       /* ASOFT_NATIVE_EL0_STUB_CPUS=a,b,...: pin stub threads */
static bool stub_cpuset_on;
static bool tlbi_all_stubs;         /* ASOFT_NATIVE_EL0_TLBI_ASID=0: page TLBIs go to every bound stub, not by ASID */
static bool lazy_inval;             /* idle stubs apply at the next entry; ASOFT_NATIVE_EL0_LAZY_INVAL=0: at once, waited for */
static int tlbi_log_left;           /* ASOFT_NATIVE_EL0_TLBI_LOG=N: describe the first N page TLBIs */
static uint64_t tlbi_asid_hits, tlbi_asid_misses;
static int asid_watch_init;
static bool asid_watch_on;
static bool evict_heat;             /* ASOFT_NATIVE_EL0_EVICT=heat[-admit]: see stub_acquire */
static bool evict_admit;
static const char *off_file;        /* ASOFT_NATIVE_EL0_OFF_FILE: while this file exists, EL0 stays in TCG (needs the sampler) */
static int native_off;
static bool patch_xo;              /* ASOFT_NATIVE_EL0_PATCH_XO=N: rewritten code pages are execute-only */
static int xo_log_left;
static uint64_t xo_reads, xo_hits;
static char *cc_base;               /* the code cache, see cc_fill */
static uint32_t cc_next;
static uint64_t cc_clean, cc_hits, cc_misses, cc_stale, cc_none;
static bool stub_mode;

/* In-process mode: one mirrored address space at a time. */
static struct {
    int used;
    uint16_t asid;
    int active;             /* vCPUs executing natively in it right now */
    int64_t last_ns;        /* last entry, CLOCK_MONOTONIC */
    uint64_t steals;
} slot;
static int64_t steal_ns = 20 * 1000000LL;   /* ASOFT_NATIVE_EL0_STEAL_MS */
static int64_t trace_t0;
static int64_t delay_ns;        /* ASOFT_NATIVE_EL0_DELAY_MS */

static inline int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline uint64_t cntvct(void)
{
    uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static uint64_t cntfrq = 19200000;
static int sampler_period;      /* ASOFT_EL_SAMPLER=N: EL sampler report every N s */
struct el_samples { uint64_t halted, el1, el0_tcg, el0_nat; };
static struct el_samples samples[MAX_CTX];

static inline uint64_t ticks_ms(uint64_t t)
{
    return t * 1000 / cntfrq;
}

static uint64_t mirror_gen;         /* in-process: bumped by every drop; fills retry */
static pthread_mutex_t fill_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int fill_lock_held;

/* Block mappings (>= 2 MiB) by 1 MiB slab, so a page TLBI can drop the whole
 * block it belongs to. Page-sized fills are not recorded. One table per
 * process: QEMU's for the in-process mirror, each stub's for its own. */
typedef struct { uint64_t base, len; } Extent;
static Extent *slabs;

static inline uint64_t read_tpidr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

static inline void write_tpidr(uint64_t v)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(v) : "memory");
}

static inline bool in_window(uint64_t a)
{
    return a >= WIN_LO && a < WIN_HI;
}

static inline void spin_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

static inline long futex(void *addr, int op, uint32_t val, const struct timespec *ts)
{
    return syscall(SYS_futex, addr, op, val, ts, NULL, 0);
}

static inline uint16_t cur_asid(CPUARMState *env);

/* ---------------------------------------------------------------------- */
/* stub mode: what QEMU and the stub processes share                      */

enum { CMD_NONE, CMD_RUN, CMD_RESUME, CMD_FILL_RESUME, CMD_EXIT };
enum { ST_IDLE, ST_RUNNING, ST_SVC, ST_FAULT, ST_ILL, ST_TRAP, ST_FPE, ST_KICK,
       ST_FILLFAIL, ST_DEAD };
enum { FAC_NONE, FAC_RECV_FD, FAC_SPAWN };
enum { PH_INIT, PH_WAIT, PH_FILL, PH_APPLY, PH_PREENTRY, PH_GUEST, PH_TRAP, PH_INVAL,
       PH_KICKREPORT, PH_REPORTED };

typedef struct StubFill {
    uint64_t va, len, off, gen;
    int32_t prot;               /* PROT_* */
    int32_t fd_index;           /* into StubShm.ram_fd */
    int32_t patch;              /* private copy with the code-page rewrite */
    int32_t pad;
} StubFill;

/* one per (stub, vCPU) */
typedef struct StubSlot {
    uint32_t cmd;               /* futex, QEMU -> stub thread */
    uint32_t status;            /* futex, stub thread -> QEMU: ST_* */
    uint32_t kick_pending;      /* QEMU asks the stub thread to come back */
    uint32_t tid;               /* the stub thread, for tgkill */
    uint32_t ready;
    uint32_t phase;             /* PH_*: what the stub thread is doing (diagnostics) */
    uint32_t phase_pad;
    int32_t sig, si_code;
    int32_t owner_pid;       /* the stub process whose thread serves this slot */
    uint64_t fault_addr;
    uint64_t tpidr;             /* guest TPIDR_EL0: in on entry, out on a trap */
    StubFill fill;
    ucontext_t uc __attribute__((aligned(16)));     /* the guest register frame */
} StubSlot;

/* one per stub process */
typedef struct StubCtl {
    int32_t pid;
    uint32_t state;             /* futex: 0 not up, 1 ready */
    uint32_t overflow;          /* the ring wrapped: flush everything */
    uint32_t pad;
    uint64_t inval_seq;         /* last range appended (QEMU) */
    uint64_t inval_applied;     /* last range applied (stub), in order */
    uint64_t fill_ticks, fills, patch_fills, apply_ticks;   /* stub-side cost (statistics) */
    struct { uint64_t lo, hi; } inval[INVAL_RING];
} StubCtl;

typedef struct StubShm {
    uint32_t fac_cmd, fac_arg, fac_done;    /* factory request/ack futexes */
    uint32_t nvcpus;
    int32_t ram_fd[8];          /* guest RAM fds as the factory (and so the stubs) sees them */
    int32_t nram;
    StubCtl ctl[STUB_MAX];
    StubSlot slot[STUB_MAX][STUB_VCPUS];
} StubShm;

static StubShm *shm;
static pid_t factory_pid;
static int fac_sock = -1;       /* QEMU's end of the fd-passing socket */
static int ram_fd_q[8];         /* QEMU-side fd numbers, same index as shm->ram_fd */
static int nram_q;
static struct {
    int asid;
    int running;                /* vCPUs driving it right now */
    int64_t last_ns;
    bool bound;
} sbind[STUB_MAX];
static int16_t asid_stub[65536];
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t inval_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stub_spawns, stub_evictions, inval_waits, inval_slow, inval_timeouts;
static int asid_max_seen;
static uint64_t asid_root_changes, asid_root_concurrent;
static uint64_t asid_last_root[65536];
static uint64_t stub_denials;       /* EL0 slices left to TCG: no stub to spare */
enum { TL_ALL, TL_ASID, TL_PAGE_ASID, TL_PAGE_ANY, TL_RANGE, TL_KINDS };
static uint64_t tlbi_kind[TL_KINDS];

/* ---------------------------------------------------------------------- */
/* set-up                                                                 */

static void stats_report(void)
{
    uint64_t enters = 0, svcs = 0, fills = 0, fill_fails = 0, faults = 0,
             ills = 0, emul = 0, brks = 0, kicks = 0, unsup = 0;
    for (int i = 0; i < MAX_CTX; i++) {
        NativeCtx *nc = ctx_by_index[i];
        if (!nc) {
            continue;
        }
        enters += nc->enters; svcs += nc->svcs; fills += nc->fills;
        fill_fails += nc->fill_fails; faults += nc->faults; ills += nc->ills;
        emul += nc->emul_mrs; brks += nc->brks; kicks += nc->kicks;
        unsup += nc->unsupported;
    }
    fprintf(stderr, "native-el0: enters %" PRIu64 " svc %" PRIu64
            " fills %" PRIu64 " (failed %" PRIu64 ") faults-injected %" PRIu64
            " undef %" PRIu64 " mrs-emulated %" PRIu64 " brk %" PRIu64
            " kicks %" PRIu64 " unsupported %" PRIu64 " tpidrro-patched %" PRIu64 "\n",
            enters, svcs, fills, fill_fails, faults, ills, emul, brks, kicks,
            unsup, tpidrro_patched);
    if (stub_mode) {
        int bound = 0;
        for (int i = 0; i < nstubs; i++) {
            bound += sbind[i].bound;
        }
        fprintf(stderr, "native-el0: stubs %d spawned %" PRIu64 " bound %d evictions %" PRIu64
                " inval-waits %" PRIu64 " (slow %" PRIu64 ", timeouts %" PRIu64 ")\n",
                nstubs, stub_spawns, bound, stub_evictions, inval_waits, inval_slow, inval_timeouts);
    } else {
        fprintf(stderr, "native-el0: slot steals %" PRIu64 "\n", slot.steals);
    }
    uint64_t nat = 0, hand = 0, ent = 0;
    for (int i = 0; i < MAX_CTX; i++) {
        NativeCtx *nc = ctx_by_index[i];
        if (nc) {
            nat += nc->native_ticks; hand += nc->handler_ticks; ent += nc->entry_ticks;
        }
    }
    fprintf(stderr, "native-el0: time native %" PRIu64 " ms, trap handling %" PRIu64
            " ms, entry %" PRIu64 " ms (wall since first vCPU %" PRId64 " ms)\n",
            ticks_ms(nat), ticks_ms(hand), ticks_ms(ent),
            trace_t0 ? (mono_ns() - trace_t0) / 1000000 : 0);
}

/*
 * ASOFT_PC_SAMPLER=<dir>: where do the vCPU threads spend their time?  The EL
 * sampler signals each of them every other tick; the handler records the host
 * PC, the guest EL and the nearest return address inside QEMU.  Written to
 * <dir>/pcprof.bin (two u64: pc | el << 62, caller) with <dir>/pcprof.maps;
 * scripts/phone/pcprof.py turns them into a profile.  Needs no perf rights on
 * the phone.
 */
#define PROF_RING (1u << 16)
static const char *prof_dir;
static uint64_t prof_ring[PROF_RING][2];
static uint32_t prof_head;
static uint32_t prof_tail;
static FILE *prof_file;
static __thread bool prof_thread_ready;
extern char __executable_start, etext;

static uint64_t prof_caller(ucontext_t *uc)
{
    const uint64_t lo = (uint64_t)&__executable_start, hi = (uint64_t)&etext;
    const uint64_t mask = (1ULL << 48) - 1;     /* a PAC signature in the saved LR */
    uint64_t lr = uc->uc_mcontext.regs[30] & mask;
    uint64_t sp = uc->uc_mcontext.sp, fp = uc->uc_mcontext.regs[29];
    if (lr >= lo && lr < hi) {
        return lr;
    }
    for (int i = 0; i < 16; i++) {
        if ((fp & 15) || fp < sp || fp > sp + (256u << 10)) {
            break;
        }
        uint64_t next = ((uint64_t *)fp)[0], ret = ((uint64_t *)fp)[1] & mask;
        if (ret >= lo && ret < hi) {
            return ret;
        }
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    return 0;
}

static void prof_handler(int sig, siginfo_t *si, void *uc_)
{
    int saved = errno;
    ucontext_t *uc = uc_;
    uint64_t pc = uc->uc_mcontext.pc;
    uint64_t el = 0;
    if (current_cpu) {
        el = extract32(cpu_env(current_cpu)->pstate, 2, 2);
    }
    uint32_t i = qatomic_fetch_inc(&prof_head) % PROF_RING;
    prof_ring[i][1] = prof_caller(uc);
    prof_ring[i][0] = (pc & ((1ULL << 62) - 1)) | (el << 62);
    errno = saved;
}

static void prof_thread_init(void)
{
    static bool handler_set;
    prof_thread_ready = true;
    if (!qatomic_xchg(&handler_set, true)) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = prof_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, NULL);
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPROF);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

static void prof_drain(void)
{
    if (!prof_file) {
        char path[512], line[1024];
        snprintf(path, sizeof(path), "%s/pcprof.maps", prof_dir);
        FILE *in = fopen("/proc/self/maps", "r"), *out = fopen(path, "w");
        while (in && out && fgets(line, sizeof(line), in)) {
            fputs(line, out);
        }
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
        snprintf(path, sizeof(path), "%s/pcprof.bin", prof_dir);
        prof_file = fopen(path, "wb");
    }
    uint32_t head = qatomic_read(&prof_head);
    while (prof_file && prof_tail != head) {
        fwrite(prof_ring[prof_tail++ % PROF_RING], 8, 2, prof_file);
    }
    if (prof_file) {
        fflush(prof_file);
    }
}

/* 1 kHz sampler: where is each vCPU? halted (WFI), EL1 (XNU), EL0 under TCG,
 * EL0 natively. Reads env->pstate and cs->halted from another thread: a
 * sample may be off by one TB, which the 1 ms grain does not notice. */
static void *sampler_main(void *arg)
{
    struct el_samples last[MAX_CTX] = { 0 };
    uint64_t lnat = 0, lhand = 0, lent = 0, lenter = 0, lfill = 0, lsteal = 0, lmrs = 0;
    rcu_register_thread();
    sleep(1);
    int64_t t0 = mono_ns();
    int64_t next = t0 + sampler_period * 1000000000LL;
    int off_tick = 0;
    for (;;) {
        usleep(1000);
        off_tick++;
        if (off_file && off_tick % 500 == 0) {
            qatomic_set(&native_off, access(off_file, F_OK) == 0);
        }
        if (prof_dir && off_tick % 4096 == 0) {
            prof_drain();
        }
        CPUState *cs;
        rcu_read_lock();
        CPU_FOREACH(cs) {
            int i = cs->cpu_index;
            if (i < 0 || i >= MAX_CTX) {
                continue;
            }
            CPUARMState *env = cpu_env(cs);
            if (prof_dir && off_tick & 1 && cs->thread_id) {
                syscall(SYS_tgkill, getpid(), cs->thread_id, SIGPROF);
            }
            if (qatomic_read(&cs->halted)) {
                samples[i].halted++;
            } else if (extract32(env->pstate, 2, 2) != 0) {
                samples[i].el1++;
            } else if (ctx_by_index[i] && qatomic_read(&ctx_by_index[i]->in_guest)) {
                samples[i].el0_nat++;
            } else {
                samples[i].el0_tcg++;
            }
        }
        rcu_read_unlock();
        int64_t now = mono_ns();
        if (now < next) {
            continue;
        }
        next += sampler_period * 1000000000LL;
        struct el_samples tot = { 0 };
        uint64_t nat = 0, hand = 0, ent = 0, enters = 0, fills = 0, mrs = 0;
        for (int i = 0; i < MAX_CTX; i++) {
            struct el_samples d = { samples[i].halted - last[i].halted,
                                    samples[i].el1 - last[i].el1,
                                    samples[i].el0_tcg - last[i].el0_tcg,
                                    samples[i].el0_nat - last[i].el0_nat };
            last[i] = samples[i];
            uint64_t n = d.halted + d.el1 + d.el0_tcg + d.el0_nat;
            if (!n) {
                continue;
            }
            tot.halted += d.halted; tot.el1 += d.el1;
            tot.el0_tcg += d.el0_tcg; tot.el0_nat += d.el0_nat;
            fprintf(stderr, "el-sample t=%" PRId64 "s cpu%d: halted %4.1f%% el1 %4.1f%% el0-tcg %4.1f%% el0-nat %4.1f%%\n",
                    (now - t0) / 1000000000LL, i, 100.0 * d.halted / n, 100.0 * d.el1 / n,
                    100.0 * d.el0_tcg / n, 100.0 * d.el0_nat / n);
            NativeCtx *nc = ctx_by_index[i];
            if (nc) {
                nat += nc->native_ticks; hand += nc->handler_ticks; ent += nc->entry_ticks;
                enters += nc->enters; fills += nc->fills; mrs += nc->emul_mrs;
            }
        }
        uint64_t n = tot.halted + tot.el1 + tot.el0_tcg + tot.el0_nat;
        if (n) {
            uint64_t steals = stub_mode ? stub_evictions : slot.steals;
            fprintf(stderr, "el-sample t=%" PRId64 "s all: halted %4.1f%% el1 %4.1f%% el0-tcg %4.1f%% el0-nat %4.1f%%"
                    " | native %" PRIu64 " ms handler %" PRIu64 " ms entry %" PRIu64 " ms enters %" PRIu64
                    " fills %" PRIu64 " mrs %" PRIu64 " %s %" PRIu64 "\n",
                    (now - t0) / 1000000000LL, 100.0 * tot.halted / n, 100.0 * tot.el1 / n,
                    100.0 * tot.el0_tcg / n, 100.0 * tot.el0_nat / n,
                    ticks_ms(nat - lnat), ticks_ms(hand - lhand), ticks_ms(ent - lent),
                    enters - lenter, fills - lfill, mrs - lmrs,
                    stub_mode ? "evictions" : "steals", steals - lsteal);
            lsteal = steals;
            if (tlbi_filter > 0) {
                fprintf(stderr, "el-sample t=%" PRId64 "s tlbi-scope: all %" PRIu64 " self %" PRIu64 " none %" PRIu64 "\n",
                        (now - t0) / 1000000000LL, tlbi_scope_n[TLBI_SCOPE_ALL],
                        tlbi_scope_n[TLBI_SCOPE_SELF], tlbi_scope_n[TLBI_SCOPE_NONE]);
            }
            if (stub_mode) {
                uint64_t ft = 0, fn = 0, fp = 0, at = 0;
                for (int s = 0; s < nstubs; s++) {
                    ft += qatomic_read(&shm->ctl[s].fill_ticks);
                    fn += qatomic_read(&shm->ctl[s].fills);
                    fp += qatomic_read(&shm->ctl[s].patch_fills);
                    at += qatomic_read(&shm->ctl[s].apply_ticks);
                }
                fprintf(stderr, "el-sample t=%" PRId64 "s stub-side: fill %" PRIu64 " ms for %" PRIu64
                        " fills (%" PRIu64 " code copies), invalidation %" PRIu64 " ms (totals)"
                        " xo-reads %" PRIu64 " hits %" PRIu64 " | code pages: clean %" PRIu64 " cached %" PRIu64
                        " new %" PRIu64 " changed %" PRIu64 " private %" PRIu64 " slots %u\n",
                        (now - t0) / 1000000000LL, ticks_ms(ft), fn, fp, ticks_ms(at), xo_reads, xo_hits,
                        cc_clean, cc_hits, cc_misses, cc_stale, cc_none, cc_next);
                fprintf(stderr, "el-sample t=%" PRId64 "s stubs: waits %" PRIu64 " slow %" PRIu64
                        " timeouts %" PRIu64 " spawned %" PRIu64 " tlbi-asid hit %" PRIu64 " miss %" PRIu64
                        " asid-max %d root-changes %" PRIu64 " concurrent %" PRIu64 " denials %" PRIu64
                        " tlbi all/asid/page-asid/page-any/range %" PRIu64 "/%" PRIu64 "/%" PRIu64
                        "/%" PRIu64 "/%" PRIu64 "\n",
                        (now - t0) / 1000000000LL, inval_waits, inval_slow, inval_timeouts, stub_spawns,
                        tlbi_asid_hits, tlbi_asid_misses, asid_max_seen, asid_root_changes, asid_root_concurrent,
                        stub_denials, tlbi_kind[TL_ALL], tlbi_kind[TL_ASID], tlbi_kind[TL_PAGE_ASID],
                        tlbi_kind[TL_PAGE_ANY], tlbi_kind[TL_RANGE]);
            }
        }
        lnat = nat; lhand = hand; lent = ent; lenter = enters; lfill = fills; lmrs = mrs;
    }
    return arg;
}

static void sampler_start(void)
{
    const char *e = getenv("ASOFT_EL_SAMPLER");
    sampler_period = e ? atoi(e) : 0;
    prof_dir = getenv("ASOFT_PC_SAMPLER");
}

static int sampler_launched;

static void sampler_launch(void)
{
    if (sampler_period <= 0 || qatomic_xchg(&sampler_launched, 1)) {
        return;
    }
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, sampler_main, NULL) != 0) {
        fprintf(stderr, "el-sample: cannot start: %s\n", strerror(errno));
    }
}

static bool stub_shm_init(void);

static void __attribute__((constructor)) native_el0_setup(void)
{
    {
        uint64_t f;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
        if (f) {
            cntfrq = f;
        }
    }
    sampler_start();
    const char *e = getenv("ASOFT_NATIVE_EL0");
    if (!e || !*e || *e == '0') {
        return;
    }
    void *w = mmap((void *)WIN_LO, WIN_HI - WIN_LO, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                   MAP_FIXED_NOREPLACE, -1, 0);
    if (w != (void *)WIN_LO) {
        fprintf(stderr, "native-el0: cannot reserve [4 GiB, 64 GiB): %s; "
                "staying on TCG\n", strerror(errno));
        return;
    }
    void *g = mmap((void *)WIN_HI, GUARD_HI - WIN_HI, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                   MAP_FIXED_NOREPLACE, -1, 0);
    if (g != (void *)WIN_HI) {
        fprintf(stderr, "native-el0: no guard above the window (%s)\n",
                strerror(errno));
    }
    slabs = calloc(NSLABS, sizeof(Extent));
    sig_kick = SIGRTMIN + 6;
    sig_enter = SIGRTMIN + 7;
    sig_inval = SIGRTMIN + 8;
    e = getenv("ASOFT_NATIVE_EL0_TRACE");
    trace_left = e ? atoi(e) : 0;
    e = getenv("ASOFT_NATIVE_EL0_STATS");
    stats_on = e && *e && *e != '0';
    e = getenv("ASOFT_NATIVE_EL0_PAC");
    pac_off = e && !strcmp(e, "off");
    e = getenv("ASOFT_NATIVE_EL0_TPIDRRO");
    tpidrro_mode = e && *e && *e != '0';
    e = getenv("ASOFT_NATIVE_EL0_DELAY_MS");
    delay_ns = e ? atoll(e) * 1000000LL : 0;
    e = getenv("ASOFT_NATIVE_EL0_PERMFALLBACK");
    perm_fallback = e && *e && *e != '0';
    e = getenv("ASOFT_NATIVE_EL0_PATCHLOG");
    patch_log = e && *e && *e != '0';
    e = getenv("ASOFT_NATIVE_EL0_STEAL_MS");
    if (e) {
        steal_ns = atoll(e) * 1000000LL;
    }
    e = getenv("ASOFT_NATIVE_EL0_STUBS");
    nstubs = e ? atoi(e) : 0;
    e = getenv("ASOFT_NATIVE_EL0_STUB_SPIN_US");
    stub_spin_ticks = (e ? atoll(e) : 300) * cntfrq / 1000000;
    e = getenv("ASOFT_NATIVE_EL0_SPIN_US");
    qemu_spin_ticks = (e ? atoll(e) : 100) * cntfrq / 1000000;
    e = getenv("ASOFT_NATIVE_EL0_TLBI_LOG");
    tlbi_log_left = e ? atoi(e) : 0;
    e = getenv("ASOFT_NATIVE_EL0_ASID_WATCH");
    asid_watch_init = e ? atoi(e) : 0;
    asid_watch_on = asid_watch_init > 0;
    e = getenv("ASOFT_NATIVE_EL0_LAZY_INVAL");
    lazy_inval = !e || !*e || *e != '0';
    e = getenv("ASOFT_NATIVE_EL0_TLBI_ASID");
    tlbi_all_stubs = e && *e == '0';
    off_file = getenv("ASOFT_NATIVE_EL0_OFF_FILE");
    e = getenv("ASOFT_NATIVE_EL0_PATCH_XO");
    patch_xo = e && *e && *e != '0';
    xo_log_left = e ? atoi(e) : 0;
    e = getenv("ASOFT_NATIVE_EL0_EVICT");
    evict_heat = e && !strncmp(e, "heat", 4);
    evict_admit = e && !strcmp(e, "heat-admit");
    e = getenv("ASOFT_NATIVE_EL0_STUB_CPUS");
    if (e && *e) {
        CPU_ZERO(&stub_cpuset);
        for (const char *q = e; *q; ) {
            char *end;
            long c = strtol(q, &end, 10);
            if (end == q) {
                break;
            }
            if (c >= 0 && c < CPU_SETSIZE) {
                CPU_SET(c, &stub_cpuset);
                stub_cpuset_on = true;
            }
            q = *end == ',' ? end + 1 : end;
        }
    }
    if (nstubs > STUB_MAX) {
        nstubs = STUB_MAX;
    }
    if (nstubs > 0) {
        /* the factory must fork while QEMU is still single-threaded, and
         * after the knobs above are set, since the stubs inherit them */
        if (stub_shm_init()) {
            stub_mode = true;
        } else {
            fprintf(stderr, "native-el0: no stub factory, in-process mirror instead\n");
            nstubs = 0;
        }
    }
    if (stats_on) {
        atexit(stats_report);
    }
    native_el0_enabled = true;
}

/* ---------------------------------------------------------------------- */
/* signal handlers and seccomp (installed in QEMU's vCPU threads in the    */
/* in-process mode, in every stub process in stub mode)                   */

static void enter_handler(int sig, siginfo_t *si, void *uc_);
static void trap_handler(int sig, siginfo_t *si, void *uc_);

static struct sigaction old_actions[65];

static void install_handlers(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    int traps[] = { SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, 0 };
    struct sigaction sa;

    for (int i = 0; traps[i]; i++) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = trap_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(traps[i], &sa, &old_actions[traps[i]]);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(sig_kick, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = enter_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, sig_kick);  /* a kick during entry waits for the guest */
    sigaction(sig_enter, &sa, NULL);
}

/* The seccomp filter: a system call from an instruction below 64 GiB (guest
 * code in the mirror) traps with SIGSYS; our own code lives above. */
static bool install_seccomp(void)
{
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + 4),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, (uint32_t)(WIN_HI >> 32), 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
    };
    struct sock_fprog prog = { .len = ARRAY_SIZE(f), .filter = f };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "native-el0: PR_SET_NO_NEW_PRIVS: %s\n", strerror(errno));
        return false;
    }
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) < 0) {
        fprintf(stderr, "native-el0: seccomp: %s\n", strerror(errno));
        return false;
    }
    return true;
}

static void disable_pac_keys(void)
{
    unsigned long keys = PR_PAC_APIAKEY | PR_PAC_APIBKEY | PR_PAC_APDAKEY |
                         PR_PAC_APDBKEY;
    if (prctl(PR_PAC_SET_ENABLED_KEYS, keys, 0, 0, 0) < 0) {
        fprintf(stderr, "native-el0: PR_PAC_SET_ENABLED_KEYS: %s\n",
                strerror(errno));
    }
}

static NativeCtx *ctx_init(CPUState *cs)
{
    int idx = cs->cpu_index;
    if (idx < 0 || idx >= MAX_CTX) {
        return NULL;
    }
    if (!trace_t0) {
        trace_t0 = mono_ns();
    }
    NativeCtx *nc = g_new0(NativeCtx, 1);
    nc->cs = cs;
    nc->stub = -1;
    nc->host_tpidr = read_tpidr();
    if (stub_mode) {
        /* nothing native runs on this thread: no alt stack, no filter */
        ctx_by_index[idx] = nc;
        fprintf(stderr, "native-el0: vCPU %d ready (stub mode)\n", idx);
        return nc;
    }
    if (posix_memalign(&nc->altbase, ALT_SIZE, ALT_SIZE) != 0) {
        g_free(nc);
        return NULL;
    }
    *(NativeCtx **)nc->altbase = nc;
    stack_t ss = {
        .ss_sp = (char *)nc->altbase + 64,
        .ss_size = ALT_SIZE - 64,
        .ss_flags = 0,
    };
    if (sigaltstack(&ss, NULL) < 0) {
        fprintf(stderr, "native-el0: sigaltstack: %s\n", strerror(errno));
        free(nc->altbase);
        g_free(nc);
        return NULL;
    }
    install_handlers();
    /* QEMU creates vCPU threads with nearly every signal blocked */
    sigset_t set;
    sigemptyset(&set);
    int sigs[] = { SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, sig_kick,
                   sig_enter };
    for (unsigned i = 0; i < ARRAY_SIZE(sigs); i++) {
        sigaddset(&set, sigs[i]);
    }
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    if (pac_off) {
        disable_pac_keys();
    }
    if (!install_seccomp()) {
        g_free(nc);
        return NULL;
    }
    altbases[idx] = nc->altbase;
    ctx_by_index[idx] = nc;
    fprintf(stderr, "native-el0: vCPU %d ready (tpidr %#" PRIx64 ")\n",
            idx, nc->host_tpidr);
    return nc;
}

/* Find this thread's context from the alternate stack we are running on,
 * without touching TLS (the guest's TPIDR_EL0 may still be loaded). */
static NativeCtx *ctx_from_stack(void *stack_local)
{
    uintptr_t base = (uintptr_t)stack_local & ~(uintptr_t)(ALT_SIZE - 1);
    for (int i = 0; i < MAX_CTX; i++) {
        if (altbases[i] && (uintptr_t)altbases[i] == base) {
            return *(NativeCtx **)base;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* the mirror: common pieces                                              */

/*
 * The code-page rewrite for iOS guests (tpidrro_mode): Linux gives EL0 a zero
 * TPIDRRO_EL0 but lets each thread keep its own TPIDR_EL0, which we load with
 * the guest's TPIDRRO (the TSD base), so "mrs Xt, TPIDRRO_EL0" becomes
 * "mrs Xt, TPIDR_EL0"; XNU keeps the cpu number in TPIDR_EL0, so that read
 * becomes TPIDRRO_EL0 (host: 0 = cpu 0, a valid hint); the counter reads
 * become CVAL encodings, which trap and are emulated with the guest counter.
 */
static inline bool is_patch_word(uint32_t w)
{
    switch (w & 0xffffffe0u) {
    case 0xd53bd060u: case 0xd53bd040u:
    case 0xd53be040u: case 0xd53be0c0u:
    case 0xd53be020u: case 0xd53be0a0u:
        return true;
    default:
        return false;
    }
}

/* what a word from is_patch_word() is rewritten to */
static inline uint32_t patched_word(uint32_t w)
{
    uint32_t rt = w & 31;
    switch (w & 0xffffffe0u) {
    case 0xd53bd060u:   /* mrs Xt, TPIDRRO_EL0 -> TPIDR_EL0 */
        return 0xd53bd040u | rt;
    case 0xd53bd040u:   /* mrs Xt, TPIDR_EL0 (XNU: cpu number) -> TPIDRRO_EL0 */
        return 0xd53bd060u | rt;
    case 0xd53be040u:   /* mrs Xt, CNTVCT_EL0   -> CNTV_CVAL_EL0 (traps) */
    case 0xd53be0c0u:   /* mrs Xt, CNTVCTSS_EL0 */
        return 0xd53be340u | rt;
    case 0xd53be020u:   /* mrs Xt, CNTPCT_EL0   -> CNTP_CVAL_EL0 (traps) */
    case 0xd53be0a0u:   /* mrs Xt, CNTPCTSS_EL0 */
        return 0xd53be240u | rt;
    default:
        return w;
    }
}

static uint64_t patch_code_words(uint32_t *w, uint64_t nwords, uint64_t pg)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < nwords; i++) {
        uint32_t op = w[i] & 0xffffffe0u;
        if (patch_log && op == 0xd51bd040u) {
            fprintf(stderr, "native-el0: guest uses TPIDR_EL0 at va=%#" PRIx64
                    " insn=%#x\n", pg + i * 4, w[i]);
        }
        if (patch_log && (op == 0xd53bd060u || op == 0xd53be040u ||
                          op == 0xd53be0c0u || op == 0xd53be020u ||
                          op == 0xd53be0a0u)) {
            fprintf(stderr, "native-el0: patch va=%#" PRIx64
                    " ctx %08x %08x [%08x] %08x %08x" "\n", pg + i * 4,
                    i >= 2 ? w[i - 2] : 0, i >= 1 ? w[i - 1] : 0, w[i],
                    i + 1 < nwords ? w[i + 1] : 0, i + 2 < nwords ? w[i + 2] : 0);
        }
        if (is_patch_word(w[i])) {
            w[i] = patched_word(w[i]);
            n++;
        }
    }
    tpidrro_patched += n;
    return n;
}

/*
 * The code cache (ASOFT_NATIVE_EL0_CODECACHE=0 turns it off): a guest code
 * page without a word to rewrite is mapped straight from guest RAM, one with
 * such words is rewritten once into a memfd that every stub maps.  QEMU does
 * the rewrite and owns the table; a stub only makes a mapping.  The frame is
 * compared with its copy at every fill, since the guest may have reused it;
 * a copy is overwritten in place, like the frame itself would have been.
 */
#define CC_PAGE         0x4000u
#define CC_WORDS        (CC_PAGE / 4)
#define CC_SLOTS        (1u << 16)      /* 1 GiB of copies at most */
#define CC_MAXP         61              /* more rewritten words than this: a private copy */
#define CC_FRAMES       (1u << 22)      /* 64 GiB of guest RAM */
#define FD_INDEX_CC     (-2)            /* StubFill.fd_index: the code cache */

typedef struct {
    uint32_t off_frame;                 /* for which frame, +1 */
    uint16_t n;
    uint16_t idx[CC_MAXP];
} CcMeta;

enum { CC_CLEAN, CC_CACHED, CC_NONE };

static int cc_fd = -1;
static uint32_t *cc_of_frame;           /* frame -> slot + 1 */
static CcMeta *cc_meta;
static pthread_mutex_t cc_lock = PTHREAD_MUTEX_INITIALIZER;

static bool cc_create(void)
{
    cc_fd = syscall(SYS_memfd_create, "asoft-codecache", 0);
    if (cc_fd < 0 || ftruncate(cc_fd, (off_t)CC_SLOTS * CC_PAGE) < 0) {
        return false;
    }
    cc_base = mmap(NULL, (size_t)CC_SLOTS * CC_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, cc_fd, 0);
    cc_of_frame = mmap(NULL, CC_FRAMES * sizeof(uint32_t), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    cc_meta = mmap(NULL, CC_SLOTS * sizeof(CcMeta), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return cc_base != MAP_FAILED && cc_of_frame != MAP_FAILED && cc_meta != MAP_FAILED;
}

/* the words of a page to rewrite; -1: more than CC_MAXP */
static int cc_scan(const uint32_t *w, uint16_t *idx)
{
    const uint32x4_t mask = vdupq_n_u32(0xffff0000u), want = vdupq_n_u32(0xd53b0000u);
    int n = 0;
    for (unsigned i = 0; i < CC_WORDS; i += 4) {
        if (vmaxvq_u32(vceqq_u32(vandq_u32(vld1q_u32(w + i), mask), want))) {
            for (unsigned j = i; j < i + 4; j++) {
                if (is_patch_word(w[j])) {
                    if (n == CC_MAXP) {
                        return -1;
                    }
                    idx[n++] = j;
                }
            }
        }
    }
    return n;
}

/* does the slot hold exactly the rewrite of this frame? */
static bool cc_same(const uint32_t *w, const uint32_t *copy, const CcMeta *m, const uint16_t *idx, int n)
{
    if (m->n != n || memcmp(m->idx, idx, n * sizeof(uint16_t))) {
        return false;
    }
    unsigned from = 0;
    for (int k = 0; k <= n; k++) {
        unsigned to = k < n ? idx[k] : CC_WORDS;
        if (memcmp(w + from, copy + from, (to - from) * 4)) {
            return false;
        }
        if (k < n && copy[to] != patched_word(w[to])) {
            return false;
        }
        from = to + 1;
    }
    return true;
}

/* @hp: the guest frame in QEMU's mapping, @off: its offset in the RAM fd */
static int cc_fill(const void *hp, uint64_t off, uint64_t *slot_off)
{
    const uint32_t *w = hp;
    uint16_t idx[CC_MAXP];
    uint64_t frame = off / CC_PAGE;
    int n = cc_scan(w, idx);
    if (n == 0) {
        qatomic_inc(&cc_clean);
        return CC_CLEAN;
    }
    if (n < 0 || frame >= CC_FRAMES) {
        qatomic_inc(&cc_none);
        return CC_NONE;
    }
    pthread_mutex_lock(&cc_lock);
    uint32_t s = cc_of_frame[frame];
    if (s) {
        s--;
        if (cc_same(w, (uint32_t *)(cc_base + (uint64_t)s * CC_PAGE), &cc_meta[s], idx, n)) {
            pthread_mutex_unlock(&cc_lock);
            *slot_off = (uint64_t)s * CC_PAGE;
            cc_hits++;
            return CC_CACHED;
        }
        cc_stale++;
    } else {
        if (cc_next == CC_SLOTS) {
            pthread_mutex_unlock(&cc_lock);
            qatomic_inc(&cc_none);
            return CC_NONE;
        }
        s = cc_next++;
        cc_of_frame[frame] = s + 1;
        cc_misses++;
    }
    uint32_t *copy = (uint32_t *)(cc_base + (uint64_t)s * CC_PAGE);
    memcpy(copy, w, CC_PAGE);
    for (int k = 0; k < n; k++) {
        copy[idx[k]] = patched_word(w[idx[k]]);
    }
    __builtin___clear_cache((char *)copy, (char *)copy + CC_PAGE);
    cc_meta[s].n = n;
    memcpy(cc_meta[s].idx, idx, n * sizeof(uint16_t));
    pthread_mutex_unlock(&cc_lock);
    *slot_off = (uint64_t)s * CC_PAGE;
    return CC_CACHED;
}

/*
 * A private code copy is built aside and moved into place whole: another
 * thread of this address space may be reading the range right now (constants
 * and strings live in the code segments) and must see no mapping or the
 * finished copy, never zeroes or half a page.  @src, or else @fd at @off.
 */
static void *patched_copy_install(uint64_t va, uint64_t len, int prot,
                                  const void *src, int fd, uint64_t off)
{
    char *tmp = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tmp == MAP_FAILED) {
        return MAP_FAILED;
    }
    if (src) {
        memcpy(tmp, src, len);
    } else {
        for (uint64_t done = 0; done < len; ) {
            ssize_t n = pread(fd, tmp + done, len - done, off + done);
            if (n <= 0) {
                munmap(tmp, len);
                return MAP_FAILED;
            }
            done += n;
        }
    }
    uint64_t patched = patch_code_words((uint32_t *)tmp, len / 4, va);
    /* before the mapping is visible; a page without read permission could not be cleaned later */
    __builtin___clear_cache(tmp, tmp + len);
    if (patch_xo && patched) {
        prot = PROT_EXEC;       /* a data read of this copy traps: see handle_trap */
    }
    mprotect(tmp, len, prot);
    void *p = mremap(tmp, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)va);
    if (p == MAP_FAILED) {
        munmap(tmp, len);
    }
    return p;
}

static void slabs_record(uint64_t pg, uint64_t len)
{
    for (uint64_t s = (pg - WIN_LO) >> SLAB_SHIFT;
         s < ((pg + len - WIN_LO) >> SLAB_SHIFT) && s < NSLABS; s++) {
        qatomic_set(&slabs[s].base, pg);
        qatomic_set(&slabs[s].len, len);
    }
}

/* the block extent containing @va, if any: cleared from the table */
static bool slabs_take_block(uint64_t va, uint64_t *base, uint64_t *len)
{
    if (!in_window(va)) {
        return false;
    }
    Extent *x = &slabs[(va - WIN_LO) >> SLAB_SHIFT];
    uint64_t b = qatomic_read(&x->base), l = qatomic_read(&x->len);
    if (!l) {
        return false;
    }
    for (uint64_t s = (b - WIN_LO) >> SLAB_SHIFT;
         s < ((b + l - WIN_LO) >> SLAB_SHIFT) && s < NSLABS; s++) {
        qatomic_set(&slabs[s].len, 0);
        qatomic_set(&slabs[s].base, 0);
    }
    *base = b;
    *len = l;
    return true;
}

static void clamp_range(uint64_t *lo, uint64_t *hi)
{
    *lo &= ~0xfffULL;
    *hi = (*hi + 0xfff) & ~0xfffULL;
    if (*lo < WIN_LO) {
        *lo = WIN_LO;
    }
    if (*hi > WIN_HI) {
        *hi = WIN_HI;
    }
}

/* ---------------------------------------------------------------------- */
/* the mirror: in-process mode                                            */

static void mirror_unmap(uint64_t lo, uint64_t hi)
{
    clamp_range(&lo, &hi);
    if (lo >= hi) {
        return;
    }
    bool lock = !fill_lock_held;
    if (lock) {
        pthread_mutex_lock(&fill_lock);
    }
    qatomic_inc(&mirror_gen);
    mmap((void *)lo, hi - lo, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (lock) {
        pthread_mutex_unlock(&fill_lock);
    }
}

static void drop_block_at(uint64_t va)
{
    uint64_t base, len;
    if (slabs_take_block(va, &base, &len)) {
        mirror_unmap(base, base + len);
    }
}

static void inproc_tlbi_all(void)
{
    if (!qatomic_read(&slot.used)) {
        return;
    }
    memset(slabs, 0, NSLABS * sizeof(Extent));
    mirror_unmap(WIN_LO, WIN_HI);
}

/* ---------------------------------------------------------------------- */
/* stub mode: the factory and the stub processes                          */
/* (this code runs in the forked processes; it shares nothing with QEMU   */
/*  but the StubShm mapping and the inherited knobs)                      */

typedef struct StubThread {
    StubSlot *slot;
    int vcpu;
    void *altbase;
    uint64_t host_tpidr;
    uint64_t host_x18;          /* Bionic's shadow call stack; the guest owns x18 */
    int in_guest;
    sigjmp_buf loop;
    size_t uc_off;              /* offset of the ucontext inside the frame */
    int tmpl_ok;
    unsigned char tmpl[8192] __attribute__((aligned(16)));  /* a kernel-built rt_sigframe */
} StubThread;

static StubCtl *my_ctl;
static int my_idx;
static StubThread *threads_by_vcpu[STUB_VCPUS];
static uint32_t tids_by_vcpu[STUB_VCPUS];
static uint32_t apply_owner;    /* vcpu+1 of the thread applying invalidations */

static bool send_fd(int sock, int fd)
{
    char byte = 'f';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof(u));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = u.buf, .msg_controllen = sizeof(u.buf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1;
}

static int recv_fd(int sock)
{
    char byte;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof(u));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = u.buf, .msg_controllen = sizeof(u.buf) };
    if (recvmsg(sock, &msg, 0) != 1) {
        return -1;
    }
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

/* this thread's context, by kernel thread id (no TLS: the guest's TPIDR may be live) */
static inline StubThread *stub_self(void)
{
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    for (int i = 0; i < STUB_VCPUS; i++) {
        if (tids_by_vcpu[i] == tid) {
            return threads_by_vcpu[i];
        }
    }
    return NULL;
}

static void stub_unmap(uint64_t lo, uint64_t hi)
{
    clamp_range(&lo, &hi);
    if (lo >= hi) {
        return;
    }
    if (lo == WIN_LO && hi == WIN_HI) {
        memset(slabs, 0, NSLABS * sizeof(Extent));
    } else {
        uint64_t b, l;
        if (slabs_take_block(lo, &b, &l)) {
            mmap((void *)b, l, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        }
        if (slabs_take_block(hi - 1, &b, &l)) {
            mmap((void *)b, l, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        }
    }
    mmap((void *)lo, hi - lo, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

/*
 * Apply the invalidation ranges QEMU appended to this stub's ring.  Any
 * thread may do it (an unmap is process-wide); one at a time, in order.  A
 * nested call (the signal handler interrupting the thread that holds the
 * ring) returns at once: the holder re-reads the sequence after every range.
 */
/* false: this thread already holds it (a handler nested in the holder) */
static bool window_lock(StubThread *st)
{
    uint32_t me = st->vcpu + 1;
    for (;;) {
        uint32_t o = qatomic_read(&apply_owner);
        if (o == me) {
            return false;
        }
        if (o == 0 && qatomic_cmpxchg(&apply_owner, 0, me) == 0) {
            return true;
        }
        spin_relax();
    }
}

static inline void window_unlock(void)
{
    qatomic_set(&apply_owner, 0);
}

static void apply_pending(StubThread *st)
{
    StubCtl *c = my_ctl;
    if (qatomic_read(&c->inval_applied) == qatomic_read(&c->inval_seq) &&
        !qatomic_read(&c->overflow)) {
        return;
    }
    uint64_t t0 = cntvct();
    if (!window_lock(st)) {
        return;
    }
    for (;;) {
        if (qatomic_read(&c->overflow)) {
            uint64_t s = qatomic_read(&c->inval_seq);
            qatomic_set(&c->overflow, 0);
            stub_unmap(WIN_LO, WIN_HI);
            qatomic_set(&c->inval_applied, s);
            continue;
        }
        uint64_t a = qatomic_read(&c->inval_applied), s = qatomic_read(&c->inval_seq);
        if (a >= s) {
            break;
        }
        smp_rmb();
        uint64_t lo = c->inval[(a + 1) % INVAL_RING].lo;
        uint64_t hi = c->inval[(a + 1) % INVAL_RING].hi;
        stub_unmap(lo, hi);
        qatomic_set(&c->inval_applied, a + 1);
    }
    window_unlock();
    qatomic_add(&c->apply_ticks, cntvct() - t0);
}

static inline bool inval_pending(void)
{
    return qatomic_read(&my_ctl->inval_applied) != qatomic_read(&my_ctl->inval_seq) ||
           qatomic_read(&my_ctl->overflow);
}

/* map the range QEMU described in slot->fill; false = report ST_FILLFAIL */
static bool stub_do_fill_locked(StubThread *st)
{
    StubFill f = st->slot->fill;
    int fd = f.fd_index == FD_INDEX_CC ? cc_fd :
             (f.fd_index >= 0 && f.fd_index < qatomic_read(&shm->nram))
             ? shm->ram_fd[f.fd_index] : -1;
    if (fd < 0 || !in_window(f.va) || f.va + f.len > WIN_HI) {
        return false;
    }
    void *p;
    if (f.patch) {
        p = patched_copy_install(f.va, f.len, f.prot, NULL, fd, f.off);
    } else {
        p = mmap((void *)f.va, f.len, f.prot, MAP_SHARED | MAP_FIXED, fd, f.off);
    }
    if (p == MAP_FAILED) {
        fprintf(stderr, "native-el0 stub%d: mmap %#" PRIx64 "+%#" PRIx64 ": %s\n",
                my_idx, f.va, f.len, strerror(errno));
        return false;
    }
    if (f.len >= (2 << 20)) {
        slabs_record(f.va, f.len);
    }
    if ((f.prot & PROT_EXEC) && !f.patch) {
        __builtin___clear_cache((char *)f.va, (char *)f.va + f.len);
    }
    if (qatomic_read(&my_ctl->inval_seq) != f.gen || qatomic_read(&my_ctl->overflow)) {
        /* a TLBI raced with the page walk: the mapping may be stale */
        stub_unmap(f.va, f.va + f.len);
    }
    return true;
}

static bool stub_do_fill(StubThread *st)
{
    uint64_t t0 = cntvct();
    window_lock(st);                /* never nested: the service loop calls it */
    bool ok = stub_do_fill_locked(st);
    window_unlock();
    qatomic_add(&my_ctl->fill_ticks, cntvct() - t0);
    qatomic_inc(&my_ctl->fills);
    if (st->slot->fill.patch) {
        qatomic_inc(&my_ctl->patch_fills);
    }
    return ok;
}

static void stub_report(StubThread *st, uint32_t status)
{
    StubSlot *slot = st->slot;
    smp_wmb();
    qatomic_set(&slot->status, status);
    futex(&slot->status, FUTEX_WAKE, 1, NULL);
}

/* The handlers start with the guest's TPIDR and x18 live: no stack protector
 * (its canary is read through TPIDR, from a page the mirror may have dropped). */
#define STUB_HANDLER __attribute__((no_stack_protector))

static inline void load_host_regs(StubThread *st)
{
    write_tpidr(st->host_tpidr);
    __asm__ volatile("mov x18, %0" :: "r"(st->host_x18));
}

/* a trap (or a kick) while the thread runs guest code: hand the frame to QEMU */
static STUB_HANDLER void stub_trap_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    uint64_t tp = read_tpidr();
    StubThread *st = stub_self();
    if (!st) {
        /* the main thread, or a thread we do not know: not a guest trap */
        if (sig == sig_kick) {
            return;
        }
        signal(sig, SIG_DFL);
        return;
    }
    load_host_regs(st);
    StubSlot *slot = st->slot;
    uint64_t pc = uc->uc_mcontext.pc;
    uint32_t phase_before = qatomic_read(&slot->phase);
    qatomic_set(&slot->phase, PH_TRAP);
    if ((uintptr_t)&uc < (uintptr_t)st->altbase || (uintptr_t)&uc >= (uintptr_t)st->altbase + ALT_SIZE) {
        static int n;
        if (n++ < 3) {
            fprintf(stderr, "native-el0 stub%d: handler for sig %d off the alt stack: sp=%p altbase=%p"
                    " guest sp=%#" PRIx64 " pc=%#" PRIx64 " phase=%u\n", my_idx, sig, (void *)&uc,
                    st->altbase, (uint64_t)uc->uc_mcontext.sp, pc, phase_before);
        }
    }

    /*
     * A guest branch beyond the host's user address space faults on the fetch
     * itself: no host instruction ran, so it is a guest trap.  JavaScriptCore
     * keeps its JIT code tens of TiB up (seen: pc 0x825c7610000); mirror_fill
     * finds it outside the window and that code runs under TCG.
     */
    bool high_fetch = (sig == SIGSEGV || sig == SIGBUS) &&
                      (uint64_t)(uintptr_t)si->si_addr == pc;
    bool guest = st->in_guest && (pc < GUARD_HI || high_fetch) &&
                 (sig != sig_kick || in_window(pc));
    if (!guest) {
        if (sig == sig_kick) {
            qatomic_set(&slot->phase, phase_before);
            write_tpidr(tp);
            return;             /* not in guest code: the entry path looks at kick_pending */
        }
        fprintf(stderr, "native-el0 stub%d: host signal %d code %d at pc=%#" PRIx64
                " addr=%p in_guest=%d tid=%ld vcpu=%d st=%p altbase=%p sp=%#" PRIx64
                " lr=%#" PRIx64 " x22=%#" PRIx64 " tpidr=%#" PRIx64 " host_tpidr=%#" PRIx64 "\n",
                my_idx, sig, si->si_code, pc, si->si_addr, st->in_guest,
                (long)syscall(SYS_gettid), st->vcpu, (void *)st, st->altbase,
                (uint64_t)uc->uc_mcontext.sp, (uint64_t)uc->uc_mcontext.regs[30],
                (uint64_t)uc->uc_mcontext.regs[22], tp, st->host_tpidr);
        signal(sig, SIG_DFL);           /* re-executes and dies with a tombstone */
        write_tpidr(tp);
        return;
    }
    st->in_guest = 0;
    uint32_t status;
    switch (sig) {
    case SIGSYS:  status = ST_SVC;   break;
    case SIGSEGV:
    case SIGBUS:  status = ST_FAULT; break;
    case SIGILL:  status = ST_ILL;   break;
    case SIGTRAP: status = ST_TRAP;  break;
    case SIGFPE:  status = ST_FPE;   break;
    default:
        status = ST_KICK;
        qatomic_set(&slot->kick_pending, 0);
        break;
    }
    slot->tpidr = tp;
    slot->sig = sig;
    slot->si_code = si->si_code;
    slot->fault_addr = (uint64_t)(uintptr_t)si->si_addr;
    memcpy(&slot->uc, uc, sizeof(ucontext_t));
    qatomic_set(&slot->phase, PH_REPORTED);
    stub_report(st, status);
    siglongjmp(st->loop, 1);
}

/* QEMU appended invalidations while this thread runs guest code */
static STUB_HANDLER void stub_inval_handler(int sig, siginfo_t *si, void *uc_)
{
    uint64_t tp = read_tpidr();
    StubThread *st = stub_self();
    if (!st) {
        return;                     /* not one of the vCPU threads */
    }
    load_host_regs(st);
    uint32_t before = qatomic_read(&st->slot->phase);
    qatomic_set(&st->slot->phase, PH_INVAL);
    apply_pending(st);
    qatomic_set(&st->slot->phase, before);
    write_tpidr(tp);
}

/* capture the frame the kernel builds for this thread: the entry template */
static void stub_tmpl_handler(int sig, siginfo_t *si, void *uc_)
{
    StubThread *st = stub_self();
    if (!st) {
        return;
    }
    unsigned char *base = (unsigned char *)si;
    size_t off = (unsigned char *)uc_ - base;
    if (((uintptr_t)base & 15) || off + sizeof(ucontext_t) > sizeof(st->tmpl)) {
        st->tmpl_ok = 0;
        return;
    }
    st->uc_off = off;
    memcpy(st->tmpl, base, off + sizeof(ucontext_t));
    st->tmpl_ok = 1;
}

/* Enter guest code: load the guest's thread pointer, point SP at the frame
 * and let rt_sigreturn restore everything else (registers, SP, PC, PSTATE,
 * FP/SIMD, signal mask) at once. The kernel also delivers any signal that
 * became pending while we had the kick/invalidation signals blocked. */
static void __attribute__((noreturn)) stub_enter(void *frame, uint64_t tp)
{
    register uint64_t x8 __asm__("x8") = SYS_rt_sigreturn;
    __asm__ volatile("msr tpidr_el0, %0\n\t"
                     "mov sp, %1\n\t"
                     "svc #0\n\t"
                     "brk #0"
                     :: "r"(tp), "r"(frame), "r"(x8) : "memory");
    __builtin_unreachable();
}

static uint32_t stub_wait_cmd(StubSlot *slot)
{
    for (;;) {
        uint64_t t0 = cntvct();
        while (!qatomic_read(&slot->cmd) && cntvct() - t0 < stub_spin_ticks) {
            spin_relax();
        }
        uint32_t c = qatomic_xchg(&slot->cmd, 0);
        if (c) {
            return c;
        }
        futex(&slot->cmd, FUTEX_WAIT, 0, NULL);
    }
}

static void *stub_thread_main(void *arg)
{
    StubThread *st = arg;
    StubSlot *slot = st->slot;

    if (posix_memalign(&st->altbase, ALT_SIZE, ALT_SIZE) != 0) {
        _exit(95);
    }
    *(StubThread **)st->altbase = st;
    stack_t ss = {
        .ss_sp = (char *)st->altbase + 64,
        .ss_size = ALT_SIZE - 64,
        .ss_flags = 0,
    };
    if (sigaltstack(&ss, NULL) < 0) {
        _exit(95);
    }
    sigset_t none, blockset;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    sigemptyset(&blockset);
    sigaddset(&blockset, sig_kick);
    sigaddset(&blockset, sig_inval);
    if (pac_off) {
        disable_pac_keys();
    }
    if (stub_cpuset_on) {
        sched_setaffinity(0, sizeof(stub_cpuset), &stub_cpuset);
    }
    st->host_tpidr = read_tpidr();
    __asm__ volatile("mov %0, x18" : "=r"(st->host_x18));
    threads_by_vcpu[st->vcpu] = st;
    smp_wmb();
    tids_by_vcpu[st->vcpu] = (uint32_t)syscall(SYS_gettid);
    qatomic_set(&slot->owner_pid, (int32_t)getpid());
    qatomic_set(&slot->tid, (uint32_t)syscall(SYS_gettid));
    raise(SIGUSR1);
    if (!st->tmpl_ok) {
        fprintf(stderr, "native-el0 stub%d: cannot capture a signal frame\n", my_idx);
        _exit(94);
    }
    /* QEMU fills the registers into a copy of a real frame of this thread */
    memcpy(&slot->uc, st->tmpl + st->uc_off, sizeof(ucontext_t));
    smp_wmb();
    qatomic_set(&slot->ready, 1);

    unsigned char *frame = (unsigned char *)st->altbase + STUB_FRAME_OFF;
    sigsetjmp(st->loop, 1);             /* traps land here */
    for (;;) {
        qatomic_set(&slot->phase, PH_WAIT);
        uint32_t c = stub_wait_cmd(slot);
        if (c == CMD_EXIT) {
            break;
        }
        if (c == CMD_FILL_RESUME) {
            qatomic_set(&slot->phase, PH_FILL);
            if (!stub_do_fill(st)) {
                stub_report(st, ST_FILLFAIL);
                continue;
            }
        }
        qatomic_set(&slot->phase, PH_APPLY);
        apply_pending(st);
        {
            stack_t cur;
            sigset_t m;
            if (sigaltstack(NULL, &cur) == 0 &&
                (cur.ss_sp != (char *)st->altbase + 64 || cur.ss_size != ALT_SIZE - 64 ||
                 (cur.ss_flags & SS_DISABLE))) {
                fprintf(stderr, "native-el0 stub%d: vcpu %d alt stack changed: sp=%p size=%zu flags=%#x\n",
                        my_idx, st->vcpu, cur.ss_sp, cur.ss_size, cur.ss_flags);
                stack_t ss = { .ss_sp = (char *)st->altbase + 64, .ss_size = ALT_SIZE - 64 };
                sigaltstack(&ss, NULL);
            }
            if (pthread_sigmask(SIG_BLOCK, NULL, &m) == 0 &&
                (sigismember(&m, SIGSEGV) || sigismember(&m, SIGSYS) || sigismember(&m, SIGBUS))) {
                fprintf(stderr, "native-el0 stub%d: vcpu %d has trap signals blocked\n", my_idx, st->vcpu);
                sigemptyset(&m);
                pthread_sigmask(SIG_SETMASK, &m, NULL);
            }
        }
        /*
         * Commit to entering: with the kick and invalidation signals blocked
         * until rt_sigreturn, anything that arrives from here on is delivered
         * on top of the guest context, where the handlers know what to do.
         */
        pthread_sigmask(SIG_BLOCK, &blockset, NULL);
        qatomic_set(&slot->phase, PH_PREENTRY);
        st->in_guest = 1;
        smp_mb();
        if (qatomic_read(&slot->kick_pending)) {
            st->in_guest = 0;
            qatomic_set(&slot->kick_pending, 0);
            slot->sig = sig_kick;
            pthread_sigmask(SIG_UNBLOCK, &blockset, NULL);
            qatomic_set(&slot->phase, PH_KICKREPORT);
            stub_report(st, ST_KICK);
            continue;
        }
        if (inval_pending()) {
            apply_pending(st);
        }
        memcpy(frame, st->tmpl, st->uc_off + sizeof(ucontext_t));
        memcpy(frame + st->uc_off, &slot->uc, sizeof(ucontext_t));
        {
            ucontext_t *fuc = (ucontext_t *)(frame + st->uc_off);
            fuc->uc_stack.ss_sp = (char *)st->altbase + 64;
            fuc->uc_stack.ss_size = ALT_SIZE - 64;
            fuc->uc_stack.ss_flags = 0;
            fuc->uc_link = NULL;
        }
        qatomic_set(&slot->phase, PH_GUEST);
        stub_enter(frame, slot->tpidr);
    }
    return NULL;
}

static void __attribute__((noreturn)) stub_main(int idx)
{
    my_idx = idx;
    my_ctl = &shm->ctl[idx];
    qatomic_set(&my_ctl->pid, (int32_t)getpid());
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) {
        _exit(0);
    }
    char name[16];
    snprintf(name, sizeof(name), "asoft-stub%d", idx);
    prctl(PR_SET_NAME, name);

    int traps[] = { SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, sig_kick, 0 };
    struct sigaction sa;
    for (int i = 0; traps[i]; i++) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = stub_trap_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(traps[i], &sa, NULL);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = stub_inval_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, sig_kick);
    sigaction(sig_inval, &sa, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = stub_tmpl_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    if (!install_seccomp()) {       /* threads created below inherit it */
        _exit(96);
    }
    int n = qatomic_read(&shm->nvcpus);
    for (int v = 0; v < n && v < STUB_VCPUS; v++) {
        StubThread *st = calloc(1, sizeof(*st));
        st->slot = &shm->slot[idx][v];
        st->vcpu = v;
        pthread_t th;
        if (pthread_create(&th, NULL, stub_thread_main, st) != 0) {
            _exit(93);
        }
    }
    qatomic_set(&my_ctl->state, 1);
    futex(&my_ctl->state, FUTEX_WAKE, 1, NULL);
    for (;;) {
        pause();
    }
}

static void __attribute__((noreturn)) factory_main(int sock)
{
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) {
        _exit(0);
    }
    prctl(PR_SET_NAME, "asoft-stubfac");
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (;;) {
        int wst;
        pid_t dead;
        while ((dead = waitpid(-1, &wst, WNOHANG)) > 0) {
            for (int i = 0; i < STUB_MAX; i++) {
                if (qatomic_read(&shm->ctl[i].pid) == dead) {
                    qatomic_set(&shm->ctl[i].state, 2);
                    if (WIFSIGNALED(wst)) {
                        fprintf(stderr, "native-el0: stub %d (pid %d) killed by signal %d%s\n",
                                i, dead, WTERMSIG(wst), WCOREDUMP(wst) ? " (core)" : "");
                    } else {
                        fprintf(stderr, "native-el0: stub %d (pid %d) exited with %d\n",
                                i, dead, WEXITSTATUS(wst));
                    }
                }
            }
        }
        uint32_t c = qatomic_read(&shm->fac_cmd);
        if (c == FAC_NONE) {
            struct timespec ts = { 0, 500 * 1000000 };
            futex(&shm->fac_cmd, FUTEX_WAIT, FAC_NONE, &ts);
            continue;
        }
        if (c == FAC_RECV_FD) {
            int fd = recv_fd(sock);
            int n = qatomic_read(&shm->nram);
            if (fd >= 0 && n < (int)ARRAY_SIZE(shm->ram_fd)) {
                shm->ram_fd[n] = fd;
                smp_wmb();
                qatomic_set(&shm->nram, n + 1);
            }
        } else if (c == FAC_SPAWN) {
            int idx = qatomic_read(&shm->fac_arg);
            pid_t p = fork();
            if (p == 0) {
                close(sock);
                stub_main(idx);
            }
            if (p < 0) {
                qatomic_set(&shm->ctl[idx].pid, -1);
            }
        }
        qatomic_set(&shm->fac_cmd, FAC_NONE);
        qatomic_set(&shm->fac_done, 1);
        futex(&shm->fac_done, FUTEX_WAKE, 1, NULL);
    }
}

/* QEMU, constructor time: the shared block, the fd socket, the factory */
static bool stub_shm_init(void)
{
    size_t sz = (sizeof(StubShm) + 0xfff) & ~(size_t)0xfff;
    shm = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        shm = NULL;
        return false;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
        return false;
    }
    for (int i = 0; i < 65536; i++) {
        asid_stub[i] = -1;
    }
    /* before the fork: the factory and so the stubs inherit the fd */
    if (tpidrro_mode && !(getenv("ASOFT_NATIVE_EL0_CODECACHE") && *getenv("ASOFT_NATIVE_EL0_CODECACHE") == '0') &&
        !cc_create()) {
        fprintf(stderr, "native-el0: no code cache: %s\n", strerror(errno));
        cc_base = NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        close(sv[0]);
        factory_main(sv[1]);
    }
    close(sv[1]);
    fac_sock = sv[0];
    factory_pid = pid;
    return true;
}

/* ---------------------------------------------------------------------- */
/* stub mode: the QEMU side                                               */

/* ask the factory for something; the caller holds pool_lock */
static bool fac_call(uint32_t cmd, uint32_t arg)
{
    qatomic_set(&shm->fac_arg, arg);
    qatomic_set(&shm->fac_done, 0);
    smp_mb();
    qatomic_set(&shm->fac_cmd, cmd);
    futex(&shm->fac_cmd, FUTEX_WAKE, 1, NULL);
    int64_t t0 = mono_ns();
    while (!qatomic_read(&shm->fac_done)) {
        struct timespec ts = { 0, 100 * 1000000 };
        futex(&shm->fac_done, FUTEX_WAIT, 0, &ts);
        if (mono_ns() - t0 > 5000000000LL) {
            fprintf(stderr, "native-el0: the stub factory does not answer\n");
            return false;
        }
    }
    return true;
}

/* ctl.pid: 0 never spawned, -2 spawning, -1 failed for good, > 0 the process */
static bool stub_spawn(int s)
{
    qatomic_set(&shm->ctl[s].pid, -2);
    if (!qatomic_read(&shm->nvcpus)) {
        int n = 0;
        CPUState *cs;
        CPU_FOREACH(cs) {
            n++;
        }
        qatomic_set(&shm->nvcpus, n > STUB_VCPUS ? STUB_VCPUS : n);
    }
    if (!fac_call(FAC_SPAWN, s)) {
        qatomic_set(&shm->ctl[s].pid, -1);
        return false;
    }
    int64_t t0 = mono_ns();
    while (qatomic_read(&shm->ctl[s].state) != 1) {
        struct timespec ts = { 0, 100 * 1000000 };
        futex(&shm->ctl[s].state, FUTEX_WAIT, 0, &ts);
        if (qatomic_read(&shm->ctl[s].state) == 2 || mono_ns() - t0 > 5000000000LL) {
            fprintf(stderr, "native-el0: stub %d did not come up\n", s);
            qatomic_set(&shm->ctl[s].pid, -1);
            return false;
        }
    }
    if (qatomic_read(&shm->ctl[s].pid) <= 0) {
        fprintf(stderr, "native-el0: stub %d is up without a pid\n", s);
        qatomic_set(&shm->ctl[s].pid, -1);
        return false;
    }
    stub_spawns++;
    return true;
}

/* the index of a guest RAM fd in the table shared with the stubs */
static int ram_fd_index(int fd)
{
    for (int i = 0; i < nram_q; i++) {
        if (ram_fd_q[i] == fd) {
            return i;
        }
    }
    return -1;
}

/*
 * Before the first stub exists, the factory must hold the guest RAM fd so
 * that the stubs inherit it: find it behind the page we are about to run.
 */
static bool stub_ram_ready(CPUARMState *env)
{
    if (nram_q) {
        return true;
    }
    GetPhysAddrResult res;
    ARMMMUFaultInfo fi;
    memset(&res, 0, sizeof(res));
    memset(&fi, 0, sizeof(fi));
    if (!get_phys_addr(env, env->pc, MMU_INST_FETCH, 0, ARMMMUIdx_E10_0, &res, &fi)) {
        return false;
    }
    MemoryRegionSection sec = memory_region_find(get_system_memory(), res.f.phys_addr, 4);
    int fd = (sec.mr && memory_region_is_ram(sec.mr)) ? memory_region_get_fd(sec.mr) : -1;
    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    if (fd < 0) {
        return false;
    }
    pthread_mutex_lock(&pool_lock);
    if (!nram_q) {
        if (send_fd(fac_sock, fd) && fac_call(FAC_RECV_FD, 0) && qatomic_read(&shm->nram) == 1) {
            ram_fd_q[0] = fd;
            nram_q = 1;
        } else {
            fprintf(stderr, "native-el0: cannot hand the RAM fd to the stub factory\n");
        }
    }
    pthread_mutex_unlock(&pool_lock);
    return nram_q > 0;
}

/*
 * Append an invalidation range to a stub's ring.  With @sync, threads that
 * run guest code in that stub right now are told by signal and we wait until
 * the range is applied (or until nobody runs in it any more: the next entry
 * applies it first).  The caller holds inval_lock.
 */
static void inval_append(int s, uint64_t lo, uint64_t hi, bool sync)
{
    StubCtl *c = &shm->ctl[s];
    uint64_t seq = qatomic_read(&c->inval_seq);
    uint64_t app = qatomic_read(&c->inval_applied);
    if (seq - app >= INVAL_RING - 2) {
        qatomic_set(&c->overflow, 1);       /* the stub flushes everything */
    }
    c->inval[(seq + 1) % INVAL_RING].lo = lo;
    c->inval[(seq + 1) % INVAL_RING].hi = hi;
    smp_wmb();
    qatomic_set(&c->inval_seq, seq + 1);
    smp_mb();
    if (!sync) {
        return;
    }
    int n = qatomic_read(&shm->nvcpus);
    bool running = false;
    for (int v = 0; v < n; v++) {
        StubSlot *sl = &shm->slot[s][v];
        if (qatomic_read(&sl->status) == ST_RUNNING) {
            running = true;
            if (qatomic_read(&sl->owner_pid) != c->pid) {
                static int n;
                if (n++ < 5) {
                    fprintf(stderr, "native-el0: slot %d/%d owned by pid %d, stub pid %d\n",
                            s, v, qatomic_read(&sl->owner_pid), c->pid);
                }
            } else if (syscall(SYS_tgkill, c->pid, qatomic_read(&sl->tid), sig_inval) < 0) {
                static int n;
                if (n++ < 5) {
                    fprintf(stderr, "native-el0: tgkill stub %d (pid %d tid %u): %s\n",
                            s, c->pid, qatomic_read(&sl->tid), strerror(errno));
                }
            }
        }
    }
    if (!running) {
        if (lazy_inval) {
            return;
        }
        /* nobody runs in it: an idle thread applies the range from its handler */
        for (int v = 0; v < n; v++) {
            StubSlot *sl = &shm->slot[s][v];
            uint32_t tid = qatomic_read(&sl->tid);
            if (qatomic_read(&sl->ready) && qatomic_read(&sl->owner_pid) == c->pid && tid &&
                syscall(SYS_tgkill, c->pid, tid, sig_inval) == 0) {
                running = true;         /* somebody will apply it */
                break;
            }
        }
        if (!running) {
            return;
        }
    }
    inval_waits++;
    int64_t t0 = mono_ns();
    int spins = 0;
    bool slow = false;
    while (qatomic_read(&c->inval_applied) < seq + 1) {
        if (lazy_inval) {
            bool any = false;
            for (int v = 0; v < n; v++) {
                if (qatomic_read(&shm->slot[s][v].status) == ST_RUNNING) {
                    any = true;
                }
            }
            if (!any) {
                return;                 /* trapped meanwhile: applied before re-entry */
            }
        }
        int64_t waited = mono_ns() - t0;
        if (waited > 50 * 1000000LL && !slow) {
            slow = true;
            inval_slow++;
        }
        if (waited > 5000 * 1000000LL) {
            if (inval_timeouts++ < 5) {
                fprintf(stderr, "native-el0: stub %d did not apply a TLB invalidation in 5 s\n", s);
            }
            return;
        }
        if (++spins > 200) {
            sched_yield();
        } else {
            spin_relax();
        }
    }
}

static void stub_kick_slot(int s, StubSlot *sl)
{
    qatomic_set(&sl->kick_pending, 1);
    smp_mb();
    if (qatomic_read(&sl->status) == ST_RUNNING) {
        uint32_t tid = qatomic_read(&sl->tid);
        if (tid) {
            syscall(SYS_tgkill, shm->ctl[s].pid, tid, sig_kick);
        }
    }
}

/*
 * Heat (ASOFT_NATIVE_EL0_EVICT=heat): per ASID, the EL0 time of the last
 * seconds, natively or in TCG for want of a stub (CNTVCT ticks, halving
 * every second).  Without a free stub the coldest idle one is evicted; with
 * heat-admit only for an address space at least as hot, else this slice
 * stays in TCG.  Under pool_lock.
 */
typedef struct { uint64_t v; int64_t t; } Heat;
static Heat asid_heat[65536];
#define HEAT_HALF_NS 1000000000LL

static uint64_t heat_get(Heat *h, int64_t now)
{
    int64_t halves = (now - h->t) / HEAT_HALF_NS;
    if (halves > 0) {
        h->v = halves >= 63 ? 0 : h->v >> halves;
        h->t += halves * HEAT_HALF_NS;      /* keep the fraction of a half */
    }
    return h->v;
}

static void heat_add(uint16_t asid, uint64_t ticks, int64_t now)
{
    Heat *h = &asid_heat[asid];
    if (!h->t) {
        h->t = now;
    }
    heat_get(h, now);
    h->v += ticks;
}

/* Bind a stub to @asid (or find the bound one) and count this vCPU in. */
static int stub_acquire(uint16_t asid)
{
    int64_t now = mono_ns();
    pthread_mutex_lock(&pool_lock);
    int s = asid_stub[asid];
    if (s < 0) {
        int victim = -1;
        int64_t oldest = INT64_MAX;
        uint64_t coldest = UINT64_MAX;
        for (int i = 0; i < nstubs; i++) {
            if (qatomic_read(&shm->ctl[i].pid) < 0 || qatomic_read(&shm->ctl[i].state) == 2) {
                continue;               /* failed to spawn, or died */
            }
            if (!sbind[i].bound) {
                s = i;
                break;
            }
            if (sbind[i].running != 0 || now - sbind[i].last_ns < steal_ns) {
                continue;
            }
            if (evict_heat) {
                uint64_t h = heat_get(&asid_heat[sbind[i].asid], now);
                if (h < coldest) {
                    coldest = h;
                    victim = i;
                }
            } else if (sbind[i].last_ns < oldest) {
                oldest = sbind[i].last_ns;
                victim = i;
            }
        }
        if (s < 0 && victim >= 0 && evict_admit &&
            heat_get(&asid_heat[asid], now) < coldest) {
            victim = -1;                /* everybody bound is hotter: TCG this time */
        }
        if (s < 0 && victim >= 0) {
            /* the least recently used idle address space gives up its stub */
            s = victim;
            asid_stub[sbind[s].asid] = -1;
            sbind[s].bound = false;
            pthread_mutex_lock(&inval_lock);
            inval_append(s, WIN_LO, WIN_HI, false);
            pthread_mutex_unlock(&inval_lock);
            stub_evictions++;
        }
        if (s < 0) {
            pthread_mutex_unlock(&pool_lock);
            return -1;
        }
        if (qatomic_read(&shm->ctl[s].pid) == 0 && !stub_spawn(s)) {
            pthread_mutex_unlock(&pool_lock);
            return -1;
        }
        sbind[s].asid = asid;
        sbind[s].bound = true;
        asid_stub[asid] = s;
    }
    sbind[s].running++;
    sbind[s].last_ns = now;
    pthread_mutex_unlock(&pool_lock);
    return s;
}

static void stub_release(NativeCtx *nc)
{
    int s = nc->stub;
    if (s < 0) {
        return;
    }
    pthread_mutex_lock(&pool_lock);
    sbind[s].running--;
    sbind[s].last_ns = mono_ns();
    if (evict_heat) {
        heat_add(sbind[s].asid, nc->native_ticks - nc->native_mark, sbind[s].last_ns);
    }
    pthread_mutex_unlock(&pool_lock);
    nc->stub = -1;
    nc->slot = NULL;
}

/* ---------------------------------------------------------------------- */
/* TLB maintenance hooks                                                  */

void native_el0_tlbi_all(void)
{
    if (!native_el0_enabled) {
        return;
    }
    if (!stub_mode) {
        inproc_tlbi_all();
        return;
    }
    tlbi_kind[TL_ALL]++;
    pthread_mutex_lock(&inval_lock);
    for (int s = 0; s < nstubs; s++) {
        if (sbind[s].bound) {
            inval_append(s, WIN_LO, WIN_HI, true);
        }
    }
    pthread_mutex_unlock(&inval_lock);
}

void native_el0_tlbi_asid(uint16_t asid)
{
    if (!native_el0_enabled) {
        return;
    }
    if (!stub_mode) {
        if (qatomic_read(&slot.used) && asid == slot.asid) {
            inproc_tlbi_all();
            qatomic_set(&slot.last_ns, 0);  /* idle: the next address space may steal it */
        }
        return;
    }
    tlbi_kind[TL_ASID]++;
    pthread_mutex_lock(&pool_lock);
    asid_last_root[asid] = 0;
    int s = asid_stub[asid];
    if (s >= 0) {
        pthread_mutex_lock(&inval_lock);
        inval_append(s, WIN_LO, WIN_HI, true);
        pthread_mutex_unlock(&inval_lock);
        if (sbind[s].running == 0) {
            /* the ASID is being recycled: the stub returns to the pool */
            asid_stub[asid] = -1;
            sbind[s].bound = false;
        } else {
            sbind[s].last_ns = 0;
        }
    }
    pthread_mutex_unlock(&pool_lock);
}

static void stub_tlbi_range(uint64_t lo, uint64_t hi, int asid)
{
    if (tlbi_all_stubs) {
        asid = -1;
    }
    smp_mb();       /* the guest's PTE store is seen by a binder we miss below */
    pthread_mutex_lock(&inval_lock);
    int s = (asid >= 0 && asid < 65536) ? asid_stub[asid] : -1;
    if (asid >= 0) {
        if (s >= 0) {
            tlbi_asid_hits++;
        } else {
            tlbi_asid_misses++;
        }
    }
    if (s >= 0) {
        inval_append(s, lo, hi, true);
    } else if (asid < 0) {
        for (s = 0; s < nstubs; s++) {
            if (sbind[s].bound) {
                inval_append(s, lo, hi, true);
            }
        }
    }
    pthread_mutex_unlock(&inval_lock);
}

void native_el0_tlbi_page(uint64_t va, int asid)
{
    if (!native_el0_enabled || !in_window(va)) {
        return;
    }
    if (tlbi_log_left > 0 && current_cpu && asid >= 0 &&
        asid != (int)extract64(cpu_env(current_cpu)->cp15.ttbr0_el[1], 48, 16)) {
        CPUARMState *env = cpu_env(current_cpu);
        tlbi_log_left--;
        fprintf(stderr, "native-el0: tlbi page va=%#" PRIx64 " asid=%d | vcpu%d ttbr0.asid=%u ttbr1.asid=%u"
                " tcr.a1=%d cur_asid=%u bound=%s\n", va, asid, current_cpu->cpu_index,
                (unsigned)extract64(env->cp15.ttbr0_el[1], 48, 16),
                (unsigned)extract64(env->cp15.ttbr1_el[1], 48, 16),
                (int)((env->cp15.tcr_el[1] >> 22) & 1), cur_asid(env),
                (stub_mode && asid >= 0 && asid < 65536 && asid_stub[asid] >= 0) ? "yes" : "no");
    }
    uint64_t lo = va & ~0x3fffULL;
    if (stub_mode) {
        tlbi_kind[asid >= 0 ? TL_PAGE_ASID : TL_PAGE_ANY]++;
        stub_tlbi_range(lo, lo + 0x4000, asid);
        return;
    }
    if (!qatomic_read(&slot.used)) {
        return;
    }
    drop_block_at(va);
    mirror_unmap(lo, lo + 0x4000);
}

void native_el0_tlbi_range(uint64_t base, uint64_t len, int asid)
{
    if (!native_el0_enabled || base + len <= WIN_LO || base >= WIN_HI) {
        return;
    }
    if (stub_mode) {
        tlbi_kind[TL_RANGE]++;
        stub_tlbi_range(base, base + len, asid);
        return;
    }
    if (!qatomic_read(&slot.used)) {
        return;
    }
    drop_block_at(base);
    drop_block_at(base + len - 1);
    mirror_unmap(base, base + len);
}

/* ---------------------------------------------------------------------- */
/* filling the mirror                                                     */

enum { FILL_OK, FILL_FAULT, FILL_UNSUPPORTED };

/*
 * Map the guest page containing @va into the mirror for @access (in stub
 * mode: describe the mapping in the slot for the stub to make).  Returns
 * FILL_FAULT (and *fi) when the guest's own translation faults, so the
 * caller injects that fault, FILL_UNSUPPORTED when the page cannot be
 * mirrored (not RAM, not fd-backed, outside the window), so the caller
 * falls back to TCG.
 */
static int mirror_fill(NativeCtx *nc, uint64_t va, MMUAccessType access,
                       ARMMMUFaultInfo *fi)
{
    CPUARMState *env = cpu_env(nc->cs);
    GetPhysAddrResult res;
    int tries = 0;

    if (!stub_mode && (!qatomic_read(&slot.used) || slot.asid != cur_asid(env))) {
        return FILL_UNSUPPORTED;      /* the mirror belongs to someone else now */
    }
    if (stub_mode && (nc->stub < 0 || sbind[nc->stub].asid != cur_asid(env))) {
        return FILL_UNSUPPORTED;
    }

retry:
    memset(&res, 0, sizeof(res));
    memset(fi, 0, sizeof(*fi));
    uint64_t gen = stub_mode ? qatomic_read(&shm->ctl[nc->stub].inval_seq)
                             : qatomic_read(&mirror_gen);
    smp_mb();       /* the walk below must not see a PTE older than @gen */

    if (!get_phys_addr(env, va, access, 0, ARMMMUIdx_E10_0, &res, fi)) {   /* true = translated */
        return FILL_FAULT;
    }
    unsigned lg = res.f.lg_page_size;
    if (lg < 12) {
        lg = 12;
    }
    uint64_t psize = 1ULL << lg;
    uint64_t pg = va & ~(psize - 1);
    uint64_t len = psize;
    if (len > MAX_FILL) {
        len = MAX_FILL;
        pg = va & ~(len - 1);
    }
    if (pg < WIN_LO || pg + len > WIN_HI) {
        return FILL_UNSUPPORTED;
    }
    int prot = res.f.prot;
    bool patch = tpidrro_mode && (prot & PAGE_EXEC) && !(prot & PAGE_WRITE);
    /* XO: a data read must not catch a whole block; the code cache works in pages */
    uint64_t patch_max = (patch_xo || cc_base) ? CC_PAGE : (2 << 20);
    if (patch && len > patch_max) {
        len = patch_max;
        pg = va & ~(len - 1);
    }
    hwaddr gpa = (res.f.phys_addr & ~(psize - 1)) + (pg - (va & ~(psize - 1)));

    MemoryRegionSection sec = memory_region_find(get_system_memory(), gpa, len);
    if (!sec.mr) {
        return FILL_UNSUPPORTED;
    }
    bool ok = memory_region_is_ram(sec.mr) &&
              int128_ge(sec.size, int128_make64(len)) &&
              sec.offset_within_address_space == gpa;
    int fd = ok ? memory_region_get_fd(sec.mr) : -1;
    uint64_t off = 0;
    ram_addr_t ra = 0;
    char *hp = NULL;
    if (fd >= 0) {
        off = sec.offset_within_region + qemu_ram_get_fd_offset(sec.mr->ram_block);
        ra = memory_region_get_ram_addr(sec.mr) + sec.offset_within_region;
        hp = (char *)memory_region_get_ram_ptr(sec.mr) + sec.offset_within_region;
    }
    memory_region_unref(sec.mr);
    if (fd < 0) {
        return FILL_UNSUPPORTED;
    }

    int hprot = ((prot & PAGE_READ) ? PROT_READ : 0) |
                ((prot & PAGE_WRITE) ? PROT_WRITE : 0) |
                ((prot & PAGE_EXEC) ? PROT_EXEC : 0);
    if (prot & PAGE_WRITE) {
        /* translated code for this page would not see native stores */
        tb_invalidate_phys_range(nc->cs, ra, ra + len - 1);
    }

    if (stub_mode) {
        int fdi = ram_fd_index(fd);
        if (fdi < 0) {
            return FILL_UNSUPPORTED;
        }
        if (patch && cc_base && !patch_xo && fdi == 0 && len == CC_PAGE) {
            uint64_t slot_off;
            switch (cc_fill(hp, off, &slot_off)) {
            case CC_CLEAN:
                patch = false;              /* nothing to rewrite: the frame itself */
                break;
            case CC_CACHED:
                patch = false;
                fdi = FD_INDEX_CC;
                off = slot_off;
                break;
            default:
                break;                      /* a private copy, as without the cache */
            }
        }
        StubFill *f = &nc->slot->fill;
        f->va = pg;
        f->len = len;
        f->off = off;
        f->gen = gen;
        f->prot = hprot;
        f->fd_index = fdi;
        f->patch = patch;
        nc->fill_pending = 1;
        nc->fills++;
        return FILL_OK;
    }

    pthread_mutex_lock(&fill_lock);
    void *p;
    if (patch) {
        p = patched_copy_install(pg, len, hprot, hp, -1, 0);
    } else {
        p = mmap((void *)pg, len, hprot, MAP_SHARED | MAP_FIXED, fd, off);
    }
    if (p == MAP_FAILED) {
        pthread_mutex_unlock(&fill_lock);
        fprintf(stderr, "native-el0: mmap %#" PRIx64 "+%#" PRIx64 ": %s\n",
                pg, len, strerror(errno));
        return FILL_UNSUPPORTED;
    }
    if (len >= (2 << 20)) {
        slabs_record(pg, len);
    }
    bool stale = qatomic_read(&mirror_gen) != gen;
    if (!stale && (prot & PAGE_EXEC) && !patch) {
        __builtin___clear_cache((char *)pg, (char *)pg + len);
    }
    if (stale) {
        /* a TLBI raced with the walk: the mapping may be stale, redo it */
        fill_lock_held = 1;
        mirror_unmap(pg, pg + len);
        fill_lock_held = 0;
    }
    pthread_mutex_unlock(&fill_lock);
    if (stale) {
        if (++tries < 8) {
            goto retry;
        }
        return FILL_UNSUPPORTED;
    }
    nc->fills++;
    return FILL_OK;
}

/* ---------------------------------------------------------------------- */
/* register state <-> signal frame                                        */

static struct fpsimd_context *find_fpsimd(ucontext_t *uc)
{
    unsigned char *p = uc->uc_mcontext.__reserved;
    unsigned char *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (h->magic == 0 || h->size == 0) {
            break;
        }
        if (h->magic == FPSIMD_MAGIC) {
            return (struct fpsimd_context *)p;
        }
        p += h->size;
    }
    return NULL;
}

static struct esr_context *find_esr(ucontext_t *uc)
{
    unsigned char *p = uc->uc_mcontext.__reserved;
    unsigned char *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (h->magic == 0 || h->size == 0) {
            break;
        }
        if (h->magic == ESR_MAGIC) {
            return (struct esr_context *)p;
        }
        p += h->size;
    }
    return NULL;
}

#define PSTATE_COPY (PSTATE_NZCV | PSTATE_DIT | PSTATE_SSBS)

static void state_to_frame(NativeCtx *nc, ucontext_t *uc)
{
    CPUARMState *env = cpu_env(nc->cs);
    for (int i = 0; i < 31; i++) {
        uc->uc_mcontext.regs[i] = env->xregs[i];
    }
    uc->uc_mcontext.sp = env->xregs[31];
    uc->uc_mcontext.pc = env->pc;
    uc->uc_mcontext.pstate = pstate_read(env) & PSTATE_COPY;   /* EL0t */
    struct fpsimd_context *f = find_fpsimd(uc);
    if (f) {
        for (int i = 0; i < 32; i++) {
            memcpy(&f->vregs[i], &env->vfp.zregs[i].d[0], 16);
        }
        f->fpsr = vfp_get_fpsr(env);
        f->fpcr = vfp_get_fpcr(env);
    }
    int sigs[] = { SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, sig_kick,
                   sig_enter, sig_inval };
    for (unsigned i = 0; i < ARRAY_SIZE(sigs); i++) {
        sigdelset(&uc->uc_sigmask, sigs[i]);
    }
    nc->guest_tpidr = tpidrro_mode ? env->cp15.tpidrro_el[0]
                                   : env->cp15.tpidr_el[0];
}

static void frame_to_state(NativeCtx *nc, ucontext_t *uc, uint64_t guest_tpidr)
{
    CPUARMState *env = cpu_env(nc->cs);
    for (int i = 0; i < 31; i++) {
        env->xregs[i] = uc->uc_mcontext.regs[i];
    }
    env->xregs[31] = uc->uc_mcontext.sp;
    env->pc = uc->uc_mcontext.pc;
    pstate_write(env, (pstate_read(env) & ~PSTATE_COPY) |
                      (uc->uc_mcontext.pstate & PSTATE_COPY));
    struct fpsimd_context *f = find_fpsimd(uc);
    if (f) {
        for (int i = 0; i < 32; i++) {
            memcpy(&env->vfp.zregs[i].d[0], &f->vregs[i], 16);
        }
        vfp_set_fpsr(env, f->fpsr);
        if (f->fpcr != vfp_get_fpcr(env)) {
            vfp_set_fpcr(env, f->fpcr);
            arm_rebuild_hflags(env);
        }
    }
    if (!tpidrro_mode) {
        env->cp15.tpidr_el[0] = guest_tpidr;
    }
}

/* ---------------------------------------------------------------------- */
/* the trap decision (both modes)                                         */

static __thread NativeCtx *cur_nc;

/* an instruction word of the guest: from the mirror (in-process, guarded
 * against a page another vCPU just dropped) or through the guest's page
 * tables (stub mode: the mirror is in another process) */
static uint32_t fetch_insn(NativeCtx *nc, uint64_t pc)
{
    uint32_t insn = 0;
    if (stub_mode) {
        if (cpu_memory_rw_debug(nc->cs, pc, &insn, 4, false) != 0) {
            insn = 0;
        }
        return insn;
    }
    if (in_window(pc) && nc) {
        if (sigsetjmp(nc->guard, 0) == 0) {
            nc->guard_active = 1;
            memcpy(&insn, (void *)pc, 4);
        }
        nc->guard_active = 0;
    }
    return insn;
}

/* back to TCG: this vCPU no longer runs natively */
static inline void leave_native(NativeCtx *nc)
{
    if (nc->trap_tick) {
        nc->handler_ticks += cntvct() - nc->trap_tick;
        nc->trap_tick = 0;
    }
    qatomic_set(&nc->in_guest, 0);
    if (stub_mode) {
        stub_release(nc);
    } else {
        qatomic_dec(&slot.active);
    }
}

/*
 * The trap is handled and the guest goes on natively.  In-process: load the
 * guest's thread pointer, the caller returns into the signal frame.  Stub
 * mode: the caller posts the resume command.  Unless somebody kicked this
 * vCPU meanwhile: then back to cpu_exec instead.
 */
static void resume_guest(NativeCtx *nc, ucontext_t *uc, uint64_t tp)
{
    qatomic_set(&nc->in_guest, 1);
    smp_mb();
    if (qatomic_read(&nc->cs->exit_request) || qatomic_read(&nc->kick_pending)) {
        qatomic_set(&nc->kick_pending, 0);
        frame_to_state(nc, uc, tp);
        nc->kicks++;
        leave_native(nc);
        cpu_loop_exit(nc->cs);
    }
    uint64_t now = cntvct();
    nc->handler_ticks += now - nc->trap_tick;
    nc->trap_tick = 0;
    nc->enter_tick = now;
    if (!stub_mode) {
        write_tpidr(nc->guest_tpidr);
    }
}

static void nat_trace(NativeCtx *nc, const char *what, uint64_t pc, uint64_t a)
{
    if (trace_left > 0) {
        trace_left--;
        fprintf(stderr, "native-el0[%d] %" PRId64 "ms: %s pc=%#" PRIx64 " a=%#" PRIx64 "\n",
                nc->cs->cpu_index, (mono_ns() - trace_t0) / 1000000, what, pc, a);
    }
}

/* MRS of a register the host does not let EL0 read: emulate the few the
 * guest uses, else let the guest see UNDEF. Returns true when handled. */
static bool emulate_mrs(NativeCtx *nc, ucontext_t *uc, uint32_t insn)
{
    if ((insn & 0xfff00000) != 0xd5300000) {
        return false;
    }
    CPUARMState *env = cpu_env(nc->cs);
    unsigned rt = insn & 31;
    unsigned op1 = (insn >> 16) & 7, crn = (insn >> 12) & 15,
             crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
    uint64_t v;
    if (op1 == 3 && crn == 13 && crm == 0 && op2 == 3) {        /* TPIDRRO_EL0 */
        v = env->cp15.tpidrro_el[0];
    } else if (op1 == 3 && crn == 14 && crm == 0 && (op2 == 1 || op2 == 2)) {
        v = gt_get_countervalue(env);                       /* CNTPCT/CNTVCT */
    } else if (op1 == 3 && crn == 14 && (crm == 2 || crm == 3) && op2 == 2) {
        v = gt_get_countervalue(env);   /* rewritten CNT[PV]CT: CVAL encodings */
    } else if (op1 == 4 && crn == 15 && crm == 10 && (op2 == 5 || op2 == 6)) {
        v = gt_get_countervalue(env);   /* Apple ACNTPCT_EL0 / ACNTVCT_EL0 */
    } else {
        return false;
    }
    if (rt != 31) {
        uc->uc_mcontext.regs[rt] = v;
    }
    uc->uc_mcontext.pc += 4;
    nc->emul_mrs++;
    return true;
}

/*
 * ASOFT_NATIVE_EL0_PATCH_XO=N: a load hit a rewritten code page, mapped
 * execute-only.  Returns true when the guest may read it: TCG then runs this
 * slice on the real bytes.  Counts the reads that really touch a rewritten
 * word (an earlier blind rewrite would have changed the guest's data) and
 * describes the first N pages.
 */
static bool xo_data_read(NativeCtx *nc, uint64_t pc, uint64_t addr)
{
    CPUARMState *env = cpu_env(nc->cs);
    GetPhysAddrResult res;
    ARMMMUFaultInfo fi;
    memset(&res, 0, sizeof(res));
    memset(&fi, 0, sizeof(fi));
    if (!get_phys_addr(env, addr, MMU_DATA_LOAD, 0, ARMMMUIdx_E10_0, &res, &fi)) {
        return false;                   /* the guest's own mapping forbids it */
    }
    qatomic_inc(&xo_reads);
    uint32_t w[16];
    uint64_t base = addr & ~3ULL;
    bool hit = false;
    if (cpu_memory_rw_debug(nc->cs, base, w, sizeof(w), false) == 0) {
        for (unsigned i = 0; i < ARRAY_SIZE(w); i++) {
            hit |= is_patch_word(w[i]);
        }
        if (hit) {
            qatomic_inc(&xo_hits);
        }
    }
    if (hit && qatomic_read(&xo_log_left) > 0 && qatomic_fetch_dec(&xo_log_left) > 0) {
        fprintf(stderr, "native-el0: xo hit pc=%#" PRIx64 " insn=%#x addr=%#" PRIx64 " words", pc,
                fetch_insn(nc, pc), addr);
        for (unsigned i = 0; i < 8; i++) {
            fprintf(stderr, " %08x", w[i]);
        }
        fprintf(stderr, "\n");
    }
    return true;
}

/*
 * Decide what a trap means.  Returns when the guest resumes natively (the
 * frame in @uc is the state to resume with; a fill may be queued); does not
 * return when the guest takes an exception or the vCPU goes back to TCG
 * (raise_exception / cpu_loop_exit longjmp into cpu_exec).
 */
static void handle_trap(NativeCtx *nc, int sig, int si_code, uint64_t fault_addr,
                        ucontext_t *uc, uint64_t tp)
{
    CPUState *cs = nc->cs;
    CPUARMState *env = cpu_env(cs);
    uint64_t pc = uc->uc_mcontext.pc;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        uint64_t addr = fault_addr;
        struct esr_context *ec = find_esr(uc);
        MMUAccessType access = MMU_DATA_LOAD;
        bool align = (sig == SIGBUS && si_code == BUS_ADRALN);
        if (ec) {
            unsigned ecls = (ec->esr >> 26) & 0x3f;
            if (ecls == 0x20 || ecls == 0x21) {
                access = MMU_INST_FETCH;
            } else if ((ec->esr & (1 << 6)) && !(ec->esr & (1 << 8))) {
                /*
                 * WnR, unless CM: cache maintenance reports WnR=1 but needs
                 * read access only, and JavaScriptCore cleans its r-x JIT
                 * pages (dc cvau) after writing them through another mapping.
                 */
                access = MMU_DATA_STORE;
            }
        } else if (addr == pc) {
            access = MMU_INST_FETCH;
        }
        if (patch_xo && ec && access == MMU_DATA_LOAD && (ec->esr & 0x3c) == 0x0c &&
            xo_data_read(nc, pc, addr)) {
            frame_to_state(nc, uc, tp);
            nat_trace(nc, "xo-read", pc, addr);
            nc->tcg_until_el1 = 1;
            leave_native(nc), cpu_loop_exit(cs);
        }
        if (!align) {
            ARMMMUFaultInfo fi;
            int r = mirror_fill(nc, addr, access, &fi);
            if (r == FILL_OK) {
                nat_trace(nc, "fill", pc, addr);
                resume_guest(nc, uc, tp);
                return;
            }
            frame_to_state(nc, uc, tp);
            if (r == FILL_FAULT && perm_fallback && fi.type == ARMFault_Permission &&
                access == MMU_DATA_STORE) {
                nat_trace(nc, "perm->tcg", pc, addr);
                nc->unsupported++;
                nc->tcg_until_el1 = 1;
                leave_native(nc), cpu_loop_exit(cs);
            }
            if (r == FILL_UNSUPPORTED) {
                nat_trace(nc, "unsupported", pc, addr);
                nc->unsupported++;
                nc->tcg_until_el1 = 1;
                leave_native(nc), cpu_loop_exit(cs);
            }
            if (trace_left > 0) {
                trace_left--;
                fprintf(stderr, "native-el0[%d]: inject type=%d level=%d access=%d"
                        " pc=%#" PRIx64 " addr=%#" PRIx64 " insn=%#x esr=%#" PRIx64 "\n",
                        cs->cpu_index, fi.type, fi.level, access, pc, addr,
                        access == MMU_INST_FETCH ? 0 : fetch_insn(nc, pc),
                        ec ? (uint64_t)ec->esr : 0);
            }
            nc->fill_fails++;
            nc->faults++;
            uint32_t fsc = arm_fi_to_lfsc(&fi);
            env->exception.vaddress = addr;
            if (access == MMU_INST_FETCH) {
                leave_native(nc), raise_exception(env, EXCP_PREFETCH_ABORT,
                                syn_insn_abort(0, fi.ea, fi.s1ptw, fsc), 1);
            } else {
                leave_native(nc), raise_exception(env, EXCP_DATA_ABORT,
                                syn_data_abort_no_iss(0, 0, fi.ea, 0, fi.s1ptw,
                                                      access == MMU_DATA_STORE,
                                                      fsc), 1);
            }
        }
        frame_to_state(nc, uc, tp);
        nat_trace(nc, "align", pc, addr);
        nc->faults++;
        env->exception.vaddress = addr;
        leave_native(nc), raise_exception(env, EXCP_DATA_ABORT,
                        syn_data_abort_no_iss(0, 0, 0, 0, 0,
                                              access == MMU_DATA_STORE, 0x21), 1);
    }
    case SIGSYS: {
        uint32_t insn = fetch_insn(nc, pc - 4);
        uint32_t imm16 = (insn >> 5) & 0xffff;
        if (trace_left > 0) {
            trace_left--;
            fprintf(stderr, "native-el0[%d] %" PRId64 "ms: svc pc=%#" PRIx64
                    " nr=%" PRId64 " x0=%#" PRIx64 " x1=%#" PRIx64 " sp=%#" PRIx64 "\n",
                    cs->cpu_index, (mono_ns() - trace_t0) / 1000000, pc,
                    (int64_t)uc->uc_mcontext.regs[16], (uint64_t)uc->uc_mcontext.regs[0],
                    (uint64_t)uc->uc_mcontext.regs[1], (uint64_t)uc->uc_mcontext.sp);
        }
        frame_to_state(nc, uc, tp);
        nc->svcs++;
        leave_native(nc), raise_exception(env, EXCP_SWI, syn_aa64_svc(imm16), 1);
    }
    case SIGILL:
    case SIGFPE: {
        uint32_t insn = fetch_insn(nc, pc);
        if (sig == SIGILL && emulate_mrs(nc, uc, insn)) {
            nat_trace(nc, "mrs", pc, insn);
            resume_guest(nc, uc, tp);
            return;
        }
        nat_trace(nc, "undef", pc, insn);
        frame_to_state(nc, uc, tp);
        nc->ills++;
        leave_native(nc), raise_exception(env, EXCP_UDEF, syn_uncategorized(), 1);
    }
    case SIGTRAP: {
        uint32_t insn = fetch_insn(nc, pc);
        nat_trace(nc, "brk", pc, insn);
        frame_to_state(nc, uc, tp);
        nc->brks++;
        if ((insn & 0xffe0001f) == 0xd4200000) {
            leave_native(nc), raise_exception(env, EXCP_BKPT, syn_aa64_bkpt((insn >> 5) & 0xffff), 1);
        }
        leave_native(nc), raise_exception(env, EXCP_UDEF, syn_uncategorized(), 1);
    }
    default:
        /* the kick: back to cpu_exec, which looks at the interrupt state */
        nat_trace(nc, "kick", pc, 0);
        frame_to_state(nc, uc, tp);
        nc->kicks++;
        leave_native(nc), cpu_loop_exit(cs);
    }
}

/* ---------------------------------------------------------------------- */
/* in-process mode: the vCPU thread's own signal handlers                 */

static void enter_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    NativeCtx *nc = ctx_from_stack(&uc);
    if (!nc) {
        return;
    }
    if (qatomic_read(&nc->kick_pending)) {
        /* somebody wants this vCPU: stay in QEMU, native_el0_try returns */
        qatomic_set(&nc->in_guest, 0);
        return;
    }
    state_to_frame(nc, uc);
    nc->enters++;
    nc->enter_tick = cntvct();
    nc->entry_ticks += nc->enter_tick - nc->try_tick;
    qatomic_set(&slot.last_ns, mono_ns());
    /* From here on no TLS: the guest's thread pointer goes live. */
    write_tpidr(nc->guest_tpidr);
}

static void chain_old(int sig, siginfo_t *si, void *uc)
{
    struct sigaction *o = (sig > 0 && sig < 65) ? &old_actions[sig] : NULL;
    if (o && (o->sa_flags & SA_SIGINFO) && o->sa_sigaction) {
        o->sa_sigaction(sig, si, uc);
        return;
    }
    if (o && !(o->sa_flags & SA_SIGINFO) && o->sa_handler != SIG_DFL &&
        o->sa_handler != SIG_IGN) {
        o->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void trap_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    uint64_t tp = read_tpidr();
    NativeCtx *nc = ctx_from_stack(&uc);

    if (!nc) {
        /* not a vCPU thread we prepared */
        if (sig == sig_kick) {
            return;
        }
        chain_old(sig, si, uc);
        return;
    }
    write_tpidr(nc->host_tpidr);            /* TLS back for QEMU's code */
    cur_nc = nc;
    if (nc->guard_active && !qatomic_read(&nc->in_guest)) {
        nc->guard_active = 0;
        siglongjmp(nc->guard, 1);
    }

    uint64_t pc = uc->uc_mcontext.pc;
    bool live = qatomic_read(&nc->in_guest);
    bool guest = live && (sig != sig_kick || in_window(pc));
    if (!guest) {
        if (sig == sig_kick) {
            qatomic_set(&nc->kick_pending, 1);
            return;
        }
        fprintf(stderr, "native-el0: host signal %d at pc=%#" PRIx64
                " addr=%p in_guest=%d\n", sig, pc, si->si_addr, nc->in_guest);
        chain_old(sig, si, uc);
        return;
    }
    qatomic_set(&nc->in_guest, 0);
    nc->trap_tick = cntvct();
    nc->native_ticks += nc->trap_tick - nc->enter_tick;
    handle_trap(nc, sig, si->si_code, (uint64_t)(uintptr_t)si->si_addr, uc, tp);
    /* resuming natively: resume_guest loaded the guest's thread pointer */
}

/* ---------------------------------------------------------------------- */
/* the TCG side                                                           */

static inline uint16_t cur_asid(CPUARMState *env)
{
    uint64_t ttbr = (env->cp15.tcr_el[1] & (1ULL << 22))
                    ? env->cp15.ttbr1_el[1] : env->cp15.ttbr0_el[1];
    return extract64(ttbr, 48, 16);
}

static bool slot_claim(uint16_t asid)
{
    bool ok = false;
    pthread_mutex_lock(&fill_lock);
    if (!slot.used) {
        slot.asid = asid;
        qatomic_set(&slot.last_ns, mono_ns());
        qatomic_set(&slot.used, 1);
        ok = true;
    } else if (slot.asid == asid) {
        ok = true;
    } else if (qatomic_read(&slot.active) == 0 &&
               mono_ns() - qatomic_read(&slot.last_ns) >= steal_ns) {
        /* another address space owns the mirror, nobody runs in it and it
         * has been quiet for a while: take it over */
        fill_lock_held = 1;
        inproc_tlbi_all();              /* drop the old owner's pages */
        fill_lock_held = 0;
        slot.asid = asid;
        qatomic_set(&slot.last_ns, mono_ns());
        slot.steals++;
        ok = true;
    }
    if (ok) {
        qatomic_inc(&slot.active);      /* this vCPU is about to run natively in it */
    }
    pthread_mutex_unlock(&fill_lock);
    return ok;
}

void native_el0_kick(CPUState *cs)
{
    if (!native_el0_enabled) {
        return;
    }
    int idx = cs->cpu_index;
    NativeCtx *nc = (idx >= 0 && idx < MAX_CTX) ? ctx_by_index[idx] : NULL;
    if (!nc || !qatomic_read(&nc->in_guest)) {
        return;
    }
    if (stub_mode) {
        int s = nc->stub;
        StubSlot *sl = nc->slot;
        if (s >= 0 && sl) {
            stub_kick_slot(s, sl);
        }
    } else {
        pthread_kill(cs->thread->thread, sig_kick);
    }
}

static inline void post_cmd(StubSlot *sl, uint32_t c)
{
    qatomic_set(&sl->status, ST_RUNNING);
    smp_wmb();
    qatomic_set(&sl->cmd, c);
    futex(&sl->cmd, FUTEX_WAKE, 1, NULL);
}

/* Drive the stub thread until the guest leaves native execution. Returns
 * only through raise_exception/cpu_loop_exit. */
static void __attribute__((noreturn)) stub_drive(NativeCtx *nc, StubSlot *sl)
{
    CPUState *cs = nc->cs;
    int s = nc->stub;
    for (;;) {
        uint32_t st;
        bool kicked = false;
        int timeouts = 0;
        for (;;) {
            uint64_t t0 = cntvct();
            do {
                st = qatomic_read(&sl->status);
                if (st != ST_RUNNING) {
                    break;
                }
                spin_relax();
            } while (cntvct() - t0 < qemu_spin_ticks);
            if (st != ST_RUNNING) {
                break;
            }
            if (!kicked && qatomic_read(&cs->exit_request)) {
                kicked = true;
                stub_kick_slot(s, sl);
            }
            struct timespec ts = { 1, 0 };
            if (futex(&sl->status, FUTEX_WAIT, ST_RUNNING, &ts) < 0 && errno == ETIMEDOUT) {
                if (++timeouts == 10) {
                    fprintf(stderr, "native-el0: vCPU %d waits 10 s on stub %d (pid %d tid %u):"
                            " cmd=%u status=%u kick=%u last sig=%d code=%d addr=%#" PRIx64
                            " pc=%#" PRIx64 " fill va=%#" PRIx64 " len=%#" PRIx64 " enters=%" PRIu64 "\n",
                            cs->cpu_index, s, shm->ctl[s].pid, qatomic_read(&sl->tid),
                            qatomic_read(&sl->cmd), qatomic_read(&sl->status),
                            qatomic_read(&sl->kick_pending), sl->sig, sl->si_code, sl->fault_addr,
                            (uint64_t)sl->uc.uc_mcontext.pc, sl->fill.va, sl->fill.len, nc->enters);
                }
                if (kill(shm->ctl[s].pid, 0) < 0) {
                    if (kill(factory_pid, 0) < 0) {
                        for (;;) {
                            pause();            /* QEMU is going down */
                        }
                    }
                    fprintf(stderr, "native-el0: stub %d died while running vCPU %d; slots:",
                            s, cs->cpu_index);
                    for (int v = 0; v < (int)qatomic_read(&shm->nvcpus); v++) {
                        StubSlot *o = &shm->slot[s][v];
                        fprintf(stderr, " [v%d phase=%u status=%u cmd=%u sig=%d pc=%#" PRIx64 " sp=%#" PRIx64
                                " fill=%#" PRIx64 "+%#" PRIx64 "]", v, qatomic_read(&o->phase),
                                qatomic_read(&o->status), qatomic_read(&o->cmd), o->sig,
                                (uint64_t)o->uc.uc_mcontext.pc, (uint64_t)o->uc.uc_mcontext.sp,
                                o->fill.va, o->fill.len);
                    }
                    fprintf(stderr, "\n");
                    abort();
                }
            }
        }
        smp_rmb();
        uint64_t now = cntvct();
        nc->native_ticks += now - nc->enter_tick;
        nc->trap_tick = now;
        if (st == ST_KICK) {
            nat_trace(nc, "kick", sl->uc.uc_mcontext.pc, 0);
            frame_to_state(nc, &sl->uc, sl->tpidr);
            nc->kicks++;
            leave_native(nc);
            cpu_loop_exit(cs);
        }
        if (st == ST_FILLFAIL) {
            nat_trace(nc, "fillfail", sl->uc.uc_mcontext.pc, sl->fill.va);
            frame_to_state(nc, &sl->uc, sl->tpidr);
            nc->unsupported++;
            nc->tcg_until_el1 = 1;
            leave_native(nc);
            cpu_loop_exit(cs);
        }
        nc->fill_pending = 0;
        handle_trap(nc, sl->sig, sl->si_code, sl->fault_addr, &sl->uc, sl->tpidr);
        /* back natively, with or without a fill */
        post_cmd(sl, nc->fill_pending ? CMD_FILL_RESUME : CMD_RESUME);
    }
}

/*
 * ASOFT_NATIVE_EL0_ASID_WATCH=N: does one hardware ASID serve two page table
 * roots without a flush between? XNU may alias live pmaps on one hw ASID and
 * flush per core at switch.
 */
static int asid_watch_left = -1;
static uint64_t stub_root[STUB_MAX];

static void asid_watch(CPUState *cs, CPUARMState *env, int s)
{
    if (!asid_watch_on) {
        return;
    }
    if (asid_watch_left < 0) {
        asid_watch_left = asid_watch_init;
    }
    uint16_t asid = cur_asid(env);
    uint64_t root = extract64(env->cp15.ttbr0_el[1], 0, 48);
    pthread_mutex_lock(&pool_lock);
    if (asid > asid_max_seen) {
        asid_max_seen = asid;
    }
    uint64_t old = asid_last_root[asid];
    int running = sbind[s].running;
    bool change = old && old != root;
    bool concurrent = change && running > 1 && stub_root[s] != root;
    asid_last_root[asid] = root;
    stub_root[s] = root;
    if (change) {
        asid_root_changes++;
    }
    if (concurrent) {
        asid_root_concurrent++;
    }
    if (change && asid_watch_left > 0) {
        asid_watch_left--;
        fprintf(stderr, "native-el0: asid %u root %#" PRIx64 " -> %#" PRIx64 " vcpu%d stub%d running %d (%" PRId64 " ms)\n",
                asid, old, root, cs->cpu_index, s, running, (mono_ns() - trace_t0) / 1000000);
    }
    pthread_mutex_unlock(&pool_lock);
}

bool native_el0_try(CPUState *cs)
{
    if (unlikely(sampler_period > 0 && !qatomic_read(&sampler_launched))) {
        sampler_launch();
    }
    if (unlikely(prof_dir && !prof_thread_ready)) {
        prof_thread_init();
    }
    if (!native_el0_enabled) {
        return false;
    }
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    int idx = cs->cpu_index;
    NativeCtx *nc = (idx >= 0 && idx < MAX_CTX) ? ctx_by_index[idx] : NULL;

    if (!is_a64(env) || arm_current_el(env) != 0) {
        if (nc) {
            nc->tcg_until_el1 = 0;
            if (nc->deny_tick) {
                /* the slice TCG ran because no stub was spared: demand */
                uint64_t d = cntvct() - nc->deny_tick;
                nc->deny_tick = 0;
                pthread_mutex_lock(&pool_lock);
                heat_add(nc->deny_asid, d, mono_ns());
                pthread_mutex_unlock(&pool_lock);
            }
        }
        return false;
    }
    if (!nc) {
        nc = ctx_init(cs);
        if (!nc) {
            return false;
        }
    }
    if (nc->tcg_until_el1 || qatomic_read(&native_off) || cpu_single_stepping(cs) ||
        !QTAILQ_EMPTY(&cs->breakpoints) || !QTAILQ_EMPTY(&cs->watchpoints) ||
        icount_enabled()) {
        return false;
    }
    if (!(env->cp15.sctlr_el[1] & SCTLR_M) ||
        ((env->cp15.cpacr_el1 >> 20) & 3) != 3 ||            /* FPEN */
        (env->pstate & PSTATE_SS)) {
        return false;
    }
    if (cpu_isar_feature(aa64_sve, cpu) &&
        ((env->cp15.cpacr_el1 >> 16) & 3) == 3) {            /* ZEN: SVE live */
        return false;
    }
    if (delay_ns && mono_ns() - trace_t0 < delay_ns) {
        return false;
    }
    if (!in_window(env->pc)) {
        return false;
    }

    if (stub_mode) {
        if (idx >= STUB_VCPUS || !stub_ram_ready(env)) {
            return false;
        }
        int s = stub_acquire(cur_asid(env));
        if (s < 0) {
            /* no stub to spare: TCG for this slice, not a pool_lock round per TB */
            nc->tcg_until_el1 = 1;
            stub_denials++;
            if (evict_heat) {
                nc->deny_asid = cur_asid(env);
                nc->deny_tick = cntvct();
            }
            return false;
        }
        nc->native_mark = nc->native_ticks;
        asid_watch(cs, env, s);
        nc->stub = s;
        StubSlot *sl = &shm->slot[s][idx];
        if (!qatomic_read(&sl->ready)) {
            stub_release(nc);
            return false;
        }
        nc->slot = sl;
        state_to_frame(nc, &sl->uc);
        sl->tpidr = nc->guest_tpidr;
        qatomic_set(&sl->kick_pending, 0);
        qatomic_set(&nc->kick_pending, 0);
        qatomic_set(&nc->in_guest, 1);
        smp_mb();
        if (qatomic_read(&cs->exit_request)) {
            qatomic_set(&nc->in_guest, 0);
            stub_release(nc);
            return false;
        }
        nc->enters++;
        nc->try_tick = nc->enter_tick = cntvct();
        post_cmd(sl, CMD_RUN);
        stub_drive(nc, sl);
    }

    if (!slot_claim(cur_asid(env))) {
        return false;
    }
    qatomic_set(&nc->kick_pending, 0);
    qatomic_set(&nc->in_guest, 1);
    smp_mb();
    if (qatomic_read(&cs->exit_request)) {
        qatomic_set(&nc->in_guest, 0);
        leave_native(nc);
        return false;
    }
    nc->try_tick = cntvct();
    pthread_kill(pthread_self(), sig_enter);
    /* Only reached when the entry handler declined (a kick arrived). */
    qatomic_set(&nc->in_guest, 0);
    leave_native(nc);
    return false;
}

#else /* not an AArch64 Linux host */

bool native_el0_try(CPUState *cs) { return false; }
void native_el0_kick(CPUState *cs) { }
void native_el0_tlbi_all(void) { }
void native_el0_tlbi_asid(uint16_t asid) { }
void native_el0_tlbi_page(uint64_t va, int asid) { }
void native_el0_tlbi_range(uint64_t base, uint64_t len, int asid) { }

#endif

#ifdef CONFIG_USER_ONLY
int native_el0_tlbi_scope(CPUState *cs, uint64_t va, uint16_t asid) { return TLBI_SCOPE_ALL; }
#endif
