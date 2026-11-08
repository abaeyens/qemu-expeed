/*
 * First code the DOOM port runs: copy the image from wherever the loader put it
 * to the address it's linked for, and start it there.
 *
 * Copyright (c) 2026 Arne Baeyens
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .syntax unified
    .arch armv7-a
    .thumb

    .section .text.start, "ax"
    .align  2
    .global _start
    .thumb_func
_start:
    push    {r4-r7, lr}
    ldr     r5, =0x9044ff00         @ DOOM_RUNNING: {"DOOM", the counter when DOOM last said it was alive}
    ldr     r6, =0x04266008         @ the 48 MHz counter's low word
    ldr     r6, [r6]
    ldr     r1, [r5]
    ldr     r2, =0x4d4f4f44         @ "DOOM"
    cmp     r1, r2
    bne     0f
    ldr     r1, [r5, #4]
    sub     r1, r6, r1              @ how long ago (wraps every 89 s: a stale marker can look fresh only by chance)
    ldr     r3, =480000000          @ 10 s
    cmp     r1, r3
    bhs     0f
    mov     r0, #-1                 @ DOOM runs already: leave it alone
    pop     {r4-r7, pc}
0:  str     r2, [r5]
    str     r6, [r5, #4]
    adr     r0, _start              @ where the loader put us
    bic     r0, r0, #1              @ (as a data address: an adr to a Thumb function may carry the Thumb bit)
    ldr     r1, =__image_start      @ where we are linked to run (link.ld)
    ldr     r2, =__image_end
    cmp     r0, r1
    beq     2f                      @ already in place (loaded at the run address)
    mov     r3, r1
1:  ldr     r4, [r0], #4            @ copy the image, word by word (the linker script aligns both ends)
    str     r4, [r3], #4
    cmp     r3, r2
    blo     1b
2:  ldr     r2, =__bss_start        @ zero .bss
    ldr     r3, =__bss_end
    movs    r4, #0
3:  cmp     r2, r3
    bhs     4f
    str     r4, [r2], #4
    b       3b
4:  bic     r0, r1, #63             @ clean the copy and .bss to the point of coherency
5:  mcr     p15, 0, r0, c7, c10, 1  @ DCCMVAC
    add     r0, r0, #64
    cmp     r0, r3
    blo     5b
    dsb
    movs    r0, #0
    mcr     p15, 0, r0, c7, c1, 0   @ ICIALLUIS: every core's instruction cache
    mcr     p15, 0, r0, c7, c1, 6   @ BPIALLIS: and branch predictor
    dsb
    isb
    ldr     r0, =doom_entry
    blx     r0                      @ in the copy (doom_entry is Thumb, so its address has bit 0 set)
    pop     {r4-r7, pc}
    .ltorg
