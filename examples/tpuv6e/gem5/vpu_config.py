# The TPU v6e machine's CPU and its functional unit pool: the tpu machine's (examples/tpu/gem5/vpu_config.py), its units
# and widths, with what the v6e changes. Each number below names the v6e fact it stands for, or says it is not one.
# libtpu's static bundle profile cited is that of a v6e's 8192 x 4096 x 4096 bf16 matmul (13475 bundles), which gives
# each kind of slot as (used/slots over the bundles), so slots / 13475 is the slots a bundle.
import importlib.util
import os
import m5
from m5.objects import *

_spec = importlib.util.spec_from_file_location(
    "tpu_vpu_config", os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "tpu", "gem5", "vpu_config.py"))
tpu = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tpu)

# The v6e's VALU: VECTOR_ALU (16027/53900), 4 slots a bundle, any vector ALU op in any of them. So 4 units take every
# vector ALU op class of the tpu machine's adder, multiplier, divider, reduction and misc units (vmv, vslide, vrgather,
# cvt, merge, sgnj) together: a mixed stream issues 4 a cycle, not 4 of each kind. Every class opLat 1, as there.
class MinorVALU(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdAdd",
            "SimdFloatAdd",
            "SimdFloatAlu",
            "SimdFloatCmp",
            "SimdShift",
            "SimdShiftAcc",
            "SimdAddAcc",
            "SimdAlu",
            "SimdCmp",
            "SimdBf16Add",
            "SimdBf16Cmp",
            "SimdMult",
            "SimdFloatMult",
            "SimdMultAcc",
            "SimdMatMultAcc",
            "SimdSqrt",
            "SimdFloatMultAcc",
            "SimdFloatMatMultAcc",
            "SimdFloatSqrt",
            "SimdDotProd",
            "SimdBf16Mult",
            "SimdBf16MultAcc",
            "SimdBf16MatMultAcc",
            "SimdBf16DotProd",
            "SimdDiv",
            "SimdFloatDiv",
            "SimdCvt",
            "SimdFloatCvt",
            "SimdFloatMisc",
            "SimdPredAlu",
            "SimdMisc",
            "SimdExt",
            "SimdFloatExt",
            "SimdBf16Cvt",
            "SimdReduceAdd",
            "SimdReduceAlu",
            "SimdReduceCmp",
            "SimdFloatReduceAdd",
            "SimdFloatReduceCmp",
        ]
    )
    opLat = 1
    unit = "Vpu"

class TPUv6eFUPool(MinorFUPool):
    funcUnits = [
        # Scalar unit
        tpu.MinorFPUnit(),
        tpu.MinorCustomIntFU(),
        tpu.MinorCustomIntFU(),
        tpu.MinorCustomIntMulFU(),
        tpu.MinorCustomIntDivFU(),
        tpu.MinorCustomPredFU(),
        tpu.MinorCustomMemFU(),
        tpu.MinorCustomMiscFU(),

        # Scalar unit
        tpu.MinorFPUnit(),
        tpu.MinorCustomIntFU(),
        tpu.MinorCustomIntFU(),
        tpu.MinorCustomIntMulFU(),
        tpu.MinorCustomIntDivFU(),
        tpu.MinorCustomPredFU(),
        tpu.MinorCustomMemFU(),
        tpu.MinorCustomMiscFU(),

        # Accelerator: the script names its model
        MinorVcixAccelFU(),

        # Vector
        tpu.MinorVecConfig(),
        tpu.MinorVecConfig(),
        tpu.MinorVecLdStore(),
        tpu.MinorVecLdStore(),

        # Vector ALU
        MinorVALU(),
        MinorVALU(),

        # Vector
        tpu.MinorVecConfig(),
        tpu.MinorVecConfig(),
        tpu.MinorVecLdStore(),
        tpu.MinorVecLdStore(),

        # Vector ALU
        MinorVALU(),
        MinorVALU(),

        # A fifth and a sixth integer unit, which the original does not have: a compensation for RVV loads having no
        # immediate offset, not a fact of the v6e (examples/tpuv6e/README.md, "Two LSQ requests and two more integer
        # units"). Last, so that every unit before them keeps its index in the statistics (funcUnits16 is the accelerator).
        tpu.MinorCustomIntFU(),
        tpu.MinorCustomIntFU(),
    ]

class TPUv6eVPU(tpu.RiscvVPU):
    # Two loads or stores enter the LSQ's request (address translation) queue together; gem5's default and the tpu
    # machine's is one. The v6e moves up to 4 vector registers a cycle (VECTOR_LOAD_AND_STORE_AND_MISC (28041/53900),
    # 4 slots a bundle); two requests is a step toward that, not its count. The scratchpad's ports are script_systolic.py's.
    executeLSQRequestsQueueSize = 2

    executeFuncUnits = TPUv6eFUPool()
