# The matrix unit of libtpuv6e.so with tpu_mxu_units: 2 and queues of 4: each array's weights and inputs prepared, a
# vmatmul of four rows that latches on each array in one cycle, then a vpop of the four from each. The two vpops share
# the pop port's four rows a cycle, so the second waits.
    .option norvc
    .text
    .globl main
main:
    li      t0, 4
    vsetvli t0, t0, e32, m1, ta, ma
    .rept 16
    nop
    .endr
    .irp array, 0x0, 0x1
    sf.vc.iv  0x2, \array, v1, 0
    sf.vc.iv  0x1, \array, v1, 8
    sf.vc.iv  0x2, \array, v1, 4
    .endr
    .balign 64
    sf.vc.iv  0x0, 0x0, v1, 4
    sf.vc.iv  0x0, 0x1, v1, 4
    sf.vc.v.i 0x2, 0x0, v2, 0
    sf.vc.v.i 0x2, 0x1, v3, 0
    li      a0, 0
    ret
