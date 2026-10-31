# The TPU v6e machine's CPU: the tpu machine's (examples/tpu/gem5/vpu_config.py) with a pool of 4 VALUs, VECTOR_ALU
# (16027/53900) in libtpu's static bundle profile of a v6e 8192 x 4096 x 4096 bf16 matmul (13475 bundles), and 2 LSQ
# requests, a compensation (examples/tpuv6e/README.md, "Two LSQ requests and two more integer units").
import importlib.util
import os
import m5
from m5.objects import *

_spec = importlib.util.spec_from_file_location(
    "tpu_vpu_config", os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "tpu", "gem5", "vpu_config.py"))
tpu = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tpu)

class TPUv6eVPU(tpu.RiscvVPU):
    executeLSQRequestsQueueSize = 2
    executeFuncUnits = tpu.vliw_pool(4)
