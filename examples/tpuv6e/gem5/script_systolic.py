# The TPU v6e machine in syscall emulation: the tpu machine (examples/tpu/gem5/script_systolic.py, whose functions build
# it), with this directory's CPU (vpu_config.py) and a scratchpad of the v6e's load and store slots.
# Usage: gem5.opt script_systolic.py -c BINARY --model libtpuv6e.so [--machine-config machine.yml] [--vlane N] [--vlen N]
import importlib.util
import os
from m5.objects import *
from vpu_config import TPUv6eVPU

_spec = importlib.util.spec_from_file_location(
    "tpu_script_systolic",
    os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "tpu", "gem5", "script_systolic.py"))
tpu = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tpu)

# The scratchpad: one MultiPortScratchpad that accepts, each cycle and at any address, up to the v6e's slots a bundle in
# libtpu's static bundle profile (examples/tpuv6e/gem5/vpu_config.py names the dump):
#   load_ports=3    VECTOR_LOAD_AND_MISC (19181/40425): 3 vector loads
#   store_ports=2   VECTOR_STORE_AND_MISC (8872/26950): 2 vector stores
#   access_ports=4  VECTOR_LOAD_AND_STORE_AND_MISC (28041/53900): 4 loads and stores together
# and answers each after latency=1 cycle, a choice: the bundles do not give the v6e's VMEM latency. It refuses a request
# only when one of those is taken this cycle. The CPU's data port goes to it directly: behind the bus every access would
# share its one layer to the scratchpad, which refuses a second packet in the same bus cycle (noncoherent_xbar.cc:115,
# xbar.cc:197). The instruction cache and the system port still come through the bus, on the scratchpad's second port.
def multiport_scratchpad(system):
    system.spad = MultiPortScratchpad(range=system.mem_ranges[0], load_ports=3, store_ports=2, access_ports=4, latency=1)
    system.cpu.dcache_port = system.spad.port
    system.membus.mem_side_ports = system.spad.port

tpu.simulate(tpu.machine(tpu.arguments(), TPUv6eVPU, multiport_scratchpad))
