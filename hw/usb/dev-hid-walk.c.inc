/*
 * usb-touchscreen for a slow guest that hit-tests a hover (ASoftware visionOS): paced moves, presses that wait
 * for the pointer to settle and last long enough to be seen, and walk mode. Every one is an option of the device
 * and off unless set, so an iOS or watchOS guest gets hid.c's reports as before:
 *   hover=on                 a pointer in the window is in range (dev-hid.c, usb_touchscreen_report)
 *   touch-pace-ms=N          at most one move report per N ms (visionOS: 66)
 *   touch-min-press-ms=N     a press lasts at least N ms (visionOS: 250)
 *   touch-min-hover-ms=N     a press waits until the pointer has rested N ms (visionOS: 300)
 *   walk-mode=on             the middle button, or qom-set <id> walk true|false, turns walk mode on and off;
 *                            the right button held is walk mode while it is down
 *   walk-hold-key=N          a key (Linux code) held down in the guest for as long as walk mode is on
 * All of them can be set with qom-set while the machine runs. Included by dev-hid.c.
 *
 * Walk mode.
 *
 * visionOS's RealitySimulation (the simulator's input, which the research VM runs) turns the head while
 * Command (HID 0xE3/0xE7) is held and a touch drags (RSDigitizerToHMDPoseConverter), and moves it with
 * W/A/S/D and R/F, Shift + those turning it (RSKeyboardTransform; K turns the keys' camera on and off).
 * Walk mode makes the mouse the head, as in a first-person game: QEMU takes relative motion (the window
 * grabs the pointer on a click; Ctrl+Alt+G lets it go), holds Command and drags a touch from the screen's
 * centre while the mouse moves, and lets both go once it rests. Between looks the pointer rests in range
 * at the centre, where visionOS shows its gaze ring: a left click taps there, and moving the mouse with
 * the button held drags (a window's bar, a list). The vertical look stops at +-walk-pitch-max, so the
 * head never turns over. The keys go to the guest's keyboard as before.
 *
 * The middle button turns walk mode on and off, as does qom-set <id> walk true|false; the right button held
 * is walk mode until it goes up (look around, then the pointer is back where it was). walk-gain is touch
 * units (0..32767 across the screen) per 100 mouse counts; walk-invert-y flips the vertical look.
 *
 * Typing and walking (walk-hold-key). RealitySimulation's keyboard camera reads every key, so typing a text
 * with W, A, S, D, Q, E, R, F or Shift in it walked and turned the head. The guest's camera is switched by
 * one key (RSToggleWASD: K, edge triggered, on at boot). A visionOS disk whose RealitySimulation is patched
 * (the installer's keycam step: the camera is on while HID usage 0x6C is down, K is an ordinary letter)
 * leaves the camera off until that key is held: walk-hold-key=183 (Linux KEY_F13, which usb-kbd sends as
 * usage 0x6C) holds it for as long as walk mode is on. So outside walk mode the keyboard only types, and in
 * walk mode the same keys walk. Held, not toggled: no count of presses to lose step with the guest, and a
 * frame the guest drew late cannot miss it. The key goes up when walk mode ends, when the machine stops (a
 * saved state then holds its release, not the key) and at each start or resume, for a state saved by an
 * older build with the key down. 0 (the default): no key, for a disk without the patch.
 */
#define WALK_CENTRE     16384
#define WALK_KEY_MS     40      /* Command before the touch, and the touch up before Command's release */
/*
 * walk-hold-ms (default 400): no motion this long ends a look. visionOS applies the drag to the head only when it
 * draws a frame while the touch is down (RSDigitizerToHMDPoseConverter, headsetDidUpdate), and draws a few frames a
 * second here: a shorter hold lost the end of a look, or a short look whole (measured, boots 33-35).
 */
#define WALK_GRIP_MS    20      /* the touch-down report alone before the look's first motion */
#define WALK_TAP_MS     90      /* a click that came during a look: the tap's length */
#define WALK_REACH      9000    /* a look's touch this far from the centre grips again at the centre */
#define WALK_LEFTMETA   125     /* Linux KEY_LEFTMETA, HID 0xE3 on usb-kbd */

/*
 * Touch pace (touch-pace-ms, 0 = off): a slow guest under TCG drowns in motion reports. The window
 * sends one per host mouse event; the guest reads each at once, but the USB interrupt → dext → IOHIDEventSystem →
 * app chain costs so much that the moves pile up and go through in a burst after the drag, so a slow drag becomes
 * a flick, a tap a long press, and a window drag never ends (iOS 26, APK-PLAN §6.22: ~120 moves/s gave 0.3-1 fps
 * during a drag, ~14/s gave 12.5-14 fps and the first reaction in 0.5-0.7 s instead of 2-2.8 s). So a move waits
 * until touch-pace-ms passed since the last one and gives way to a newer one; a press or a release is never held,
 * merged or dropped, and carries the newest position. The Android app paces by iOS frames as well; this has no
 * frame signal from the display yet, so time alone.
 */

