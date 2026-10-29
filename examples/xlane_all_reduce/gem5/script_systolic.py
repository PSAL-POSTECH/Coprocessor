# The xlane_all_reduce model (examples/xlane_all_reduce/xlane.cc) is timed on the TPU v6e machine: the tpu machine (examples/tpu/gem5/script_systolic.py) with vpu_config.py's CPU and a scratchpad of
# the v6e's 3 load, 2 store and 4 access slots a bundle, latency 1 a choice (examples/tpuv6e/README.md, "The gem5 machine").
# Usage: gem5.opt script_systolic.py -c BINARY --model libxlane_all_reduce.so [--machine-config machine.yml] [--vlane N] [--vlen N]
import importlib.util
import os
from m5.objects import *
from vpu_config import TPUv6eVPU

_spec = importlib.util.spec_from_file_location(
    "tpu_script_systolic",
    os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "tpu", "gem5", "script_systolic.py"))
tpu = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tpu)

tpu.simulate(tpu.machine(tpu.arguments(), TPUv6eVPU, tpu.multiport_scratchpad(load=3, store=2, access=4, latency=1)))
