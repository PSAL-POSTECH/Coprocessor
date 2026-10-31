# On gem5: two sums that need nothing from each other, then two that each need the one before, all of 8 elements.
# Each commit prints when its instruction issued and how long it took.
    .text
    .globl main
main:
    li         t0, 8
    vsetvli    t0, t0, e32, m1, ta, ma
    vmv.v.i    v1, 0
    .rept 16
    nop
    .endr
    sf.vc.v.iv 0, v8, v1, 0
    sf.vc.v.iv 0, v9, v1, 0
    sf.vc.v.iv 0, v10, v9, 0
    sf.vc.v.iv 0, v11, v10, 0
    li         a0, 0
    ret
