# The matrix unit of libtpu_mxu.so takes a word a cycle: a vmatmul of four words of singles and, queued behind it, a
# vmatmul of four words of bfloat16, then two weights pushed, which enter nothing, a cycle each while the input queue
# empties, and the six pops of the twelve rows, the lower and the upper half of the columns by turns. Sixteen
# instructions, the 64 bytes of one cache line.
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
    sf.vc.iv  0x2, 0x0, v1, 4
    sf.vc.iv  0x0, 0x0, v1, 4
    sf.vc.iv  0x2, 0x0, v1, 7
    sf.vc.iv  0x0, 0x0, v1, 3
    .rept 2
    sf.vc.iv  0x2, 0x0, v1, 3
    sf.vc.iv  0x1, 0x0, v1, 11
    .endr
    .rept 3
    sf.vc.v.i 0x2, 0x0, v2, 0
    sf.vc.v.i 0x2, 0x0, v3, 4
    .endr
    li      a0, 0
    ret