enum {
    WALK_IDLE,          /* in range at the centre, Command up */
    WALK_ARMING,        /* a look starts: Command goes down at the timer (never from inside the input layer) */
    WALK_ARMED,         /* Command down, the touch not yet */
    WALK_GRIPPED,       /* the touch down at the centre, its first motion at the next tick: a report of its own,
                         * or hid.c merges it into the touch-down report and the guest never sees it move */
    WALK_LOOKING,       /* Command down, the touch dragging */
    WALK_REGRIP,        /* Command down, the touch up for a moment, going down at the centre again */
    WALK_RELEASING,     /* the touch up, Command going up */
    WALK_TAP,           /* a click's tap at the centre, going up */
};

static int walk_clamp(int64_t v)
{
    return v < 0 ? 0 : v > 32767 ? 32767 : (int)v;
}

/* Feed hid.c's own tablet handler, as if the window had sent it: the touchscreen's reports follow. */
static void walk_feed(USBHIDState *us, QemuInputEvent *evt)
{
    us->ts_handler->event((DeviceState *)&us->hid, NULL, evt);
}

static void walk_point(USBHIDState *us, int x, int y, bool tip)
{
    QemuInputEvent e = { .type = INPUT_EVENT_KIND_ABS };

    e.abs.axis = INPUT_AXIS_X;
    e.abs.value = x;
    walk_feed(us, &e);
    e.abs.axis = INPUT_AXIS_Y;
    e.abs.value = y;
    walk_feed(us, &e);
    e.type = INPUT_EVENT_KIND_BTN;
    e.btn.button = INPUT_BUTTON_LEFT;
    e.btn.down = tip;
    walk_feed(us, &e);
    us->ts_handler->sync((DeviceState *)&us->hid);
}

static void walk_meta(bool down)
{
    qemu_input_event_send_key_linux(NULL, WALK_LEFTMETA, down);
}

