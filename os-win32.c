/*
 * os-win32.c
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2010 Red Hat, Inc.
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
#include <windows.h>
#include <mmsystem.h>
#include "system/runstate.h"

static BOOL WINAPI qemu_ctrl_handler(DWORD type)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    /* Windows 7 kills application when the function returns.
       Sleep here to give QEMU a try for closing.
       Sleep period is 10000ms because Windows kills the program
       after 10 seconds anyway. */
    Sleep(10000);

    return TRUE;
}

static TIMECAPS mm_tc;

static void os_undo_timer_resolution(void)
{
    timeEndPeriod(mm_tc.wPeriodMin);
}

/*
 * Windows 11 slows a process whose window is covered, minimized or not in front: its threads may run at the
 * efficiency cores' pace (power throttling of the execution speed), and the timer period asked for below is no
 * longer kept (15.6 ms again). A guest under TCG needs both whether its window is looked at or not: a boot of
 * minutes is left to run behind other windows. ASOFT_THROTTLE=1 leaves Windows its own way (to measure).
 */
static void os_never_throttle(void)
{
    struct {
        ULONG Version, ControlMask, StateMask;
    } st = { 1, 0x1 /* EXECUTION_SPEED */ | 0x4 /* IGNORE_TIMER_RESOLUTION */, 0 };
    const char *env = getenv("ASOFT_THROTTLE");

    if (env && env[0] == '1') {
        return;
    }
    /* (4 = ProcessPowerThrottling; a Windows that does not know the timer mask refuses both: the speed alone) */
    if (!SetProcessInformation(GetCurrentProcess(), 4, &st, sizeof(st))) {
        st.ControlMask = 0x1;
        SetProcessInformation(GetCurrentProcess(), 4, &st, sizeof(st));
    }
}

void os_setup_early_signal_handling(void)
{
    SetConsoleCtrlHandler(qemu_ctrl_handler, TRUE);
    os_never_throttle();
    timeGetDevCaps(&mm_tc, sizeof(mm_tc));
    timeBeginPeriod(mm_tc.wPeriodMin);
    atexit(os_undo_timer_resolution);
}

void os_set_line_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}
