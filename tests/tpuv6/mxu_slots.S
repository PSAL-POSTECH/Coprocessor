# The matrix unit of libtpuv6.so issues a vpop and a vmatmul of the same array in one cycle, as the v6e's bundles do
# (vpop.mxu1 with vmatmul.mxu1): two inputs of four words of bfloat16, the first's lower pop of four ahead of the
# second's vmatmul, which issues in the cycle the pop does, then the rest of the pops, both halves by turns.
    .option norvc
    .text
    .globl main
main:
    li      t0, 4
    vsetvli t0, t0, e32, m1, ta, ma
    .rept 16
    nop
    .endr
    .balign 64
    sf.vc.iv  0x2, 0x0, v1, 3
    sf.vc.iv  0x1, 0x0, v1, 11
    sf.vc.iv  0x2, 0x0, v1, 7
    sf.vc.iv  0x0, 0x0, v1, 7
    sf.vc.iv  0x2, 0x0, v1, 7
    sf.vc.v.i 0x2, 0x0, v2, 0
    sf.vc.iv  0x0, 0x0, v1, 3
    sf.vc.v.i 0x2, 0x0, v3, 4
    sf.vc.v.i 0x2, 0x0, v4, 0
    sf.vc.v.i 0x2, 0x0, v5, 4
    sf.vc.v.i 0x2, 0x0, v6, 0
    sf.vc.v.i 0x2, 0x0, v7, 4
    sf.vc.v.i 0x2, 0x0, v8, 0
    sf.vc.v.i 0x2, 0x0, v9, 4
    li      a0, 0
    ret