static int64_t walk_now(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

/* The touch at the centre, Command already down: a look's grip. */
static void walk_grip(USBHIDState *us)
{
    us->walk_x = us->walk_y = (int64_t)WALK_CENTRE * 100;
    walk_point(us, WALK_CENTRE, WALK_CENTRE, true);
    us->walk_state = WALK_LOOKING;
}

/* Move the touch by mouse counts: a look (Command held) or a drag (a click held). */
static void walk_move(USBHIDState *us, int dx, int dy, bool look)
{
    int64_t mx = (int64_t)dx * us->walk_gain;
    int64_t my = (int64_t)dy * us->walk_gain * (us->walk_invert_y ? -1 : 1);

    if (look) {
        int64_t max = (int64_t)us->walk_pitch_max * 100;
        int64_t p = us->walk_pitch + my;

        if (p > max) {
            my -= p - max;
        } else if (p < -max) {
            my += -max - p;
        }
        us->walk_pitch += my;
    }
    us->walk_x += mx;
    us->walk_y += my;
    walk_point(us, walk_clamp(us->walk_x / 100), walk_clamp(us->walk_y / 100), true);
    if (look && (llabs(us->walk_x / 100 - WALK_CENTRE) > WALK_REACH ||
                 llabs(us->walk_y / 100 - WALK_CENTRE) > WALK_REACH)) {
        /* far from the centre: let go, and grip at the centre again (Command stays down) */
        walk_point(us, walk_clamp(us->walk_x / 100), walk_clamp(us->walk_y / 100), false);
        us->walk_state = WALK_REGRIP;
        timer_mod(us->walk_timer, walk_now() + WALK_KEY_MS / 2);
    }
}

/* End a look: the touch up now, Command once the guest has seen that. */
static void walk_release(USBHIDState *us)
{
    if (us->walk_state == WALK_LOOKING || us->walk_state == WALK_GRIPPED) {
        walk_point(us, walk_clamp(us->walk_x / 100), walk_clamp(us->walk_y / 100), false);
    }
    us->walk_state = WALK_RELEASING;
    timer_mod(us->walk_timer, walk_now() + WALK_KEY_MS);
}

static void walk_timer_cb(void *opaque)
{
    USBHIDState *us = opaque;
    int64_t now = walk_now();

    switch (us->walk_state) {
    case WALK_ARMING:
        walk_meta(true);
        us->walk_state = WALK_ARMED;
        timer_mod(us->walk_timer, now + WALK_KEY_MS);
        break;
    case WALK_ARMED:
    case WALK_REGRIP:
        walk_grip(us);
        us->walk_state = WALK_GRIPPED;
        timer_mod(us->walk_timer, now + WALK_GRIP_MS);
        break;
    case WALK_GRIPPED:
        us->walk_state = WALK_LOOKING;
        if (us->walk_dx || us->walk_dy) {
            walk_move(us, us->walk_dx, us->walk_dy, true);
            us->walk_dx = us->walk_dy = 0;
        }
        if (us->walk_state == WALK_LOOKING) {
            timer_mod(us->walk_timer, now + us->walk_hold_ms);
        }
        break;
    case WALK_LOOKING:
        if (now - us->walk_last_motion < us->walk_hold_ms) {
            timer_mod(us->walk_timer, us->walk_last_motion + us->walk_hold_ms);
        } else {
            walk_release(us);
        }
        break;
    case WALK_RELEASING:
        walk_meta(false);
        us->walk_x = us->walk_y = (int64_t)WALK_CENTRE * 100;
        if (us->walk_click_pending) {
            /* a click came during the look: tap at the centre now */
            us->walk_click_pending = false;
            walk_point(us, WALK_CENTRE, WALK_CENTRE, true);
            us->walk_clicking = true;
            if (us->walk_button) {
                us->walk_state = WALK_IDLE;
            } else {
                us->walk_state = WALK_TAP;
                timer_mod(us->walk_timer, now + MAX(WALK_TAP_MS, (int64_t)us->ts_min_press_ms));
            }
        } else {
            walk_point(us, WALK_CENTRE, WALK_CENTRE, false);
            us->walk_state = WALK_IDLE;
        }
        break;
    case WALK_TAP:
        walk_point(us, WALK_CENTRE, WALK_CENTRE, false);
        us->walk_clicking = false;
        us->walk_state = WALK_IDLE;
        break;
    default:
        break;
    }
}

static void walk_click(USBHIDState *us, bool down)
{
    us->walk_button = down;
    if (down && (us->walk_shift_down || us->walk_key_x || us->walk_key_y)) {
        /* look keys go up at the key timer, not from inside the input layer */
        us->walk_keys_release = true;
        timer_mod(us->walk_key_timer, walk_now());
    }
    if (down) {
        if (us->walk_state == WALK_IDLE && !us->walk_clicking) {
            us->walk_x = us->walk_y = (int64_t)WALK_CENTRE * 100;
            walk_point(us, WALK_CENTRE, WALK_CENTRE, true);
            us->walk_clicking = true;
        } else if (us->walk_state != WALK_TAP) {
            us->walk_click_pending = true;
            if (us->walk_state != WALK_RELEASING) {
                walk_release(us);
            }
        }
        return;
    }
    if (us->walk_clicking && us->walk_state == WALK_IDLE) {
        walk_point(us, walk_clamp(us->walk_x / 100), walk_clamp(us->walk_y / 100), false);
        us->walk_clicking = false;
        /* back to rest at the centre */
        us->walk_x = us->walk_y = (int64_t)WALK_CENTRE * 100;
        walk_point(us, WALK_CENTRE, WALK_CENTRE, false);
    }
}

/* A look's or a held click's motion at the touch pace: now, or the pace timer sends what gathered. */
static void walk_paced_move(USBHIDState *us, bool look)
{
    int64_t now = walk_now();

    if (!us->ts_pace_ms || now - us->walk_last_report >= us->ts_pace_ms) {
        walk_move(us, us->walk_dx, us->walk_dy, look);
        us->walk_dx = us->walk_dy = 0;
        us->walk_last_report = now;
    } else if (!timer_pending(us->walk_pace_timer)) {
        timer_mod(us->walk_pace_timer, us->walk_last_report + us->ts_pace_ms);
    }
}

static void walk_pace_cb(void *opaque)
{
    USBHIDState *us = opaque;

    if (!us->walk_dx && !us->walk_dy) {
        return;
    }
    if (us->walk_state == WALK_LOOKING) {
        walk_paced_move(us, true);
    } else if (us->walk_state == WALK_IDLE && us->walk_clicking) {
        walk_paced_move(us, false);
    }
}

static void walk_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    USBHIDState *us = (USBHIDState *)dev;

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        if (evt->rel.axis == INPUT_AXIS_X) {
            us->walk_dx += evt->rel.value;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            us->walk_dy += evt->rel.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            walk_click(us, evt->btn.down);
        } else if (evt->btn.button == INPUT_BUTTON_MIDDLE && evt->btn.down) {
            us->walk_toggle = true;     /* off at the sync: not from inside the handler list walk */
        } else if (evt->btn.button == INPUT_BUTTON_RIGHT && !evt->btn.down && us->walk_by_hold) {
            us->walk_toggle = true;     /* the right button went up: its walk mode ends */
        }
        break;
    default:
        break;
    }
}

