/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "system/tcg.h"
#include "system/cpus.h"
#include "system/replay.h"
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "hw/core/boards.h"
#include "accel/tcg/cpu-loop.h"
#include "tcg/startup.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"

typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

#ifdef __linux__
/*
 * ASOFT_VCPU_CPUS=a,b,...: every vCPU thread may run only on these host
 * CPUs (a phone with few big cores keeps them off the small, in-order ones).
 */
static void asoft_vcpu_affinity(void)
{
    const char *e = getenv("ASOFT_VCPU_CPUS");
    cpu_set_t set;

    if (!e || !*e) {
        return;
    }
    CPU_ZERO(&set);
    for (const char *q = e; *q; ) {
        char *end;
        long c = strtol(q, &end, 10);
        if (end == q) {
            break;
        }
        if (c >= 0 && c < CPU_SETSIZE) {
            CPU_SET(c, &set);
        }
        q = *end == ',' ? end + 1 : end;
    }
    if (CPU_COUNT(&set) && sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("ASOFT_VCPU_CPUS: sched_setaffinity");
    }
}
#endif

/*
 * ASOFT_BQL_LITE=1: when cpu_exec returns only because the thread was kicked and nothing waits for
 * it (no queued work, no stop, not halted), run again without taking the BQL; anything else goes
 * through qemu_process_cpu_events with the BQL. Clearing exit_request first means a kick that
 * comes after the check makes the next cpu_exec return at once. (An iOS 26 boot on the S25 took
 * the BQL here 1.65 M times and waited 22 s for it.)
 */
static bool asoft_mttcg_again(CPUState *cpu)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("ASOFT_BQL_LITE");

        on = e && *e == '1';
    }
    if (!on) {
        return false;
    }
    qatomic_set_mb(&cpu->exit_request, false);
    qatomic_set_mb(&cpu->thread_kicked, false);
    return !qatomic_read(&cpu->stop) && !qatomic_read(&cpu->unplug) &&
           !qatomic_read(&cpu->halted) && cpu_work_list_empty(cpu) && cpu_can_run(cpu);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

#ifdef __linux__
    asoft_vcpu_affinity();
#endif
    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        qemu_process_cpu_events(cpu);

        if (cpu_can_run(cpu)) {
            int r;
            bql_unlock();
            r = tcg_cpu_exec(cpu);
            while (r == EXCP_INTERRUPT && asoft_mttcg_again(cpu)) {
                r = tcg_cpu_exec(cpu);
            }
            bql_lock();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                bql_unlock();
                cpu_exec_step_atomic(cpu);
                bql_lock();
            default:
                /* Ignore everything else? */
                break;
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpu_destroy(cpu);
    bql_unlock();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();
    return NULL;
}

void mttcg_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}
