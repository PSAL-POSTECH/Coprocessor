# Test fixture for the example and the tests: gem5's default MinorCPU pool plus the accelerator units.
# Usage: gem5.opt gem5_se.py --model M.so [--config machine.yml] [--units N] [--max-in-flight N] [--ready-warn-cycles N] [...] BINARY
import argparse
import sys

import m5
from m5.objects import *

from machine_description import machine_description

parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("--model", required=True)
parser.add_argument("--config", help="machine description (YAML) the model is configured from")
parser.add_argument("--vlen", type=int, default=256)
parser.add_argument("--max-ticks", type=int, help="stop the simulation after this many ticks")
parser.add_argument("--units", type=int, default=1, help="accelerator units, each naming the model")
parser.add_argument("--max-in-flight", type=int, help="each unit's coprocessorMaxInFlight, when not gem5's default")
parser.add_argument("--ready-warn-cycles", type=int, help="each unit's vcixReadyWarnCycles, when not gem5's default")
args = parser.parse_args()

settings = machine_description(args.config) if args.config else {}

bound = {} if args.max_in_flight is None else {"coprocessorMaxInFlight": args.max_in_flight}
if args.ready_warn_cycles is not None:
    bound["vcixReadyWarnCycles"] = args.ready_warn_cycles


class ExampleFUPool(MinorFUPool):
    funcUnits = MinorDefaultFUPool.funcUnits + [
        MinorCoprocessorFU(
            coprocessorModel=args.model,
            coprocessorConfigKeys=list(settings.keys()),
            coprocessorConfigValues=list(settings.values()),
            **bound,
        )
        for _ in range(args.units)
    ]


system = System()
system.clk_domain = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MB")]

system.cpu = RiscvMinorCPU(executeFuncUnits=ExampleFUPool())
system.cpu.ArchISA.vlen = args.vlen

system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.mem_ctrl = SimpleMemory(range=system.mem_ranges[0], port=system.membus.mem_side_ports)
system.system_port = system.membus.cpu_side_ports

system.workload = SEWorkload.init_compatible(args.binary)
system.cpu.workload = Process(cmd=[args.binary])
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate(*([args.max_ticks] if args.max_ticks is not None else []))
print(f"exit: {event.getCause()} at cycle {m5.curTick() // 1000}")

PROGRAM_EXITED = "exiting with last active thread context"
if event.getCause() != PROGRAM_EXITED:
    print(f"gem5_se.py: the simulation did not end with the program exiting: {event.getCause()}", file=sys.stderr)
    sys.exit(1)
sys.exit(event.getCode())
