/*
 * The NXP MMA8452Q 3-axis accelerometer, I2C slave
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "hw/i2c/mma845x.h"

OBJECT_DECLARE_SIMPLE_TYPE(Mma8452qState, MMA8452Q)

#define MMA_NREGS 0x32

#define MMA_STATUS        0x00
#define MMA_OUT_X_MSB     0x01
#define MMA_OUT_Z_LSB     0x06
#define MMA_SYSMOD        0x0b
#define MMA_INT_SOURCE    0x0c
#define MMA_WHO_AM_I      0x0d
#define MMA_XYZ_DATA_CFG  0x0e
#define MMA_PL_STATUS     0x10
#define MMA_FF_MT_SRC     0x16
#define MMA_TRANSIENT_SRC 0x1e
#define MMA_PULSE_SRC     0x22
#define MMA_CTRL_REG1     0x2a
#define MMA_CTRL_REG2     0x2b

#define MMA_WHO_AM_I_VAL 0x2a
#define MMA_CTRL1_ACTIVE 0x01
#define MMA_CTRL1_F_READ 0x02
#define MMA_CTRL2_RST    0x40
#define MMA_STATUS_ZYXDR 0x08
#define MMA_STATUS_ZYXOW 0x80

struct Mma8452qState {
    I2CSlave parent_obj;

    uint8_t regs[MMA_NREGS];
    uint8_t ptr;
    bool have_ptr;
    int32_t mg[3];
    int16_t latched[3];
    int64_t sample_base_ns;
    int64_t consumed;
};

static const int64_t mma_period_ns[8] = {
    1250000, 2500000, 5000000, 10000000, 20000000, 80000000, 160000000, 640000000,
};

static bool mma_active(Mma8452qState *s)
{
    return s->regs[MMA_CTRL_REG1] & MMA_CTRL1_ACTIVE;
}

static int64_t mma_sample_index(Mma8452qState *s)
{
    if (!mma_active(s)) {
        return -1;
    }
    int64_t dt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->sample_base_ns;
    return dt / mma_period_ns[(s->regs[MMA_CTRL_REG1] >> 3) & 7];
}

static uint8_t mma_status(Mma8452qState *s)
{
    int64_t pending = mma_sample_index(s) - s->consumed;

    if (pending <= 0) {
        return 0;
    }
    return 0x0f | (pending > 1 ? 0xf0 : 0);
}

static void mma_latch(Mma8452qState *s)
{
    unsigned fs = s->regs[MMA_XYZ_DATA_CFG] & 3;

    for (int i = 0; i < 3; i++) {
        int32_t v = (int32_t)((int64_t)s->mg[i] * 1024 / 1000) >> (fs == 3 ? 2 : fs);

        s->latched[i] = MAX(-2048, MIN(2047, v));
    }
    s->consumed = mma_sample_index(s);
}

static uint8_t mma_read_reg(Mma8452qState *s, uint8_t r)
{
    if (r == MMA_STATUS) {
        return mma_status(s);
    }
    if (r >= MMA_OUT_X_MSB && r <= MMA_OUT_Z_LSB) {
        int16_t v = s->latched[(r - MMA_OUT_X_MSB) / 2];

        return (r & 1) ? (uint8_t)(v >> 4) : (uint8_t)(v << 4);
    }
    if (r == MMA_SYSMOD) {
        return mma_active(s) ? 1 : 0;
    }
    return r < MMA_NREGS ? s->regs[r] : 0;
}

static void mma_reset_regs(Mma8452qState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MMA_WHO_AM_I] = MMA_WHO_AM_I_VAL;
    s->ptr = 0;
    s->have_ptr = false;
    s->consumed = -1;
    memset(s->latched, 0, sizeof(s->latched));
}

static int mma_event(I2CSlave *i2c, enum i2c_event event)
{
    Mma8452qState *s = MMA8452Q(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->have_ptr = false;
        break;
    case I2C_START_RECV:
        break;
    case I2C_FINISH:
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t mma_recv(I2CSlave *i2c)
{
    Mma8452qState *s = MMA8452Q(i2c);
    uint8_t r = s->ptr;
    bool fast = s->regs[MMA_CTRL_REG1] & MMA_CTRL1_F_READ;

    if (r == MMA_OUT_X_MSB) {
        mma_latch(s);
    }
    uint8_t v = mma_read_reg(s, r);

    if (fast && (r == 0x01 || r == 0x03)) {
        s->ptr = r + 2;
    } else if (fast && r == 0x05) {
        s->ptr = 0x00;
    } else {
        s->ptr = (r + 1) % MMA_NREGS;
    }
    return v;
}

static bool mma_read_only(uint8_t r)
{
    return r <= MMA_OUT_Z_LSB || r == MMA_SYSMOD || r == MMA_INT_SOURCE || r == MMA_WHO_AM_I ||
           r == MMA_PL_STATUS || r == MMA_FF_MT_SRC || r == MMA_TRANSIENT_SRC || r == MMA_PULSE_SRC ||
           r >= MMA_NREGS;
}

static void mma_write_reg(Mma8452qState *s, uint8_t r, uint8_t v)
{
    if (mma_read_only(r)) {
        return;
    }
    if (r == MMA_CTRL_REG2 && (v & MMA_CTRL2_RST)) {
        mma_reset_regs(s);
        return;
    }
    if (r == MMA_CTRL_REG1) {
        bool was = mma_active(s);

        s->regs[r] = v;
        if (!was && mma_active(s)) {
            s->sample_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->consumed = 0;
        }
        return;
    }
    if (mma_active(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mma8452q: reg 0x%02x written while ACTIVE "
                      "(the part wants STANDBY for configuration)\n",
                      r);
    }
    s->regs[r] = v;
}

static int mma_send(I2CSlave *i2c, uint8_t data)
{
    Mma8452qState *s = MMA8452Q(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        mma_write_reg(s, s->ptr, data);
        s->ptr = (s->ptr + 1) % MMA_NREGS;
    }
    return 0;
}

static void mma_reset_hold(Object *obj, ResetType type)
{
    mma_reset_regs(MMA8452Q(obj));
}

static void mma_realize(DeviceState *dev, Error **errp)
{
    Mma8452qState *s = MMA8452Q(dev);

    s->mg[0] = -1016;
    s->mg[1] = -34;
    s->mg[2] = -91;
    mma_reset_regs(s);
}

static const VMStateDescription vmstate_mma = {
    .name = TYPE_MMA8452Q,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){ VMSTATE_I2C_SLAVE(parent_obj, Mma8452qState),
                                      VMSTATE_UINT8_ARRAY(regs, Mma8452qState, MMA_NREGS),
                                      VMSTATE_UINT8(ptr, Mma8452qState), VMSTATE_BOOL(have_ptr, Mma8452qState),
                                      VMSTATE_INT32_ARRAY(mg, Mma8452qState, 3),
                                      VMSTATE_INT16_ARRAY(latched, Mma8452qState, 3),
                                      VMSTATE_INT64(sample_base_ns, Mma8452qState),
                                      VMSTATE_INT64(consumed, Mma8452qState), VMSTATE_END_OF_LIST() }
};

static void mma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = mma_realize;
    dc->vmsd = &vmstate_mma;
    k->event = mma_event;
    k->recv = mma_recv;
    k->send = mma_send;
    rc->phases.hold = mma_reset_hold;
}

static const TypeInfo mma_info = {
    .name = TYPE_MMA8452Q,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Mma8452qState),
    .class_init = mma_class_init,
};

static void mma_register_types(void)
{
    type_register_static(&mma_info);
}

type_init(mma_register_types)