static void walk_set(USBHIDState *us, bool on);

/*
 * The walk key, to the keyboard whatever the run state: qemu_input_event_send() drops events while the
 * machine is stopped, and the release at a stop has to reach the keyboard's queue before the state is saved.
 */
static void walk_hold_key_send(unsigned int lnx, bool down)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_KEY,
        .key = { .key = lnx, .down = down },
    };

    qemu_input_event_send_impl(NULL, &evt);
    qemu_input_event_sync_impl();
}

/*
 * Look by keys (the default; walk-look-touch=true takes the Command + touch drag instead): RealitySimulation's own
 * keyboard camera turns the head while Shift is held with A/D (left/right) or W/S (up/down). Measured: Command +
 * touch looks got lost about half the time (the guest seems to drop Command shortly after it went down), while
 * keys reach backboardd in ~65 ms. The camera reads the keys at each frame it draws (a few a second here), so a
 * press shorter than ~300 ms can fall between two frames; a press also speeds up while it is held. Mouse counts
 * gather per axis as pixels of turn (walk-key-gain: hundredths of a pixel per count); a press starts once an axis
 * owes walk-key-min-px and lasts ms = walk-key-ms-per-px x px / 10 within [walk-key-min-ms, walk-key-max-ms];
 * motion during a press adds to the next one. Shift goes down before the first direction key and up once no
 * direction key is held, so W/A/S/D walk again in between.
 */
#define WALK_KEY_TICK_MS    30
#define KEY_LEFTSHIFT_LNX   42
#define KEY_W_LNX           17
#define KEY_A_LNX           30
#define KEY_S_LNX           31
#define KEY_D_LNX           32

static void walk_key(unsigned int lnx, bool down)
{
    qemu_input_event_send_key_linux(NULL, lnx, down);
}

/* one axis: owe (hundredths of px; + = right/down), the key held and its release time */
static void walk_key_axis(USBHIDState *us, int64_t *owe, unsigned int *held, int64_t *until,
                          unsigned int neg_key, unsigned int pos_key, int64_t now)
{
    int64_t px, ms;

    if (*held) {
        if (now >= *until) {
            walk_key(*held, false);
            *held = 0;
        }
        return;
    }
    px = llabs(*owe) / 100;
    if (px < us->walk_key_min_px || !us->walk_shift_down) {
        return;
    }
    ms = px * us->walk_key_ms_per_px / 10;
    ms = MAX(ms, us->walk_key_min_ms);
    ms = MIN(ms, us->walk_key_max_ms);
    *held = *owe < 0 ? neg_key : pos_key;
    *until = now + ms;
    /* what this press is meant to deliver comes off the debt (the rest waits for the next press) */
    px = ms * 10 / MAX(us->walk_key_ms_per_px, 1);
    *owe += *owe < 0 ? px * 100 : -px * 100;
    if (llabs(*owe) < 100 * (int64_t)us->walk_key_min_px / 2) {
        *owe = 0;
    }
    walk_key(*held, true);
}

static void walk_keys_up(USBHIDState *us);

static void walk_key_tick(void *opaque)
{
    USBHIDState *us = opaque;
    int64_t now = walk_now();
    bool owing = llabs(us->walk_owe_x) / 100 >= us->walk_key_min_px ||
                 llabs(us->walk_owe_y) / 100 >= us->walk_key_min_px;

    if (us->walk_keys_release) {
        us->walk_keys_release = false;
        walk_keys_up(us);
        return;
    }
    if (owing && !us->walk_shift_down && !us->walk_clicking) {
        walk_key(KEY_LEFTSHIFT_LNX, true);       /* the direction keys follow at the next tick */
        us->walk_shift_down = true;
        us->walk_shift_since = now;
        timer_mod(us->walk_key_timer, now + WALK_KEY_TICK_MS);
        return;
    }
    if (us->walk_shift_down) {
        walk_key_axis(us, &us->walk_owe_x, &us->walk_key_x, &us->walk_until_x, KEY_A_LNX, KEY_D_LNX, now);
        walk_key_axis(us, &us->walk_owe_y, &us->walk_key_y, &us->walk_until_y,
                      us->walk_invert_y ? KEY_S_LNX : KEY_W_LNX, us->walk_invert_y ? KEY_W_LNX : KEY_S_LNX, now);
        if (!us->walk_key_x && !us->walk_key_y && now - us->walk_shift_since >= WALK_KEY_TICK_MS) {
            owing = llabs(us->walk_owe_x) / 100 >= us->walk_key_min_px ||
                    llabs(us->walk_owe_y) / 100 >= us->walk_key_min_px;
            if (!owing) {
                walk_key(KEY_LEFTSHIFT_LNX, false);
                us->walk_shift_down = false;
                return;
            }
        }
        timer_mod(us->walk_key_timer, now + WALK_KEY_TICK_MS);
    }
}

