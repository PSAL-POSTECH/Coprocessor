# 64 independent vector ops in a window, a vfadd and a vfmul in turn, on the v6e machine's four VALUs: no op waits on
# another, so only the units bound the issue. No branch in the window, and 40 dependent scalar adds after the reset and
# after the ops, as the CPU issues what follows an m5op before it resolves.
    .option norvc

# sixteen: eight vfadds and eight vfmuls in turn, into v0-v15, from v16 and v17
.macro sixteen
    vfadd.vv v0, v16, v17
    vfmul.vv v1, v16, v17
    vfadd.vv v2, v16, v17
    vfmul.vv v3, v16, v17
    vfadd.vv v4, v16, v17
    vfmul.vv v5, v16, v17
    vfadd.vv v6, v16, v17
    vfmul.vv v7, v16, v17
    vfadd.vv v8, v16, v17
    vfmul.vv v9, v16, v17
    vfadd.vv v10, v16, v17
    vfmul.vv v11, v16, v17
    vfadd.vv v12, v16, v17
    vfmul.vv v13, v16, v17
    vfadd.vv v14, v16, v17
    vfmul.vv v15, v16, v17
.endm

    .text
    .globl main
main:
    li      t0, 8
    vsetvli t0, t0, e32, m1, ta, ma
    .insn r 0x7b, 0, 0x40, x0, x0, x0
    .rept 40
    addi    t1, t1, 1
    .endr
    .rept 4
    sixteen
    .endr
    .rept 40
    addi    t1, t1, 1
    .endr
    .insn r 0x7b, 0, 0x41, x0, x0, x0
    li      a0, 0
    ret
