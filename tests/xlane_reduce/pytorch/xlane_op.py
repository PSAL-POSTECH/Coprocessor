# The xlane unit as a PyTorch op: a Triton kernel registered with triton_op, compiled by PyTorchSim through PTB, run on the npu.
import torch
import triton
import triton.language as tl
from torch.library import triton_op, wrap_triton
from triton.language.extra import torchsim


@triton.jit
def xlane_sum_kernel(x_ptr, o_ptr, M: tl.constexpr, N: tl.constexpr):
    offs = tl.arange(0, M)[:, None] * N + tl.arange(0, N)[None, :]
    x = tl.load(x_ptr + offs)
    y = torchsim.vcix("v.iv", opcode=0, imm=0, args=[x], layout=torchsim.lane_layout(1))
    tl.store(o_ptr + offs, y)


@triton_op("tutorial::xlane_sum", mutates_args=())
def xlane_sum(x: torch.Tensor) -> torch.Tensor:
    o = torch.empty_like(x)
    wrap_triton(xlane_sum_kernel)[(1, 1, 1)](x, o, 8, 128)
    return o


if __name__ == "__main__":
    import torch_openreg  # noqa: F401  (the npu device)
    torch.manual_seed(0)
    x = torch.randn(8, 128)
    f = torch.compile(lambda t: xlane_sum(t), dynamic=False)
    y = f(x.to("npu")).cpu()
    want = x.sum(dim=1, keepdim=True).expand(8, 128)
    print("vs x.sum(dim=1):", torch.allclose(y, want, atol=1e-4), "max abs err", (y - want).abs().max().item())
    print("vs torch.erf(x):", torch.allclose(y, torch.erf(x), atol=1e-4), "max abs err", (y - torch.erf(x)).abs().max().item())