/* let every look key go (walk mode off, a click) */
static void walk_keys_up(USBHIDState *us)
{
    timer_del(us->walk_key_timer);
    if (us->walk_key_x) {
        walk_key(us->walk_key_x, false);
    }
    if (us->walk_key_y) {
        walk_key(us->walk_key_y, false);
    }
    if (us->walk_shift_down) {
        walk_key(KEY_LEFTSHIFT_LNX, false);
    }
    us->walk_key_x = us->walk_key_y = 0;
    us->walk_shift_down = false;
    us->walk_owe_x = us->walk_owe_y = 0;
}

static void walk_sync(DeviceState *dev)
{
    USBHIDState *us = (USBHIDState *)dev;

    if (us->walk_toggle) {
        us->walk_toggle = false;
        us->walk_want = false;
        qemu_bh_schedule(us->walk_bh);
        return;
    }
    if (!us->walk_dx && !us->walk_dy) {
        return;
    }
    us->walk_last_motion = walk_now();
    if (!us->walk_look_touch && !us->walk_clicking && us->walk_state == WALK_IDLE) {
        /* look by keys: the debt grows, the key timer pays it (never from inside the input layer) */
        us->walk_owe_x += (int64_t)us->walk_dx * us->walk_key_gain;
        us->walk_owe_y += (int64_t)us->walk_dy * us->walk_key_gain;
        us->walk_dx = us->walk_dy = 0;
        if (!timer_pending(us->walk_key_timer)) {
            timer_mod(us->walk_key_timer, us->walk_last_motion);
        }
        return;
    }
    switch (us->walk_state) {
    case WALK_IDLE:
        if (us->walk_clicking) {
            walk_paced_move(us, false);
        } else {
            us->walk_state = WALK_ARMING;
            timer_mod(us->walk_timer, us->walk_last_motion);
        }
        break;
    case WALK_LOOKING:
        walk_paced_move(us, true);
        break;
    case WALK_RELEASING:
        if (!us->walk_click_pending) {
            /* moving again before Command went up: grip again */
            us->walk_state = WALK_REGRIP;
            timer_mod(us->walk_timer, us->walk_last_motion + WALK_KEY_MS / 2);
        } else {
            us->walk_dx = us->walk_dy = 0;
        }
        break;
    case WALK_TAP:
        us->walk_dx = us->walk_dy = 0;
        break;
    default:
        /* arming, armed, regrip: the motion waits for the grip */
        break;
    }
}

/* walk mode on or off from a bottom half: activate/deactivate reorder the handler list the input layer walks */
static void walk_bh_cb(void *opaque)
{
    USBHIDState *us = opaque;

    walk_set(us, us->walk_want);
}

static const QemuInputHandler walk_handler = {
    .name  = "QEMU Touchscreen walk mode",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = walk_event,
    .sync  = walk_sync,
};

static void walk_set(USBHIDState *us, bool on)
{
    if (on == us->walk) {
        return;
    }
    us->walk = on;
    if (!on) {
        us->walk_by_hold = false;
    }
    timer_del(us->walk_timer);
    walk_keys_up(us);
    if (us->walk_state != WALK_IDLE || us->walk_clicking) {
        walk_point(us, walk_clamp(us->walk_x / 100), walk_clamp(us->walk_y / 100), false);
        if (us->walk_state != WALK_IDLE && us->walk_state != WALK_TAP) {
            walk_meta(false);
        }
    }
    us->walk_state = WALK_IDLE;
    us->walk_clicking = us->walk_click_pending = us->walk_button = false;
    us->walk_dx = us->walk_dy = 0;
    us->walk_pitch = 0;
    us->walk_x = us->walk_y = (int64_t)WALK_CENTRE * 100;
    if (on) {
        walk_point(us, WALK_CENTRE, WALK_CENTRE, false);
        qemu_input_handler_activate(us->walk_s);
    } else {
        qemu_input_handler_deactivate(us->walk_s);
        qemu_input_handler_activate(us->hid.s);
    }
    if (us->walk_hold_key) {
        walk_hold_key_send(us->walk_hold_key, on);
    }
}

