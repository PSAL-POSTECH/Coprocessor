# 64 vector loads, each followed by the addi that steps its pointer (an RVV load has no immediate offset): eight
# pointers, 128 bytes a step, two windows, the first the warm-up. With N integer units the N+1st addi waits for a load
# to commit. 40 dependent adds after the reset and the loads, as the CPU issues past an m5op before it resolves.
    .option norvc

.macro pair v, p
    vle32.v \v, (\p)
    addi    \p, \p, 128
.endm

.macro eight
    pair    v1, t3
    pair    v2, a1
    pair    v3, a2
    pair    v4, a3
    pair    v5, a4
    pair    v6, a5
    pair    v7, a6
    pair    v8, a7
.endm

    .text
    .globl main
main:
    li      t0, 8
    vsetvli t0, t0, e32, m1, ta, ma
    li      t5, 2
window:
    la      t3, lines
    addi    a1, t3, 16
    addi    a2, t3, 32
    addi    a3, t3, 48
    addi    a4, t3, 64
    addi    a5, t3, 80
    addi    a6, t3, 96
    addi    a7, t3, 112
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .rept 40
    addi    t6, t6, 1
    .endr
    .rept 8
    eight
    .endr
    .rept 40
    addi    t6, t6, 1
    .endr
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    addi    t5, t5, -1
    bnez    t5, window
    li      a0, 0
    ret

    .bss
    .balign 128
lines:
    .space 2048
