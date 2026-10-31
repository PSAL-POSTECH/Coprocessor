# Hands-on 3: the kernel you wrote in Hands-on 2 becomes a PyTorch op. Uncomment Step 1 (lines 23-29) to register
# the kernel, then run:  TORCHSIM_TARGET=$X/pytorch/xlane_all_reduce.yml python3 my_xlane_all_reduce_op.py
# The finished file is xlane_all_reduce_op.py next to this one.
import torch
import triton
import triton.language as tl
from torch.library import triton_op, wrap_triton
from triton.language.extra import pytorchsim


# The kernel from Hands-on 2, unchanged
@triton.jit
def my_xlane_all_reduce_kernel(x_ptr, o_ptr,
        M: tl.constexpr, N: tl.constexpr):
    offs = tl.arange(0, M)[:, None] * N + tl.arange(0, N)[None, :]
    x = tl.load(x_ptr + offs)
    y = pytorchsim.vcix("v.iv", opcode=0, imm=0, args=[x],
                        layout=pytorchsim.lane_layout(1))
    tl.store(o_ptr + offs, y)


# >>> Step 1: register the kernel as an op
# @triton_op("tutorial::xlane_all_reduce_sum", mutates_args=())
# def xlane_all_reduce_sum(x: torch.Tensor) -> torch.Tensor:
#     o = torch.empty_like(x)    # output buffer
#     grid = (1, 1, 1)           # X Y Z
#     M, N = x.shape
#     wrap_triton(my_xlane_all_reduce_kernel)[grid](x, o, M, N)
#     return o
# <<< Step 1


if __name__ == "__main__":
    torch.manual_seed(0)
    x = torch.randn(8, 128)
    f = torch.compile(lambda t: xlane_all_reduce_sum(t), dynamic=False)
    y = f(x.to("npu")).cpu()
    want = x.sum(dim=1, keepdim=True).expand(8, 128)
    ok = torch.allclose(y, want, atol=1e-4)
    err = (y - want).abs().max().item()
    print("vs x.sum(dim=1):", ok, "max abs err", err)
    raise SystemExit(0 if ok else 1)
