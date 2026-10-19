# A loop of 16 trips in a window, each trip an e16 m2 and an e32 m4 vsetvli, each followed by a vadd of its type, in four
# windows that differ only in where the code sits: the windows start 0, 1, 2 and 3 instructions past a 64-byte line. Each
# window runs twice, the first its own warm-up. 40 dependent scalar adds after the reset and after the loop, as the CPU
# issues what follows an m5op before it resolves.
    .option norvc

# pair: an e16 m2 vadd, then an e32 m4 vadd, each after the vsetvli that sets its type
.macro pair
    vsetvli t0, t4, e16, m2, ta, ma
    vadd.vv v2, v4, v6
    vsetvli t0, t4, e32, m4, ta, ma
    vadd.vv v8, v12, v16
.endm

.macro window pad, label
    li      t5, 2
    .balign 64
    .rept \pad
    nop
    .endr
\label:
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .rept 40
    addi    t6, t6, 1
    .endr
    li      t3, 16
1:
    pair
    addi    t3, t3, -1
    bnez    t3, 1b
    .rept 40
    addi    t6, t6, 1
    .endr
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    addi    t5, t5, -1
    bnez    t5, \label
.endm

    .text
    .globl main
main:
    li      t4, 8
    window  0, pad0
    window  1, pad1
    window  2, pad2
    window  3, pad3
    li      a0, 0
    ret
