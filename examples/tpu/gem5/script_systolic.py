# The tpu machine in syscall emulation: PyTorchSim's gem5_script/script_systolic.py on the gem5 of branch vcix.
# Usage: gem5.opt script_systolic.py -c BINARY --model libtpu.so [--machine-config machine.yml] [--vlane N] [--vlen N]
# examples/tpuv6e/gem5/script_systolic.py builds its machine with this file's functions, its own CPU and scratchpad.
import os
import time
import argparse
import sys
import math
import m5
from m5.objects import *

sys.path.append(os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", ".."))
from machine_description import machine_description

def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--cmd", default="", help="The binary to run in syscall emulation mode.")
    parser.add_argument("-o", "--options", default="", help="""The options to pass to the binary, use around the entire string""")
    parser.add_argument("--cpu", choices=["RiscvAtomicSimpleCPU", "RiscvTimingSimpleCPU", "RiscvMinorCPU", "RiscvDerivO3CPU",
                                          "RiscvMinorCPU", "RiscvCustomCPU", "RiscvMinorV2CPU", "RiscvMinorV4CPU", "RiscvVPU",
                                          "RiscvSparseVPU"], default="RiscvVPU")
    parser.add_argument("--mem", choices=["SimpleMemory", "ScratchpadMemory", "DDR3_1600_8x8"], default="ScratchpadMemory")
    parser.add_argument("--sparse", type=bool, default=False)
    parser.add_argument("--model", required=True, help="The accelerator model (.so) of the tpu unit.")
    parser.add_argument("--machine-config", help="The machine description (YAML) the model is configured from.")
    parser.add_argument("--vlane", type=int, help="The model's vpu_num_lanes, over the machine description's.")
    parser.add_argument("--vlen", type=int, default=256)
    return parser.parse_args()

class InstMemory(SimpleMemory):
    latency = "1ns"
    bandwidth = "64GB/s"

class SpadMemory(SimpleMemory):
    latency = "1ns"

    def __init__(self, bandwidth="4GB/s"):
        super().__init__()
        self.bandwidth = bandwidth

class MultiBankMemorySystem():
    def __init__(self, bus_port, mem_range, num_banks=8, granule_size=4, total_bandwidth="32GB/s"):
        self.num_banks = num_banks
        self.granule_size = granule_size

        self.intlvBits = int(math.log2(self.num_banks))
        self.intlvLowBit = int(math.log2(self.granule_size))
        self.intlvHighBit = self.intlvLowBit + self.intlvBits - 1
        self.mem_ctrls = []
        self.bandwidth_per_bank = self.divide_bandwidth(total_bandwidth[:-2], self.num_banks)

        self.create_memory_banks(bus_port, mem_range)

    # One memory per bank, interleaved over the range.
    def create_memory_banks(self, bus_port, mem_range):
        for i in range(self.num_banks):
            mem = SpadMemory(self.bandwidth_per_bank)
            if self.num_banks!=1:
                print("intlvBits:",self.intlvBits, " intlvHighBits: ", self.intlvHighBit)
                mem.range = AddrRange(
                    start=mem_range.start, size=mem_range.size(),
                    intlvBits=self.intlvBits, intlvMatch=i, intlvHighBit=self.intlvHighBit
                )
            else:
                mem.range = AddrRange(start=mem_range.start, size=mem_range.size())
            mem.port = bus_port
            self.mem_ctrls.append(mem)

    def divide_bandwidth(self, total_bandwidth, num_banks):
        total_bandwidth_bytes = self.bandwidth_to_bytes(total_bandwidth)
        per_bank_bandwidth_bytes = total_bandwidth_bytes / num_banks
        return self.bytes_to_bandwidth(per_bank_bandwidth_bytes)

    def bandwidth_to_bytes(self, bandwidth):
        value, unit = bandwidth[:-2], bandwidth[-2:]
        value = float(value)
        if unit == "GB":
            return value * 1e9
        elif unit == "MB":
            return value * 1e6
        elif unit == "KB":
            return value * 1e3
        elif unit == "B":
            return value
        else:
            raise ValueError(f"Unknown bandwidth unit: {unit}")

    def bytes_to_bandwidth(self, bandwidth_bytes):
        if bandwidth_bytes >= 1e9:
            return f"{bandwidth_bytes / 1e9}GB/s"
        elif bandwidth_bytes >= 1e6:
            return f"{bandwidth_bytes / 1e6}MB/s"
        elif bandwidth_bytes >= 1e3:
            return f"{bandwidth_bytes / 1e3}KB/s"
        else:
            return f"{bandwidth_bytes}B/s"

    def get_ctrls(self):
        return self.mem_ctrls

class L1Cache(NoncoherentCache):
    assoc = 8
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20
    def connectBus(self, bus):
        self.mem_side = bus.cpu_side_ports

    def connectCPU(self, cpu):
        raise NotImplementedError

class L1ICache(L1Cache):
    size = "8192kB"
    tag_latency = 0
    data_latency = 0
    response_latency = 0

    def connectCPU(self, cpu):
        self.cpu_side = cpu.icache_port

# machine <args> <vpu> <scratchpad>: the system, its CPU the --cpu named, vpu the machine's RiscvVPU; scratchpad(system)
# connects the CPU's data port and the bus's memory side
def machine(args, vpu, scratchpad):
    valid_cpu = {
        "RiscvMinorCPU": RiscvMinorCPU,
        "RiscvDerivO3CPU": RiscvO3CPU,
        "RiscvVPU": vpu,
    }

    # The model's configuration: the machine description, with --vlane over its vpu_num_lanes.
    settings = machine_description(args.machine_config) if args.machine_config else {}
    if args.vlane is not None:
        settings["vpu_num_lanes"] = str(args.vlane)
    binary = args.cmd

    # Main System Setup
    system = System()
    system.workload = SEWorkload.init_compatible(binary)

    # Clock setting
    system.clk_domain = SrcClockDomain()
    system.clk_domain.clock = "1GHz"
    system.clk_domain.voltage_domain = VoltageDomain()

    fast_clk = SrcClockDomain()
    fast_clk.clock = '8GHz'
    fast_clk.voltage_domain = VoltageDomain()

    system.mem_mode = "timing"
    system.cache_line_size = 64
    system.cpu = valid_cpu[args.cpu]()
    system.cpu.ArchISA.vlen = args.vlen
    for unit in system.cpu.executeFuncUnits.funcUnits:
        if isinstance(unit, MinorVcixAccelFU):
            unit.vcixModel = args.model
            unit.vcixConfigKeys = list(settings.keys())
            unit.vcixConfigValues = list(settings.values())

    system.mem_ranges = [AddrRange(start=0, size="16GB")]

    system.membus = SpmXBar(
            width = 64,
            header_latency = 0,
            frontend_latency = 0,
            forward_latency = 0,
            response_latency = 0)
    system.membus.clk_domain = fast_clk

    # Instruction cache connection
    system.cpu.icache= L1ICache()
    system.cpu.icache.connectCPU(system.cpu)
    system.cpu.icache.connectBus(system.membus)
    scratchpad(system)
    system.cpu.createInterruptController()

    system.system_port = system.membus.cpu_side_ports

    process = Process()
    process.cmd = [binary] + args.options.split()
    system.cpu.workload = process
    system.cpu.createThreads()
    return system

# banked_scratchpad <system>: this machine's scratchpad. The scratchpad is two banks, interleaved by the 64-byte line,
# 64 GB/s in all (32 GB/s each); the original's is one bank of 32 GB/s. The interleave is no finer than
# cache_line_size: an access of a whole line in two banks has no destination on the bus (gem5's fatal "Unable to find
# destination for [0:0x40]" at 32 bytes).
def banked_scratchpad(system):
    granule_sz = 64
    spad_num_bank = 2
    spad_bandwidth = "64GB/s"
    system.cpu.dcache_port = system.membus.cpu_side_ports
    multi_banked_spm = MultiBankMemorySystem(system.membus.mem_side_ports, system.mem_ranges[0], num_banks=spad_num_bank, granule_size=granule_sz, total_bandwidth=spad_bandwidth)
    system.mem_ctrls = multi_banked_spm.get_ctrls()

def simulate(system):
    root = Root(full_system=False, system=system)
    m5.instantiate()
    start_time = time.time()
    exit_event = m5.simulate()

    if exit_event.getCause() != "exiting with last active thread context":
        exit(1)
    end_time = time.time()
    elapsed_seconds = end_time - start_time
    print(f"Simulation time: {elapsed_seconds:.6f} seconds")

# Run as gem5's script, not imported by another machine's: this is the generic machine, which times a model only as a
# stand-in. A warning on stderr says so; nothing the run writes to m5out changes.
if __name__ == "__m5_main__":
    print("warning: examples/tpu/gem5 is the generic machine; time a model with its own machine, "
          "examples/<model>/gem5 (e.g. examples/tpuv6e/gem5)", file=sys.stderr)
    from vpu_config import RiscvVPU
    simulate(machine(arguments(), RiscvVPU, banked_scratchpad))