/*
 * The machine stops (a pause, a save): walk mode ends, so the state holds the walk key's release and no
 * touch in the air. It starts or resumes: the walk key goes up once more, for a loaded state that has it down.
 */
static void walk_vm_state(void *opaque, bool running, RunState state)
{
    USBHIDState *us = opaque;

    if (!running) {
        if (us->walk) {
            walk_set(us, false);
        }
    } else if (us->walk_hold_key && !us->walk) {
        walk_hold_key_send(us->walk_hold_key, false);
    }
}

/* The held-back position of a paced move, into hid.c's state (a press or a release then carries it). */
static void ts_pace_feed(USBHIDState *us)
{
    QemuInputEvent e = { .type = INPUT_EVENT_KIND_ABS };

    if (!us->ts_pending) {
        return;
    }
    e.abs.axis = INPUT_AXIS_X;
    e.abs.value = us->ts_x;
    us->ts_handler->event((DeviceState *)&us->hid, NULL, &e);
    e.abs.axis = INPUT_AXIS_Y;
    e.abs.value = us->ts_y;
    us->ts_handler->event((DeviceState *)&us->hid, NULL, &e);
    us->ts_pending = false;
}

static void ts_pace_flush(void *opaque)
{
    USBHIDState *us = opaque;

    if (us->ts_pending) {
        ts_pace_feed(us);
        us->ts_handler->sync((DeviceState *)&us->hid);
        us->ts_last = walk_now();
    }
}

/*
 * Minimum press (touch-min-press-ms, 0 = off). Measured on visionOS 27.2 (VISIONOS.md §11): a touch of
 * 0.1-0.2 s on a Home icon does nothing, 0.25 s opens the app, 0.3 s is a long press (edit mode); a human click is
 * about 0.1 s, so most clicks were lost. The release of the left button is held back until the press lasted this
 * long: the release event goes through the pace timer, with the position it carried.
 */
static void ts_release_now(USBHIDState *us)
{
    QemuInputEvent e = { .type = INPUT_EVENT_KIND_BTN };

    e.btn.button = INPUT_BUTTON_LEFT;
    e.btn.down = false;
    us->ts_handler->event((DeviceState *)&us->hid, NULL, &e);
    us->ts_release_held = false;
    us->ts_edge = true;
}

static void ts_release_cb(void *opaque)
{
    USBHIDState *us = opaque;

    if (!us->ts_release_held) {
        return;
    }
    ts_pace_feed(us);
    ts_release_now(us);
    us->ts_edge = false;
    us->ts_handler->sync((DeviceState *)&us->hid);
    us->ts_last = walk_now();
}

/*
 * Settle before a press (touch-min-hover-ms, 0 = off). The guest's hit test reads the hover (InRange
 * without Tip) a frame at a time, and the simulated input is sampled at 7-9 Hz while the guest is busy: a click right
 * after the pointer moved reached the guest before the hover at that point did, and only a second click, with the
 * pointer at rest, opened anything (the user: "everything works with a double click"). A press within this long after
 * the last move sends the hover at once and the press when the pointer has rested that long; a release in between
 * waits for it, then touch-min-press-ms applies as for any press.
 */
static void ts_press_cb(void *opaque)
{
    USBHIDState *us = opaque;
    QemuInputEvent e = { .type = INPUT_EVENT_KIND_BTN };

    if (!us->ts_press_deferred) {
        return;
    }
    us->ts_press_deferred = false;
    ts_pace_feed(us);
    e.btn.button = INPUT_BUTTON_LEFT;
    e.btn.down = true;
    us->ts_handler->event((DeviceState *)&us->hid, NULL, &e);
    us->ts_handler->sync((DeviceState *)&us->hid);
    us->ts_press_at = us->ts_last = walk_now();
    if (us->ts_release_deferred) {
        us->ts_release_deferred = false;
        us->ts_release_held = true;
        timer_mod(us->ts_release_timer, us->ts_press_at + us->ts_min_press_ms);
    }
}

