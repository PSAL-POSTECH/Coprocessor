# The vector code of mxu.c, at run.sh's KERNEL_BASE. No instruction has 2 in rd but the vmatmul of k_mxu_matmul_32_a2,
# refused before it runs, and the first is not an addi. Every function: a0 = source in lane 0's scratchpad, a1 =
# destination, a2 = elements asked, a3 = the instruction's field; returns those done, one register group at most.
    .option norvc
    .section .kernel, "ax"

# An operand of the matrix unit, a group of two registers of elements of `sew` bits: its vmatprep (sf.vc.iv 2),
# vmatpush (sf.vc.iv 1) or vmatmul (sf.vc.iv 0). The field, 0 to 31: [1:0] what a word holds, [2] the operand of a
# vmatprep or the latch of a vmatmul, [4:3] the half of the depth a vmatpush fills. The array it is for is
# sf.vc.iv's rd, which names no register: 0 unless said.
.macro mxu_operand name, p, sew, array=0
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
    sf.vc.iv \p, \array, v8, \field
    j       2f
  .endr
  .irp field, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
    sf.vc.iv \p, \array, v8, \field-32
    j       2f
  .endr
2:  mv      a0, t0
    ret
.endm
    mxu_operand k_mxu_prep_32, 0x2, 32
    mxu_operand k_mxu_push_32, 0x1, 32
    mxu_operand k_mxu_matmul_32, 0x0, 32
    mxu_operand k_mxu_prep_32_a1, 0x2, 32, 1
    mxu_operand k_mxu_push_32_a1, 0x1, 32, 1
    mxu_operand k_mxu_matmul_32_a1, 0x0, 32, 1
    mxu_operand k_mxu_matmul_32_a2, 0x0, 32, 2
# Elements of 16 bits, which the unit refuses: its operands are words.
    mxu_operand k_mxu_push_16, 0x1, 16

# Its vpop (sf.vc.v.i 2) into the destination: the field, 0 to 7, [1:0] the format and [2] the half of the columns.
# The array it pops is sf.vc.v.i's rs2, which names no register: 0 unless said.
.macro mxu_pop name, array=0
    .globl \name
\name:
    vsetvli t0, a2, e32, m2, ta, ma
    vle32.v v16, (a1)
    la      t1, 1f
    slli    t3, a3, 3
    add     t1, t1, t3
    jr      t1
1:
  .irp field, 0, 1, 2, 3, 4, 5, 6, 7
    sf.vc.v.i 0x2, \array, v16, \field
    j       2f
  .endr
2:  vse32.v v16, (a1)
    mv      a0, t0
    ret
.endm
    mxu_pop k_mxu_pop_32
    mxu_pop k_mxu_pop_32_a1, 1
