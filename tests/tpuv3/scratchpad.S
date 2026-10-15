# Independent vector loads and stores in a window, a load and a store in turn, on the v3 machine's scratchpad: loads
# from 8 lines, stores to 2 other lines, so no load waits on a store. 64 accesses, 2 a pair where it takes 2 a cycle.
# No branch in the window; 40 dependent scalar adds around it, as the CPU issues what follows an m5op before it resolves.
    .option norvc

# pair: a load into vd and a store
.macro pair vd, l, s
    vle32.v \vd, (\l)
    vse32.v v9, (\s)
.endm

# eight: eight pairs, the loads into v1-v8 over the 8 lines, the stores over the 2
.macro eight
    pair    v1, t3, s2
    pair    v2, a1, s3
    pair    v3, a2, s2
    pair    v4, a3, s3
    pair    v5, a4, s2
    pair    v6, a5, s3
    pair    v7, a6, s2
    pair    v8, a7, s3
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
    .rept 4
    eight
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
