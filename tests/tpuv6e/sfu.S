# The special-function unit of libtpuv6e.so, 4 lanes: an EUP push of a group of eight registers, then its pop, which
# the unit takes once the eighth register is out of the pipeline.
    .option norvc
    .text
    .globl main
main:
    li      t0, 64
    vsetvli t0, t0, e32, m8, ta, ma
    .rept 16
    nop
    .endr
    .balign 64
    sf.vc.vv  0x3, 0x0, v8, v8
    sf.vc.v.x 0x3, 0x0, v16, zero
    li      a0, 0
    ret
