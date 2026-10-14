# The vector code of the programs in this directory, linked at KERNEL_BASE of run.sh. Spike runs a vector
# instruction in every lane only inside --kernel-addr; there a load or store at a scratchpad address is each
# lane's own copy of it. No instruction here has 2 in its rd field, and the first is not an addi: Spike
# reads both as the kernel moving its stack.
#
# Every function: a0 = source in lane 0's scratchpad, a1 = destination there, a2 = elements asked.
# Returns the elements done, at most one register group.
    .option norvc
    .section .kernel, "ax"

# vd = f(vs2): sf.vc.v.iv \p, vd, vs2, \simm on a group of eight registers.
.macro special name, sew, p, simm
    .globl \name
\name:
    vsetvli t0, a2, e\sew, m8, ta, ma
    vle\sew\().v v8, (a0)
    vle\sew\().v v16, (a1)
    sf.vc.v.iv \p, v16, v8, \simm
    vse\sew\().v v16, (a1)
    mv      a0, t0
    ret
.endm
.irp sew, 16, 32, 64
    special k_verf_\sew, \sew, 0x0, 0
    special k_vtanh_\sew, \sew, 0x1, 0
    special k_vsin_\sew, \sew, 0x2, 0
    special k_vcos_\sew, \sew, 0x2, 1
    special k_vlog_\sew, \sew, 0x2, 2
    special k_vatan_\sew, \sew, 0x2, 3
    special k_vexp_\sew, \sew, 0x3, 0
.endr

# The systolic array: a push of the source, and a pop into the destination, which keeps what the pop leaves.
# a3 = what an element is, the number that rides the instruction in its rs1 field: 0 to 3.
.macro systolic_push name, sew, p
    .globl \name
\name:
    vsetvli t0, a2, e\sew, m2, ta, ma
    vle\sew\().v v8, (a0)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
1:  sf.vc.iv \p, 0x0, v8, 0
    j       2f
    sf.vc.iv \p, 0x0, v8, 1
    j       2f
    sf.vc.iv \p, 0x0, v8, 2
    j       2f
    sf.vc.iv \p, 0x0, v8, 3
    j       2f
2:  mv      a0, t0
    ret
.endm
.macro systolic_pop name, sew
    .globl \name
\name:
    vsetvli t0, a2, e\sew, m2, ta, ma
    vle\sew\().v v16, (a1)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
1:  sf.vc.v.i 0x2, 0x0, v16, 0
    j       2f
    sf.vc.v.i 0x2, 0x0, v16, 1
    j       2f
    sf.vc.v.i 0x2, 0x0, v16, 2
    j       2f
    sf.vc.v.i 0x2, 0x0, v16, 3
    j       2f
2:  vse\sew\().v v16, (a1)
    mv      a0, t0
    ret
.endm
.irp sew, 8, 16, 32, 64
    systolic_push k_input_\sew, \sew, 0x0
    systolic_push k_weight_\sew, \sew, 0x1
    systolic_pop k_pop_\sew, \sew
.endr
    .globl k_compute
k_compute:
    sf.vc.i 0x1, 0, 0, 4
    ret

# The cross-lane unit. A group holds 64 elements at every width, so a pattern of 64 words fits its own group.
# a3 = the operation, 0 to 23: the field of the push. a4 = the pattern of a push that carries one, a word per element.
.macro cross_operations form, operand
1:
  .irp operation, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    \form 0x3, \operand, v8, \operation
    j       2f
  .endr
  .irp operation, 16, 17, 18, 19, 20, 21, 22, 23
    \form 0x3, \operand, v8, \operation-32
    j       2f
  .endr
2:
.endm
.macro cross_push name, sew, lmul
    .globl \name
\name:
    vsetvli t0, a2, e\sew, \lmul, ta, ma
    vle\sew\().v v8, (a0)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
    cross_operations sf.vc.iv, 0
    mv      a0, t0
    ret
.endm
.macro cross_push_pattern name, sew, lmul
    .globl \name
\name:
    vsetvli t0, a2, e32, m8, ta, ma
    vle32.v v24, (a4)
    vsetvli t0, a2, e\sew, \lmul, ta, ma
    vle\sew\().v v8, (a0)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
    cross_operations sf.vc.ivv, v24
    mv      a0, t0
    ret
.endm
.macro cross_pop name, sew, lmul
    .globl \name
\name:
    vsetvli t0, a2, e\sew, \lmul, ta, ma
    vle\sew\().v v16, (a1)
    sf.vc.v.i 0x1, 0, v16, 4
    vse\sew\().v v16, (a1)
    mv      a0, t0
    ret
.endm
    cross_pop k_cross_pop_8, 8, m2
    cross_pop k_cross_pop_16, 16, m4
    cross_pop k_cross_pop_32, 32, m8
    cross_push k_cross_push_8, 8, m2
    cross_push k_cross_push_16, 16, m4
    cross_push k_cross_push_32, 32, m8
    cross_push_pattern k_cross_pattern_8, 8, m2
    cross_push_pattern k_cross_pattern_16, 16, m4
    cross_push_pattern k_cross_pattern_32, 32, m8
# The pattern of the shuffle after the crossing: a push whose field says 24, of words whatever the tile's width.
    .globl k_cross_load
k_cross_load:
    vsetvli t0, a2, e32, m8, ta, ma
    vle32.v v8, (a0)
    sf.vc.iv 0x3, 0, v8, -8
    mv      a0, t0
    ret

# The multi-precision array. a3 = the field of the instruction, 0 to 31 for a push and 0 to 3 for a pop.
.macro msa_push name, sew
    .globl \name
\name:
    vsetvli t0, a2, e\sew, m2, ta, ma
    vle\sew\().v v8, (a0)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
1:
  .irp field, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    sf.vc.iv 0x2, 0, v8, \field
    j       2f
  .endr
  .irp field, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    sf.vc.iv 0x2, 0, v8, \field-32
    j       2f
  .endr
2:  mv      a0, t0
    ret
.endm
.macro msa_pop name, sew
    .globl \name
\name:
    vsetvli t0, a2, e\sew, m2, ta, ma
    vle\sew\().v v16, (a1)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
1:
  .irp field, 0, 1, 2, 3
    sf.vc.v.i 0x3, 0, v16, \field
    j       2f
  .endr
2:  vse\sew\().v v16, (a1)
    mv      a0, t0
    ret
.endm
    msa_push k_msa_push_32, 32
    msa_push k_msa_push_16, 16
    msa_pop k_msa_pop_8, 8
    msa_pop k_msa_pop_16, 16
    msa_pop k_msa_pop_32, 32

# vlane_idx at an element width: a1 receives a3 doublewords of the group it wrote into, which was 0.
.macro lane_index name, sew
    .globl \name
\name:
    vsetvli t0, a3, e64, m8, ta, ma
    vmv.v.i v16, 0
    vsetvli t1, a2, e\sew, m1, ta, ma
    sf.vc.v.i 0x0, 0, v16, 0
    vsetvli t0, a3, e64, m8, ta, ma
    vse64.v v16, (a1)
    mv      a0, t1
    ret
.endm
.irp sew, 8, 16, 32, 64
    lane_index k_lane_index_\sew, \sew
.endr
