/*
 * The Z 6II Ex MCU
 *
 * The main board's STM32F412 that owns the switches, buttons and battery:
 * the blocks pushed to the master unsolicited (Z6II_EXMCU_PUSH),
 * and the camera's buttons on the display window's keys (Z6II_KEYS).
 *
 * References:
 * - Magic Lantern's QEMU (qemu-eos):
 *   the host-key-to-button concept and its key layout and help (hw/eos/mpu.c)
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "expeed6_sio.h"
#include "z6ii.h"
#include "migration/vmstate.h"
#include "ui/input.h"
#include "qapi/qapi-types-ui.h"

static void exmcu_push_bytes(Z6iiExmcuState *s, const uint8_t *blk, const char *what)
{
    if (s->rq_tail - s->rq_head >= ARRAY_SIZE(s->rq)) {
        s->rq_head++;
    }
    memcpy(s->rq[s->rq_tail % ARRAY_SIZE(s->rq)], blk, 256);
    s->rq_tail++;
    exmcu_reply_pin_update(s);
    if (s->extint) {
        extint_raise(s->extint, 3);
    }
}

static bool exmcu_push_block(Z6iiExmcuState *s, const char *path)
{
    gchar *data = NULL;
    gsize len = 0;

    if (!s || !g_file_get_contents(path, &data, &len, NULL) || len < 256) {
        g_free(data);
        return false;
    }
    exmcu_push_bytes(s, (const uint8_t *)data, path);
    g_free(data);
    return true;
}

void exmcu_push_prop_set(Object *obj, const char *value, Error **errp)
{
    if (!s_exmcu) {
        error_setg(errp, "exmcu-push: no Ex MCU link on this machine");
        return;
    }
    if (!exmcu_push_block(s_exmcu, value)) {
        error_setg(errp, "exmcu-push: %s is not a >=256-byte block", value);
    }
}

void exmcu_schedule_prop_set(Object *obj, const char *value, Error **errp)
{
    if (!s_exmcu) {
        error_setg(errp, "exmcu-schedule: no Ex MCU link on this machine");
        return;
    }
    g_auto(GStrv) toks = g_strsplit(value, ";", -1);
    unsigned count = 0;
    for (char **t = toks; *t; t++) {
        const char *at = strrchr(*t, '@');
        char *end;
        double sec = at ? strtod(at + 1, &end) : 22.0;
        if (!**t) {
            continue;
        }
        if (at && (end == at + 1 || (*end && *end != '+' && *end != '/'))) {
            error_setg(errp, "exmcu-schedule: '%s' is not <file>@<seconds>[+<period>]", *t);
            return;
        }
        if ((int64_t)(sec * 1e9) < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
            error_setg(errp, "exmcu-schedule: '%s' is in the past", *t);
            return;
        }
        count++;
    }
    if (s_exmcu->n_push + count > EXMCU_PUSH_MAX) {
        error_setg(errp, "exmcu-schedule: %u entries would exceed %d (EXMCU_PUSH_MAX)", s_exmcu->n_push + count,
                   EXMCU_PUSH_MAX);
        return;
    }
    unsigned n0 = s_exmcu->n_push;
    exmcu_push_schedule(s_exmcu, value);
    for (unsigned i = n0; i < s_exmcu->n_push; i++) {
        ExmcuPush *p = &s_exmcu->push[i];
        p->due_ns = p->first_ns;
        timer_mod(p->t, p->due_ns);
    }
}

typedef enum { NK_BUTTON, NK_DIAL, NK_LEVER, NK_HELP } Z6iiKeyRoute;

#define NK_NO_GPIO 0xff

typedef struct {
    QKeyCode qcode;
    Z6iiKeyRoute route;
    uint8_t off;
    uint8_t off2;
    uint8_t gpio_bit;
    int8_t arg;
    const uint8_t *order;
    const char *pc_key;
    const char *button;
} Z6iiKey;

#define B(q, off, name)                                                                                           \
    {                                                                                                             \
        q, NK_BUTTON, off, 0, NK_NO_GPIO, 0, NULL, NULL, name                                                     \
    }
#define BG(q, off, pc, name)                                                                                      \
    {                                                                                                             \
        q, NK_BUTTON, off, 0, NK_NO_GPIO, 0, NULL, pc, name                                                       \
    }

static const uint8_t z6ii_mode_dial[] = { 10, 0, 1, 2, 3, 4, 5, 6 };
static const char *const z6ii_mode_dial_name[] = { "AUTO", "P", "S", "A", "M", "U1", "U2", "U3" };
#define Z6II_MODE_DIAL_N 8

static const Z6iiKey z6ii_keys[] = {
    BG(Q_KEY_CODE_UP, 0x24, "Arrow keys", "multi-selector"),
    B(Q_KEY_CODE_LEFT, 0x23, "multi-selector left"),
    B(Q_KEY_CODE_DOWN, 0x25, "multi-selector down"),
    B(Q_KEY_CODE_RIGHT, 0x22, "multi-selector right"),
    BG(Q_KEY_CODE_KP_8, 0x2d, "Numpad", "sub-selector (8 directions)"),
    B(Q_KEY_CODE_KP_9, 0x2f, "sub-selector up-right"),
    B(Q_KEY_CODE_KP_6, 0x2b, "sub-selector right"),
    B(Q_KEY_CODE_KP_3, 0x30, "sub-selector down-right"),
    B(Q_KEY_CODE_KP_2, 0x2e, "sub-selector down"),
    B(Q_KEY_CODE_KP_1, 0x32, "sub-selector down-left"),
    B(Q_KEY_CODE_KP_4, 0x2c, "sub-selector left"),
    B(Q_KEY_CODE_KP_7, 0x31, "sub-selector up-left"),
    BG(Q_KEY_CODE_KP_5, 0x33, "Numpad 5", "sub-selector centre"),
    { Q_KEY_CODE_PGUP, NK_DIAL, 0x0b, 0, NK_NO_GPIO, 1, NULL, "PgUp, PgDn", "main command dial (rear)" },
    { Q_KEY_CODE_PGDN, NK_DIAL, 0x0b, 0, NK_NO_GPIO, -1, NULL, NULL, "main command dial (rear)" },
    { Q_KEY_CODE_BRACKET_RIGHT, NK_DIAL, 0x0c, 0, NK_NO_GPIO, -1, NULL, "[ and ]", "sub-command dial (front)" },
    { Q_KEY_CODE_BRACKET_LEFT, NK_DIAL, 0x0c, 0, NK_NO_GPIO, 1, NULL, NULL, "sub-command dial (front)" },
    BG(Q_KEY_CODE_SPC, 0x4a, "Space, Enter", "OK"),
    B(Q_KEY_CODE_RET, 0x4a, "OK"),
    BG(Q_KEY_CODE_DELETE, 0x45, "Delete", "delete"),
    BG(Q_KEY_CODE_M, 0x43, "M", "MENU"),
    BG(Q_KEY_CODE_P, 0x44, "P", "playback"),
    BG(Q_KEY_CODE_I, 0x48, "I", "i (the i menu)"),
    BG(Q_KEY_CODE_D, 0x53, "D", "DISP"),
    BG(Q_KEY_CODE_F, 0x3b, "F", "Fn1"),
    BG(Q_KEY_CODE_G, 0x3c, "G", "Fn2"),
    BG(Q_KEY_CODE_A, 0x1b, "A", "AF-ON"),
    BG(Q_KEY_CODE_O, 0x16, "O", "ISO"),
    BG(Q_KEY_CODE_E, 0x15, "E", "exposure compensation"),
    BG(Q_KEY_CODE_Z, 0x4b, "Z/X", "zoom in/out"),
    B(Q_KEY_CODE_X, 0x4c, "zoom out"),
    { Q_KEY_CODE_SHIFT, NK_BUTTON, 0x07, 0, 11, 0, NULL, "Shift", "shutter half-press" },
    { Q_KEY_CODE_SHIFT_R, NK_BUTTON, 0x07, 0, 11, 0, NULL, NULL, "shutter half-press" },
    { Q_KEY_CODE_S, NK_BUTTON, 0x08, 0x07, 12, 0, NULL, "S", "shutter full press" },
    BG(Q_KEY_CODE_R, 0x09, "R", "movie-record"),
    { Q_KEY_CODE_0, NK_LEVER, 0x0f, 0, NK_NO_GPIO, 1, z6ii_mode_dial, "0/9", "mode dial" },
    { Q_KEY_CODE_9, NK_LEVER, 0x0f, 0, NK_NO_GPIO, -1, z6ii_mode_dial, NULL, "mode dial" },
    { Q_KEY_CODE_V, NK_LEVER, 0x57, 0, NK_NO_GPIO, 0, NULL, "V", "photo/movie selector" },
    { Q_KEY_CODE_F1, NK_HELP, 0, 0, NK_NO_GPIO, 0, NULL, "F1", "show this help" },
};

#undef B
#undef BG

#define Z6II_STATUS_PLEN EXMCU_STATUS_PLEN

static struct {
    bool held[ARRAY_SIZE(z6ii_keys)];
    int32_t lever[0x80];
} s_keys;

static void z6ii_keys_overlay(uint8_t *payload, bool all)
{
    for (unsigned i = 0; all && i < ARRAY_SIZE(z6ii_keys); i++) {
        const Z6iiKey *k = &z6ii_keys[i];
        if (k->route == NK_BUTTON || k->route == NK_DIAL) {
            payload[k->off] = 0;
            if (k->off2) {
                payload[k->off2] = 0;
            }
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(z6ii_keys); i++) {
        const Z6iiKey *k = &z6ii_keys[i];
        if (!s_keys.held[i]) {
            continue;
        }
        if (k->route == NK_BUTTON) {
            payload[k->off] = 1;
            if (k->off2) {
                payload[k->off2] = 1;
            }
        } else if (k->route == NK_DIAL && all) {
            payload[k->off] = (uint8_t)(payload[k->off] + k->arg);
        }
    }
    for (unsigned off = 0; off < ARRAY_SIZE(s_keys.lever); off++) {
        if (s_keys.lever[off] >= 0) {
            payload[off] = s_keys.lever[off];
        }
    }
}

static void exmcu_block_checksum(uint8_t *blk)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 0xff; i++) {
        sum += blk[i];
    }
    blk[0xff] = (uint8_t)(0xff - sum);
}

void expeed6_keys_overlay_reply(uint8_t *blk)
{
    if ((blk[0] == 2 || blk[0] == 8) && blk[1] == 2 && blk[8] == 1) {
        uint8_t before[Z6II_STATUS_PLEN];
        memcpy(before, blk + 8, sizeof(before));
        z6ii_keys_overlay(blk + 8, false);
        if (memcmp(before, blk + 8, sizeof(before))) {
            exmcu_block_checksum(blk);
        }
    }
}

static bool z6ii_keys_peer_status(Z6iiExmcuState *s, uint8_t *payload)
{
    if (!s->status_valid) {
        return false;
    }
    memcpy(payload, s->status_last, Z6II_STATUS_PLEN);
    return payload[0] == 1;
}

static void z6ii_keys_send_status(Z6iiExmcuState *s)
{
    uint8_t blk[256] = { 0 };
    uint8_t *payload = blk + 8;

    if (!z6ii_keys_peer_status(s, payload)) {
        memset(payload, 0, Z6II_STATUS_PLEN);
        payload[0x04] = 1;
        payload[0x0f] = 1;
    }
    payload[0] = 1;
    z6ii_keys_overlay(payload, true);
    blk[0] = 2;
    blk[1] = 2;
    blk[2] = Z6II_STATUS_PLEN;
    blk[0xfc] = 1;
    blk[0xfd] = 1;
    exmcu_block_checksum(blk);
    exmcu_push_bytes(s, blk, "key");
}

static void z6ii_keys_help(void)
{
    fprintf(stderr, "expeed6.keys: the camera's buttons (layout after Magic Lantern's QEMU):\n");
    for (unsigned i = 0; i < ARRAY_SIZE(z6ii_keys); i++) {
        const Z6iiKey *k = &z6ii_keys[i];
        if (k->pc_key) {
            fprintf(stderr, "  %-12s : %s\n", k->pc_key, k->button);
        }
    }
}

static double z6ii_keys_vsec(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9;
}

static const Z6iiKey *z6ii_key_by_off(uint8_t off)
{
    for (unsigned i = 0; i < ARRAY_SIZE(z6ii_keys); i++) {
        if (z6ii_keys[i].route == NK_BUTTON && z6ii_keys[i].off == off && z6ii_keys[i].gpio_bit != NK_NO_GPIO) {
            return &z6ii_keys[i];
        }
    }
    return NULL;
}

static void expeed6_keys_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    InputKeyEvent *key = evt->u.key.data;
    const int qcode = qemu_input_key_value_to_qcode(key->key);

    for (unsigned i = 0; i < ARRAY_SIZE(z6ii_keys); i++) {
        const Z6iiKey *k = &z6ii_keys[i];
        if (k->qcode != qcode || s_keys.held[i] == key->down) {
            continue;
        }
        s_keys.held[i] = key->down;
        switch (k->route) {
        case NK_BUTTON:
            fprintf(stderr, "expeed6.keys: %s %s at %.3f s\n", k->button, key->down ? "down" : "up",
                    z6ii_keys_vsec());
            if (k->gpio_bit != NK_NO_GPIO && s_master_gpio) {
                const Z6iiKey *k2 = k->off2 ? z6ii_key_by_off(k->off2) : NULL;
                if (k2 && key->down) {
                    gpio_drive_input(s_master_gpio, 0x14, k2->gpio_bit, true);
                }
                gpio_drive_input(s_master_gpio, 0x14, k->gpio_bit, key->down);
                if (k2 && !key->down && !s_keys.held[k2 - z6ii_keys]) {
                    gpio_drive_input(s_master_gpio, 0x14, k2->gpio_bit, false);
                }
            }
            if (s_exmcu) {
                z6ii_keys_send_status(s_exmcu);
            }
            break;
        case NK_DIAL:
            if (key->down) {
                fprintf(stderr, "expeed6.keys: %s %+d at %.3f s\n", k->button, k->arg, z6ii_keys_vsec());
            }
            if (s_exmcu) {
                z6ii_keys_send_status(s_exmcu);
            }
            break;
        case NK_LEVER:
            if (!key->down) {
                break;
            }
            if (s_keys.lever[k->off] < 0) {
                uint8_t cur[Z6II_STATUS_PLEN];
                s_keys.lever[k->off] = s_exmcu && z6ii_keys_peer_status(s_exmcu, cur) ? cur[k->off] : 0;
            }
            if (k->arg) {
                unsigned pos = 0;
                while (pos < Z6II_MODE_DIAL_N && k->order[pos] != s_keys.lever[k->off]) {
                    pos++;
                }
                pos = (pos == Z6II_MODE_DIAL_N) ? 0 : (pos + Z6II_MODE_DIAL_N + k->arg) % Z6II_MODE_DIAL_N;
                s_keys.lever[k->off] = k->order[pos];
                fprintf(stderr, "expeed6.keys: %s -> %s (%d)\n", k->button, z6ii_mode_dial_name[pos],
                        s_keys.lever[k->off]);
            } else {
                s_keys.lever[k->off] = !s_keys.lever[k->off];
                fprintf(stderr, "expeed6.keys: %s -> %d\n", k->button, s_keys.lever[k->off]);
            }
            if (s_exmcu) {
                z6ii_keys_send_status(s_exmcu);
            }
            break;
        case NK_HELP:
            if (key->down) {
                z6ii_keys_help();
            }
            break;
        }
    }
}

static const QemuInputHandler z6ii_keys_handler = {
    .name = "Nikon camera buttons",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = expeed6_keys_event,
};

typedef struct {
    QKeyCode q;
    bool down;
} Z6iiKeyAt;

static void z6ii_keys_timer(void *opaque)
{
    Z6iiKeyAt *k = opaque;
    qemu_input_event_send_key_qcode(NULL, k->q, k->down);
    qemu_input_event_sync();
}

static void z6ii_keys_schedule(const char *spec)
{
    g_autofree char *dup = g_strdup(spec);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        char *at = strrchr(tok, '@');
        int q = at ? qapi_enum_parse(&QKeyCode_lookup, (*at = 0, tok), -1, NULL) : -1;
        if (q < 0) {
            warn_report("z6ii: Z6II_KEYS: ignoring %s (want <key>@<seconds>)", tok);
            continue;
        }
        const char *slash = strchr(at + 1, '/');
        const int64_t t = (int64_t)(atof(at + 1) * 1e9);
        const int64_t hold = slash ? (int64_t)(atof(slash + 1) * 1e9) : 100 * SCALE_MS;
        for (int down = 1; down >= 0; down--) {
            Z6iiKeyAt *k = g_new0(Z6iiKeyAt, 1);
            *k = (Z6iiKeyAt){ .q = q, .down = down };
            timer_mod(timer_new_ns(QEMU_CLOCK_VIRTUAL, z6ii_keys_timer, k), down ? t : t + hold);
        }
    }
}

static const VMStateDescription vmstate_z6ii_keys = {
    .name = "z6ii-keys",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_BOOL_ARRAY(held, typeof(s_keys), ARRAY_SIZE(z6ii_keys)),
                                      VMSTATE_INT32_ARRAY(lever, typeof(s_keys), 0x80), VMSTATE_END_OF_LIST() }
};

void z6ii_keys_init(void)
{
    for (unsigned off = 0; off < ARRAY_SIZE(s_keys.lever); off++) {
        s_keys.lever[off] = -1;
    }
    vmstate_register_any(NULL, &vmstate_z6ii_keys, &s_keys);
    qemu_input_handler_activate(qemu_input_handler_register(NULL, &z6ii_keys_handler));
    if (getenv("Z6II_KEYS")) {
        expeed6_unmigrated("the scheduled key presses (Z6II_KEYS)");
        z6ii_keys_schedule(getenv("Z6II_KEYS"));
    }
}

static void exmcu_push_timer(void *opaque)
{
    ExmcuPush *p = opaque;
    if (!exmcu_push_block(p->s, p->path)) {
        warn_report("z6ii: Z6II_EXMCU_PUSH: %s is not a >=256-byte block", p->path);
        return;
    }
    if (p->period_ns > 0) {
        p->due_ns += p->period_ns;
        timer_mod(p->t, p->due_ns);
    }
}

void exmcu_push_init(Z6iiExmcuState *s)
{
    for (unsigned i = 0; i < EXMCU_PUSH_MAX; i++) {
        s->push[i].s = s;
        s->push[i].t = timer_new_ns(QEMU_CLOCK_VIRTUAL, exmcu_push_timer, &s->push[i]);
    }
}

void exmcu_push_arm(Z6iiExmcuState *s)
{
    for (unsigned i = 0; i < s->n_push; i++) {
        s->push[i].due_ns = s->push[i].first_ns;
        timer_mod(s->push[i].t, s->push[i].due_ns);
    }
}

void exmcu_push_schedule(Z6iiExmcuState *s, const char *spec)
{
    char *dup = g_strdup(spec);
    for (char *tok = strtok(dup, ";"); tok; tok = strtok(NULL, ";")) {
        double at = 22.0, period = 0;
        char *at_sign = strrchr(tok, '@');
        if (at_sign) {
            char *plus = strchr(at_sign + 1, '+');
            *at_sign = 0;
            char *slash = strchr(at_sign + 1, '/');
            if (slash) {
                period = atof(slash + 1);
            }
            at = atof(at_sign + 1);
            if (plus) {
                period = atof(plus + 1);
            }
        }
        if (s->n_push >= EXMCU_PUSH_MAX) {
            error_report("Z6II_EXMCU_PUSH: more than %d entries (raise EXMCU_PUSH_MAX)", EXMCU_PUSH_MAX);
            exit(1);
        }
        ExmcuPush *p = &s->push[s->n_push++];
        p->s = s;
        p->path = g_strdup(tok);
        p->period_ns = (int64_t)(period * 1e9);
        p->first_ns = p->due_ns = (int64_t)(at * 1e9); /* armed by the SIO's reset */
    }
    g_free(dup);
}
