# Three loads and two stores in turn in a window on the v6e machine's scratchpad, loads from 8 lines and stores to 2
# others, so no load waits on a store: 60 accesses, more than its 4 a cycle take. No branch in the window; 40 dependent
# adds after the reset and the accesses, as the CPU issues past an m5op before it resolves.
    .option norvc

# five: three loads and two stores
.macro five l0, l1, l2
    vle32.v v1, (\l0)
    vle32.v v2, (\l1)
    vle32.v v3, (\l2)
    vse32.v v9, (s2)
    vse32.v v10, (s3)
.endm

# twenty: four groups of five, the loads over the 8 lines and half again
.macro twenty
    five    t3, a1, a2
    five    a3, a4, a5
    five    a6, a7, t3
    five    a1, a2, a3
.endm

    .text
    .globl main
main:
    li      t0, 8
    vsetvli t0, t0, e32, m1, ta, ma
    la      t3, lines
    addi    a1, t3, 64
    addi    a2, t3, 128
    addi    a3, t3, 192
    addi    a4, t3, 256
    addi    a5, t3, 320
    addi    a6, t3, 384
    addi    a7, t3, 448
    addi    s2, t3, 512
    addi    s3, t3, 576
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .rept 40
    addi    t1, t1, 1
    .endr
    .rept 3
    twenty
    .endr
    .rept 40
    addi    t1, t1, 1
    .endr
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    li      a0, 0
    ret

    .bss
    .balign 64
lines:
    .space 640
