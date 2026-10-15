# The array's timing on gem5: weights, inputs, a weight push before their pop, then a longer stream.
    .option norvc
    .text
    .globl main
main:
    li      t0, 4
    vsetvli t0, t0, e8, m1, ta, ma
    .rept 16
    nop
    .endr
    .balign 64
    sf.vc.iv  0x1, 0x0, v1, 0
    sf.vc.iv  0x0, 0x0, v1, 0
    sf.vc.iv  0x1, 0x0, v1, 0
    sf.vc.v.i 0x2, 0x0, v2, 0
    li      t0, 8
    vsetvli t0, t0, e8, m1, ta, ma
    sf.vc.iv  0x0, 0x0, v1, 0
    sf.vc.v.i 0x2, 0x0, v2, 0
    li      a0, 0
    ret
