# The matrix unit of libtpu_mxu.so: the two halves of a weight operand, pushed to the first half of the depth, the
# two halves of an input of four words of two rows each, which latches the weights, and its four pops, the lower
# and the upper half of the columns by turns.
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
    sf.vc.v.i 0x2, 0x0, v2, 0
    sf.vc.v.i 0x2, 0x0, v3, 4
    sf.vc.v.i 0x2, 0x0, v4, 0
    sf.vc.v.i 0x2, 0x0, v5, 4
    li      a0, 0
    ret