/* The touchscreen's own handler: hid.c's, but the middle button turns walk mode on, and moves are paced. */
static void walk_ts_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    USBHIDState *us = (USBHIDState *)dev;

    if (us->walk_mode && evt->type == INPUT_EVENT_KIND_BTN &&
        (evt->btn.button == INPUT_BUTTON_MIDDLE || evt->btn.button == INPUT_BUTTON_RIGHT)) {
        if (evt->btn.down) {
            us->walk_toggle = true;
            us->walk_by_hold = evt->btn.button == INPUT_BUTTON_RIGHT;
        }
        return;
    }
    if (evt->type == INPUT_EVENT_KIND_BTN && evt->btn.button == INPUT_BUTTON_LEFT &&
        (us->ts_min_press_ms || us->ts_min_hover_ms || us->ts_press_deferred || us->ts_release_held)) {
        int64_t now = walk_now();

        if (evt->btn.down) {
            if (us->ts_release_held) {
                /* a new press while the last release waits: let that release go first */
                ts_pace_feed(us);
                ts_release_now(us);
                us->ts_handler->sync((DeviceState *)&us->hid);
            }
            if (us->ts_min_hover_ms && now - us->ts_last_move < us->ts_min_hover_ms) {
                /* the pointer just moved: the hover at the press point goes now, the press when it settled */
                ts_pace_feed(us);
                timer_del(us->ts_pace_timer);
                us->ts_handler->sync((DeviceState *)&us->hid);
                us->ts_last = now;
                us->ts_press_deferred = true;
                us->ts_release_deferred = false;
                timer_mod(us->ts_press_timer, us->ts_last_move + us->ts_min_hover_ms);
                return;
            }
            us->ts_press_at = now;
        } else if (us->ts_press_deferred) {
            /* released before the deferred press went: the press timer sends both */
            us->ts_release_deferred = true;
            return;
        } else if (now - us->ts_press_at < us->ts_min_press_ms) {
            us->ts_release_held = true;
            timer_mod(us->ts_release_timer, us->ts_press_at + us->ts_min_press_ms);
            return;
        }
    }
    if (evt->type == INPUT_EVENT_KIND_ABS) {
        us->ts_last_move = walk_now();
        if (us->ts_pace_ms) {
            if (evt->abs.axis == INPUT_AXIS_X) {
                us->ts_x = evt->abs.value;
            } else if (evt->abs.axis == INPUT_AXIS_Y) {
                us->ts_y = evt->abs.value;
            }
            us->ts_pending = true;
            return;
        }
    }
    if (evt->type == INPUT_EVENT_KIND_BTN) {
        ts_pace_feed(us);       /* the edge goes at the newest position */
        us->ts_edge = true;
    }
    us->ts_handler->event((DeviceState *)&us->hid, src, evt);
}

static void walk_ts_sync(DeviceState *dev)
{
    USBHIDState *us = (USBHIDState *)dev;
    int64_t now;

    if (us->walk_toggle) {
        us->walk_toggle = false;
        us->walk_want = true;
        qemu_bh_schedule(us->walk_bh);
        return;
    }
    now = walk_now();
    if (us->ts_edge || !us->ts_pace_ms) {
        us->ts_edge = false;
        ts_pace_feed(us);
        timer_del(us->ts_pace_timer);
        us->ts_handler->sync((DeviceState *)&us->hid);
        us->ts_last = now;
        return;
    }
    if (!us->ts_pending) {
        return;
    }
    if (now - us->ts_last >= us->ts_pace_ms) {
        ts_pace_flush(us);
    } else if (!timer_pending(us->ts_pace_timer)) {
        timer_mod(us->ts_pace_timer, us->ts_last + us->ts_pace_ms);
    }
}

static const QemuInputHandler walk_ts_handler = {
    .name  = "QEMU HID Touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = walk_ts_event,
    .sync  = walk_ts_sync,
};

static bool walk_prop_get(Object *obj, Error **errp)
{
    return USB_HID(obj)->walk;
}

static void walk_prop_set(Object *obj, bool value, Error **errp)
{
    USBHIDState *us = USB_HID(obj);

    if (!us->walk_mode) {
        if (value) {
            error_setg(errp, "usb-touchscreen: walk needs walk-mode=on");
        }
        return;
    }
    if (!us->walk_s) {
        us->walk_want = value;      /* before realize: taken there */
        return;
    }
    walk_set(us, value);
}

static bool walk_mode_get(Object *obj, Error **errp)
{
    return USB_HID(obj)->walk_mode;
}

static void walk_mode_set(Object *obj, bool value, Error **errp)
{
    USBHIDState *us = USB_HID(obj);

    if (us->walk_s && !value && us->walk) {
        walk_set(us, false);
    }
    us->walk_mode = value;
}

static bool ts_hover_get(Object *obj, Error **errp)
{
    return USB_HID(obj)->ts_hover;
}

static void ts_hover_set(Object *obj, bool value, Error **errp)
{
    USB_HID(obj)->ts_hover = value;
}

static bool walk_invert_get(Object *obj, Error **errp)
{
    return USB_HID(obj)->walk_invert_y;
}

static void walk_invert_set(Object *obj, bool value, Error **errp)
{
    USB_HID(obj)->walk_invert_y = value;
}

