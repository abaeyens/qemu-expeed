# Copyright (c) 2026 Arne Baeyens
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the COPYING file in the top-level directory.
# SPDX-License-Identifier: GPL-2.0-or-later
"""The synthetic parameter flash: the regions a boot needs, with no camera's data in them.

Every Nikon Z 6II carries per-body and per-model data (factory tuning, calibration, the sensor's
parameter blob) in the flash above the firmware image, and the update file has none of it.
Some of it is needed to boot and to run live view and a release; this declares exactly those
fields, each in its own unit, and leaves everything else zero:

  - the overheat limits of the temperature channels and of the battery pack: with zeros the
    firmware reports an overheated camera at boot;
  - the accelerometer service's tuning, without which its task never starts;
  - the flicker detector's frame count, without which live view never arms;
  - auto exposure's tuning (its metering bias, convergence gains and steps, slew limits and
    thresholds); the network weights it also holds stay zero, which makes it meter the plain
    average of the picture;
  - from the sensor parameter blob its version byte, black levels, exposure-timing, gain and
    register tables, the sensor command that selects the drive mode, and the readout sizes.

Values are the camera's own for a single limit, tuning value, design constant or register value,
and the ideal one for per-body calibration (gain trims 1.0, gain codes from the gain law).
No bulk table, command set, calibration data or network weight is copied.

make_flash_image.py puts the result into the flash image the machine boots from.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binstruct import U8, U16, U32, Record, Tagged, scaled  # noqa: E402
from param_flash import ParamFlash  # noqa: E402

DEGC16 = scaled(U32, 16)  # 1/16 degC, the I2C temperature sensor's reading
EV = scaled(U32, 192)  # the firmware's 1/192 EV
GAIN = scaled(U32, 192)  # 192 = 1.0
STEP = scaled(U32, 120)  # fraction of the error per frame, in 1/120
Q12 = scaled(U16, 4096)

# --- container 0x11: factory tuning, tagged regions -------------------------------------------

TEMP = Tagged(  # 0x13
    "TEMP",
    1,
    i2c_limit=DEGC16[3] @ 0x24,  # channel 2, levels 0..2: hot at reading >= limit
    # channels 0 and 1, 10-bit readings that fall as it warms: hot at reading <= limit
    thermistor0_limit=U32[3] @ 0x30,
    thermistor1_limit=U32[2] @ 0x3C,
)

ACCEL = Record(  # 0x15; untagged, as the firmware ignores the name check
    poll_ms=U32 @ 0x20,
    mma_spike_thr=U32 @ 0x24,
    mma_spike_limit=U32 @ 0x28,
    mean_n=U32 @ 0x2C,
    hysteresis=U32[4] @ 0x3C,  # degrees, per sector
    mma_regs=U32[25] @ 0x4C,  # one MMA845x register per word
)
MMA_CTRL_REG1, MMA_CTRL_REG2 = 17, 18  # indices into mma_regs

BATTERY = Tagged(  # 0x1c
    "Battery", 7, pack_temp_limit=U8 @ 0xE4, pack_temp_limit_2=U8 @ 0xE8  # degC
)

SZFLK = Tagged("SZFLK", 4, frames=U32 @ 0x848)  # 0x23, frames the flicker detector accumulates

AE = Tagged(  # 0x24; the network's weights from +0x600 stay zero
    "AE",
    9,
    weight_base=GAIN[2] @ 0x3C,  # the radial weight maps' base; their strengths stay 0: flat
    meter_bias=EV @ 0x60,
    cell_floor=U32 @ 0x98,  # raw counts
    slew_limit=EV @ 0x330,
    slew_limit_2=EV @ 0x33C,
    converge_gain=GAIN[15] @ 0x350,
    converge_step=STEP[3] @ 0x38C,
    converge_gain_2=GAIN[4] @ 0x398,
    dark_threshold=U32 @ 0x3B8,  # raw counts per cell
    bright_threshold=U32 @ 0x3C0,
    dark_threshold_2=U32 @ 0x3C4,
    flag_zone_count=U32[2] @ 0x3C8,  # zones of 25 that raise a scene flag
    # whole EV: the upper and lower limit of a still's two exposure values; zero refuses a release
    still_ev_limit=Record(upper=U32 @ 0, lower=U32 @ 4)[2] @ 0x3D0,
)

# --- region 0x50: the sensor parameter blob ----------------------------------------------------

TIMING = Record(  # per readout mode and rate
    0x10,
    line_length=U16[2] @ 2,  # 72 MHz counts
    lines=U16 @ 6,
    width=U16 @ 8,  # pixels
    offset=U16 @ 0xA,  # integration offset, 72 MHz counts
)

SENSOR_BLOB = Record(
    readout_mode=Record(0x50, width=U16 @ 0x10, height=U16 @ 0x18)[30] @ 0xEA0,
    black_level=Record(12, level=U16 @ 6)[265] @ 0xC8E00,
    version=U8 @ 0xCE014,  # the boot checks it
    timing=TIMING[4][15] @ 0xCE180,
    gain_code=Record(0x10, code=U16[2] @ 0)[10] @ 0xCE540,  # per ISO step; G = 4095/(4095-code)
    gain_code_2=Record(0x10, code=U16[2] @ 0)[10] @ 0xCE5F0,
    gain_mode_regs=U8[0x20][3] @ 0xCE6A0,
    channel_trim=Q12[4][10] @ 0xCE830,  # per ISO step
    mode_cmds=U8[0x200][17] @ 0xCF770,  # one sensor command set per readout mode
    mode_cmd_lengths=U16[2] @ 0xD1B70,  # each command's length, 0 ends the list
)

SENSOR_BLACK = 1008  # 14-bit; the sensor model's black level
SENSOR_TIMING = [  # (line_length, lines, width, offset) per readout mode 0..13
    (911, 4270, 6104, 532),
    (470, 4270, 6104, 374),
    (470, 3640, 6104, 374),
    (470, 2478, 3976, 374),
    (470, 1418, 6104, 374),
    (330, 1368, 2036, 252),
    (470, 2478, 3976, 374),
    (470, 1610, 6104, 374),
    (330, 196, 2036, 252),
    (330, 196, 2036, 252),
    (470, 4270, 6104, 374),
    (911, 4270, 6104, 532),
    (470, 4270, 6104, 374),
    (470, 912, 6104, 374),
]
READOUT_SIZES = (  # (width, height) per record: a 2-line read before each mode's own
    6 * [(6104, 2), (6104, 4040)]  # full frame
    + [(6104, 2), (6104, 3412), (3976, 2), (3976, 2250)]  # 4K FX, 4K DX
    + [(6104, 2), (6104, 1138), (2034, 2), (2034, 1138)]  # 1080 FX, 1080/120p
    + [(3976, 2), (3976, 2250), (2034, 2), (2034, 1342)]  # 4K DX, photo live view
    + [(6104, 2), (6104, 4040), (2034, 136), (2034, 136)]  # full frame, a 136-line strip
    + [(2034, 2), (2034, 672)]  # a 672-line band
)
# analog gain per ISO step from ISO 100: G = 1.121 x ISO/100, from ISO 800 the high conversion
# gain's 1.761 x ISO/800; code = 4095 (1 - 1/G), capped where the sensor stops
GAIN_LAW = [1.121, 2.242, 4.484, 1.761, 3.522, 7.044, 14.088, 28.176, 56.352]
GAIN_CODE_CAP = 3966
GAIN_MODE_REGS = [  # the sensor's register bytes per gain mode
    bytes.fromhex("0001000100000100010001000100"),
    bytes.fromhex("292a2929000e001d00"),
    bytes.fromhex("292a2929000e001d00"),
]
# the drive mode (sensor register 0x0001) of mode sets 0..16: 0/1 the stills, 5 photo live view
DRIVE_MODE = {0: 0, 1: 1, 2: 0, 3: 1, 4: 0, 5: 1, 11: 5, 12: 1, 15: 1}


def sensor_write(reg, value):
    """One command of a sensor command set: 0x81, the register's address, a data byte."""
    return bytes((0x81, reg >> 8, reg & 0xFF, value))


