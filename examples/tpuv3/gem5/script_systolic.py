# The TPU v3 machine: the tpu machine (examples/tpu/gem5/script_systolic.py) with vpu_config.py's CPU, 940 MHz, and a
# scratchpad of 1 vector load and 1 store slot (2 accesses, their sum), the TPUv2 bundle's taken as the v3's; latency 1 and
# no VMEM banks are placeholders (examples/tpuv3/README.md, "The gem5 machine"). Usage: as tpuv6e/gem5's, --model libtpuv3.so
import importlib.util
import os
from m5.objects import *
from vpu_config import TPUv3VPU

_spec = importlib.util.spec_from_file_location(
    "tpu_script_systolic",
    os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "tpu", "gem5", "script_systolic.py"))
tpu = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tpu)

tpu.simulate(tpu.machine(tpu.arguments(), TPUv3VPU, tpu.multiport_scratchpad(load=1, store=1, access=2, latency=1),
                         clock="940MHz"))
