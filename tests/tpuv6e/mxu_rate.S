# The matrix unit of libtpuv6e.so takes a word a cycle: a vmatmul of four words of singles, one of bfloat16 queued
# behind it, two weight pushes a cycle each while the input queue empties, and six pops of the twelve rows, the halves
# of the columns by turns. Sixteen instructions, one cache line.
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
