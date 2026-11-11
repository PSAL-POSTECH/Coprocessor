# The matrix unit of libtpuv6e.so with words of four int8 rows (format 1): mxu.S's program, the two halves of a weight
# operand pushed to the first half of the depth, an input of four words, which latches them, sixteen rows where
# mxu.S's four words are eight bfloat16 rows; then its eight pops as int32 (format 1), the halves of the columns by
# turns.
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
    sf.vc.iv  0x2, 0x0, v1, 1
    sf.vc.iv  0x1, 0x0, v1, 9
    sf.vc.iv  0x2, 0x0, v1, 5
    sf.vc.iv  0x0, 0x0, v1, 5
    .rept 4
    sf.vc.v.i 0x2, 0x0, v2, 1
    sf.vc.v.i 0x2, 0x0, v3, 5
    .endr
    li      a0, 0
    ret
