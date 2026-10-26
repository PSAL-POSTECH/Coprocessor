# The tpu machine's CPU and its functional unit pool: PyTorchSim's gem5_script/vpu_config.py on the gem5 of branch vcix.
# MinorVALU and vliw_pool are a VLIW TPU's: a model's machine (examples/<model>/gem5) gives its count of VALU slots.
import m5
from m5.objects import *

class MinorFPUnit(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "FloatAdd",
            "FloatCmp",
            "FloatCvt",
            "FloatMult",
            "FloatMultAcc",
            "FloatDiv",
            "FloatMisc",
            "FloatSqrt",
            "Bf16Cvt",
        ]
    )

class MinorVecAdder(MinorFU):
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
        ]
    )
    opLat = 1
    unit = "Vpu"

class MinorVecMultiplier(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
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
        ]
    )
    opLat = 1
    unit = "Vpu"

class MinorVecDivider(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdDiv",
            "SimdFloatDiv",
        ]
    )
    opLat = 1
    unit = "Vpu"

class MinorVecReduce(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdReduceAdd",
            "SimdReduceAlu",
            "SimdReduceCmp",
            "SimdFloatReduceAdd",
            "SimdFloatReduceCmp",
        ]
    )
    opLat = 1
    unit = "Vpu"

class MinorVecLdStore(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdUnitStrideLoad",
            "SimdUnitStrideStore",
            "SimdUnitStrideMaskLoad",
            "SimdUnitStrideMaskStore",
            "SimdStridedLoad",
            "SimdStridedStore",
            "SimdIndexedLoad",
            "SimdIndexedStore",
            "SimdUnitStrideFaultOnlyFirstLoad",
            "SimdWholeRegisterLoad",
            "SimdWholeRegisterStore",
            "SimdUnitStrideSegmentedLoad",
            "SimdUnitStrideSegmentedStore",
            "SimdUnitStrideSegmentedFaultOnlyFirstLoad",
            "SimdStrideSegmentedLoad",
            "SimdStrideSegmentedStore",
        ]
    )
    opLat = 1

class MinorVecMisc(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdCvt",
            "SimdFloatCvt",
            "SimdFloatMisc",
            "SimdPredAlu",
            "SimdMisc",
            "SimdExt",
            "SimdFloatExt",
            "SimdBf16Cvt",
        ]
    )
    opLat = 1

class MinorVecConfig(MinorFU):
    opClasses = minorMakeOpClassSet(
        [
            "SimdConfig",
        ]
    )
    opLat = 1

class MinorCustomIntFU(MinorDefaultIntFU):
    opLat = 1

class MinorCustomIntDivFU(MinorDefaultIntDivFU):
    opLat = 1

class MinorCustomIntMulFU(MinorDefaultIntMulFU):
    opLat = 1

class MinorCustomPredFU(MinorDefaultPredFU):
    opLat = 1

class MinorCustomMemFU(MinorDefaultMemFU):
    opLat = 1

class MinorCustomMiscFU(MinorDefaultMiscFU):
    opLat = 1

class MinorCustomFUPool(MinorFUPool):
    funcUnits = [
        # Scalar unit
        MinorFPUnit(),
        MinorCustomIntFU(),
        MinorCustomIntFU(),
        MinorCustomIntMulFU(),
        MinorCustomIntDivFU(),
        MinorCustomPredFU(),
        MinorCustomMemFU(),
        MinorCustomMiscFU(),

        # Scalar unit
        MinorFPUnit(),
        MinorCustomIntFU(),
        MinorCustomIntFU(),
        MinorCustomIntMulFU(),
        MinorCustomIntDivFU(),
        MinorCustomPredFU(),
        MinorCustomMemFU(),
        MinorCustomMiscFU(),

        # Accelerator: the script names its model
        MinorCoprocessorFU(),

        # Vector
        MinorVecConfig(),
        MinorVecConfig(),
        MinorVecMisc(),
        MinorVecMisc(),
        MinorVecLdStore(),
        MinorVecLdStore(),

        # Vector ALU0
        MinorVecAdder(),
        MinorVecMultiplier(),
        MinorVecDivider(),
        MinorVecReduce(),

        # Vector ALU1
        MinorVecAdder(),
        MinorVecMultiplier(),
        MinorVecDivider(),
        MinorVecReduce(),

        # Vector
        MinorVecConfig(),
        MinorVecConfig(),
        MinorVecMisc(),
        MinorVecMisc(),
        MinorVecLdStore(),
        MinorVecLdStore(),

        # Vector ALU0
        MinorVecAdder(),
        MinorVecMultiplier(),
        MinorVecDivider(),
        MinorVecReduce(),

        # Vector ALU1
        MinorVecAdder(),
        MinorVecMultiplier(),
        MinorVecDivider(),
        MinorVecReduce(),

        # A fifth and a sixth integer unit, which the original does not have: a compensation for RVV loads having no
        # immediate offset, not a fact of the v6e (examples/tpu/README.md, "Two more integer units"). Last, so that
        # every unit before them keeps its index in the statistics (funcUnits16 is the accelerator).
        MinorCustomIntFU(),
        MinorCustomIntFU(),
    ]

# A VALU slot of a VLIW TPU, where any vector ALU op takes any VALU slot: every vector ALU op class of the adder,
# multiplier, divider, reduction and misc units above (vmv, vslide, vrgather, cvt, merge, sgnj), each opLat 1 as there.
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

def _scalar_unit():
    return [MinorFPUnit(), MinorCustomIntFU(), MinorCustomIntFU(), MinorCustomIntMulFU(), MinorCustomIntDivFU(),
            MinorCustomPredFU(), MinorCustomMemFU(), MinorCustomMiscFU()]

def _vector_unit(valus):
    return [MinorVecConfig(), MinorVecConfig(), MinorVecLdStore(), MinorVecLdStore()] + [MinorVALU() for _ in range(valus)]

# vliw_pool <valus>: the pool above with its vector ALUs replaced by valus MinorVALUs, half in each vector group, and the
# fifth and sixth integer units last (examples/tpu/README.md, "Two more integer units"); funcUnits16 is the accelerator.
# The VALUs are funcUnits21.. and from 25 + valus / 2 (21, 22, 27 and 28 of four; 21 and 26 of two).
def vliw_pool(valus):
    if valus < 2 or valus % 2:
        raise ValueError(f"vliw_pool: {valus} VALUs is not an even count of at least 2")
    return MinorFUPool(funcUnits=_scalar_unit() + _scalar_unit() + [MinorCoprocessorFU()] + _vector_unit(valus // 2)
                       + _vector_unit(valus // 2) + [MinorCustomIntFU(), MinorCustomIntFU()])

class RiscvVPU(RiscvMinorCPU):
    fetch1FetchLimit = 8
    decodeInputWidth = 8
    fetch1ToFetch2BackwardDelay = 0
    fetch2InputBufferSize = 8
    decodeInputBufferSize = 8
    decodeInputWidth = 8
    executeInputBufferSize = 128
    executeInputWidth = 12
    executeIssueLimit = 12
    executeCommitLimit = 12

    # Memory
    executeMemoryIssueLimit = 8
    executeMemoryCommitLimit = 8
    executeMaxAccessesInMemory = 8
    executeLSQMaxStoreBufferStoresPerCycle = 8
    executeLSQTransfersQueueSize = 8
    executeLSQStoreBufferSize = 8
    # Two loads or stores enter the LSQ's request (address translation) queue together; gem5's default and the
    # original's is one.
    executeLSQRequestsQueueSize = 2

    executeFuncUnits = MinorCustomFUPool()
