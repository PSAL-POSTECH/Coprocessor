# Test fixture: a bare-metal RISC-V machine in M mode, with one accelerator unit. Usage: gem5.opt gem5_bare.py --model M.so ELF
import argparse
import sys

import m5
from m5.objects import *

parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("--model", required=True)
args = parser.parse_args()


class BareFUPool(MinorFUPool):
    funcUnits = MinorDefaultFUPool.funcUnits + [MinorCoprocessorFU(coprocessorModel=args.model)]


system = System()
system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange(0x80000000, size="16MB")]

system.cpu = RiscvMinorCPU(executeFuncUnits=BareFUPool())

system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.mem_ctrl = SimpleMemory(range=system.mem_ranges[0], port=system.membus.mem_side_ports)
system.system_port = system.membus.cpu_side_ports

system.workload = RiscvBareMetal(bootloader=args.binary)
system.cpu.createThreads()

root = Root(full_system=True, system=system)
m5.instantiate()
event = m5.simulate(10**9)
print(f"exit: {event.getCause()}, code {event.getCode()}")
if event.getCause() == "m5_exit instruction encountered":
    sys.exit(0)
sys.exit(event.getCode() or 1)
