"""A kernel issues the cross-lane reduction unit's max (sf.vc.v.iv 1) on an 8x128 tile, lanes on axis 1: row i of every
lane is reduced across the 128 lanes and every lane gets the result. torch: x.amax(dim=1, keepdim=True), broadcast to the tile."""
from pytorchsim_triton_compiler.contract.kernel_spec import KernelSpec, Arg, IN, OUT

M, N = 8, 128


def kernel():
    import triton
    import triton.language as tl
    from triton.language.extra import torchsim

    @triton.jit
    def xlane_max_kernel(x_ptr, o_ptr, M: tl.constexpr, N: tl.constexpr):
        offs = tl.arange(0, M)[:, None] * N + tl.arange(0, N)[None, :]
        x = tl.load(x_ptr + offs)
        y = torchsim.vcix("v.iv", opcode=1, imm=0, args=[x], layout=torchsim.lane_layout(1))
        tl.store(o_ptr + offs, y)

    return xlane_max_kernel


def make_inputs(torch, seed=0):
    g = torch.Generator().manual_seed(seed)
    return {"x": torch.randn(M, N, generator=g, dtype=torch.float32)}


def reference(inputs):
    x = inputs["x"]
    return {"o": x.amax(dim=1, keepdim=True).expand(M, N).contiguous()}


SPEC = KernelSpec(
    name="xlane_max_kernel", kernel=kernel,
    signature={"x_ptr": "*fp32", "o_ptr": "*fp32", "M": "constexpr", "N": "constexpr"},
    constexprs={"M": M, "N": N},
    args=[Arg("x", IN, "float32", (M, N)), Arg("o", OUT, "float32", (M, N))],
    grid=(1,), reference=reference, make_inputs=make_inputs,
    rtol=1e-4, atol=1e-4,
)
