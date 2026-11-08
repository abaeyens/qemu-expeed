/*
 * The QEMU machine `z6ii`
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_H
#define Z6II_H

#include "expeed6.h"
#include "sensor_model.h"

#define IMAGE_BASE 0x10000000ULL
#define IMAGE_SIZE 0x04000000ULL

#define RAM_BASE 0x80100000ULL
#define RAM_SIZE 0x7FF00000ULL

#define RING_MAS  0xc7ef0000ULL
#define RING_SIZE 0x00010000ULL


#define SENSOR_SIO_CH    8
#define SENSOR_WHITE_LIN ((double)SENSOR_WHITE14 - SENSOR_BLACK14)

typedef struct {
    bool on;
    double e0;
    AddressSpace *as;
    uint8_t reg[SENSOR_REGS];
    int pos;
    uint16_t addr;

    bool in_still;
    bool still_dirty;
    double still_f;
    float still_dg;
    void (*still_done)(void *opaque, double f, float dg);
    void *still_opaque;
} Z6iiSensorState;

extern Z6iiSensorState s_sensor;
void sensor_spi_start(void);
void sensor_spi_word(uint16_t w);
double sensor_exposure_factor(void);

extern SensorScene s_scene;
void z6ii_sensor_config(AddressSpace *as, bool master);

void exmcu_push_schedule(Z6iiExmcuState *s, const char *spec);
void exmcu_push_init(Z6iiExmcuState *s);
void exmcu_push_arm(Z6iiExmcuState *s);
void exmcu_push_prop_set(Object *obj, const char *value, Error **errp);
void exmcu_schedule_prop_set(Object *obj, const char *value, Error **errp);
void z6ii_keys_init(void);
void expeed6_keys_overlay_reply(uint8_t *blk);

void z6ii_image_init(MemoryRegion *image);

#endif /* Z6II_H */
