"""Hands-on 2: your kernel calls your unit. As given, the kernel copies the tile, so the check against torch FAILs.
Replace the one TODO line between the Step 1 markers with the vcix call, then run
    python3 tests/pytorchsim-compile tests/xlane_all_reduce/ptb/my_xlane_all_reduce.py
from pytorchsim-triton-compiler. The finished kernel is xlane_all_reduce_sum.py next to this file."""
from pytorchsim_triton_compiler.contract.kernel_spec import KernelSpec, Arg, IN, OUT

M, N = 8, 128  # one tile: 8 rows of 128 elements, one element per lane


def kernel():
    import triton
    import triton.language as tl
    from triton.language.extra import pytorchsim

    @triton.jit
    def my_xlane_all_reduce_kernel(x_ptr, o_ptr, M: tl.constexpr, N: tl.constexpr):
        offs = tl.arange(0, M)[:, None] * N + tl.arange(0, N)[None, :]
        x = tl.load(x_ptr + offs)
        # >>> Step 1: issue the unit's sum on the tile: form "v.iv", opcode 0, imm 0, lanes on axis 1
        y = x  # TODO: y = pytorchsim.vcix(...)
        # <<< Step 1
        tl.store(o_ptr + offs, y)

    return my_xlane_all_reduce_kernel


def make_inputs(torch, seed=0):
    g = torch.Generator().manual_seed(seed)
    return {"x": torch.randn(M, N, generator=g, dtype=torch.float32)}


def reference(inputs):
    x = inputs["x"]
    return {"o": x.sum(dim=1, keepdim=True).expand(M, N).contiguous()}  # every lane holds its row's sum


SPEC = KernelSpec(
    name="my_xlane_all_reduce_kernel", kernel=kernel,
    signature={"x_ptr": "*fp32", "o_ptr": "*fp32", "M": "constexpr", "N": "constexpr"},
    constexprs={"M": M, "N": N},
    args=[Arg("x", IN, "float32", (M, N)), Arg("o", OUT, "float32", (M, N))],
    grid=(1,), reference=reference, make_inputs=make_inputs,
    rtol=1e-4, atol=1e-4,
)
