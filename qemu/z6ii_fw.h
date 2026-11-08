/*
 * The memory addresses the machine models read
 *
 * Preferably, the machine models only read their registers.
 * However, sometimes it's not clear which register, or how to interpret
 * a particular register, and in that case one can apply the ugly hack
 * of letting the model read some particular memory directly.
 * Avoid doing this because this form of "emulation" doesn't generalize
 * to other firmware versions and definitely not to custom code.
 * But if you do it, put the relevant addresses in this file.
 *
 * All addresses in this file are for the Nikon Z 6II v1.70 (Z6_2_0170.bin).
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef Z6II_FW_H
#define Z6II_FW_H

#define FW_SENSOR_PARAMS_VA 0xc18a7694u

#endif
