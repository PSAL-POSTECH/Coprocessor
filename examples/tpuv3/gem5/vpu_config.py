# The TPU v3 machine's CPU: the tpu machine's (examples/tpu/gem5/vpu_config.py) with a pool of 2 VALU slots, the TPUv2
# bundle's (Norrie et al., IEEE Micro 2021, p.4), taken as the v3's, and 2 LSQ requests, a compensation as on the v6e,
# not a count of the v3 (examples/tpuv3/README.md, "The gem5 machine").
import importlib.util
import os
import m5
from m5.objects import *

_spec = importlib.util.spec_from_file_location(
    "tpu_vpu_config", os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "tpu", "gem5", "vpu_config.py"))
tpu = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tpu)

class TPUv3VPU(tpu.RiscvVPU):
    executeLSQRequestsQueueSize = 2
    executeFuncUnits = tpu.vliw_pool(2)