static bool walk_look_touch_get(Object *obj, Error **errp)
{
    return USB_HID(obj)->walk_look_touch;
}

static void walk_look_touch_set(Object *obj, bool value, Error **errp)
{
    USBHIDState *us = USB_HID(obj);

    walk_keys_up(us);
    us->walk_look_touch = value;
}

/* The head of ui/input.c's QemuInputHandlerState: hid.c registered its handler, which this wraps. */
struct WalkHandlerStateHead {
    DeviceState *dev;
    const QemuInputHandler *handler;
};

/* The options, all off: a device without them is hid.c's touchscreen. */
static void usb_touchscreen_walk_instance_init(Object *obj)
{
    USBHIDState *us = USB_HID(obj);

    object_property_add_bool(obj, "hover", ts_hover_get, ts_hover_set);
    object_property_add_uint32_ptr(obj, "touch-pace-ms", &us->ts_pace_ms, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "touch-min-press-ms", &us->ts_min_press_ms, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "touch-min-hover-ms", &us->ts_min_hover_ms, OBJ_PROP_FLAG_READWRITE);
    object_property_add_bool(obj, "walk-mode", walk_mode_get, walk_mode_set);
    object_property_add_bool(obj, "walk", walk_prop_get, walk_prop_set);
    object_property_add_uint32_ptr(obj, "walk-hold-key", &us->walk_hold_key, OBJ_PROP_FLAG_READWRITE);
    us->walk_hold_ms = 400;
    us->walk_gain = 1500;       /* measured: 200 counts turn visionOS's Home view 377 px of 1920 */
    us->walk_pitch_max = 2600;
    us->walk_x = us->walk_y = (int64_t)WALK_CENTRE * 100;
    object_property_add_uint32_ptr(obj, "walk-gain", &us->walk_gain, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "walk-pitch-max", &us->walk_pitch_max, OBJ_PROP_FLAG_READWRITE);
    object_property_add_bool(obj, "walk-invert-y", walk_invert_get, walk_invert_set);
    object_property_add_uint32_ptr(obj, "walk-hold-ms", &us->walk_hold_ms, OBJ_PROP_FLAG_READWRITE);
    /* look by keys (a first guess from boot 31: Shift+A 0.15 s turned 130 px, 0.4 s 490 px) */
    us->walk_key_gain = 150;
    us->walk_key_min_px = 120;
    us->walk_key_ms_per_px = 9;
    us->walk_key_min_ms = 200;
    us->walk_key_max_ms = 900;
    object_property_add_bool(obj, "walk-look-touch", walk_look_touch_get, walk_look_touch_set);
    object_property_add_uint32_ptr(obj, "walk-key-gain", &us->walk_key_gain, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "walk-key-min-px", &us->walk_key_min_px, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "walk-key-ms-per-px", &us->walk_key_ms_per_px, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "walk-key-min-ms", &us->walk_key_min_ms, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "walk-key-max-ms", &us->walk_key_max_ms, OBJ_PROP_FLAG_READWRITE);
}

/*
 * hid.c's handler behind ours. With every option off ours passes each event and each sync straight on, so the
 * reports are hid.c's; the options can still be turned on later with qom-set.
 */
static void usb_touchscreen_walk_realize(USBHIDState *us)
{
    us->ts_handler = ((struct WalkHandlerStateHead *)us->hid.s)->handler;
    qemu_input_handler_unregister(us->hid.s);
    us->hid.s = qemu_input_handler_register((DeviceState *)us, &walk_ts_handler);
    us->walk_s = qemu_input_handler_register((DeviceState *)us, &walk_handler);
    qemu_input_handler_deactivate(us->walk_s);
    us->walk_timer = timer_new_ms(QEMU_CLOCK_REALTIME, walk_timer_cb, us);
    us->walk_bh = qemu_bh_new(walk_bh_cb, us);
    us->walk_pace_timer = timer_new_ms(QEMU_CLOCK_REALTIME, walk_pace_cb, us);
    us->ts_pace_timer = timer_new_ms(QEMU_CLOCK_REALTIME, ts_pace_flush, us);
    us->ts_release_timer = timer_new_ms(QEMU_CLOCK_REALTIME, ts_release_cb, us);
    us->ts_press_timer = timer_new_ms(QEMU_CLOCK_REALTIME, ts_press_cb, us);
    us->walk_key_timer = timer_new_ms(QEMU_CLOCK_REALTIME, walk_key_tick, us);
    qemu_add_vm_change_state_handler(walk_vm_state, us);
    if (us->walk_mode && us->walk_want) {
        qemu_bh_schedule(us->walk_bh);
    }
}
