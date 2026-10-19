# The matrix unit of libtpuv6e.so with tpu_mxu_units: 2 (the array is sf.vc.iv's rd, sf.vc.v.i's rs2): each array's
# weights and four words of two rows, the two vmatmuls a cycle apart so the second's rows enter while the first's do,
# then each array's pops, the lower and the upper half of the columns by turns.
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
