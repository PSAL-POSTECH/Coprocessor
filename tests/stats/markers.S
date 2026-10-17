# A weight push, then five statistics windows cut by m5ops: three pushes of four and their pops; none, a push right after
# its dump; a push and a pop, cut by a dumpreset whose a0 and a1 ask 2000 ns of delay and a 1000 ns period; two pushes
# and pops; none.
    .option norvc
    .text
    .globl main
main:
    li      t0, 4
    vsetvli t0, t0, e32, m1, ta, ma
    sf.vc.iv  0x1, 0x0, v1, 0
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .rept 3
    sf.vc.iv  0x0, 0x0, v1, 0
    .endr
    .rept 3
    sf.vc.v.i 0x2, 0x0, v2, 0
    .endr
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    sf.vc.iv  0x0, 0x0, v1, 0
    sf.vc.v.i 0x2, 0x0, v2, 0
    li      a0, 2000
    li      a1, 1000
    .insn r 0x7b, 0, 0x42, x0, x0, x0
    .rept 2
    sf.vc.iv  0x0, 0x0, v1, 0
    .endr
    .rept 2
    sf.vc.v.i 0x2, 0x0, v2, 0
    .endr
    li      t1, 40
1:  addi    t1, t1, -1
    bnez    t1, 1b
    .insn r 0x7b, 0, 0x42, x0, x0, x0
    li      a0, 0
    ret
