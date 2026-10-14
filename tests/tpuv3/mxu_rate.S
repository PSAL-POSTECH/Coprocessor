# The matrix unit of libtpuv3.so takes a row a cycle into its input queue's room: weights, a vmatmul of four
# singles, then a vmatmul of four words of bfloat16, eight rows, which a queue of 8 takes only once the singles
# have entered, and three pops of four rows.
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
    sf.vc.iv  0x1, 0x0, v1, 8
    sf.vc.iv  0x0, 0x0, v1, 4
    sf.vc.iv  0x0, 0x0, v1, 3
    .rept 3
    sf.vc.v.i 0x2, 0x0, v2, 0
    .endr
    li      a0, 0
    ret
