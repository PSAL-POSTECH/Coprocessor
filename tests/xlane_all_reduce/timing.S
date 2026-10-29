# On gem5: four sums of 8 elements that need nothing from each other, then three sums that each need the one before,
# one of 4 elements and one of 1, each needing the one before. Each commit prints when its instruction issued and how
# long it took.
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
    sf.vc.v.iv 0, v10, v1, 0
    sf.vc.v.iv 0, v11, v1, 0
    sf.vc.v.iv 0, v12, v11, 0
    sf.vc.v.iv 0, v13, v12, 0
    sf.vc.v.iv 0, v14, v13, 0
    li         t0, 4
    vsetvli    t0, t0, e32, m1, ta, ma
    sf.vc.v.iv 0, v15, v14, 0
    li         t0, 1
    vsetvli    t0, t0, e32, m1, ta, ma
    sf.vc.v.iv 0, v16, v15, 0
    li         a0, 0
    ret
