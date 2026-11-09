/*
 * The key log: the last keys and buttons sent to the guest and what became
 * of each, for the display's key overlay (ui/gtk.c, F12).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef UI_KEYLOG_H
#define UI_KEYLOG_H

enum {
    KEYLOG_WAIT,    /* queued: waits for the guest's read */
    KEYLOG_GOT,     /* the guest's USB driver read the report that holds it */
    KEYLOG_FULL,    /* found the queue full: a release is dropped, a held press goes in late */
    KEYLOG_LATER,   /* a release that waits for room in the queue */
    KEYLOG_KEPT,    /* pressed again before its release went in: still down in the guest */
    KEYLOG_LOST,    /* the guest reset the device before it read this */
};

/*
 * @src names the device (any pointer that stays the same for it). @end is
 * the source's own count of items queued once this one is in: keylog_taken()
 * marks every waiting entry of @src whose @end is at most @taken as read.
 * All of it runs under the BQL.
 */
void keylog_add(const void *src, const char *name, bool down, int state, uint64_t end);
/* a waiting release of @name went into the queue */
void keylog_queued(const void *src, const char *name, uint64_t end);
void keylog_taken(const void *src, uint64_t taken);
void keylog_lost(const void *src);

/* for the display: a counter of changes, whether an entry still waits, and the log as Pango markup */
unsigned keylog_gen(void);
bool keylog_waiting(void);
char *keylog_markup(void);

#endif
