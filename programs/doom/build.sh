#!/usr/bin/env bash
# Build the DOOM port for the Z 6II master (Cortex-A7, Thumb, soft-float, newlib-nano),
# from doomgeneric (external/doomgeneric) and this directory's platform layer.
#
#   programs/doom/build.sh   # -> out/doom_port/doom.bin (load it anywhere: start.S moves it into place)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DG="$ROOT/external/doomgeneric/doomgeneric"
OUT="$ROOT/out/doom_port"
NAME=doom
mkdir -p "$OUT/obj"
[ -f "$DG/doomgeneric.c" ] || { echo "external/doomgeneric is missing: git submodule update --init" >&2; exit 1; }

CC=arm-none-eabi-gcc
ARCH=(-mthumb -mcpu=cortex-a7 -mfloat-abi=soft)
# CMAP256 with a 320 x 200 window: doomgeneric hands DG_DrawFrame DOOM's own palette indices,
# which doom_z6ii.c scales and converts to the overlay plane's YCbCr itself.
# FEATURE_SOUND: doomgeneric's sound code drives the port's sound module (sound_z6ii.c);
# include/ holds the empty SDL_mixer.h that doomgeneric's i_sound.c includes under that define.
DEFS=(-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -D_DEFAULT_SOURCE -DFEATURE_SOUND -I"$HERE/include")
# -ffile-prefix-map: the source paths assert() embeds are relative to the repository,
# so the image is the same wherever the repository is checked out
CFLAGS=("${ARCH[@]}" -O2 -specs=nano.specs -ffunction-sections -fdata-sections -ffile-prefix-map="$ROOT/"= "${DEFS[@]}")

# doomgeneric's own list of its core sources, without a platform file
SRC=$(sed -n 's/^SRC_DOOM = //p' "$DG/Makefile" | sed 's/doomgeneric_xlib\.o//; s/\.o/.c/g')
OBJS=()
for f in $SRC; do
    o="$OUT/obj/${f%.c}.o"
    if [ ! -f "$o" ] || [ "$DG/$f" -nt "$o" ] || [ "$0" -nt "$o" ]; then
        "$CC" "${CFLAGS[@]}" -w -c "$DG/$f" -o "$o"   # doomgeneric's warnings are its own
    fi
    OBJS+=("$o")
done
# Chocolate Doom's OPL music player and DBOPL (external/chocolate-doom, release 2.2.1)
# replace opl.c with opl_z6ii.c
CD="$ROOT/external/chocolate-doom"
[ -f "$CD/opl/dbopl.c" ] || { echo "external/chocolate-doom is missing: git submodule update --init" >&2; exit 1; }
for f in src/i_oplmusic.c src/midifile.c src/mus2mid.c opl/opl_queue.c opl/dbopl.c; do
    o="$OUT/obj/cd_$(basename "${f%.c}").o"
    if [ ! -f "$o" ] || [ "$CD/$f" -nt "$o" ] || [ "$0" -nt "$o" ]; then
        "$CC" "${CFLAGS[@]}" -w -I"$CD/opl" -c "$CD/$f" -o "$o"
    fi
    OBJS+=("$o")
done
for f in doom_z6ii.c sound_z6ii.c opl_z6ii.c syscalls.c; do
    o="$OUT/obj/${f%.c}.o"
    "$CC" "${CFLAGS[@]}" -Wall -Wextra -Werror -I"$DG" -I"$CD/opl" -c "$HERE/$f" -o "$o"
    OBJS+=("$o")
done
"$CC" "${ARCH[@]}" -c "$HERE/start.S" -o "$OUT/obj/start.o"

"$CC" "${CFLAGS[@]}" -specs=nosys.specs -nostartfiles -T "$HERE/link.ld" -Wl,--gc-sections -Wl,--wrap=I_Error \
    -Wl,-Map,"$OUT/$NAME.map" "$OUT/obj/start.o" "${OBJS[@]}" -lc -lm -lgcc -o "$OUT/$NAME.elf"
arm-none-eabi-objcopy -O binary -R .bss "$OUT/$NAME.elf" "$OUT/$NAME.bin"

entry=$(arm-none-eabi-nm "$OUT/$NAME.elf" | awk '$3 == "_start" {print $1}')
[ "$entry" = 90900000 ] || { echo "ERROR: _start is at 0x$entry, not at the image start 0x90900000" >&2; exit 1; }
size=$(stat -c%s "$OUT/$NAME.bin")
bss_end=$(arm-none-eabi-nm "$OUT/$NAME.elf" | awk '$3 == "__bss_end" {print $1}')
echo "built $OUT/$NAME.bin ($size bytes, image + .bss to 0x$bss_end), sha256 $(sha256sum "$OUT/$NAME.bin" | cut -c1-16)"