def gain_code(gain):
    return min(round(4095 * (1 - 1 / gain)), GAIN_CODE_CAP)


def gain_trim(gain):
    """1.0, or for a capped code the gain it leaves out."""
    code = gain_code(gain)
    return 1.0 if code < GAIN_CODE_CAP else gain * (4095 - code) / 4095


# --- the regions -------------------------------------------------------------------------------


def declared():
    """{region id: (its declaration, the values it sets)}."""
    codes = [{"code": (gain_code(g), gain_code(g))} for g in GAIN_LAW]
    return {
        0x13: (
            TEMP,
            dict(
                i2c_limit=(68, 68, 67),
                thermistor0_limit=(157, 157, 164),
                thermistor1_limit=(203, 212),
            ),
        ),
        0x15: (
            ACCEL,
            dict(
                poll_ms=50,
                mma_spike_thr=50,
                mma_spike_limit=3,
                mean_n=2,
                hysteresis=4 * [10],
                mma_regs={MMA_CTRL_REG1: 0x2D, MMA_CTRL_REG2: 0x02},  # 12.5 Hz low-noise, high-res
            ),
        ),
        0x1C: (BATTERY, dict(pack_temp_limit=70, pack_temp_limit_2=65)),
        0x23: (SZFLK, dict(frames=60)),
        0x24: (
            AE,
            dict(  # the gains and steps in the firmware's units
                weight_base=(1.0, 1.0),
                meter_bias=54 / 192,
                cell_floor=100,
                slew_limit=20,
                slew_limit_2=2,
                converge_gain=[g / 192 for g in (192, 173, 96, 96, 96, 154, 192, 192)]
                + [g / 192 for g in (192, 192, 192, 192, 29, 29, 192)],
                converge_step=[s / 120 for s in (48, 40, 24)],
                converge_gain_2=[g / 192 for g in (192, 77, 192, 192)],
                dark_threshold=200,
                bright_threshold=15200,
                dark_threshold_2=100,
                flag_zone_count=(20, 20),
                still_ev_limit=[{"upper": 15, "lower": 5}, {"upper": 13, "lower": 6}],
            ),
        ),
        0x50: (
            SENSOR_BLOB,
            dict(
                readout_mode=[{"width": w, "height": h} for w, h in READOUT_SIZES],
                black_level=265 * [{"level": SENSOR_BLACK}],
                version=0x15,
                timing=[
                    4 * [{"line_length": (h, h), "lines": v, "width": w, "offset": o}]
                    for h, v, w, o in SENSOR_TIMING
                ],
                gain_code=codes,
                gain_code_2=codes,
                gain_mode_regs=GAIN_MODE_REGS,
                channel_trim=[4 * [gain_trim(g)] for g in GAIN_LAW] + [4 * [1.0]],
                mode_cmds={i: sensor_write(0x0001, d) for i, d in DRIVE_MODE.items()},
                mode_cmd_lengths=(4, 0),
            ),
        ),
    }


def regions():
    """{region id: its bank image}."""
    return {rid: decl(**values) for rid, (decl, values) in declared().items()}


def flash(layout):
    """The synthetic flash, as a ParamFlash."""
    out = ParamFlash(layout)
    out.put_regions(regions())
    return out
