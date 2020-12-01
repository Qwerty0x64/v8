// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/backend/instruction-scheduler.h"

namespace v8 {
namespace internal {
namespace compiler {

bool InstructionScheduler::SchedulerSupported() { return true; }

int InstructionScheduler::GetTargetInstructionFlags(
    const Instruction* instr) const {
  switch (instr->arch_opcode()) {
    case kIA32Add:
    case kIA32And:
    case kIA32Cmp:
    case kIA32Cmp16:
    case kIA32Cmp8:
    case kIA32Test:
    case kIA32Test16:
    case kIA32Test8:
    case kIA32Or:
    case kIA32Xor:
    case kIA32Sub:
    case kIA32Imul:
    case kIA32ImulHigh:
    case kIA32UmulHigh:
    case kIA32Not:
    case kIA32Neg:
    case kIA32Shl:
    case kIA32Shr:
    case kIA32Sar:
    case kIA32AddPair:
    case kIA32SubPair:
    case kIA32MulPair:
    case kIA32ShlPair:
    case kIA32ShrPair:
    case kIA32SarPair:
    case kIA32Rol:
    case kIA32Ror:
    case kIA32Lzcnt:
    case kIA32Tzcnt:
    case kIA32Popcnt:
    case kIA32Bswap:
    case kIA32Lea:
    case kSSEFloat32Cmp:
    case kSSEFloat32Add:
    case kSSEFloat32Sub:
    case kSSEFloat32Mul:
    case kSSEFloat32Div:
    case kSSEFloat32Abs:
    case kSSEFloat32Neg:
    case kSSEFloat32Sqrt:
    case kSSEFloat32Round:
    case kSSEFloat64Cmp:
    case kSSEFloat64Add:
    case kSSEFloat64Sub:
    case kSSEFloat64Mul:
    case kSSEFloat64Div:
    case kSSEFloat64Mod:
    case kSSEFloat32Max:
    case kSSEFloat64Max:
    case kSSEFloat32Min:
    case kSSEFloat64Min:
    case kSSEFloat64Abs:
    case kSSEFloat64Neg:
    case kSSEFloat64Sqrt:
    case kSSEFloat64Round:
    case kSSEFloat32ToFloat64:
    case kSSEFloat64ToFloat32:
    case kSSEFloat32ToInt32:
    case kSSEFloat32ToUint32:
    case kSSEFloat64ToInt32:
    case kSSEFloat64ToUint32:
    case kSSEInt32ToFloat32:
    case kSSEUint32ToFloat32:
    case kSSEInt32ToFloat64:
    case kSSEUint32ToFloat64:
    case kSSEFloat64ExtractLowWord32:
    case kSSEFloat64ExtractHighWord32:
    case kSSEFloat64InsertLowWord32:
    case kSSEFloat64InsertHighWord32:
    case kSSEFloat64LoadLowWord32:
    case kSSEFloat64SilenceNaN:
    case kAVXFloat32Add:
    case kAVXFloat32Sub:
    case kAVXFloat32Mul:
    case kAVXFloat32Div:
    case kAVXFloat64Add:
    case kAVXFloat64Sub:
    case kAVXFloat64Mul:
    case kAVXFloat64Div:
    case kAVXFloat64Abs:
    case kAVXFloat64Neg:
    case kAVXFloat32Abs:
    case kAVXFloat32Neg:
    case kIA32BitcastFI:
    case kIA32BitcastIF:
    case kSSEF64x2Splat:
    case kAVXF64x2Splat:
    case kSSEF64x2ExtractLane:
    case kAVXF64x2ExtractLane:
    case kSSEF64x2ReplaceLane:
    case kAVXF64x2ReplaceLane:
    case kIA32F64x2Sqrt:
    case kIA32F64x2Add:
    case kIA32F64x2Sub:
    case kIA32F64x2Mul:
    case kIA32F64x2Div:
    case kIA32F64x2Min:
    case kIA32F64x2Max:
    case kIA32F64x2Eq:
    case kIA32F64x2Ne:
    case kIA32F64x2Lt:
    case kIA32F64x2Le:
    case kIA32F64x2Pmin:
    case kIA32F64x2Pmax:
    case kIA32F64x2Round:
    case kIA32I64x2SplatI32Pair:
    case kIA32I64x2ReplaceLaneI32Pair:
    case kIA32I64x2Neg:
    case kIA32I64x2Shl:
    case kIA32I64x2ShrS:
    case kIA32I64x2Add:
    case kIA32I64x2Sub:
    case kIA32I64x2Mul:
    case kIA32I64x2ShrU:
    case kIA32I64x2BitMask:
    case kIA32I64x2Eq:
    case kSSEF32x4Splat:
    case kAVXF32x4Splat:
    case kSSEF32x4ExtractLane:
    case kAVXF32x4ExtractLane:
    case kSSEF32x4ReplaceLane:
    case kAVXF32x4ReplaceLane:
    case kIA32F32x4SConvertI32x4:
    case kSSEF32x4UConvertI32x4:
    case kAVXF32x4UConvertI32x4:
    case kSSEF32x4Abs:
    case kAVXF32x4Abs:
    case kSSEF32x4Neg:
    case kAVXF32x4Neg:
    case kSSEF32x4Sqrt:
    case kAVXF32x4Sqrt:
    case kIA32F32x4RecipApprox:
    case kIA32F32x4RecipSqrtApprox:
    case kSSEF32x4Add:
    case kAVXF32x4Add:
    case kSSEF32x4AddHoriz:
    case kAVXF32x4AddHoriz:
    case kSSEF32x4Sub:
    case kAVXF32x4Sub:
    case kSSEF32x4Mul:
    case kAVXF32x4Mul:
    case kSSEF32x4Div:
    case kAVXF32x4Div:
    case kSSEF32x4Min:
    case kAVXF32x4Min:
    case kSSEF32x4Max:
    case kAVXF32x4Max:
    case kSSEF32x4Eq:
    case kAVXF32x4Eq:
    case kSSEF32x4Ne:
    case kAVXF32x4Ne:
    case kSSEF32x4Lt:
    case kAVXF32x4Lt:
    case kSSEF32x4Le:
    case kAVXF32x4Le:
    case kIA32F32x4Pmin:
    case kIA32F32x4Pmax:
    case kIA32F32x4Round:
    case kIA32I32x4Splat:
    case kIA32I32x4ExtractLane:
    case kSSEI32x4ReplaceLane:
    case kAVXI32x4ReplaceLane:
    case kSSEI32x4SConvertF32x4:
    case kAVXI32x4SConvertF32x4:
    case kIA32I32x4SConvertI16x8Low:
    case kIA32I32x4SConvertI16x8High:
    case kIA32I32x4Neg:
    case kIA32I32x4Shl:
    case kIA32I32x4ShrS:
    case kSSEI32x4Add:
    case kAVXI32x4Add:
    case kSSEI32x4AddHoriz:
    case kAVXI32x4AddHoriz:
    case kSSEI32x4Sub:
    case kAVXI32x4Sub:
    case kSSEI32x4Mul:
    case kAVXI32x4Mul:
    case kSSEI32x4MinS:
    case kAVXI32x4MinS:
    case kSSEI32x4MaxS:
    case kAVXI32x4MaxS:
    case kSSEI32x4Eq:
    case kAVXI32x4Eq:
    case kSSEI32x4Ne:
    case kAVXI32x4Ne:
    case kSSEI32x4GtS:
    case kAVXI32x4GtS:
    case kSSEI32x4GeS:
    case kAVXI32x4GeS:
    case kSSEI32x4UConvertF32x4:
    case kAVXI32x4UConvertF32x4:
    case kIA32I32x4UConvertI16x8Low:
    case kIA32I32x4UConvertI16x8High:
    case kIA32I32x4ShrU:
    case kSSEI32x4MinU:
    case kAVXI32x4MinU:
    case kSSEI32x4MaxU:
    case kAVXI32x4MaxU:
    case kSSEI32x4GtU:
    case kAVXI32x4GtU:
    case kSSEI32x4GeU:
    case kAVXI32x4GeU:
    case kIA32I32x4Abs:
    case kIA32I32x4BitMask:
    case kIA32I32x4DotI16x8S:
    case kIA32I16x8Splat:
    case kIA32I16x8ExtractLaneU:
    case kIA32I16x8ExtractLaneS:
    case kSSEI16x8ReplaceLane:
    case kAVXI16x8ReplaceLane:
    case kIA32I16x8SConvertI8x16Low:
    case kIA32I16x8SConvertI8x16High:
    case kIA32I16x8Neg:
    case kIA32I16x8Shl:
    case kIA32I16x8ShrS:
    case kSSEI16x8SConvertI32x4:
    case kAVXI16x8SConvertI32x4:
    case kSSEI16x8Add:
    case kAVXI16x8Add:
    case kSSEI16x8AddSatS:
    case kAVXI16x8AddSatS:
    case kSSEI16x8AddHoriz:
    case kAVXI16x8AddHoriz:
    case kSSEI16x8Sub:
    case kAVXI16x8Sub:
    case kSSEI16x8SubSatS:
    case kAVXI16x8SubSatS:
    case kSSEI16x8Mul:
    case kAVXI16x8Mul:
    case kSSEI16x8MinS:
    case kAVXI16x8MinS:
    case kSSEI16x8MaxS:
    case kAVXI16x8MaxS:
    case kSSEI16x8Eq:
    case kAVXI16x8Eq:
    case kSSEI16x8Ne:
    case kAVXI16x8Ne:
    case kSSEI16x8GtS:
    case kAVXI16x8GtS:
    case kSSEI16x8GeS:
    case kAVXI16x8GeS:
    case kIA32I16x8UConvertI8x16Low:
    case kIA32I16x8UConvertI8x16High:
    case kIA32I16x8ShrU:
    case kSSEI16x8UConvertI32x4:
    case kAVXI16x8UConvertI32x4:
    case kSSEI16x8AddSatU:
    case kAVXI16x8AddSatU:
    case kSSEI16x8SubSatU:
    case kAVXI16x8SubSatU:
    case kSSEI16x8MinU:
    case kAVXI16x8MinU:
    case kSSEI16x8MaxU:
    case kAVXI16x8MaxU:
    case kSSEI16x8GtU:
    case kAVXI16x8GtU:
    case kSSEI16x8GeU:
    case kAVXI16x8GeU:
    case kIA32I16x8RoundingAverageU:
    case kIA32I16x8Abs:
    case kIA32I16x8BitMask:
    case kIA32I8x16Splat:
    case kIA32I8x16ExtractLaneU:
    case kIA32I8x16ExtractLaneS:
    case kSSEI8x16ReplaceLane:
    case kAVXI8x16ReplaceLane:
    case kIA32Pinsrb:
    case kIA32Pinsrw:
    case kIA32Pinsrd:
    case kIA32Pextrb:
    case kIA32Pextrw:
    case kIA32S128Store32Lane:
    case kSSEI8x16SConvertI16x8:
    case kAVXI8x16SConvertI16x8:
    case kIA32I8x16Neg:
    case kIA32I8x16Shl:
    case kIA32I8x16ShrS:
    case kSSEI8x16Add:
    case kAVXI8x16Add:
    case kSSEI8x16AddSatS:
    case kAVXI8x16AddSatS:
    case kSSEI8x16Sub:
    case kAVXI8x16Sub:
    case kSSEI8x16SubSatS:
    case kAVXI8x16SubSatS:
    case kSSEI8x16Mul:
    case kAVXI8x16Mul:
    case kSSEI8x16MinS:
    case kAVXI8x16MinS:
    case kSSEI8x16MaxS:
    case kAVXI8x16MaxS:
    case kSSEI8x16Eq:
    case kAVXI8x16Eq:
    case kSSEI8x16Ne:
    case kAVXI8x16Ne:
    case kSSEI8x16GtS:
    case kAVXI8x16GtS:
    case kSSEI8x16GeS:
    case kAVXI8x16GeS:
    case kSSEI8x16UConvertI16x8:
    case kAVXI8x16UConvertI16x8:
    case kSSEI8x16AddSatU:
    case kAVXI8x16AddSatU:
    case kSSEI8x16SubSatU:
    case kAVXI8x16SubSatU:
    case kIA32I8x16ShrU:
    case kSSEI8x16MinU:
    case kAVXI8x16MinU:
    case kSSEI8x16MaxU:
    case kAVXI8x16MaxU:
    case kSSEI8x16GtU:
    case kAVXI8x16GtU:
    case kSSEI8x16GeU:
    case kAVXI8x16GeU:
    case kIA32I8x16RoundingAverageU:
    case kIA32I8x16Abs:
    case kIA32I8x16BitMask:
    case kIA32S128Const:
    case kIA32S128Zero:
    case kIA32S128AllOnes:
    case kSSES128Not:
    case kAVXS128Not:
    case kSSES128And:
    case kAVXS128And:
    case kSSES128Or:
    case kAVXS128Or:
    case kSSES128Xor:
    case kAVXS128Xor:
    case kSSES128Select:
    case kAVXS128Select:
    case kIA32S128AndNot:
    case kIA32I8x16Swizzle:
    case kIA32I8x16Shuffle:
    case kIA32S32x4Swizzle:
    case kIA32S32x4Shuffle:
    case kIA32S16x8Blend:
    case kIA32S16x8HalfShuffle1:
    case kIA32S16x8HalfShuffle2:
    case kIA32S8x16Alignr:
    case kIA32S16x8Dup:
    case kIA32S8x16Dup:
    case kSSES16x8UnzipHigh:
    case kAVXS16x8UnzipHigh:
    case kSSES16x8UnzipLow:
    case kAVXS16x8UnzipLow:
    case kSSES8x16UnzipHigh:
    case kAVXS8x16UnzipHigh:
    case kSSES8x16UnzipLow:
    case kAVXS8x16UnzipLow:
    case kIA32S64x2UnpackHigh:
    case kIA32S32x4UnpackHigh:
    case kIA32S16x8UnpackHigh:
    case kIA32S8x16UnpackHigh:
    case kIA32S64x2UnpackLow:
    case kIA32S32x4UnpackLow:
    case kIA32S16x8UnpackLow:
    case kIA32S8x16UnpackLow:
    case kSSES8x16TransposeLow:
    case kAVXS8x16TransposeLow:
    case kSSES8x16TransposeHigh:
    case kAVXS8x16TransposeHigh:
    case kSSES8x8Reverse:
    case kAVXS8x8Reverse:
    case kSSES8x4Reverse:
    case kAVXS8x4Reverse:
    case kSSES8x2Reverse:
    case kAVXS8x2Reverse:
    case kIA32V32x4AnyTrue:
    case kIA32V32x4AllTrue:
    case kIA32V16x8AnyTrue:
    case kIA32V16x8AllTrue:
    case kIA32V8x16AnyTrue:
    case kIA32V8x16AllTrue:
      return (instr->addressing_mode() == kMode_None)
                 ? kNoOpcodeFlags
                 : kIsLoadOperation | kHasSideEffect;

    case kIA32Idiv:
    case kIA32Udiv:
      return (instr->addressing_mode() == kMode_None)
                 ? kMayNeedDeoptOrTrapCheck
                 : kMayNeedDeoptOrTrapCheck | kIsLoadOperation | kHasSideEffect;

    case kIA32Movsxbl:
    case kIA32Movzxbl:
    case kIA32Movb:
    case kIA32Movsxwl:
    case kIA32Movzxwl:
    case kIA32Movw:
    case kIA32Movl:
    case kIA32Movss:
    case kIA32Movsd:
    case kIA32Movdqu:
    case kIA32Movlps:
    case kIA32Movhps:
    // Moves are used for memory load/store operations.
    case kIA32S128Load8Splat:
    case kIA32S128Load16Splat:
    case kIA32S128Load32Splat:
    case kIA32S128Load64Splat:
    case kIA32S128Load8x8S:
    case kIA32S128Load8x8U:
    case kIA32S128Load16x4S:
    case kIA32S128Load16x4U:
    case kIA32S128Load32x2S:
    case kIA32S128Load32x2U:
      return instr->HasOutput() ? kIsLoadOperation : kHasSideEffect;

    case kIA32Peek:
      return kIsLoadOperation;

    case kIA32Push:
    case kIA32PushFloat32:
    case kIA32PushFloat64:
    case kIA32PushSimd128:
    case kIA32Poke:
    case kIA32MFence:
    case kIA32LFence:
      return kHasSideEffect;

    case kIA32Word32AtomicPairLoad:
      return kIsLoadOperation;

    case kIA32Word32AtomicPairStore:
    case kIA32Word32AtomicPairAdd:
    case kIA32Word32AtomicPairSub:
    case kIA32Word32AtomicPairAnd:
    case kIA32Word32AtomicPairOr:
    case kIA32Word32AtomicPairXor:
    case kIA32Word32AtomicPairExchange:
    case kIA32Word32AtomicPairCompareExchange:
      return kHasSideEffect;

#define CASE(Name) case k##Name:
      COMMON_ARCH_OPCODE_LIST(CASE)
#undef CASE
      // Already covered in architecture independent code.
      UNREACHABLE();
  }

  UNREACHABLE();
}

int InstructionScheduler::GetInstructionLatency(const Instruction* instr) {
  // Basic latency modeling for ia32 instructions. They have been determined
  // in an empirical way.
  switch (instr->arch_opcode()) {
    case kSSEFloat64Mul:
      return 5;
    case kIA32Imul:
    case kIA32ImulHigh:
      return 5;
    case kSSEFloat32Cmp:
    case kSSEFloat64Cmp:
      return 9;
    case kSSEFloat32Add:
    case kSSEFloat32Sub:
    case kSSEFloat32Abs:
    case kSSEFloat32Neg:
    case kSSEFloat64Add:
    case kSSEFloat64Sub:
    case kSSEFloat64Max:
    case kSSEFloat64Min:
    case kSSEFloat64Abs:
    case kSSEFloat64Neg:
      return 5;
    case kSSEFloat32Mul:
      return 4;
    case kSSEFloat32ToFloat64:
    case kSSEFloat64ToFloat32:
      return 6;
    case kSSEFloat32Round:
    case kSSEFloat64Round:
    case kSSEFloat32ToInt32:
    case kSSEFloat64ToInt32:
      return 8;
    case kSSEFloat32ToUint32:
      return 21;
    case kSSEFloat64ToUint32:
      return 15;
    case kIA32Idiv:
      return 33;
    case kIA32Udiv:
      return 26;
    case kSSEFloat32Div:
      return 35;
    case kSSEFloat64Div:
      return 63;
    case kSSEFloat32Sqrt:
    case kSSEFloat64Sqrt:
      return 25;
    case kSSEFloat64Mod:
      return 50;
    case kArchTruncateDoubleToI:
      return 9;
    default:
      return 1;
  }
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
