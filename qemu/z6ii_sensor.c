/*
 * The Z 6II image sensor register interface
 * 
 * ... on SIO channel 8 and the exposure it asks for,
 * the pixel processing comes from qemu/sensor_model.c
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "expeed6.h"
#include "z6ii.h"
#include "z6ii_fw.h"
#include "sensor_model.h"
#include "migration/vmstate.h"
#include "sysemu/reset.h"

Z6iiSensorState s_sensor = { .pos = -1 };

static bool sensor_exposure_now(double *f);


void sensor_spi_start(void)
{
    Z6iiSensorState *s = &s_sensor;
    double f;
    s->pos = 0;
    if (s->in_still && s->still_dirty && s->on && sensor_exposure_now(&f)) {
        s->still_f = f;
        s->still_dirty = false;
        uint8_t c[0xd8];
        expeed6_fw_peek(s->as, FW_SENSOR_PARAMS_VA, c, sizeof(c));
        const uint32_t b0 = ldl_le_p(c + 0xb0), gr = ldl_le_p(c + 0xcc), gb = ldl_le_p(c + 0xd0);
        s->still_dg = (gr && gb && b0) ? (float)((gr + gb) / 2.0 / 4096.0 * b0 / 4096.0) : 1.0f;
    }
}

void sensor_spi_word(uint16_t w)
{
    Z6iiSensorState *s = &s_sensor;
    const uint8_t b = w & 0xff;
    if (s->pos < 0) {
        return;
    }
    if (s->pos == 0 && b != 0x81) {
        s->pos = -1;
        return;
    }
    if (s->pos == 1) {
        s->addr = b << 8;
    } else if (s->pos == 2) {
        s->addr |= b;
    } else if (s->pos > 2) {
        const uint16_t r = (uint16_t)(s->addr + s->pos - 3);
        if (r == 0x0001) {
            const bool still = b <= 1;
            if (s->in_still && !still && s->still_f > 0 && s->still_done) {

                s->still_done(s->still_opaque, s->still_f, s->still_dg);
            }
            if (still && !s->in_still) {
                s->still_f = 0;
            }
            s->in_still = still;
        }

        s->still_dirty |=
            s->in_still && ((r >= 0x0005 && r <= 0x0006) || (r >= 0x003d && r <= 0x004a) || r == 0x05e1);
        s->reg[r] = b;
    }
    s->pos++;
}

static bool sensor_exposure_now(double *f)
{
    Z6iiSensorState *s = &s_sensor;
    SensorExposure ex;
    if (!s->on || !s->as) {
        return false;
    }
    uint8_t b[0x10];
    expeed6_fw_peek(s->as, FW_SENSOR_PARAMS_VA + 0x94, b, sizeof(b));
    const uint32_t v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    const uint32_t h = b[8] | b[9] << 8;
    const uint32_t ofs = b[0xc] | b[0xd] << 8 | b[0xe] << 16 | (uint32_t)b[0xf] << 24;
    if (sensor_exposure(s->reg, v, h, ofs, &ex)) {
        return false;
    }
    *f = ex.sens / s->e0;
    return true;
}

double sensor_exposure_factor(void)
{
    double f;
    return sensor_exposure_now(&f) ? f : 1.0;
}

SensorScene s_scene;

static void sensor_reset(void *opaque)
{
    Z6iiSensorState *s = opaque;

    memset(s->reg, 0, sizeof(s->reg));
    s->pos = -1;
    s->addr = 0;
    s->in_still = s->still_dirty = false;
    s->still_f = 0;
    s->still_dg = 0;
}

static const VMStateDescription vmstate_sensor = {
    .name = "z6ii-sensor",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){ VMSTATE_UINT8_ARRAY(reg, Z6iiSensorState, SENSOR_REGS),
                                      VMSTATE_INT32(pos, Z6iiSensorState), VMSTATE_UINT16(addr, Z6iiSensorState),
                                      VMSTATE_BOOL(in_still, Z6iiSensorState),
                                      VMSTATE_BOOL(still_dirty, Z6iiSensorState),
                                      VMSTATE_BUFFER_UNSAFE(still_f, Z6iiSensorState, 0, sizeof(double)),
                                      VMSTATE_BUFFER_UNSAFE(still_dg, Z6iiSensorState, 0, sizeof(float)),
                                      VMSTATE_END_OF_LIST() }
};

void z6ii_sensor_config(AddressSpace *as, bool master)
{
    static bool registered;
    if (!registered) {
        registered = true;
        vmstate_register_any(NULL, &vmstate_sensor, &s_sensor);
        qemu_register_reset(sensor_reset, &s_sensor);
    }
    const char *se = getenv("Z6II_SENSOR_EXPOSURE");
    if (master && se && *se && strcmp(se, "0")) {
        char *end;
        const double t0 = g_ascii_strtod(se, &end);
        const double iso0 = *end == ',' ? g_ascii_strtod(end + 1, NULL) : 0;
        if (t0 <= 0 || iso0 <= 0) {
            error_report("Z6II_SENSOR_EXPOSURE=<t0_s>,<iso0>: the stand-in frame's shutter time and ISO");
            exit(1);
        }
        s_sensor.on = true;
        s_sensor.e0 = t0 * iso0 / 100;
        s_sensor.as = as;
    }
}
