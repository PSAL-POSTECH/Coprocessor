# The matrix unit of libtpu_mxu.so with tpu_mxu_units: 2: each array's weights (one half of the depth) and each
# array's input of four words of two rows, the two vmatmuls a cycle apart, so that the second array's rows enter
# while the first's do, and the pops of each, the lower and the upper half of the columns by turns. The array is
# sf.vc.iv's rd and sf.vc.v.i's rs2.
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
    sf.vc.iv  0x2, 0x1, v1, 3
    sf.vc.iv  0x1, 0x1, v1, 11
    sf.vc.iv  0x2, 0x0, v1, 7
    sf.vc.iv  0x2, 0x1, v1, 7
    sf.vc.iv  0x0, 0x0, v1, 7
    sf.vc.iv  0x0, 0x1, v1, 7
    sf.vc.v.i 0x2, 0x0, v2, 0
    sf.vc.v.i 0x2, 0x1, v3, 0
    sf.vc.v.i 0x2, 0x0, v4, 4
    sf.vc.v.i 0x2, 0x1, v5, 4
    li      a0, 0
    ret
