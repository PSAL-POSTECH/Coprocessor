# The vector code of values.c, linked at .kernel 0x2000000 (CMakeLists.txt), where Spike runs a vector instruction in
# every lane.
# a0 = source in lane 0's scratchpad, a1 = destination, a2 = elements asked; returns those done.
    .option norvc
    .section .kernel, "ax"
.macro reduce name, op
    .globl \name
\name:
    vsetvli    t0, a2, e32, m1, ta, ma
    vle32.v    v8, (a0)
    sf.vc.v.iv \op, v16, v8, 0
    vse32.v    v16, (a1)
    mv         a0, t0
    ret
.endm
    reduce k_sum, 0
