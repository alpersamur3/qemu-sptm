/*
 * The key log: the last keys and buttons sent to the guest and what became
 * of each, for the display's key overlay (ui/gtk.c, F12). The keyboard
 * (hw/input/hid.c) and usb-consumer (hw/usb/dev-hid.c) write it: an entry
 * when a key is queued, "got" when the guest's USB driver read the report
 * that holds it. A slow guest reads late, and until this showed it a key
 * that did nothing could not be told from one that never arrived.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "ui/keylog.h"

typedef struct KeyLogEntry {
    const void *src;
    char name[24];
    bool down;
    int state;
    uint64_t end;           /* the source's read count at which the guest has it */
    int64_t t0, t1;         /* host ms: queued (or began to wait), read */
} KeyLogEntry;

#define KEYLOG_N 10
static KeyLogEntry keylog[KEYLOG_N];
static unsigned keylog_count;       /* entries ever logged */
static unsigned keylog_changes;     /* for the display */

void keylog_add(const void *src, const char *name, bool down, int state, uint64_t end)
{
    KeyLogEntry *e = &keylog[keylog_count++ % KEYLOG_N];

    e->src = src;
    g_strlcpy(e->name, name, sizeof(e->name));
    e->down = down;
    e->state = state;
    e->end = end;
    e->t0 = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    e->t1 = 0;
    keylog_changes++;
}

void keylog_queued(const void *src, const char *name, uint64_t end)
{
    unsigned i;

    for (i = 0; i < KEYLOG_N; i++) {
        KeyLogEntry *e = &keylog[i];

        if (e->src == src && e->state == KEYLOG_LATER && !strcmp(e->name, name)) {
            e->state = KEYLOG_WAIT;
            e->end = end;
            keylog_changes++;
            return;
        }
    }
    keylog_add(src, name, false, KEYLOG_WAIT, end);
}

void keylog_taken(const void *src, uint64_t taken)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    unsigned i;

    for (i = 0; i < KEYLOG_N; i++) {
        KeyLogEntry *e = &keylog[i];

        if (e->src == src && e->state == KEYLOG_WAIT && e->end <= taken) {
            e->state = KEYLOG_GOT;
            e->t1 = now;
            keylog_changes++;
        }
    }
}

void keylog_lost(const void *src)
{
    unsigned i;

    for (i = 0; i < KEYLOG_N; i++) {
        KeyLogEntry *e = &keylog[i];

        if (e->src == src && (e->state == KEYLOG_WAIT || e->state == KEYLOG_LATER)) {
            e->state = KEYLOG_LOST;
            keylog_changes++;
        }
    }
}

unsigned keylog_gen(void)
{
    return keylog_changes;
}

bool keylog_waiting(void)
{
    unsigned i;

    for (i = 0; i < KEYLOG_N; i++) {
        if (keylog[i].src && (keylog[i].state == KEYLOG_WAIT || keylog[i].state == KEYLOG_LATER)) {
            return true;
        }
    }
    return false;
}

/* Pango markup, oldest first; entries done more than 8 s ago dimmed */
char *keylog_markup(void)
{
    GString *m = g_string_new("<b>Keys → guest</b>   <span foreground=\"#8e8e93\">F12 hides</span>");
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    unsigned n = MIN(keylog_count, KEYLOG_N), i;

    if (!n) {
        g_string_append(m, "\n<span foreground=\"#8e8e93\">no key yet</span>");
    }
    for (i = keylog_count - n; i != keylog_count; i++) {
        KeyLogEntry *e = &keylog[i % KEYLOG_N];
        bool open = e->state == KEYLOG_WAIT || e->state == KEYLOG_LATER;
        int64_t took = e->t1 - e->t0;

        g_string_append_printf(m, "\n<span%s>%-10s %-7s ", !open && now - e->t0 > 8000 ? " fgalpha=\"40%\"" : "",
                               e->name, e->down ? "press" : "release");
        switch (e->state) {
        case KEYLOG_WAIT:
            g_string_append_printf(m, "<span foreground=\"#ffd60a\">… waiting %.1f s</span>",
                                   (now - e->t0) / 1000.0);
            break;
        case KEYLOG_GOT:
            if (took < 1000) {
                g_string_append_printf(m, "<span foreground=\"#30d158\">✓ got it, %d ms</span>", (int)took);
            } else {
                g_string_append_printf(m, "<span foreground=\"#ff9f0a\">✓ got it, %.1f s</span>",
                                       took / 1000.0);
            }
            break;
        case KEYLOG_FULL:
            g_string_append(m, e->down ? "<span foreground=\"#ff453a\">✗ no room (held: goes in late)</span>"
                                       : "<span foreground=\"#ff453a\">✗ dropped: queue full</span>");
            break;
        case KEYLOG_LATER:
            g_string_append(m, "<span foreground=\"#ff9f0a\">→ queue full, goes in with room</span>");
            break;
        case KEYLOG_KEPT:
            g_string_append(m, "<span foreground=\"#30d158\">= still down in the guest</span>");
            break;
        case KEYLOG_LOST:
            g_string_append(m, "<span foreground=\"#ff453a\">✗ lost: the guest reset the device</span>");
            break;
        }
        g_string_append(m, "</span>");
    }
    return g_string_free(m, FALSE);
}
