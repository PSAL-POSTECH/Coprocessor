# A weight push, then one input row alone in its window, then 64 rows pushed and popped back to back in theirs.
    .option norvc
    .text
    .globl main
main:
    li      t0, 1
    vsetvli t0, t0, e32, m1, ta, ma
    sf.vc.iv  0x1, 0x0, v1, 0
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    sf.vc.iv  0x0, 0x0, v1, 0
    sf.vc.v.i 0x2, 0x0, v2, 0
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    li      t0, 4
    vsetvli t0, t0, e32, m1, ta, ma
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    sf.vc.iv  0x0, 0x0, v1, 0
    .rept 15
    sf.vc.iv  0x0, 0x0, v1, 0
    sf.vc.v.i 0x2, 0x0, v2, 0
    .endr
    sf.vc.v.i 0x2, 0x0, v2, 0
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    li      a0, 0
    ret
