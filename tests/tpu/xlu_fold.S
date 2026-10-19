# A fold push of two and its pop: timed as xlu_push.
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
    sf.vc.ivv   0x2, v2, v4, 1
    sf.vc.v.i   0x1, 0, v5, 4
    li      a0, 0
    ret
