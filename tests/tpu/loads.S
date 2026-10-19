# 64 independent vector loads of 32 bytes in a window: from 8 lines 128 bytes apart, all in one bank of the gem5
# scratchpad, then 64 bytes apart, the two banks in turn; two windows each, the first the warm-up. No branch in a
# window; 40 dependent adds after the reset and the loads, as the CPU issues past an m5op before it resolves.
    .option norvc

# lines <apart>: t3, a1-a7 = lines, lines + apart, ..., lines + 7 apart (not a0: an m5op writes it)
.macro lines apart
    la      t3, lines
    li      t2, \apart
    add     a1, t3, t2
    add     a2, a1, t2
    add     a3, a2, t2
    add     a4, a3, t2
    add     a5, a4, t2
    add     a6, a5, t2
    add     a7, a6, t2
.endm

# sixteen: one load of each line into v1-v8, then again into v9-v16
.macro sixteen
    vle32.v v1, (t3)
    vle32.v v2, (a1)
    vle32.v v3, (a2)
    vle32.v v4, (a3)
    vle32.v v5, (a4)
    vle32.v v6, (a5)
    vle32.v v7, (a6)
    vle32.v v8, (a7)
    vle32.v v9, (t3)
    vle32.v v10, (a1)
    vle32.v v11, (a2)
    vle32.v v12, (a3)
    vle32.v v13, (a4)
    vle32.v v14, (a5)
    vle32.v v15, (a6)
    vle32.v v16, (a7)
.endm

.macro window apart, label
    lines   \apart
    li      t5, 2
\label:
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .rept 40
    addi    t6, t6, 1
    .endr
    .rept 4
    sixteen
    .endr
    .rept 40
    addi    t6, t6, 1
    .endr
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    addi    t5, t5, -1
    bnez    t5, \label
.endm

    .text
    .globl main
main:
    li      t0, 8
    vsetvli t0, t0, e32, m1, ta, ma
    window  128, one_bank
    window  64, two_banks
    li      a0, 0
    ret

    .bss
    .balign 128
lines:
    .space 1024
