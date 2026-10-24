# A pattern set, a push it shapes (shuffle before by pattern) and its pop; then a set between two pushes of one
# tile, and its pop.
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
    sf.vc.ivv   0x3, v1, v1, 0
    sf.vc.iv    0x3, 0, v4, -16
    sf.vc.v.i   0x1, 0, v5, 4
    sf.vc.ivv   0x3, v1, v1, 0
    sf.vc.iv    0x3, 0, v4, -16
    sf.vc.ivv   0x3, v1, v1, 0
    sf.vc.iv    0x3, 0, v4, -16
    sf.vc.v.i   0x1, 0, v5, 4
    li      a0, 0
    ret
