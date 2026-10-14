# The matrix unit of libtpuv3.so, an operand one register: the weights pushed with no vmatprep, to the one half of
# the depth there is, an input of four words of two rows each, which latches them, and two pops of four rows, of
# the one half of the columns there is.
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
    sf.vc.iv  0x1, 0x0, v1, 11
    sf.vc.iv  0x0, 0x0, v1, 7
    sf.vc.v.i 0x2, 0x0, v2, 0
    sf.vc.v.i 0x2, 0x0, v3, 0
    li      a0, 0
    ret
