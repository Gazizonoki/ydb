# Generated by devtools/yamaker.

LIBRARY()

LICENSE(
    Apache-2.0 WITH LLVM-exception AND
    NCSA
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/include
    contrib/libs/llvm16/lib/Analysis
    contrib/libs/llvm16/lib/BinaryFormat
    contrib/libs/llvm16/lib/CodeGen
    contrib/libs/llvm16/lib/CodeGen/AsmPrinter
    contrib/libs/llvm16/lib/CodeGen/GlobalISel
    contrib/libs/llvm16/lib/CodeGen/SelectionDAG
    contrib/libs/llvm16/lib/IR
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/Target
    contrib/libs/llvm16/lib/Target/PowerPC/MCTargetDesc
    contrib/libs/llvm16/lib/Target/PowerPC/TargetInfo
    contrib/libs/llvm16/lib/TargetParser
    contrib/libs/llvm16/lib/Transforms/Scalar
    contrib/libs/llvm16/lib/Transforms/Utils
)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/llvm16/lib/Target/PowerPC
    contrib/libs/llvm16/lib/Target/PowerPC
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    GISel/PPCCallLowering.cpp
    GISel/PPCInstructionSelector.cpp
    GISel/PPCLegalizerInfo.cpp
    GISel/PPCRegisterBankInfo.cpp
    PPCAsmPrinter.cpp
    PPCBoolRetToInt.cpp
    PPCBranchCoalescing.cpp
    PPCBranchSelector.cpp
    PPCCCState.cpp
    PPCCTRLoops.cpp
    PPCCTRLoopsVerify.cpp
    PPCCallingConv.cpp
    PPCEarlyReturn.cpp
    PPCExpandAtomicPseudoInsts.cpp
    PPCExpandISEL.cpp
    PPCFastISel.cpp
    PPCFrameLowering.cpp
    PPCGenScalarMASSEntries.cpp
    PPCHazardRecognizers.cpp
    PPCISelDAGToDAG.cpp
    PPCISelLowering.cpp
    PPCInstrInfo.cpp
    PPCLoopInstrFormPrep.cpp
    PPCLowerMASSVEntries.cpp
    PPCMCInstLower.cpp
    PPCMIPeephole.cpp
    PPCMachineFunctionInfo.cpp
    PPCMachineScheduler.cpp
    PPCMacroFusion.cpp
    PPCPreEmitPeephole.cpp
    PPCReduceCRLogicals.cpp
    PPCRegisterInfo.cpp
    PPCSubtarget.cpp
    PPCTLSDynamicCall.cpp
    PPCTOCRegDeps.cpp
    PPCTargetMachine.cpp
    PPCTargetObjectFile.cpp
    PPCTargetTransformInfo.cpp
    PPCVSXCopy.cpp
    PPCVSXFMAMutate.cpp
    PPCVSXSwapRemoval.cpp
)

END()