# A weight push, then ten input pushes of two elements back to back, into an input queue of eight.
    .option norvc
    .text
    .globl main
main:
    li      t0, 2
    vsetvli t0, t0, e32, m1, ta, ma
    .rept 16
    nop
    .endr
    .balign 64
    sf.vc.iv  0x1, 0x0, v1, 0
    .rept 10
    sf.vc.iv  0x0, 0x0, v1, 0
    .endr
    li      a0, 0
    ret
