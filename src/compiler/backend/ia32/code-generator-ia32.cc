// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/overflowing-math.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/callable.h"
#include "src/codegen/ia32/assembler-ia32.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/backend/code-generator-impl.h"
#include "src/compiler/backend/code-generator.h"
#include "src/compiler/backend/gap-resolver.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/osr.h"
#include "src/execution/frame-constants.h"
#include "src/execution/frames.h"
#include "src/heap/memory-chunk.h"
#include "src/objects/smi.h"
#include "src/wasm/wasm-code-manager.h"
#include "src/wasm/wasm-objects.h"

namespace v8 {
namespace internal {
namespace compiler {

#define __ tasm()->

#define kScratchDoubleReg xmm0

// Adds IA-32 specific methods for decoding operands.
class IA32OperandConverter : public InstructionOperandConverter {
 public:
  IA32OperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  Operand InputOperand(size_t index, int extra = 0) {
    return ToOperand(instr_->InputAt(index), extra);
  }

  Immediate InputImmediate(size_t index) {
    return ToImmediate(instr_->InputAt(index));
  }

  Operand OutputOperand() { return ToOperand(instr_->Output()); }

  Operand ToOperand(InstructionOperand* op, int extra = 0) {
    if (op->IsRegister()) {
      DCHECK_EQ(0, extra);
      return Operand(ToRegister(op));
    } else if (op->IsFPRegister()) {
      DCHECK_EQ(0, extra);
      return Operand(ToDoubleRegister(op));
    }
    DCHECK(op->IsStackSlot() || op->IsFPStackSlot());
    return SlotToOperand(AllocatedOperand::cast(op)->index(), extra);
  }

  Operand SlotToOperand(int slot, int extra = 0) {
    FrameOffset offset = frame_access_state()->GetFrameOffset(slot);
    return Operand(offset.from_stack_pointer() ? esp : ebp,
                   offset.offset() + extra);
  }

  Immediate ToImmediate(InstructionOperand* operand) {
    Constant constant = ToConstant(operand);
    if (constant.type() == Constant::kInt32 &&
        RelocInfo::IsWasmReference(constant.rmode())) {
      return Immediate(static_cast<Address>(constant.ToInt32()),
                       constant.rmode());
    }
    switch (constant.type()) {
      case Constant::kInt32:
        return Immediate(constant.ToInt32());
      case Constant::kFloat32:
        return Immediate::EmbeddedNumber(constant.ToFloat32());
      case Constant::kFloat64:
        return Immediate::EmbeddedNumber(constant.ToFloat64().value());
      case Constant::kExternalReference:
        return Immediate(constant.ToExternalReference());
      case Constant::kHeapObject:
        return Immediate(constant.ToHeapObject());
      case Constant::kCompressedHeapObject:
        break;
      case Constant::kDelayedStringConstant:
        return Immediate::EmbeddedStringConstant(
            constant.ToDelayedStringConstant());
      case Constant::kInt64:
        break;
      case Constant::kRpoNumber:
        return Immediate::CodeRelativeOffset(ToLabel(operand));
    }
    UNREACHABLE();
  }

  static size_t NextOffset(size_t* offset) {
    size_t i = *offset;
    (*offset)++;
    return i;
  }

  static ScaleFactor ScaleFor(AddressingMode one, AddressingMode mode) {
    STATIC_ASSERT(0 == static_cast<int>(times_1));
    STATIC_ASSERT(1 == static_cast<int>(times_2));
    STATIC_ASSERT(2 == static_cast<int>(times_4));
    STATIC_ASSERT(3 == static_cast<int>(times_8));
    int scale = static_cast<int>(mode - one);
    DCHECK(scale >= 0 && scale < 4);
    return static_cast<ScaleFactor>(scale);
  }

  Operand MemoryOperand(size_t* offset) {
    AddressingMode mode = AddressingModeField::decode(instr_->opcode());
    switch (mode) {
      case kMode_MR: {
        Register base = InputRegister(NextOffset(offset));
        int32_t disp = 0;
        return Operand(base, disp);
      }
      case kMode_MRI: {
        Register base = InputRegister(NextOffset(offset));
        Constant ctant = ToConstant(instr_->InputAt(NextOffset(offset)));
        return Operand(base, ctant.ToInt32(), ctant.rmode());
      }
      case kMode_MR1:
      case kMode_MR2:
      case kMode_MR4:
      case kMode_MR8: {
        Register base = InputRegister(NextOffset(offset));
        Register index = InputRegister(NextOffset(offset));
        ScaleFactor scale = ScaleFor(kMode_MR1, mode);
        int32_t disp = 0;
        return Operand(base, index, scale, disp);
      }
      case kMode_MR1I:
      case kMode_MR2I:
      case kMode_MR4I:
      case kMode_MR8I: {
        Register base = InputRegister(NextOffset(offset));
        Register index = InputRegister(NextOffset(offset));
        ScaleFactor scale = ScaleFor(kMode_MR1I, mode);
        Constant ctant = ToConstant(instr_->InputAt(NextOffset(offset)));
        return Operand(base, index, scale, ctant.ToInt32(), ctant.rmode());
      }
      case kMode_M1:
      case kMode_M2:
      case kMode_M4:
      case kMode_M8: {
        Register index = InputRegister(NextOffset(offset));
        ScaleFactor scale = ScaleFor(kMode_M1, mode);
        int32_t disp = 0;
        return Operand(index, scale, disp);
      }
      case kMode_M1I:
      case kMode_M2I:
      case kMode_M4I:
      case kMode_M8I: {
        Register index = InputRegister(NextOffset(offset));
        ScaleFactor scale = ScaleFor(kMode_M1I, mode);
        Constant ctant = ToConstant(instr_->InputAt(NextOffset(offset)));
        return Operand(index, scale, ctant.ToInt32(), ctant.rmode());
      }
      case kMode_MI: {
        Constant ctant = ToConstant(instr_->InputAt(NextOffset(offset)));
        return Operand(ctant.ToInt32(), ctant.rmode());
      }
      case kMode_Root: {
        Register base = kRootRegister;
        int32_t disp = InputInt32(NextOffset(offset));
        return Operand(base, disp);
      }
      case kMode_None:
        UNREACHABLE();
    }
    UNREACHABLE();
  }

  Operand MemoryOperand(size_t first_input = 0) {
    return MemoryOperand(&first_input);
  }

  Operand NextMemoryOperand(size_t offset = 0) {
    AddressingMode mode = AddressingModeField::decode(instr_->opcode());
    Register base = InputRegister(NextOffset(&offset));
    const int32_t disp = 4;
    if (mode == kMode_MR1) {
      Register index = InputRegister(NextOffset(&offset));
      ScaleFactor scale = ScaleFor(kMode_MR1, kMode_MR1);
      return Operand(base, index, scale, disp);
    } else if (mode == kMode_MRI) {
      Constant ctant = ToConstant(instr_->InputAt(NextOffset(&offset)));
      return Operand(base, ctant.ToInt32() + disp, ctant.rmode());
    } else {
      UNREACHABLE();
    }
  }

  void MoveInstructionOperandToRegister(Register destination,
                                        InstructionOperand* op) {
    if (op->IsImmediate() || op->IsConstant()) {
      gen_->tasm()->mov(destination, ToImmediate(op));
    } else if (op->IsRegister()) {
      gen_->tasm()->Move(destination, ToRegister(op));
    } else {
      gen_->tasm()->mov(destination, ToOperand(op));
    }
  }
};

namespace {

bool HasAddressingMode(Instruction* instr) {
  return instr->addressing_mode() != kMode_None;
}

bool HasImmediateInput(Instruction* instr, size_t index) {
  return instr->InputAt(index)->IsImmediate();
}

bool HasRegisterInput(Instruction* instr, size_t index) {
  return instr->InputAt(index)->IsRegister();
}

class OutOfLineLoadFloat32NaN final : public OutOfLineCode {
 public:
  OutOfLineLoadFloat32NaN(CodeGenerator* gen, XMMRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final {
    __ xorps(result_, result_);
    __ divss(result_, result_);
  }

 private:
  XMMRegister const result_;
};

class OutOfLineLoadFloat64NaN final : public OutOfLineCode {
 public:
  OutOfLineLoadFloat64NaN(CodeGenerator* gen, XMMRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final {
    __ xorpd(result_, result_);
    __ divsd(result_, result_);
  }

 private:
  XMMRegister const result_;
};

class OutOfLineTruncateDoubleToI final : public OutOfLineCode {
 public:
  OutOfLineTruncateDoubleToI(CodeGenerator* gen, Register result,
                             XMMRegister input, StubCallMode stub_mode)
      : OutOfLineCode(gen),
        result_(result),
        input_(input),
        stub_mode_(stub_mode),
        isolate_(gen->isolate()),
        zone_(gen->zone()) {}

  void Generate() final {
    __ AllocateStackSpace(kDoubleSize);
    __ Movsd(MemOperand(esp, 0), input_);
    if (stub_mode_ == StubCallMode::kCallWasmRuntimeStub) {
      // A direct call to a wasm runtime stub defined in this module.
      // Just encode the stub index. This will be patched when the code
      // is added to the native module and copied into wasm code space.
      __ wasm_call(wasm::WasmCode::kDoubleToI, RelocInfo::WASM_STUB_CALL);
    } else if (tasm()->options().inline_offheap_trampolines) {
      __ CallBuiltin(Builtins::kDoubleToI);
    } else {
      __ Call(BUILTIN_CODE(isolate_, DoubleToI), RelocInfo::CODE_TARGET);
    }
    __ mov(result_, MemOperand(esp, 0));
    __ add(esp, Immediate(kDoubleSize));
  }

 private:
  Register const result_;
  XMMRegister const input_;
  StubCallMode stub_mode_;
  Isolate* isolate_;
  Zone* zone_;
};

class OutOfLineRecordWrite final : public OutOfLineCode {
 public:
  OutOfLineRecordWrite(CodeGenerator* gen, Register object, Operand operand,
                       Register value, Register scratch0, Register scratch1,
                       RecordWriteMode mode, StubCallMode stub_mode)
      : OutOfLineCode(gen),
        object_(object),
        operand_(operand),
        value_(value),
        scratch0_(scratch0),
        scratch1_(scratch1),
        mode_(mode),
        stub_mode_(stub_mode),
        zone_(gen->zone()) {}

  void Generate() final {
    if (mode_ > RecordWriteMode::kValueIsPointer) {
      __ JumpIfSmi(value_, exit());
    }
    __ CheckPageFlag(value_, scratch0_,
                     MemoryChunk::kPointersToHereAreInterestingMask, zero,
                     exit());
    __ lea(scratch1_, operand_);
    RememberedSetAction const remembered_set_action =
        mode_ > RecordWriteMode::kValueIsMap ? EMIT_REMEMBERED_SET
                                             : OMIT_REMEMBERED_SET;
    SaveFPRegsMode const save_fp_mode =
        frame()->DidAllocateDoubleRegisters() ? kSaveFPRegs : kDontSaveFPRegs;
    if (mode_ == RecordWriteMode::kValueIsEphemeronKey) {
      __ CallEphemeronKeyBarrier(object_, scratch1_, save_fp_mode);
    } else if (stub_mode_ == StubCallMode::kCallWasmRuntimeStub) {
      // A direct call to a wasm runtime stub defined in this module.
      // Just encode the stub index. This will be patched when the code
      // is added to the native module and copied into wasm code space.
      __ CallRecordWriteStub(object_, scratch1_, remembered_set_action,
                             save_fp_mode, wasm::WasmCode::kRecordWrite);
    } else {
      __ CallRecordWriteStub(object_, scratch1_, remembered_set_action,
                             save_fp_mode);
    }
  }

 private:
  Register const object_;
  Operand const operand_;
  Register const value_;
  Register const scratch0_;
  Register const scratch1_;
  RecordWriteMode const mode_;
  StubCallMode const stub_mode_;
  Zone* zone_;
};

}  // namespace

#define ASSEMBLE_COMPARE(asm_instr)                              \
  do {                                                           \
    if (HasAddressingMode(instr)) {                              \
      size_t index = 0;                                          \
      Operand left = i.MemoryOperand(&index);                    \
      if (HasImmediateInput(instr, index)) {                     \
        __ asm_instr(left, i.InputImmediate(index));             \
      } else {                                                   \
        __ asm_instr(left, i.InputRegister(index));              \
      }                                                          \
    } else {                                                     \
      if (HasImmediateInput(instr, 1)) {                         \
        if (HasRegisterInput(instr, 0)) {                        \
          __ asm_instr(i.InputRegister(0), i.InputImmediate(1)); \
        } else {                                                 \
          __ asm_instr(i.InputOperand(0), i.InputImmediate(1));  \
        }                                                        \
      } else {                                                   \
        if (HasRegisterInput(instr, 1)) {                        \
          __ asm_instr(i.InputRegister(0), i.InputRegister(1));  \
        } else {                                                 \
          __ asm_instr(i.InputRegister(0), i.InputOperand(1));   \
        }                                                        \
      }                                                          \
    }                                                            \
  } while (0)

#define ASSEMBLE_IEEE754_BINOP(name)                                     \
  do {                                                                   \
    /* Pass two doubles as arguments on the stack. */                    \
    __ PrepareCallCFunction(4, eax);                                     \
    __ movsd(Operand(esp, 0 * kDoubleSize), i.InputDoubleRegister(0));   \
    __ movsd(Operand(esp, 1 * kDoubleSize), i.InputDoubleRegister(1));   \
    __ CallCFunction(ExternalReference::ieee754_##name##_function(), 4); \
    /* Return value is in st(0) on ia32. */                              \
    /* Store it into the result register. */                             \
    __ AllocateStackSpace(kDoubleSize);                                  \
    __ fstp_d(Operand(esp, 0));                                          \
    __ movsd(i.OutputDoubleRegister(), Operand(esp, 0));                 \
    __ add(esp, Immediate(kDoubleSize));                                 \
  } while (false)

#define ASSEMBLE_IEEE754_UNOP(name)                                      \
  do {                                                                   \
    /* Pass one double as argument on the stack. */                      \
    __ PrepareCallCFunction(2, eax);                                     \
    __ movsd(Operand(esp, 0 * kDoubleSize), i.InputDoubleRegister(0));   \
    __ CallCFunction(ExternalReference::ieee754_##name##_function(), 2); \
    /* Return value is in st(0) on ia32. */                              \
    /* Store it into the result register. */                             \
    __ AllocateStackSpace(kDoubleSize);                                  \
    __ fstp_d(Operand(esp, 0));                                          \
    __ movsd(i.OutputDoubleRegister(), Operand(esp, 0));                 \
    __ add(esp, Immediate(kDoubleSize));                                 \
  } while (false)

#define ASSEMBLE_BINOP(asm_instr)                             \
  do {                                                        \
    if (HasAddressingMode(instr)) {                           \
      size_t index = 1;                                       \
      Operand right = i.MemoryOperand(&index);                \
      __ asm_instr(i.InputRegister(0), right);                \
    } else {                                                  \
      if (HasImmediateInput(instr, 1)) {                      \
        __ asm_instr(i.InputOperand(0), i.InputImmediate(1)); \
      } else {                                                \
        __ asm_instr(i.InputRegister(0), i.InputOperand(1));  \
      }                                                       \
    }                                                         \
  } while (0)

#define ASSEMBLE_ATOMIC_BINOP(bin_inst, mov_inst, cmpxchg_inst) \
  do {                                                          \
    Label binop;                                                \
    __ bind(&binop);                                            \
    __ mov_inst(eax, i.MemoryOperand(1));                       \
    __ Move(i.TempRegister(0), eax);                            \
    __ bin_inst(i.TempRegister(0), i.InputRegister(0));         \
    __ lock();                                                  \
    __ cmpxchg_inst(i.MemoryOperand(1), i.TempRegister(0));     \
    __ j(not_equal, &binop);                                    \
  } while (false)

#define ASSEMBLE_I64ATOMIC_BINOP(instr1, instr2)                \
  do {                                                          \
    Label binop;                                                \
    __ bind(&binop);                                            \
    __ mov(eax, i.MemoryOperand(2));                            \
    __ mov(edx, i.NextMemoryOperand(2));                        \
    __ push(ebx);                                               \
    frame_access_state()->IncreaseSPDelta(1);                   \
    i.MoveInstructionOperandToRegister(ebx, instr->InputAt(0)); \
    __ push(i.InputRegister(1));                                \
    __ instr1(ebx, eax);                                        \
    __ instr2(i.InputRegister(1), edx);                         \
    __ lock();                                                  \
    __ cmpxchg8b(i.MemoryOperand(2));                           \
    __ pop(i.InputRegister(1));                                 \
    __ pop(ebx);                                                \
    frame_access_state()->IncreaseSPDelta(-1);                  \
    __ j(not_equal, &binop);                                    \
  } while (false);

#define ASSEMBLE_MOVX(mov_instr)                            \
  do {                                                      \
    if (HasAddressingMode(instr)) {                         \
      __ mov_instr(i.OutputRegister(), i.MemoryOperand());  \
    } else if (HasRegisterInput(instr, 0)) {                \
      __ mov_instr(i.OutputRegister(), i.InputRegister(0)); \
    } else {                                                \
      __ mov_instr(i.OutputRegister(), i.InputOperand(0));  \
    }                                                       \
  } while (0)

#define ASSEMBLE_SIMD_PUNPCK_SHUFFLE(opcode)                         \
  do {                                                               \
    XMMRegister src0 = i.InputSimd128Register(0);                    \
    Operand src1 = i.InputOperand(instr->InputCount() == 2 ? 1 : 0); \
    if (CpuFeatures::IsSupported(AVX)) {                             \
      CpuFeatureScope avx_scope(tasm(), AVX);                        \
      __ v##opcode(i.OutputSimd128Register(), src0, src1);           \
    } else {                                                         \
      DCHECK_EQ(i.OutputSimd128Register(), src0);                    \
      __ opcode(i.OutputSimd128Register(), src1);                    \
    }                                                                \
  } while (false)

#define ASSEMBLE_SIMD_IMM_SHUFFLE(opcode, SSELevel, imm)               \
  if (CpuFeatures::IsSupported(AVX)) {                                 \
    CpuFeatureScope avx_scope(tasm(), AVX);                            \
    __ v##opcode(i.OutputSimd128Register(), i.InputSimd128Register(0), \
                 i.InputOperand(1), imm);                              \
  } else {                                                             \
    CpuFeatureScope sse_scope(tasm(), SSELevel);                       \
    DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));   \
    __ opcode(i.OutputSimd128Register(), i.InputOperand(1), imm);      \
  }

#define ASSEMBLE_SIMD_ALL_TRUE(opcode)               \
  do {                                               \
    Register dst = i.OutputRegister();               \
    Operand src = i.InputOperand(0);                 \
    Register tmp = i.TempRegister(0);                \
    XMMRegister tmp_simd = i.TempSimd128Register(1); \
    __ mov(tmp, Immediate(1));                       \
    __ xor_(dst, dst);                               \
    __ Pxor(tmp_simd, tmp_simd);                     \
    __ opcode(tmp_simd, src);                        \
    __ Ptest(tmp_simd, tmp_simd);                    \
    __ cmov(zero, dst, tmp);                         \
  } while (false)

#define ASSEMBLE_SIMD_SHIFT(opcode, width)             \
  do {                                                 \
    XMMRegister dst = i.OutputSimd128Register();       \
    DCHECK_EQ(dst, i.InputSimd128Register(0));         \
    if (HasImmediateInput(instr, 1)) {                 \
      __ opcode(dst, dst, byte{i.InputInt##width(1)}); \
    } else {                                           \
      XMMRegister tmp = i.TempSimd128Register(0);      \
      Register tmp_shift = i.TempRegister(1);          \
      constexpr int mask = (1 << width) - 1;           \
      __ mov(tmp_shift, i.InputRegister(1));           \
      __ and_(tmp_shift, Immediate(mask));             \
      __ Movd(tmp, tmp_shift);                         \
      __ opcode(dst, dst, tmp);                        \
    }                                                  \
  } while (false)

#define ASSEMBLE_SIMD_PINSR(OPCODE, CPU_FEATURE)             \
  do {                                                       \
    XMMRegister dst = i.OutputSimd128Register();             \
    XMMRegister src = i.InputSimd128Register(0);             \
    int8_t laneidx = i.InputInt8(1);                         \
    if (HasAddressingMode(instr)) {                          \
      if (CpuFeatures::IsSupported(AVX)) {                   \
        CpuFeatureScope avx_scope(tasm(), AVX);              \
        __ v##OPCODE(dst, src, i.MemoryOperand(2), laneidx); \
      } else {                                               \
        DCHECK_EQ(dst, src);                                 \
        CpuFeatureScope sse_scope(tasm(), CPU_FEATURE);      \
        __ OPCODE(dst, i.MemoryOperand(2), laneidx);         \
      }                                                      \
    } else {                                                 \
      UNIMPLEMENTED();                                       \
    }                                                        \
  } while (false)

void CodeGenerator::AssembleDeconstructFrame() {
  __ mov(esp, ebp);
  __ pop(ebp);
}

void CodeGenerator::AssemblePrepareTailCall() {
  if (frame_access_state()->has_frame()) {
    __ mov(ebp, MemOperand(ebp, 0));
  }
  frame_access_state()->SetFrameAccessToSP();
}

void CodeGenerator::AssemblePopArgumentsAdaptorFrame(Register args_reg,
                                                     Register, Register,
                                                     Register) {
  // There are not enough temp registers left on ia32 for a call instruction
  // so we pick some scratch registers and save/restore them manually here.
  int scratch_count = 3;
  Register scratch1 = esi;
  Register scratch2 = ecx;
  Register scratch3 = edx;
  DCHECK(!AreAliased(args_reg, scratch1, scratch2, scratch3));
  Label done;

  // Check if current frame is an arguments adaptor frame.
  __ cmp(Operand(ebp, StandardFrameConstants::kContextOffset),
         Immediate(StackFrame::TypeToMarker(StackFrame::ARGUMENTS_ADAPTOR)));
  __ j(not_equal, &done, Label::kNear);

  __ push(scratch1);
  __ push(scratch2);
  __ push(scratch3);

  // Load arguments count from current arguments adaptor frame (note, it
  // does not include receiver).
  Register caller_args_count_reg = scratch1;
  __ mov(caller_args_count_reg,
         Operand(ebp, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ SmiUntag(caller_args_count_reg);

  __ PrepareForTailCall(args_reg, caller_args_count_reg, scratch2, scratch3,
                        scratch_count);
  __ pop(scratch3);
  __ pop(scratch2);
  __ pop(scratch1);

  __ bind(&done);
}

namespace {

void AdjustStackPointerForTailCall(TurboAssembler* tasm,
                                   FrameAccessState* state,
                                   int new_slot_above_sp,
                                   bool allow_shrinkage = true) {
  int current_sp_offset = state->GetSPToFPSlotCount() +
                          StandardFrameConstants::kFixedSlotCountAboveFp;
  int stack_slot_delta = new_slot_above_sp - current_sp_offset;
  if (stack_slot_delta > 0) {
    tasm->AllocateStackSpace(stack_slot_delta * kSystemPointerSize);
    state->IncreaseSPDelta(stack_slot_delta);
  } else if (allow_shrinkage && stack_slot_delta < 0) {
    tasm->add(esp, Immediate(-stack_slot_delta * kSystemPointerSize));
    state->IncreaseSPDelta(stack_slot_delta);
  }
}

#ifdef DEBUG
bool VerifyOutputOfAtomicPairInstr(IA32OperandConverter* converter,
                                   const Instruction* instr) {
  if (instr->OutputCount() == 2) {
    return (converter->OutputRegister(0) == eax &&
            converter->OutputRegister(1) == edx);
  }
  if (instr->OutputCount() == 1) {
    return (converter->OutputRegister(0) == eax &&
            converter->TempRegister(0) == edx) ||
           (converter->OutputRegister(0) == edx &&
            converter->TempRegister(0) == eax);
  }
  DCHECK_EQ(instr->OutputCount(), 0);
  return (converter->TempRegister(0) == eax &&
          converter->TempRegister(1) == edx);
}
#endif

}  // namespace

void CodeGenerator::AssembleTailCallBeforeGap(Instruction* instr,
                                              int first_unused_stack_slot) {
  CodeGenerator::PushTypeFlags flags(kImmediatePush | kScalarPush);
  ZoneVector<MoveOperands*> pushes(zone());
  GetPushCompatibleMoves(instr, flags, &pushes);

  if (!pushes.empty() &&
      (LocationOperand::cast(pushes.back()->destination()).index() + 1 ==
       first_unused_stack_slot)) {
    IA32OperandConverter g(this, instr);
    for (auto move : pushes) {
      LocationOperand destination_location(
          LocationOperand::cast(move->destination()));
      InstructionOperand source(move->source());
      AdjustStackPointerForTailCall(tasm(), frame_access_state(),
                                    destination_location.index());
      if (source.IsStackSlot()) {
        LocationOperand source_location(LocationOperand::cast(source));
        __ push(g.SlotToOperand(source_location.index()));
      } else if (source.IsRegister()) {
        LocationOperand source_location(LocationOperand::cast(source));
        __ push(source_location.GetRegister());
      } else if (source.IsImmediate()) {
        __ Push(Immediate(ImmediateOperand::cast(source).inline_value()));
      } else {
        // Pushes of non-scalar data types is not supported.
        UNIMPLEMENTED();
      }
      frame_access_state()->IncreaseSPDelta(1);
      move->Eliminate();
    }
  }
  AdjustStackPointerForTailCall(tasm(), frame_access_state(),
                                first_unused_stack_slot, false);
}

void CodeGenerator::AssembleTailCallAfterGap(Instruction* instr,
                                             int first_unused_stack_slot) {
  AdjustStackPointerForTailCall(tasm(), frame_access_state(),
                                first_unused_stack_slot);
}

// Check that {kJavaScriptCallCodeStartRegister} is correct.
void CodeGenerator::AssembleCodeStartRegisterCheck() {
  __ push(eax);  // Push eax so we can use it as a scratch register.
  __ ComputeCodeStartAddress(eax);
  __ cmp(eax, kJavaScriptCallCodeStartRegister);
  __ Assert(equal, AbortReason::kWrongFunctionCodeStart);
  __ pop(eax);  // Restore eax.
}

// Check if the code object is marked for deoptimization. If it is, then it
// jumps to the CompileLazyDeoptimizedCode builtin. In order to do this we need
// to:
//    1. read from memory the word that contains that bit, which can be found in
//       the flags in the referenced {CodeDataContainer} object;
//    2. test kMarkedForDeoptimizationBit in those flags; and
//    3. if it is not zero then it jumps to the builtin.
void CodeGenerator::BailoutIfDeoptimized() {
  int offset = Code::kCodeDataContainerOffset - Code::kHeaderSize;
  __ push(eax);  // Push eax so we can use it as a scratch register.
  __ mov(eax, Operand(kJavaScriptCallCodeStartRegister, offset));
  __ test(FieldOperand(eax, CodeDataContainer::kKindSpecificFlagsOffset),
          Immediate(1 << Code::kMarkedForDeoptimizationBit));
  __ pop(eax);  // Restore eax.

  Label skip;
  __ j(zero, &skip, Label::kNear);
  __ Jump(BUILTIN_CODE(isolate(), CompileLazyDeoptimizedCode),
          RelocInfo::CODE_TARGET);
  __ bind(&skip);
}

void CodeGenerator::GenerateSpeculationPoisonFromCodeStartRegister() {
  // TODO(860429): Remove remaining poisoning infrastructure on ia32.
  UNREACHABLE();
}

void CodeGenerator::AssembleRegisterArgumentPoisoning() {
  // TODO(860429): Remove remaining poisoning infrastructure on ia32.
  UNREACHABLE();
}

// Assembles an instruction after register allocation, producing machine code.
CodeGenerator::CodeGenResult CodeGenerator::AssembleArchInstruction(
    Instruction* instr) {
  IA32OperandConverter i(this, instr);
  InstructionCode opcode = instr->opcode();
  ArchOpcode arch_opcode = ArchOpcodeField::decode(opcode);
  switch (arch_opcode) {
    case kArchCallCodeObject: {
      InstructionOperand* op = instr->InputAt(0);
      if (op->IsImmediate()) {
        Handle<Code> code = i.InputCode(0);
        __ Call(code, RelocInfo::CODE_TARGET);
      } else {
        Register reg = i.InputRegister(0);
        DCHECK_IMPLIES(
            instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
            reg == kJavaScriptCallCodeStartRegister);
        __ LoadCodeObjectEntry(reg, reg);
        if (instr->HasCallDescriptorFlag(CallDescriptor::kRetpoline)) {
          __ RetpolineCall(reg);
        } else {
          __ call(reg);
        }
      }
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchCallBuiltinPointer: {
      DCHECK(!HasImmediateInput(instr, 0));
      Register builtin_index = i.InputRegister(0);
      __ CallBuiltinByIndex(builtin_index);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchCallWasmFunction: {
      if (HasImmediateInput(instr, 0)) {
        Constant constant = i.ToConstant(instr->InputAt(0));
        Address wasm_code = static_cast<Address>(constant.ToInt32());
        if (DetermineStubCallMode() == StubCallMode::kCallWasmRuntimeStub) {
          __ wasm_call(wasm_code, constant.rmode());
        } else {
          if (instr->HasCallDescriptorFlag(CallDescriptor::kRetpoline)) {
            __ RetpolineCall(wasm_code, constant.rmode());
          } else {
            __ call(wasm_code, constant.rmode());
          }
        }
      } else {
        Register reg = i.InputRegister(0);
        if (instr->HasCallDescriptorFlag(CallDescriptor::kRetpoline)) {
          __ RetpolineCall(reg);
        } else {
          __ call(reg);
        }
      }
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallCodeObjectFromJSFunction:
    case kArchTailCallCodeObject: {
      if (arch_opcode == kArchTailCallCodeObjectFromJSFunction) {
        AssemblePopArgumentsAdaptorFrame(kJavaScriptCallArgCountRegister,
                                         no_reg, no_reg, no_reg);
      }
      if (HasImmediateInput(instr, 0)) {
        Handle<Code> code = i.InputCode(0);
        __ Jump(code, RelocInfo::CODE_TARGET);
      } else {
        Register reg = i.InputRegister(0);
        DCHECK_IMPLIES(
            instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
            reg == kJavaScriptCallCodeStartRegister);
        __ LoadCodeObjectEntry(reg, reg);
        if (instr->HasCallDescriptorFlag(CallDescriptor::kRetpoline)) {
          __ RetpolineJump(reg);
        } else {
          __ jmp(reg);
        }
      }
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchTailCallWasm: {
      if (HasImmediateInput(instr, 0)) {
        Constant constant = i.ToConstant(instr->InputAt(0));
        Address wasm_code = static_cast<Address>(constant.ToInt32());
        __ jmp(wasm_code, constant.rmode());
      } else {
        Register reg = i.InputRegister(0);
        if (instr->HasCallDescriptorFlag(CallDescriptor::kRetpoline)) {
          __ RetpolineJump(reg);
        } else {
          __ jmp(reg);
        }
      }
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchTailCallAddress: {
      CHECK(!HasImmediateInput(instr, 0));
      Register reg = i.InputRegister(0);
      DCHECK_IMPLIES(
          instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
          reg == kJavaScriptCallCodeStartRegister);
      if (instr->HasCallDescriptorFlag(CallDescriptor::kRetpoline)) {
        __ RetpolineJump(reg);
      } else {
        __ jmp(reg);
      }
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchCallJSFunction: {
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ cmp(esi, FieldOperand(func, JSFunction::kContextOffset));
        __ Assert(equal, AbortReason::kWrongFunctionContext);
      }
      static_assert(kJavaScriptCallCodeStartRegister == ecx, "ABI mismatch");
      __ mov(ecx, FieldOperand(func, JSFunction::kCodeOffset));
      __ CallCodeObject(ecx);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchPrepareCallCFunction: {
      // Frame alignment requires using FP-relative frame addressing.
      frame_access_state()->SetFrameAccessToFP();
      int const num_parameters = MiscField::decode(instr->opcode());
      __ PrepareCallCFunction(num_parameters, i.TempRegister(0));
      break;
    }
    case kArchSaveCallerRegisters: {
      fp_mode_ =
          static_cast<SaveFPRegsMode>(MiscField::decode(instr->opcode()));
      DCHECK(fp_mode_ == kDontSaveFPRegs || fp_mode_ == kSaveFPRegs);
      // kReturnRegister0 should have been saved before entering the stub.
      int bytes = __ PushCallerSaved(fp_mode_, kReturnRegister0);
      DCHECK(IsAligned(bytes, kSystemPointerSize));
      DCHECK_EQ(0, frame_access_state()->sp_delta());
      frame_access_state()->IncreaseSPDelta(bytes / kSystemPointerSize);
      DCHECK(!caller_registers_saved_);
      caller_registers_saved_ = true;
      break;
    }
    case kArchRestoreCallerRegisters: {
      DCHECK(fp_mode_ ==
             static_cast<SaveFPRegsMode>(MiscField::decode(instr->opcode())));
      DCHECK(fp_mode_ == kDontSaveFPRegs || fp_mode_ == kSaveFPRegs);
      // Don't overwrite the returned value.
      int bytes = __ PopCallerSaved(fp_mode_, kReturnRegister0);
      frame_access_state()->IncreaseSPDelta(-(bytes / kSystemPointerSize));
      DCHECK_EQ(0, frame_access_state()->sp_delta());
      DCHECK(caller_registers_saved_);
      caller_registers_saved_ = false;
      break;
    }
    case kArchPrepareTailCall:
      AssemblePrepareTailCall();
      break;
    case kArchCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
      Label return_location;
      if (linkage()->GetIncomingDescriptor()->IsWasmCapiFunction()) {
        // Put the return address in a stack slot.
        Register scratch = eax;
        __ push(scratch);
        __ PushPC();
        int pc = __ pc_offset();
        __ pop(scratch);
        __ sub(scratch, Immediate(pc + Code::kHeaderSize - kHeapObjectTag));
        __ add(scratch, Immediate::CodeRelativeOffset(&return_location));
        __ mov(MemOperand(ebp, WasmExitFrameConstants::kCallingPCOffset),
               scratch);
        __ pop(scratch);
      }
      if (HasImmediateInput(instr, 0)) {
        ExternalReference ref = i.InputExternalReference(0);
        __ CallCFunction(ref, num_parameters);
      } else {
        Register func = i.InputRegister(0);
        __ CallCFunction(func, num_parameters);
      }
      __ bind(&return_location);
      if (linkage()->GetIncomingDescriptor()->IsWasmCapiFunction()) {
        RecordSafepoint(instr->reference_map(), Safepoint::kNoLazyDeopt);
      }
      frame_access_state()->SetFrameAccessToDefault();
      // Ideally, we should decrement SP delta to match the change of stack
      // pointer in CallCFunction. However, for certain architectures (e.g.
      // ARM), there may be more strict alignment requirement, causing old SP
      // to be saved on the stack. In those cases, we can not calculate the SP
      // delta statically.
      frame_access_state()->ClearSPDelta();
      if (caller_registers_saved_) {
        // Need to re-sync SP delta introduced in kArchSaveCallerRegisters.
        // Here, we assume the sequence to be:
        //   kArchSaveCallerRegisters;
        //   kArchCallCFunction;
        //   kArchRestoreCallerRegisters;
        int bytes =
            __ RequiredStackSizeForCallerSaved(fp_mode_, kReturnRegister0);
        frame_access_state()->IncreaseSPDelta(bytes / kSystemPointerSize);
      }
      break;
    }
    case kArchJmp:
      AssembleArchJump(i.InputRpo(0));
      break;
    case kArchBinarySearchSwitch:
      AssembleArchBinarySearchSwitch(instr);
      break;
    case kArchTableSwitch:
      AssembleArchTableSwitch(instr);
      break;
    case kArchComment:
      __ RecordComment(reinterpret_cast<const char*>(i.InputInt32(0)));
      break;
    case kArchAbortCSAAssert:
      DCHECK(i.InputRegister(0) == edx);
      {
        // We don't actually want to generate a pile of code for this, so just
        // claim there is a stack frame, without generating one.
        FrameScope scope(tasm(), StackFrame::NONE);
        __ Call(
            isolate()->builtins()->builtin_handle(Builtins::kAbortCSAAssert),
            RelocInfo::CODE_TARGET);
      }
      __ int3();
      break;
    case kArchDebugBreak:
      __ DebugBreak();
      break;
    case kArchNop:
    case kArchThrowTerminator:
      // don't emit code for nops.
      break;
    case kArchDeoptimize: {
      DeoptimizationExit* exit =
          BuildTranslation(instr, -1, 0, OutputFrameStateCombine::Ignore());
      __ jmp(exit->label());
      break;
    }
    case kArchRet:
      AssembleReturn(instr->InputAt(0));
      break;
    case kArchFramePointer:
      __ mov(i.OutputRegister(), ebp);
      break;
    case kArchParentFramePointer:
      if (frame_access_state()->has_frame()) {
        __ mov(i.OutputRegister(), Operand(ebp, 0));
      } else {
        __ mov(i.OutputRegister(), ebp);
      }
      break;
    case kArchStackPointerGreaterThan: {
      // Potentially apply an offset to the current stack pointer before the
      // comparison to consider the size difference of an optimized frame versus
      // the contained unoptimized frames.
      Register lhs_register = esp;
      uint32_t offset;

      if (ShouldApplyOffsetToStackCheck(instr, &offset)) {
        lhs_register = i.TempRegister(0);
        __ lea(lhs_register, Operand(esp, -1 * static_cast<int32_t>(offset)));
      }

      constexpr size_t kValueIndex = 0;
      if (HasAddressingMode(instr)) {
        __ cmp(lhs_register, i.MemoryOperand(kValueIndex));
      } else {
        __ cmp(lhs_register, i.InputRegister(kValueIndex));
      }
      break;
    }
    case kArchStackCheckOffset:
      __ Move(i.OutputRegister(), Smi::FromInt(GetStackCheckOffset()));
      break;
    case kArchTruncateDoubleToI: {
      auto result = i.OutputRegister();
      auto input = i.InputDoubleRegister(0);
      auto ool = zone()->New<OutOfLineTruncateDoubleToI>(
          this, result, input, DetermineStubCallMode());
      __ cvttsd2si(result, Operand(input));
      __ cmp(result, 1);
      __ j(overflow, ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kArchStoreWithWriteBarrier: {
      RecordWriteMode mode =
          static_cast<RecordWriteMode>(MiscField::decode(instr->opcode()));
      Register object = i.InputRegister(0);
      size_t index = 0;
      Operand operand = i.MemoryOperand(&index);
      Register value = i.InputRegister(index);
      Register scratch0 = i.TempRegister(0);
      Register scratch1 = i.TempRegister(1);
      auto ool = zone()->New<OutOfLineRecordWrite>(this, object, operand, value,
                                                   scratch0, scratch1, mode,
                                                   DetermineStubCallMode());
      __ mov(operand, value);
      __ CheckPageFlag(object, scratch0,
                       MemoryChunk::kPointersFromHereAreInterestingMask,
                       not_zero, ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kArchStackSlot: {
      FrameOffset offset =
          frame_access_state()->GetFrameOffset(i.InputInt32(0));
      Register base = offset.from_stack_pointer() ? esp : ebp;
      __ lea(i.OutputRegister(), Operand(base, offset.offset()));
      break;
    }
    case kIeee754Float64Acos:
      ASSEMBLE_IEEE754_UNOP(acos);
      break;
    case kIeee754Float64Acosh:
      ASSEMBLE_IEEE754_UNOP(acosh);
      break;
    case kIeee754Float64Asin:
      ASSEMBLE_IEEE754_UNOP(asin);
      break;
    case kIeee754Float64Asinh:
      ASSEMBLE_IEEE754_UNOP(asinh);
      break;
    case kIeee754Float64Atan:
      ASSEMBLE_IEEE754_UNOP(atan);
      break;
    case kIeee754Float64Atanh:
      ASSEMBLE_IEEE754_UNOP(atanh);
      break;
    case kIeee754Float64Atan2:
      ASSEMBLE_IEEE754_BINOP(atan2);
      break;
    case kIeee754Float64Cbrt:
      ASSEMBLE_IEEE754_UNOP(cbrt);
      break;
    case kIeee754Float64Cos:
      ASSEMBLE_IEEE754_UNOP(cos);
      break;
    case kIeee754Float64Cosh:
      ASSEMBLE_IEEE754_UNOP(cosh);
      break;
    case kIeee754Float64Expm1:
      ASSEMBLE_IEEE754_UNOP(expm1);
      break;
    case kIeee754Float64Exp:
      ASSEMBLE_IEEE754_UNOP(exp);
      break;
    case kIeee754Float64Log:
      ASSEMBLE_IEEE754_UNOP(log);
      break;
    case kIeee754Float64Log1p:
      ASSEMBLE_IEEE754_UNOP(log1p);
      break;
    case kIeee754Float64Log2:
      ASSEMBLE_IEEE754_UNOP(log2);
      break;
    case kIeee754Float64Log10:
      ASSEMBLE_IEEE754_UNOP(log10);
      break;
    case kIeee754Float64Pow:
      ASSEMBLE_IEEE754_BINOP(pow);
      break;
    case kIeee754Float64Sin:
      ASSEMBLE_IEEE754_UNOP(sin);
      break;
    case kIeee754Float64Sinh:
      ASSEMBLE_IEEE754_UNOP(sinh);
      break;
    case kIeee754Float64Tan:
      ASSEMBLE_IEEE754_UNOP(tan);
      break;
    case kIeee754Float64Tanh:
      ASSEMBLE_IEEE754_UNOP(tanh);
      break;
    case kIA32Add:
      ASSEMBLE_BINOP(add);
      break;
    case kIA32And:
      ASSEMBLE_BINOP(and_);
      break;
    case kIA32Cmp:
      ASSEMBLE_COMPARE(cmp);
      break;
    case kIA32Cmp16:
      ASSEMBLE_COMPARE(cmpw);
      break;
    case kIA32Cmp8:
      ASSEMBLE_COMPARE(cmpb);
      break;
    case kIA32Test:
      ASSEMBLE_COMPARE(test);
      break;
    case kIA32Test16:
      ASSEMBLE_COMPARE(test_w);
      break;
    case kIA32Test8:
      ASSEMBLE_COMPARE(test_b);
      break;
    case kIA32Imul:
      if (HasImmediateInput(instr, 1)) {
        __ imul(i.OutputRegister(), i.InputOperand(0), i.InputInt32(1));
      } else {
        __ imul(i.OutputRegister(), i.InputOperand(1));
      }
      break;
    case kIA32ImulHigh:
      __ imul(i.InputRegister(1));
      break;
    case kIA32UmulHigh:
      __ mul(i.InputRegister(1));
      break;
    case kIA32Idiv:
      __ cdq();
      __ idiv(i.InputOperand(1));
      break;
    case kIA32Udiv:
      __ Move(edx, Immediate(0));
      __ div(i.InputOperand(1));
      break;
    case kIA32Not:
      __ not_(i.OutputOperand());
      break;
    case kIA32Neg:
      __ neg(i.OutputOperand());
      break;
    case kIA32Or:
      ASSEMBLE_BINOP(or_);
      break;
    case kIA32Xor:
      ASSEMBLE_BINOP(xor_);
      break;
    case kIA32Sub:
      ASSEMBLE_BINOP(sub);
      break;
    case kIA32Shl:
      if (HasImmediateInput(instr, 1)) {
        __ shl(i.OutputOperand(), i.InputInt5(1));
      } else {
        __ shl_cl(i.OutputOperand());
      }
      break;
    case kIA32Shr:
      if (HasImmediateInput(instr, 1)) {
        __ shr(i.OutputOperand(), i.InputInt5(1));
      } else {
        __ shr_cl(i.OutputOperand());
      }
      break;
    case kIA32Sar:
      if (HasImmediateInput(instr, 1)) {
        __ sar(i.OutputOperand(), i.InputInt5(1));
      } else {
        __ sar_cl(i.OutputOperand());
      }
      break;
    case kIA32AddPair: {
      // i.OutputRegister(0) == i.InputRegister(0) ... left low word.
      // i.InputRegister(1) ... left high word.
      // i.InputRegister(2) ... right low word.
      // i.InputRegister(3) ... right high word.
      bool use_temp = false;
      if ((HasRegisterInput(instr, 1) &&
           i.OutputRegister(0).code() == i.InputRegister(1).code()) ||
          i.OutputRegister(0).code() == i.InputRegister(3).code()) {
        // We cannot write to the output register directly, because it would
        // overwrite an input for adc. We have to use the temp register.
        use_temp = true;
        __ Move(i.TempRegister(0), i.InputRegister(0));
        __ add(i.TempRegister(0), i.InputRegister(2));
      } else {
        __ add(i.OutputRegister(0), i.InputRegister(2));
      }
      i.MoveInstructionOperandToRegister(i.OutputRegister(1),
                                         instr->InputAt(1));
      __ adc(i.OutputRegister(1), Operand(i.InputRegister(3)));
      if (use_temp) {
        __ Move(i.OutputRegister(0), i.TempRegister(0));
      }
      break;
    }
    case kIA32SubPair: {
      // i.OutputRegister(0) == i.InputRegister(0) ... left low word.
      // i.InputRegister(1) ... left high word.
      // i.InputRegister(2) ... right low word.
      // i.InputRegister(3) ... right high word.
      bool use_temp = false;
      if ((HasRegisterInput(instr, 1) &&
           i.OutputRegister(0).code() == i.InputRegister(1).code()) ||
          i.OutputRegister(0).code() == i.InputRegister(3).code()) {
        // We cannot write to the output register directly, because it would
        // overwrite an input for adc. We have to use the temp register.
        use_temp = true;
        __ Move(i.TempRegister(0), i.InputRegister(0));
        __ sub(i.TempRegister(0), i.InputRegister(2));
      } else {
        __ sub(i.OutputRegister(0), i.InputRegister(2));
      }
      i.MoveInstructionOperandToRegister(i.OutputRegister(1),
                                         instr->InputAt(1));
      __ sbb(i.OutputRegister(1), Operand(i.InputRegister(3)));
      if (use_temp) {
        __ Move(i.OutputRegister(0), i.TempRegister(0));
      }
      break;
    }
    case kIA32MulPair: {
      __ imul(i.OutputRegister(1), i.InputOperand(0));
      i.MoveInstructionOperandToRegister(i.TempRegister(0), instr->InputAt(1));
      __ imul(i.TempRegister(0), i.InputOperand(2));
      __ add(i.OutputRegister(1), i.TempRegister(0));
      __ mov(i.OutputRegister(0), i.InputOperand(0));
      // Multiplies the low words and stores them in eax and edx.
      __ mul(i.InputRegister(2));
      __ add(i.OutputRegister(1), i.TempRegister(0));

      break;
    }
    case kIA32ShlPair:
      if (HasImmediateInput(instr, 2)) {
        __ ShlPair(i.InputRegister(1), i.InputRegister(0), i.InputInt6(2));
      } else {
        // Shift has been loaded into CL by the register allocator.
        __ ShlPair_cl(i.InputRegister(1), i.InputRegister(0));
      }
      break;
    case kIA32ShrPair:
      if (HasImmediateInput(instr, 2)) {
        __ ShrPair(i.InputRegister(1), i.InputRegister(0), i.InputInt6(2));
      } else {
        // Shift has been loaded into CL by the register allocator.
        __ ShrPair_cl(i.InputRegister(1), i.InputRegister(0));
      }
      break;
    case kIA32SarPair:
      if (HasImmediateInput(instr, 2)) {
        __ SarPair(i.InputRegister(1), i.InputRegister(0), i.InputInt6(2));
      } else {
        // Shift has been loaded into CL by the register allocator.
        __ SarPair_cl(i.InputRegister(1), i.InputRegister(0));
      }
      break;
    case kIA32Rol:
      if (HasImmediateInput(instr, 1)) {
        __ rol(i.OutputOperand(), i.InputInt5(1));
      } else {
        __ rol_cl(i.OutputOperand());
      }
      break;
    case kIA32Ror:
      if (HasImmediateInput(instr, 1)) {
        __ ror(i.OutputOperand(), i.InputInt5(1));
      } else {
        __ ror_cl(i.OutputOperand());
      }
      break;
    case kIA32Lzcnt:
      __ Lzcnt(i.OutputRegister(), i.InputOperand(0));
      break;
    case kIA32Tzcnt:
      __ Tzcnt(i.OutputRegister(), i.InputOperand(0));
      break;
    case kIA32Popcnt:
      __ Popcnt(i.OutputRegister(), i.InputOperand(0));
      break;
    case kIA32Bswap:
      __ bswap(i.OutputRegister());
      break;
    case kArchWordPoisonOnSpeculation:
      // TODO(860429): Remove remaining poisoning infrastructure on ia32.
      UNREACHABLE();
    case kIA32MFence:
      __ mfence();
      break;
    case kIA32LFence:
      __ lfence();
      break;
    case kSSEFloat32Cmp:
      __ ucomiss(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat32Add:
      __ addss(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat32Sub:
      __ subss(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat32Mul:
      __ mulss(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat32Div:
      __ divss(i.InputDoubleRegister(0), i.InputOperand(1));
      // Don't delete this mov. It may improve performance on some CPUs,
      // when there is a (v)mulss depending on the result.
      __ movaps(i.OutputDoubleRegister(), i.OutputDoubleRegister());
      break;
    case kSSEFloat32Sqrt:
      __ sqrtss(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kSSEFloat32Abs: {
      // TODO(bmeurer): Use 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psrlq(tmp, 33);
      __ andps(i.OutputDoubleRegister(), tmp);
      break;
    }
    case kSSEFloat32Neg: {
      // TODO(bmeurer): Use 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psllq(tmp, 31);
      __ xorps(i.OutputDoubleRegister(), tmp);
      break;
    }
    case kSSEFloat32Round: {
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      RoundingMode const mode =
          static_cast<RoundingMode>(MiscField::decode(instr->opcode()));
      __ roundss(i.OutputDoubleRegister(), i.InputDoubleRegister(0), mode);
      break;
    }
    case kSSEFloat64Cmp:
      __ ucomisd(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat64Add:
      __ addsd(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat64Sub:
      __ subsd(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat64Mul:
      __ mulsd(i.InputDoubleRegister(0), i.InputOperand(1));
      break;
    case kSSEFloat64Div:
      __ divsd(i.InputDoubleRegister(0), i.InputOperand(1));
      // Don't delete this mov. It may improve performance on some CPUs,
      // when there is a (v)mulsd depending on the result.
      __ movaps(i.OutputDoubleRegister(), i.OutputDoubleRegister());
      break;
    case kSSEFloat32Max: {
      Label compare_swap, done_compare;
      if (instr->InputAt(1)->IsFPRegister()) {
        __ ucomiss(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ ucomiss(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      auto ool =
          zone()->New<OutOfLineLoadFloat32NaN>(this, i.OutputDoubleRegister());
      __ j(parity_even, ool->entry());
      __ j(above, &done_compare, Label::kNear);
      __ j(below, &compare_swap, Label::kNear);
      __ movmskps(i.TempRegister(0), i.InputDoubleRegister(0));
      __ test(i.TempRegister(0), Immediate(1));
      __ j(zero, &done_compare, Label::kNear);
      __ bind(&compare_swap);
      if (instr->InputAt(1)->IsFPRegister()) {
        __ movss(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ movss(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      __ bind(&done_compare);
      __ bind(ool->exit());
      break;
    }

    case kSSEFloat64Max: {
      Label compare_swap, done_compare;
      if (instr->InputAt(1)->IsFPRegister()) {
        __ ucomisd(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ ucomisd(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      auto ool =
          zone()->New<OutOfLineLoadFloat64NaN>(this, i.OutputDoubleRegister());
      __ j(parity_even, ool->entry());
      __ j(above, &done_compare, Label::kNear);
      __ j(below, &compare_swap, Label::kNear);
      __ movmskpd(i.TempRegister(0), i.InputDoubleRegister(0));
      __ test(i.TempRegister(0), Immediate(1));
      __ j(zero, &done_compare, Label::kNear);
      __ bind(&compare_swap);
      if (instr->InputAt(1)->IsFPRegister()) {
        __ movsd(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ movsd(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      __ bind(&done_compare);
      __ bind(ool->exit());
      break;
    }
    case kSSEFloat32Min: {
      Label compare_swap, done_compare;
      if (instr->InputAt(1)->IsFPRegister()) {
        __ ucomiss(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ ucomiss(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      auto ool =
          zone()->New<OutOfLineLoadFloat32NaN>(this, i.OutputDoubleRegister());
      __ j(parity_even, ool->entry());
      __ j(below, &done_compare, Label::kNear);
      __ j(above, &compare_swap, Label::kNear);
      if (instr->InputAt(1)->IsFPRegister()) {
        __ movmskps(i.TempRegister(0), i.InputDoubleRegister(1));
      } else {
        __ movss(kScratchDoubleReg, i.InputOperand(1));
        __ movmskps(i.TempRegister(0), kScratchDoubleReg);
      }
      __ test(i.TempRegister(0), Immediate(1));
      __ j(zero, &done_compare, Label::kNear);
      __ bind(&compare_swap);
      if (instr->InputAt(1)->IsFPRegister()) {
        __ movss(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ movss(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      __ bind(&done_compare);
      __ bind(ool->exit());
      break;
    }
    case kSSEFloat64Min: {
      Label compare_swap, done_compare;
      if (instr->InputAt(1)->IsFPRegister()) {
        __ ucomisd(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ ucomisd(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      auto ool =
          zone()->New<OutOfLineLoadFloat64NaN>(this, i.OutputDoubleRegister());
      __ j(parity_even, ool->entry());
      __ j(below, &done_compare, Label::kNear);
      __ j(above, &compare_swap, Label::kNear);
      if (instr->InputAt(1)->IsFPRegister()) {
        __ movmskpd(i.TempRegister(0), i.InputDoubleRegister(1));
      } else {
        __ movsd(kScratchDoubleReg, i.InputOperand(1));
        __ movmskpd(i.TempRegister(0), kScratchDoubleReg);
      }
      __ test(i.TempRegister(0), Immediate(1));
      __ j(zero, &done_compare, Label::kNear);
      __ bind(&compare_swap);
      if (instr->InputAt(1)->IsFPRegister()) {
        __ movsd(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      } else {
        __ movsd(i.InputDoubleRegister(0), i.InputOperand(1));
      }
      __ bind(&done_compare);
      __ bind(ool->exit());
      break;
    }
    case kSSEFloat64Mod: {
      Register tmp = i.TempRegister(1);
      __ mov(tmp, esp);
      __ AllocateStackSpace(kDoubleSize);
      __ and_(esp, -8);  // align to 8 byte boundary.
      // Move values to st(0) and st(1).
      __ movsd(Operand(esp, 0), i.InputDoubleRegister(1));
      __ fld_d(Operand(esp, 0));
      __ movsd(Operand(esp, 0), i.InputDoubleRegister(0));
      __ fld_d(Operand(esp, 0));
      // Loop while fprem isn't done.
      Label mod_loop;
      __ bind(&mod_loop);
      // This instruction traps on all kinds of inputs, but we are assuming the
      // floating point control word is set to ignore them all.
      __ fprem();
      // fnstsw_ax clobbers eax.
      DCHECK_EQ(eax, i.TempRegister(0));
      __ fnstsw_ax();
      __ sahf();
      __ j(parity_even, &mod_loop);
      // Move output to stack and clean up.
      __ fstp(1);
      __ fstp_d(Operand(esp, 0));
      __ movsd(i.OutputDoubleRegister(), Operand(esp, 0));
      __ mov(esp, tmp);
      break;
    }
    case kSSEFloat64Abs: {
      // TODO(bmeurer): Use 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psrlq(tmp, 1);
      __ andpd(i.OutputDoubleRegister(), tmp);
      break;
    }
    case kSSEFloat64Neg: {
      // TODO(bmeurer): Use 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psllq(tmp, 63);
      __ xorpd(i.OutputDoubleRegister(), tmp);
      break;
    }
    case kSSEFloat64Sqrt:
      __ sqrtsd(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kSSEFloat64Round: {
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      RoundingMode const mode =
          static_cast<RoundingMode>(MiscField::decode(instr->opcode()));
      __ roundsd(i.OutputDoubleRegister(), i.InputDoubleRegister(0), mode);
      break;
    }
    case kSSEFloat32ToFloat64:
      __ cvtss2sd(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kSSEFloat64ToFloat32:
      __ cvtsd2ss(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kSSEFloat32ToInt32:
      __ cvttss2si(i.OutputRegister(), i.InputOperand(0));
      break;
    case kSSEFloat32ToUint32:
      __ Cvttss2ui(i.OutputRegister(), i.InputOperand(0),
                   i.TempSimd128Register(0));
      break;
    case kSSEFloat64ToInt32:
      __ cvttsd2si(i.OutputRegister(), i.InputOperand(0));
      break;
    case kSSEFloat64ToUint32:
      __ Cvttsd2ui(i.OutputRegister(), i.InputOperand(0),
                   i.TempSimd128Register(0));
      break;
    case kSSEInt32ToFloat32:
      __ cvtsi2ss(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kSSEUint32ToFloat32:
      __ Cvtui2ss(i.OutputDoubleRegister(), i.InputOperand(0),
                  i.TempRegister(0));
      break;
    case kSSEInt32ToFloat64:
      __ cvtsi2sd(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kSSEUint32ToFloat64:
      __ Cvtui2sd(i.OutputDoubleRegister(), i.InputOperand(0),
                  i.TempRegister(0));
      break;
    case kSSEFloat64ExtractLowWord32:
      if (instr->InputAt(0)->IsFPStackSlot()) {
        __ mov(i.OutputRegister(), i.InputOperand(0));
      } else {
        __ movd(i.OutputRegister(), i.InputDoubleRegister(0));
      }
      break;
    case kSSEFloat64ExtractHighWord32:
      if (instr->InputAt(0)->IsFPStackSlot()) {
        __ mov(i.OutputRegister(), i.InputOperand(0, kDoubleSize / 2));
      } else {
        __ Pextrd(i.OutputRegister(), i.InputDoubleRegister(0), 1);
      }
      break;
    case kSSEFloat64InsertLowWord32:
      __ Pinsrd(i.OutputDoubleRegister(), i.InputOperand(1), 0);
      break;
    case kSSEFloat64InsertHighWord32:
      __ Pinsrd(i.OutputDoubleRegister(), i.InputOperand(1), 1);
      break;
    case kSSEFloat64LoadLowWord32:
      __ movd(i.OutputDoubleRegister(), i.InputOperand(0));
      break;
    case kAVXFloat32Add: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vaddss(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      break;
    }
    case kAVXFloat32Sub: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vsubss(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      break;
    }
    case kAVXFloat32Mul: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vmulss(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      break;
    }
    case kAVXFloat32Div: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vdivss(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      // Don't delete this mov. It may improve performance on some CPUs,
      // when there is a (v)mulss depending on the result.
      __ movaps(i.OutputDoubleRegister(), i.OutputDoubleRegister());
      break;
    }
    case kAVXFloat64Add: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vaddsd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      break;
    }
    case kAVXFloat64Sub: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vsubsd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      break;
    }
    case kAVXFloat64Mul: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vmulsd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      break;
    }
    case kAVXFloat64Div: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vdivsd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                i.InputOperand(1));
      // Don't delete this mov. It may improve performance on some CPUs,
      // when there is a (v)mulsd depending on the result.
      __ movaps(i.OutputDoubleRegister(), i.OutputDoubleRegister());
      break;
    }
    case kAVXFloat32Abs: {
      // TODO(bmeurer): Use RIP relative 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psrlq(tmp, 33);
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vandps(i.OutputDoubleRegister(), tmp, i.InputOperand(0));
      break;
    }
    case kAVXFloat32Neg: {
      // TODO(bmeurer): Use RIP relative 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psllq(tmp, 31);
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vxorps(i.OutputDoubleRegister(), tmp, i.InputOperand(0));
      break;
    }
    case kAVXFloat64Abs: {
      // TODO(bmeurer): Use RIP relative 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psrlq(tmp, 1);
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vandpd(i.OutputDoubleRegister(), tmp, i.InputOperand(0));
      break;
    }
    case kAVXFloat64Neg: {
      // TODO(bmeurer): Use RIP relative 128-bit constants.
      XMMRegister tmp = i.TempSimd128Register(0);
      __ pcmpeqd(tmp, tmp);
      __ psllq(tmp, 63);
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vxorpd(i.OutputDoubleRegister(), tmp, i.InputOperand(0));
      break;
    }
    case kSSEFloat64SilenceNaN:
      __ xorpd(kScratchDoubleReg, kScratchDoubleReg);
      __ subsd(i.InputDoubleRegister(0), kScratchDoubleReg);
      break;
    case kIA32Movsxbl:
      ASSEMBLE_MOVX(movsx_b);
      break;
    case kIA32Movzxbl:
      ASSEMBLE_MOVX(movzx_b);
      break;
    case kIA32Movb: {
      size_t index = 0;
      Operand operand = i.MemoryOperand(&index);
      if (HasImmediateInput(instr, index)) {
        __ mov_b(operand, i.InputInt8(index));
      } else {
        __ mov_b(operand, i.InputRegister(index));
      }
      break;
    }
    case kIA32Movsxwl:
      ASSEMBLE_MOVX(movsx_w);
      break;
    case kIA32Movzxwl:
      ASSEMBLE_MOVX(movzx_w);
      break;
    case kIA32Movw: {
      size_t index = 0;
      Operand operand = i.MemoryOperand(&index);
      if (HasImmediateInput(instr, index)) {
        __ mov_w(operand, i.InputInt16(index));
      } else {
        __ mov_w(operand, i.InputRegister(index));
      }
      break;
    }
    case kIA32Movl:
      if (instr->HasOutput()) {
        __ mov(i.OutputRegister(), i.MemoryOperand());
      } else {
        size_t index = 0;
        Operand operand = i.MemoryOperand(&index);
        if (HasImmediateInput(instr, index)) {
          __ mov(operand, i.InputImmediate(index));
        } else {
          __ mov(operand, i.InputRegister(index));
        }
      }
      break;
    case kIA32Movsd:
      if (instr->HasOutput()) {
        __ Movsd(i.OutputDoubleRegister(), i.MemoryOperand());
      } else {
        size_t index = 0;
        Operand operand = i.MemoryOperand(&index);
        __ Movsd(operand, i.InputDoubleRegister(index));
      }
      break;
    case kIA32Movss:
      if (instr->HasOutput()) {
        __ Movss(i.OutputDoubleRegister(), i.MemoryOperand());
      } else {
        size_t index = 0;
        Operand operand = i.MemoryOperand(&index);
        __ Movss(operand, i.InputDoubleRegister(index));
      }
      break;
    case kIA32Movdqu:
      if (instr->HasOutput()) {
        __ Movdqu(i.OutputSimd128Register(), i.MemoryOperand());
      } else {
        size_t index = 0;
        Operand operand = i.MemoryOperand(&index);
        __ Movdqu(operand, i.InputSimd128Register(index));
      }
      break;
    case kIA32BitcastFI:
      if (instr->InputAt(0)->IsFPStackSlot()) {
        __ mov(i.OutputRegister(), i.InputOperand(0));
      } else {
        __ Movd(i.OutputRegister(), i.InputDoubleRegister(0));
      }
      break;
    case kIA32BitcastIF:
      if (HasRegisterInput(instr, 0)) {
        __ Movd(i.OutputDoubleRegister(), i.InputRegister(0));
      } else {
        __ Movss(i.OutputDoubleRegister(), i.InputOperand(0));
      }
      break;
    case kIA32Lea: {
      AddressingMode mode = AddressingModeField::decode(instr->opcode());
      // Shorten "leal" to "addl", "subl" or "shll" if the register allocation
      // and addressing mode just happens to work out. The "addl"/"subl" forms
      // in these cases are faster based on measurements.
      if (mode == kMode_MI) {
        __ Move(i.OutputRegister(), Immediate(i.InputInt32(0)));
      } else if (i.InputRegister(0) == i.OutputRegister()) {
        if (mode == kMode_MRI) {
          int32_t constant_summand = i.InputInt32(1);
          if (constant_summand > 0) {
            __ add(i.OutputRegister(), Immediate(constant_summand));
          } else if (constant_summand < 0) {
            __ sub(i.OutputRegister(),
                   Immediate(base::NegateWithWraparound(constant_summand)));
          }
        } else if (mode == kMode_MR1) {
          if (i.InputRegister(1) == i.OutputRegister()) {
            __ shl(i.OutputRegister(), 1);
          } else {
            __ add(i.OutputRegister(), i.InputRegister(1));
          }
        } else if (mode == kMode_M2) {
          __ shl(i.OutputRegister(), 1);
        } else if (mode == kMode_M4) {
          __ shl(i.OutputRegister(), 2);
        } else if (mode == kMode_M8) {
          __ shl(i.OutputRegister(), 3);
        } else {
          __ lea(i.OutputRegister(), i.MemoryOperand());
        }
      } else if (mode == kMode_MR1 &&
                 i.InputRegister(1) == i.OutputRegister()) {
        __ add(i.OutputRegister(), i.InputRegister(0));
      } else {
        __ lea(i.OutputRegister(), i.MemoryOperand());
      }
      break;
    }
    case kIA32PushFloat32:
      if (instr->InputAt(0)->IsFPRegister()) {
        __ AllocateStackSpace(kFloatSize);
        __ Movss(Operand(esp, 0), i.InputDoubleRegister(0));
        frame_access_state()->IncreaseSPDelta(kFloatSize / kSystemPointerSize);
      } else if (HasImmediateInput(instr, 0)) {
        __ Move(kScratchDoubleReg, i.InputFloat32(0));
        __ AllocateStackSpace(kFloatSize);
        __ Movss(Operand(esp, 0), kScratchDoubleReg);
        frame_access_state()->IncreaseSPDelta(kFloatSize / kSystemPointerSize);
      } else {
        __ Movss(kScratchDoubleReg, i.InputOperand(0));
        __ AllocateStackSpace(kFloatSize);
        __ Movss(Operand(esp, 0), kScratchDoubleReg);
        frame_access_state()->IncreaseSPDelta(kFloatSize / kSystemPointerSize);
      }
      break;
    case kIA32PushFloat64:
      if (instr->InputAt(0)->IsFPRegister()) {
        __ AllocateStackSpace(kDoubleSize);
        __ Movsd(Operand(esp, 0), i.InputDoubleRegister(0));
        frame_access_state()->IncreaseSPDelta(kDoubleSize / kSystemPointerSize);
      } else if (HasImmediateInput(instr, 0)) {
        __ Move(kScratchDoubleReg, i.InputDouble(0));
        __ AllocateStackSpace(kDoubleSize);
        __ Movsd(Operand(esp, 0), kScratchDoubleReg);
        frame_access_state()->IncreaseSPDelta(kDoubleSize / kSystemPointerSize);
      } else {
        __ Movsd(kScratchDoubleReg, i.InputOperand(0));
        __ AllocateStackSpace(kDoubleSize);
        __ Movsd(Operand(esp, 0), kScratchDoubleReg);
        frame_access_state()->IncreaseSPDelta(kDoubleSize / kSystemPointerSize);
      }
      break;
    case kIA32PushSimd128:
      if (instr->InputAt(0)->IsFPRegister()) {
        __ AllocateStackSpace(kSimd128Size);
        __ Movups(Operand(esp, 0), i.InputSimd128Register(0));
      } else {
        __ Movups(kScratchDoubleReg, i.InputOperand(0));
        __ AllocateStackSpace(kSimd128Size);
        __ Movups(Operand(esp, 0), kScratchDoubleReg);
      }
      frame_access_state()->IncreaseSPDelta(kSimd128Size / kSystemPointerSize);
      break;
    case kIA32Push:
      if (HasAddressingMode(instr)) {
        size_t index = 0;
        Operand operand = i.MemoryOperand(&index);
        __ push(operand);
        frame_access_state()->IncreaseSPDelta(kFloatSize / kSystemPointerSize);
      } else if (instr->InputAt(0)->IsFPRegister()) {
        __ AllocateStackSpace(kFloatSize);
        __ Movsd(Operand(esp, 0), i.InputDoubleRegister(0));
        frame_access_state()->IncreaseSPDelta(kFloatSize / kSystemPointerSize);
      } else if (HasImmediateInput(instr, 0)) {
        __ push(i.InputImmediate(0));
        frame_access_state()->IncreaseSPDelta(1);
      } else {
        __ push(i.InputOperand(0));
        frame_access_state()->IncreaseSPDelta(1);
      }
      break;
    case kIA32Poke: {
      int slot = MiscField::decode(instr->opcode());
      if (HasImmediateInput(instr, 0)) {
        __ mov(Operand(esp, slot * kSystemPointerSize), i.InputImmediate(0));
      } else {
        __ mov(Operand(esp, slot * kSystemPointerSize), i.InputRegister(0));
      }
      break;
    }
    case kIA32Peek: {
      int reverse_slot = i.InputInt32(0);
      int offset =
          FrameSlotToFPOffset(frame()->GetTotalFrameSlotCount() - reverse_slot);
      if (instr->OutputAt(0)->IsFPRegister()) {
        LocationOperand* op = LocationOperand::cast(instr->OutputAt(0));
        if (op->representation() == MachineRepresentation::kFloat64) {
          __ Movsd(i.OutputDoubleRegister(), Operand(ebp, offset));
        } else if (op->representation() == MachineRepresentation::kFloat32) {
          __ Movss(i.OutputFloatRegister(), Operand(ebp, offset));
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, op->representation());
          __ Movdqu(i.OutputSimd128Register(), Operand(ebp, offset));
        }
      } else {
        __ mov(i.OutputRegister(), Operand(ebp, offset));
      }
      break;
    }
    case kSSEF64x2Splat: {
      DCHECK_EQ(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      XMMRegister dst = i.OutputSimd128Register();
      __ shufpd(dst, dst, 0x0);
      break;
    }
    case kAVXF64x2Splat: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src = i.InputDoubleRegister(0);
      __ vshufpd(i.OutputSimd128Register(), src, src, 0x0);
      break;
    }
    case kSSEF64x2ExtractLane: {
      DCHECK_EQ(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      XMMRegister dst = i.OutputDoubleRegister();
      int8_t lane = i.InputInt8(1);
      if (lane != 0) {
        DCHECK_LT(lane, 4);
        __ shufpd(dst, dst, lane);
      }
      break;
    }
    case kAVXF64x2ExtractLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputDoubleRegister();
      XMMRegister src = i.InputSimd128Register(0);
      int8_t lane = i.InputInt8(1);
      if (lane == 0) {
        if (dst != src) __ vmovapd(dst, src);
      } else {
        DCHECK_LT(lane, 4);
        __ vshufpd(dst, src, src, lane);
      }
      break;
    }
    case kSSEF64x2ReplaceLane: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      int8_t lane = i.InputInt8(1);
      DoubleRegister rep = i.InputDoubleRegister(2);

      // insertps takes a mask which contains (high to low):
      // - 2 bit specifying source float element to copy
      // - 2 bit specifying destination float element to write to
      // - 4 bits specifying which elements of the destination to zero
      DCHECK_LT(lane, 2);
      if (lane == 0) {
        __ insertps(dst, rep, 0b00000000);
        __ insertps(dst, rep, 0b01010000);
      } else {
        __ insertps(dst, rep, 0b00100000);
        __ insertps(dst, rep, 0b01110000);
      }
      break;
    }
    case kAVXF64x2ReplaceLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src = i.InputSimd128Register(0);
      int8_t lane = i.InputInt8(1);
      DoubleRegister rep = i.InputDoubleRegister(2);
      DCHECK_NE(dst, rep);

      DCHECK_LT(lane, 2);
      if (lane == 0) {
        __ vinsertps(dst, src, rep, 0b00000000);
        __ vinsertps(dst, dst, rep, 0b01010000);
      } else {
        __ vinsertps(dst, src, rep, 0b00100000);
        __ vinsertps(dst, dst, rep, 0b01110000);
      }
      break;
    }
    case kIA32F64x2Sqrt: {
      __ Sqrtpd(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32F64x2Add: {
      __ Addpd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputOperand(1));
      break;
    }
    case kIA32F64x2Sub: {
      __ Subpd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputOperand(1));
      break;
    }
    case kIA32F64x2Mul: {
      __ Mulpd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputOperand(1));
      break;
    }
    case kIA32F64x2Div: {
      __ Divpd(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
               i.InputOperand(1));
      break;
    }
    case kIA32F64x2Min: {
      Operand src1 = i.InputOperand(1);
      XMMRegister dst = i.OutputSimd128Register(),
                  src = i.InputSimd128Register(0),
                  tmp = i.TempSimd128Register(0);
      // The minpd instruction doesn't propagate NaNs and +0's in its first
      // operand. Perform minpd in both orders, merge the resuls, and adjust.
      __ Movupd(tmp, src1);
      __ Minpd(tmp, tmp, src);
      __ Minpd(dst, src, src1);
      // propagate -0's and NaNs, which may be non-canonical.
      __ Orpd(tmp, dst);
      // Canonicalize NaNs by quieting and clearing the payload.
      __ Cmpunordpd(dst, dst, tmp);
      __ Orpd(tmp, dst);
      __ Psrlq(dst, 13);
      __ Andnpd(dst, tmp);
      break;
    }
    case kIA32F64x2Max: {
      Operand src1 = i.InputOperand(1);
      XMMRegister dst = i.OutputSimd128Register(),
                  src = i.InputSimd128Register(0),
                  tmp = i.TempSimd128Register(0);
      // The maxpd instruction doesn't propagate NaNs and +0's in its first
      // operand. Perform maxpd in both orders, merge the resuls, and adjust.
      __ Movupd(tmp, src1);
      __ Maxpd(tmp, tmp, src);
      __ Maxpd(dst, src, src1);
      // Find discrepancies.
      __ Xorpd(dst, tmp);
      // Propagate NaNs, which may be non-canonical.
      __ Orpd(tmp, dst);
      // Propagate sign discrepancy and (subtle) quiet NaNs.
      __ Subpd(tmp, tmp, dst);
      // Canonicalize NaNs by clearing the payload. Sign is non-deterministic.
      __ Cmpunordpd(dst, dst, tmp);
      __ Psrlq(dst, 13);
      __ Andnpd(dst, tmp);
      break;
    }
    case kIA32F64x2Eq: {
      __ Cmpeqpd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kIA32F64x2Ne: {
      __ Cmpneqpd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kIA32F64x2Lt: {
      __ Cmpltpd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kIA32F64x2Le: {
      __ Cmplepd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kIA32F64x2Pmin: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ Minpd(dst, dst, i.InputSimd128Register(1));
      break;
    }
    case kIA32F64x2Pmax: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ Maxpd(dst, dst, i.InputSimd128Register(1));
      break;
    }
    case kIA32F64x2Round: {
      RoundingMode const mode =
          static_cast<RoundingMode>(MiscField::decode(instr->opcode()));
      __ Roundpd(i.OutputSimd128Register(), i.InputDoubleRegister(0), mode);
      break;
    }
    case kIA32I64x2SplatI32Pair: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Pinsrd(dst, i.InputRegister(0), 0);
      __ Pinsrd(dst, i.InputOperand(1), 1);
      __ Pshufd(dst, dst, 0x44);
      break;
    }
    case kIA32I64x2ReplaceLaneI32Pair: {
      int8_t lane = i.InputInt8(1);
      __ Pinsrd(i.OutputSimd128Register(), i.InputOperand(2), lane * 2);
      __ Pinsrd(i.OutputSimd128Register(), i.InputOperand(3), lane * 2 + 1);
      break;
    }
    case kIA32I64x2Neg: {
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(0);
      __ Pxor(dst, dst);
      __ Psubq(dst, src);
      break;
    }
    case kIA32I64x2Shl: {
      ASSEMBLE_SIMD_SHIFT(Psllq, 6);
      break;
    }
    case kIA32I64x2ShrS: {
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src = i.InputSimd128Register(0);
      XMMRegister tmp = i.TempSimd128Register(0);
      XMMRegister tmp2 = i.TempSimd128Register(1);
      Operand shift = i.InputOperand(1);

      // Take shift value modulo 64.
      __ and_(shift, Immediate(63));
      __ Movd(tmp, shift);

      // Set up a mask [0x80000000,0,0x80000000,0].
      __ Pcmpeqb(tmp2, tmp2);
      __ Psllq(tmp2, tmp2, 63);

      __ Psrlq(tmp2, tmp2, tmp);
      __ Psrlq(dst, src, tmp);
      __ Pxor(dst, tmp2);
      __ Psubq(dst, tmp2);
      break;
    }
    case kIA32I64x2Add: {
      __ Paddq(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputOperand(1));
      break;
    }
    case kIA32I64x2Sub: {
      __ Psubq(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputOperand(1));
      break;
    }
    case kIA32I64x2Mul: {
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister left = i.InputSimd128Register(0);
      XMMRegister right = i.InputSimd128Register(1);
      XMMRegister tmp1 = i.TempSimd128Register(0);
      XMMRegister tmp2 = i.TempSimd128Register(1);

      __ Movaps(tmp1, left);
      __ Movaps(tmp2, right);

      // Multiply high dword of each qword of left with right.
      __ Psrlq(tmp1, 32);
      __ Pmuludq(tmp1, tmp1, right);

      // Multiply high dword of each qword of right with left.
      __ Psrlq(tmp2, 32);
      __ Pmuludq(tmp2, tmp2, left);

      __ Paddq(tmp2, tmp2, tmp1);
      __ Psllq(tmp2, tmp2, 32);

      __ Pmuludq(dst, left, right);
      __ Paddq(dst, dst, tmp2);
      break;
    }
    case kIA32I64x2ShrU: {
      ASSEMBLE_SIMD_SHIFT(Psrlq, 6);
      break;
    }
    case kIA32I64x2BitMask: {
      __ Movmskpd(i.OutputRegister(), i.InputSimd128Register(0));
      break;
    }
    case kIA32I64x2Eq: {
      __ Pcmpeqq(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kSSEF32x4Splat: {
      DCHECK_EQ(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      XMMRegister dst = i.OutputSimd128Register();
      __ shufps(dst, dst, 0x0);
      break;
    }
    case kAVXF32x4Splat: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src = i.InputFloatRegister(0);
      __ vshufps(i.OutputSimd128Register(), src, src, 0x0);
      break;
    }
    case kSSEF32x4ExtractLane: {
      DCHECK_EQ(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      XMMRegister dst = i.OutputFloatRegister();
      int8_t lane = i.InputInt8(1);
      if (lane != 0) {
        DCHECK_LT(lane, 4);
        __ shufps(dst, dst, lane);
      }
      break;
    }
    case kAVXF32x4ExtractLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputFloatRegister();
      XMMRegister src = i.InputSimd128Register(0);
      int8_t lane = i.InputInt8(1);
      if (lane == 0) {
        if (dst != src) __ vmovaps(dst, src);
      } else {
        DCHECK_LT(lane, 4);
        __ vshufps(dst, src, src, lane);
      }
      break;
    }
    case kSSEF32x4ReplaceLane: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ insertps(i.OutputSimd128Register(), i.InputOperand(2),
                  i.InputInt8(1) << 4);
      break;
    }
    case kAVXF32x4ReplaceLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vinsertps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                   i.InputOperand(2), i.InputInt8(1) << 4);
      break;
    }
    case kIA32F32x4SConvertI32x4: {
      __ Cvtdq2ps(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kSSEF32x4UConvertI32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      __ pxor(kScratchDoubleReg, kScratchDoubleReg);      // zeros
      __ pblendw(kScratchDoubleReg, dst, 0x55);           // get lo 16 bits
      __ psubd(dst, kScratchDoubleReg);                   // get hi 16 bits
      __ cvtdq2ps(kScratchDoubleReg, kScratchDoubleReg);  // convert lo exactly
      __ psrld(dst, 1);                  // divide by 2 to get in unsigned range
      __ cvtdq2ps(dst, dst);             // convert hi exactly
      __ addps(dst, dst);                // double hi, exactly
      __ addps(dst, kScratchDoubleReg);  // add hi and lo, may round.
      break;
    }
    case kAVXF32x4UConvertI32x4: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src = i.InputSimd128Register(0);
      __ vpxor(kScratchDoubleReg, kScratchDoubleReg,
               kScratchDoubleReg);  // zeros
      __ vpblendw(kScratchDoubleReg, kScratchDoubleReg, src,
                  0x55);                                   // get lo 16 bits
      __ vpsubd(dst, src, kScratchDoubleReg);              // get hi 16 bits
      __ vcvtdq2ps(kScratchDoubleReg, kScratchDoubleReg);  // convert lo exactly
      __ vpsrld(dst, dst, 1);    // divide by 2 to get in unsigned range
      __ vcvtdq2ps(dst, dst);    // convert hi exactly
      __ vaddps(dst, dst, dst);  // double hi, exactly
      __ vaddps(dst, dst, kScratchDoubleReg);  // add hi and lo, may round.
      break;
    }
    case kSSEF32x4Abs: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(i.InputSimd128Register(0), dst);
      __ pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
      __ psrld(kScratchDoubleReg, 1);
      __ andps(dst, kScratchDoubleReg);
      break;
    }
    case kAVXF32x4Abs: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqd(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpsrld(kScratchDoubleReg, kScratchDoubleReg, 1);
      __ vandps(i.OutputSimd128Register(), kScratchDoubleReg,
                i.InputOperand(0));
      break;
    }
    case kSSEF32x4Neg: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
      __ pslld(kScratchDoubleReg, 31);
      __ xorps(dst, kScratchDoubleReg);
      break;
    }
    case kAVXF32x4Neg: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqd(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpslld(kScratchDoubleReg, kScratchDoubleReg, 31);
      __ vxorps(i.OutputSimd128Register(), kScratchDoubleReg,
                i.InputOperand(0));
      break;
    }
    case kSSEF32x4Sqrt: {
      __ sqrtps(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kAVXF32x4Sqrt: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vsqrtps(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32F32x4RecipApprox: {
      __ Rcpps(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32F32x4RecipSqrtApprox: {
      __ Rsqrtps(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kSSEF32x4Add: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ addps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Add: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vaddps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEF32x4AddHoriz: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE3);
      __ haddps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4AddHoriz: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vhaddps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEF32x4Sub: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ subps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Sub: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vsubps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEF32x4Mul: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ mulps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Mul: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vmulps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEF32x4Div: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ divps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Div: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vdivps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEF32x4Min: {
      XMMRegister src1 = i.InputSimd128Register(1),
                  dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      // The minps instruction doesn't propagate NaNs and +0's in its first
      // operand. Perform minps in both orders, merge the resuls, and adjust.
      __ movaps(kScratchDoubleReg, src1);
      __ minps(kScratchDoubleReg, dst);
      __ minps(dst, src1);
      // propagate -0's and NaNs, which may be non-canonical.
      __ orps(kScratchDoubleReg, dst);
      // Canonicalize NaNs by quieting and clearing the payload.
      __ cmpps(dst, kScratchDoubleReg, 3);
      __ orps(kScratchDoubleReg, dst);
      __ psrld(dst, 10);
      __ andnps(dst, kScratchDoubleReg);
      break;
    }
    case kAVXF32x4Min: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src0 = i.InputSimd128Register(0);
      Operand src1 = i.InputOperand(1);
      // See comment above for correction of minps.
      __ vmovups(kScratchDoubleReg, src1);
      __ vminps(kScratchDoubleReg, kScratchDoubleReg, src0);
      __ vminps(dst, src0, src1);
      __ vorps(dst, dst, kScratchDoubleReg);
      __ vcmpneqps(kScratchDoubleReg, dst, dst);
      __ vorps(dst, dst, kScratchDoubleReg);
      __ vpsrld(kScratchDoubleReg, kScratchDoubleReg, 10);
      __ vandnps(dst, kScratchDoubleReg, dst);
      break;
    }
    case kSSEF32x4Max: {
      XMMRegister src1 = i.InputSimd128Register(1),
                  dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      // The maxps instruction doesn't propagate NaNs and +0's in its first
      // operand. Perform maxps in both orders, merge the resuls, and adjust.
      __ movaps(kScratchDoubleReg, src1);
      __ maxps(kScratchDoubleReg, dst);
      __ maxps(dst, src1);
      // Find discrepancies.
      __ xorps(dst, kScratchDoubleReg);
      // Propagate NaNs, which may be non-canonical.
      __ orps(kScratchDoubleReg, dst);
      // Propagate sign discrepancy and (subtle) quiet NaNs.
      __ subps(kScratchDoubleReg, dst);
      // Canonicalize NaNs by clearing the payload.
      __ cmpps(dst, kScratchDoubleReg, 3);
      __ psrld(dst, 10);
      __ andnps(dst, kScratchDoubleReg);
      break;
    }
    case kAVXF32x4Max: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src0 = i.InputSimd128Register(0);
      Operand src1 = i.InputOperand(1);
      // See comment above for correction of maxps.
      __ vmovups(kScratchDoubleReg, src1);
      __ vmaxps(kScratchDoubleReg, kScratchDoubleReg, src0);
      __ vmaxps(dst, src0, src1);
      __ vxorps(dst, dst, kScratchDoubleReg);
      __ vorps(kScratchDoubleReg, kScratchDoubleReg, dst);
      __ vsubps(kScratchDoubleReg, kScratchDoubleReg, dst);
      __ vcmpneqps(dst, kScratchDoubleReg, kScratchDoubleReg);
      __ vpsrld(dst, dst, 10);
      __ vandnps(dst, dst, kScratchDoubleReg);
      break;
    }
    case kSSEF32x4Eq: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ cmpeqps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Eq: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vcmpeqps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEF32x4Ne: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ cmpneqps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Ne: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vcmpneqps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                   i.InputOperand(1));
      break;
    }
    case kSSEF32x4Lt: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ cmpltps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Lt: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vcmpltps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEF32x4Le: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ cmpleps(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXF32x4Le: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vcmpleps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kIA32F32x4Pmin: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ Minps(dst, dst, i.InputSimd128Register(1));
      break;
    }
    case kIA32F32x4Pmax: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ Maxps(dst, dst, i.InputSimd128Register(1));
      break;
    }
    case kIA32F32x4Round: {
      RoundingMode const mode =
          static_cast<RoundingMode>(MiscField::decode(instr->opcode()));
      __ Roundps(i.OutputSimd128Register(), i.InputDoubleRegister(0), mode);
      break;
    }
    case kIA32I32x4Splat: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Movd(dst, i.InputOperand(0));
      __ Pshufd(dst, dst, 0x0);
      break;
    }
    case kIA32I32x4ExtractLane: {
      __ Pextrd(i.OutputRegister(), i.InputSimd128Register(0), i.InputInt8(1));
      break;
    }
    case kSSEI32x4ReplaceLane: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pinsrd(i.OutputSimd128Register(), i.InputOperand(2), i.InputInt8(1));
      break;
    }
    case kAVXI32x4ReplaceLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpinsrd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(2), i.InputInt8(1));
      break;
    }
    case kSSEI32x4SConvertF32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      XMMRegister dst = i.OutputSimd128Register();
      // NAN->0
      __ movaps(kScratchDoubleReg, dst);
      __ cmpeqps(kScratchDoubleReg, kScratchDoubleReg);
      __ pand(dst, kScratchDoubleReg);
      // Set top bit if >= 0 (but not -0.0!)
      __ pxor(kScratchDoubleReg, dst);
      // Convert
      __ cvttps2dq(dst, dst);
      // Set top bit if >=0 is now < 0
      __ pand(kScratchDoubleReg, dst);
      __ psrad(kScratchDoubleReg, 31);
      // Set positive overflow lanes to 0x7FFFFFFF
      __ pxor(dst, kScratchDoubleReg);
      break;
    }
    case kAVXI32x4SConvertF32x4: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src = i.InputSimd128Register(0);
      // NAN->0
      __ vcmpeqps(kScratchDoubleReg, src, src);
      __ vpand(dst, src, kScratchDoubleReg);
      // Set top bit if >= 0 (but not -0.0!)
      __ vpxor(kScratchDoubleReg, kScratchDoubleReg, dst);
      // Convert
      __ vcvttps2dq(dst, dst);
      // Set top bit if >=0 is now < 0
      __ vpand(kScratchDoubleReg, kScratchDoubleReg, dst);
      __ vpsrad(kScratchDoubleReg, kScratchDoubleReg, 31);
      // Set positive overflow lanes to 0x7FFFFFFF
      __ vpxor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kIA32I32x4SConvertI16x8Low: {
      __ Pmovsxwd(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32I32x4SConvertI16x8High: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Palignr(dst, i.InputOperand(0), 8);
      __ Pmovsxwd(dst, dst);
      break;
    }
    case kIA32I32x4Neg: {
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(0);
      if (src.is_reg(dst)) {
        __ Pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
        __ Psignd(dst, kScratchDoubleReg);
      } else {
        __ Pxor(dst, dst);
        __ Psubd(dst, src);
      }
      break;
    }
    case kIA32I32x4Shl: {
      ASSEMBLE_SIMD_SHIFT(Pslld, 5);
      break;
    }
    case kIA32I32x4ShrS: {
      ASSEMBLE_SIMD_SHIFT(Psrad, 5);
      break;
    }
    case kSSEI32x4Add: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4Add: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEI32x4AddHoriz: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSSE3);
      __ phaddd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4AddHoriz: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vphaddd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI32x4Sub: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4Sub: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEI32x4Mul: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pmulld(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4Mul: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmulld(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI32x4MinS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pminsd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4MinS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpminsd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI32x4MaxS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pmaxsd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4MaxS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmaxsd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI32x4Eq: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpeqd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4Eq: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI32x4Ne: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpeqd(i.OutputSimd128Register(), i.InputOperand(1));
      __ pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(i.OutputSimd128Register(), kScratchDoubleReg);
      break;
    }
    case kAVXI32x4Ne: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      __ vpcmpeqd(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(i.OutputSimd128Register(), i.OutputSimd128Register(),
               kScratchDoubleReg);
      break;
    }
    case kSSEI32x4GtS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpgtd(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4GtS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpgtd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI32x4GeS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pminsd(dst, src);
      __ pcmpeqd(dst, src);
      break;
    }
    case kAVXI32x4GeS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpminsd(kScratchDoubleReg, src1, src2);
      __ vpcmpeqd(i.OutputSimd128Register(), kScratchDoubleReg, src2);
      break;
    }
    case kSSEI32x4UConvertF32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister tmp = i.TempSimd128Register(0);
      // NAN->0, negative->0
      __ pxor(kScratchDoubleReg, kScratchDoubleReg);
      __ maxps(dst, kScratchDoubleReg);
      // scratch: float representation of max_signed
      __ pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
      __ psrld(kScratchDoubleReg, 1);                     // 0x7fffffff
      __ cvtdq2ps(kScratchDoubleReg, kScratchDoubleReg);  // 0x4f000000
      // tmp: convert (src-max_signed).
      // Positive overflow lanes -> 0x7FFFFFFF
      // Negative lanes -> 0
      __ movaps(tmp, dst);
      __ subps(tmp, kScratchDoubleReg);
      __ cmpleps(kScratchDoubleReg, tmp);
      __ cvttps2dq(tmp, tmp);
      __ pxor(tmp, kScratchDoubleReg);
      __ pxor(kScratchDoubleReg, kScratchDoubleReg);
      __ pmaxsd(tmp, kScratchDoubleReg);
      // convert. Overflow lanes above max_signed will be 0x80000000
      __ cvttps2dq(dst, dst);
      // Add (src-max_signed) for overflow lanes.
      __ paddd(dst, tmp);
      break;
    }
    case kAVXI32x4UConvertF32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister tmp = i.TempSimd128Register(0);
      // NAN->0, negative->0
      __ vpxor(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vmaxps(dst, dst, kScratchDoubleReg);
      // scratch: float representation of max_signed
      __ vpcmpeqd(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpsrld(kScratchDoubleReg, kScratchDoubleReg, 1);  // 0x7fffffff
      __ vcvtdq2ps(kScratchDoubleReg, kScratchDoubleReg);  // 0x4f000000
      // tmp: convert (src-max_signed).
      // Positive overflow lanes -> 0x7FFFFFFF
      // Negative lanes -> 0
      __ vsubps(tmp, dst, kScratchDoubleReg);
      __ vcmpleps(kScratchDoubleReg, kScratchDoubleReg, tmp);
      __ vcvttps2dq(tmp, tmp);
      __ vpxor(tmp, tmp, kScratchDoubleReg);
      __ vpxor(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpmaxsd(tmp, tmp, kScratchDoubleReg);
      // convert. Overflow lanes above max_signed will be 0x80000000
      __ vcvttps2dq(dst, dst);
      // Add (src-max_signed) for overflow lanes.
      __ vpaddd(dst, dst, tmp);
      break;
    }
    case kIA32I32x4UConvertI16x8Low: {
      __ Pmovzxwd(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32I32x4UConvertI16x8High: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Palignr(dst, i.InputOperand(0), 8);
      __ Pmovzxwd(dst, dst);
      break;
    }
    case kIA32I32x4ShrU: {
      ASSEMBLE_SIMD_SHIFT(Psrld, 5);
      break;
    }
    case kSSEI32x4MinU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pminud(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4MinU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpminud(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI32x4MaxU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pmaxud(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI32x4MaxU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmaxud(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI32x4GtU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pmaxud(dst, src);
      __ pcmpeqd(dst, src);
      __ pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(dst, kScratchDoubleReg);
      break;
    }
    case kAVXI32x4GtU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpmaxud(kScratchDoubleReg, src1, src2);
      __ vpcmpeqd(dst, kScratchDoubleReg, src2);
      __ vpcmpeqd(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kSSEI32x4GeU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pminud(dst, src);
      __ pcmpeqd(dst, src);
      break;
    }
    case kAVXI32x4GeU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpminud(kScratchDoubleReg, src1, src2);
      __ vpcmpeqd(i.OutputSimd128Register(), kScratchDoubleReg, src2);
      break;
    }
    case kIA32I32x4Abs: {
      __ Pabsd(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kIA32I32x4BitMask: {
      __ Movmskps(i.OutputRegister(), i.InputSimd128Register(0));
      break;
    }
    case kIA32I32x4DotI16x8S: {
      __ Pmaddwd(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputSimd128Register(1));
      break;
    }
    case kIA32I16x8Splat: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Movd(dst, i.InputOperand(0));
      __ Pshuflw(dst, dst, 0x0);
      __ Pshufd(dst, dst, 0x0);
      break;
    }
    case kIA32I16x8ExtractLaneU: {
      Register dst = i.OutputRegister();
      __ Pextrw(dst, i.InputSimd128Register(0), i.InputInt8(1));
      break;
    }
    case kIA32I16x8ExtractLaneS: {
      Register dst = i.OutputRegister();
      __ Pextrw(dst, i.InputSimd128Register(0), i.InputInt8(1));
      __ movsx_w(dst, dst);
      break;
    }
    case kSSEI16x8ReplaceLane: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pinsrw(i.OutputSimd128Register(), i.InputOperand(2), i.InputInt8(1));
      break;
    }
    case kAVXI16x8ReplaceLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpinsrw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(2), i.InputInt8(1));
      break;
    }
    case kIA32I16x8SConvertI8x16Low: {
      __ Pmovsxbw(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32I16x8SConvertI8x16High: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Palignr(dst, i.InputOperand(0), 8);
      __ Pmovsxbw(dst, dst);
      break;
    }
    case kIA32I16x8Neg: {
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(0);
      if (src.is_reg(dst)) {
        __ Pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
        __ Psignw(dst, kScratchDoubleReg);
      } else {
        __ Pxor(dst, dst);
        __ Psubw(dst, src);
      }
      break;
    }
    case kIA32I16x8Shl: {
      ASSEMBLE_SIMD_SHIFT(Psllw, 4);
      break;
    }
    case kIA32I16x8ShrS: {
      ASSEMBLE_SIMD_SHIFT(Psraw, 4);
      break;
    }
    case kSSEI16x8SConvertI32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ packssdw(i.OutputSimd128Register(), i.InputSimd128Register(1));
      break;
    }
    case kAVXI16x8SConvertI32x4: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpackssdw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                   i.InputSimd128Register(1));
      break;
    }
    case kSSEI16x8Add: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8Add: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEI16x8AddSatS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddsw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8AddSatS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddsw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8AddHoriz: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSSE3);
      __ phaddw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8AddHoriz: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vphaddw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8Sub: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8Sub: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEI16x8SubSatS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubsw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8SubSatS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubsw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8Mul: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pmullw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8Mul: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmullw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8MinS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pminsw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8MinS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpminsw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8MaxS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pmaxsw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8MaxS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmaxsw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8Eq: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpeqw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8Eq: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI16x8Ne: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpeqw(i.OutputSimd128Register(), i.InputOperand(1));
      __ pcmpeqw(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(i.OutputSimd128Register(), kScratchDoubleReg);
      break;
    }
    case kAVXI16x8Ne: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      __ vpcmpeqw(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(i.OutputSimd128Register(), i.OutputSimd128Register(),
               kScratchDoubleReg);
      break;
    }
    case kSSEI16x8GtS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpgtw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8GtS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpgtw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI16x8GeS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pminsw(dst, src);
      __ pcmpeqw(dst, src);
      break;
    }
    case kAVXI16x8GeS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpminsw(kScratchDoubleReg, src1, src2);
      __ vpcmpeqw(i.OutputSimd128Register(), kScratchDoubleReg, src2);
      break;
    }
    case kIA32I16x8UConvertI8x16Low: {
      __ Pmovzxbw(i.OutputSimd128Register(), i.InputOperand(0));
      break;
    }
    case kIA32I16x8UConvertI8x16High: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Palignr(dst, i.InputOperand(0), 8);
      __ Pmovzxbw(dst, dst);
      break;
    }
    case kIA32I16x8ShrU: {
      ASSEMBLE_SIMD_SHIFT(Psrlw, 4);
      break;
    }
    case kSSEI16x8UConvertI32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ packusdw(i.OutputSimd128Register(), i.InputSimd128Register(1));
      break;
    }
    case kAVXI16x8UConvertI32x4: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      __ vpackusdw(dst, dst, i.InputSimd128Register(1));
      break;
    }
    case kSSEI16x8AddSatU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddusw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8AddSatU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddusw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI16x8SubSatU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubusw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8SubSatU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubusw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI16x8MinU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pminuw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8MinU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpminuw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8MaxU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pmaxuw(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI16x8MaxU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmaxuw(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI16x8GtU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pmaxuw(dst, src);
      __ pcmpeqw(dst, src);
      __ pcmpeqw(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(dst, kScratchDoubleReg);
      break;
    }
    case kAVXI16x8GtU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpmaxuw(kScratchDoubleReg, src1, src2);
      __ vpcmpeqw(dst, kScratchDoubleReg, src2);
      __ vpcmpeqw(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kSSEI16x8GeU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pminuw(dst, src);
      __ pcmpeqw(dst, src);
      break;
    }
    case kAVXI16x8GeU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpminuw(kScratchDoubleReg, src1, src2);
      __ vpcmpeqw(i.OutputSimd128Register(), kScratchDoubleReg, src2);
      break;
    }
    case kIA32I16x8RoundingAverageU: {
      __ Pavgw(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputOperand(1));
      break;
    }
    case kIA32I16x8Abs: {
      __ Pabsw(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kIA32I16x8BitMask: {
      Register dst = i.OutputRegister();
      XMMRegister tmp = i.TempSimd128Register(0);
      __ Packsswb(tmp, i.InputSimd128Register(0));
      __ Pmovmskb(dst, tmp);
      __ shr(dst, 8);
      break;
    }
    case kIA32I8x16Splat: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Movd(dst, i.InputOperand(0));
      __ Pxor(kScratchDoubleReg, kScratchDoubleReg);
      __ Pshufb(dst, kScratchDoubleReg);
      break;
    }
    case kIA32I8x16ExtractLaneU: {
      Register dst = i.OutputRegister();
      __ Pextrb(dst, i.InputSimd128Register(0), i.InputInt8(1));
      break;
    }
    case kIA32I8x16ExtractLaneS: {
      Register dst = i.OutputRegister();
      __ Pextrb(dst, i.InputSimd128Register(0), i.InputInt8(1));
      __ movsx_b(dst, dst);
      break;
    }
    case kSSEI8x16ReplaceLane: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pinsrb(i.OutputSimd128Register(), i.InputOperand(2), i.InputInt8(1));
      break;
    }
    case kAVXI8x16ReplaceLane: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpinsrb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(2), i.InputInt8(1));
      break;
    }
    case kIA32Pinsrb: {
      // TODO(zhin): Move i8x16 replace lane into this opcode.
      ASSEMBLE_SIMD_PINSR(pinsrb, SSE4_1);
      break;
    }
    case kIA32Pinsrw: {
      // TODO(zhin): Move i16x8 replace lane into this opcode.
      ASSEMBLE_SIMD_PINSR(pinsrw, SSE4_1);
      break;
    }
    case kIA32Pinsrd: {
      // TODO(zhin): Move i32x4 replace lane into this opcode.
      ASSEMBLE_SIMD_PINSR(pinsrd, SSE4_1);
      break;
    }
    case kIA32Movlps: {
      if (instr->HasOutput()) {
        __ Movlps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.MemoryOperand(2));
      } else {
        size_t index = 0;
        Operand dst = i.MemoryOperand(&index);
        __ Movlps(dst, i.InputSimd128Register(index));
      }
      break;
    }
    case kIA32Movhps: {
      if (instr->HasOutput()) {
        __ Movhps(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.MemoryOperand(2));
      } else {
        size_t index = 0;
        Operand dst = i.MemoryOperand(&index);
        __ Movhps(dst, i.InputSimd128Register(index));
      }
      break;
    }
    case kIA32Pextrb: {
      // TODO(zhin): Move i8x16 extract lane u into this opcode.
      DCHECK(HasAddressingMode(instr));
      size_t index = 0;
      Operand operand = i.MemoryOperand(&index);
      __ Pextrb(operand, i.InputSimd128Register(index),
                i.InputUint8(index + 1));
      break;
    }
    case kIA32Pextrw: {
      // TODO(zhin): Move i16x8 extract lane u into this opcode.
      DCHECK(HasAddressingMode(instr));
      size_t index = 0;
      Operand operand = i.MemoryOperand(&index);
      __ Pextrw(operand, i.InputSimd128Register(index),
                i.InputUint8(index + 1));
      break;
    }
    case kIA32S128Store32Lane: {
      size_t index = 0;
      Operand operand = i.MemoryOperand(&index);
      uint8_t laneidx = i.InputUint8(index + 1);
      if (laneidx == 0) {
        __ Movss(operand, i.InputSimd128Register(index));
      } else {
        DCHECK_GE(3, laneidx);
        __ Extractps(operand, i.InputSimd128Register(index), 1);
      }
      break;
    }
    case kSSEI8x16SConvertI16x8: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ packsswb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16SConvertI16x8: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpacksswb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                   i.InputOperand(1));
      break;
    }
    case kIA32I8x16Neg: {
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(0);
      if (src.is_reg(dst)) {
        __ Pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
        __ Psignb(dst, kScratchDoubleReg);
      } else {
        __ Pxor(dst, dst);
        __ Psubb(dst, src);
      }
      break;
    }
    case kIA32I8x16Shl: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      Register tmp = i.ToRegister(instr->TempAt(0));
      XMMRegister tmp_simd = i.TempSimd128Register(1);

      if (HasImmediateInput(instr, 1)) {
        // Perform 16-bit shift, then mask away low bits.
        uint8_t shift = i.InputInt3(1);
        __ Psllw(dst, dst, byte{shift});

        uint8_t bmask = static_cast<uint8_t>(0xff << shift);
        uint32_t mask = bmask << 24 | bmask << 16 | bmask << 8 | bmask;
        __ mov(tmp, mask);
        __ Movd(tmp_simd, tmp);
        __ Pshufd(tmp_simd, tmp_simd, 0);
        __ Pand(dst, tmp_simd);
      } else {
        // Take shift value modulo 8.
        __ mov(tmp, i.InputRegister(1));
        __ and_(tmp, 7);
        // Mask off the unwanted bits before word-shifting.
        __ Pcmpeqw(kScratchDoubleReg, kScratchDoubleReg);
        __ add(tmp, Immediate(8));
        __ Movd(tmp_simd, tmp);
        __ Psrlw(kScratchDoubleReg, kScratchDoubleReg, tmp_simd);
        __ Packuswb(kScratchDoubleReg, kScratchDoubleReg);
        __ Pand(dst, kScratchDoubleReg);
        // TODO(zhin): sub here to avoid asking for another temporary register,
        // examine codegen for other i8x16 shifts, they use less instructions.
        __ sub(tmp, Immediate(8));
        __ Movd(tmp_simd, tmp);
        __ Psllw(dst, dst, tmp_simd);
      }
      break;
    }
    case kIA32I8x16ShrS: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      if (HasImmediateInput(instr, 1)) {
        __ Punpckhbw(kScratchDoubleReg, dst);
        __ Punpcklbw(dst, dst);
        uint8_t shift = i.InputInt3(1) + 8;
        __ Psraw(kScratchDoubleReg, shift);
        __ Psraw(dst, shift);
        __ Packsswb(dst, kScratchDoubleReg);
      } else {
        Register tmp = i.ToRegister(instr->TempAt(0));
        XMMRegister tmp_simd = i.TempSimd128Register(1);
        // Unpack the bytes into words, do arithmetic shifts, and repack.
        __ Punpckhbw(kScratchDoubleReg, dst);
        __ Punpcklbw(dst, dst);
        __ mov(tmp, i.InputRegister(1));
        // Take shift value modulo 8.
        __ and_(tmp, 7);
        __ add(tmp, Immediate(8));
        __ Movd(tmp_simd, tmp);
        __ Psraw(kScratchDoubleReg, kScratchDoubleReg, tmp_simd);
        __ Psraw(dst, dst, tmp_simd);
        __ Packsswb(dst, kScratchDoubleReg);
      }
      break;
    }
    case kSSEI8x16Add: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16Add: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEI8x16AddSatS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddsb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16AddSatS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddsb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI8x16Sub: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16Sub: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                i.InputOperand(1));
      break;
    }
    case kSSEI8x16SubSatS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubsb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16SubSatS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubsb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI8x16Mul: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      XMMRegister right = i.InputSimd128Register(1);
      XMMRegister tmp = i.TempSimd128Register(0);

      // I16x8 view of I8x16
      // left = AAaa AAaa ... AAaa AAaa
      // right= BBbb BBbb ... BBbb BBbb

      // t = 00AA 00AA ... 00AA 00AA
      // s = 00BB 00BB ... 00BB 00BB
      __ movaps(tmp, dst);
      __ movaps(kScratchDoubleReg, right);
      __ psrlw(tmp, 8);
      __ psrlw(kScratchDoubleReg, 8);
      // dst = left * 256
      __ psllw(dst, 8);

      // t = I16x8Mul(t, s)
      //    => __PP __PP ...  __PP  __PP
      __ pmullw(tmp, kScratchDoubleReg);
      // dst = I16x8Mul(left * 256, right)
      //    => pp__ pp__ ...  pp__  pp__
      __ pmullw(dst, right);

      // t = I16x8Shl(t, 8)
      //    => PP00 PP00 ...  PP00  PP00
      __ psllw(tmp, 8);

      // dst = I16x8Shr(dst, 8)
      //    => 00pp 00pp ...  00pp  00pp
      __ psrlw(dst, 8);

      // dst = I16x8Or(dst, t)
      //    => PPpp PPpp ...  PPpp  PPpp
      __ por(dst, tmp);
      break;
    }
    case kAVXI8x16Mul: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister left = i.InputSimd128Register(0);
      XMMRegister right = i.InputSimd128Register(1);
      XMMRegister tmp = i.TempSimd128Register(0);

      // I16x8 view of I8x16
      // left = AAaa AAaa ... AAaa AAaa
      // right= BBbb BBbb ... BBbb BBbb

      // t = 00AA 00AA ... 00AA 00AA
      // s = 00BB 00BB ... 00BB 00BB
      __ vpsrlw(tmp, left, 8);
      __ vpsrlw(kScratchDoubleReg, right, 8);

      // t = I16x8Mul(t0, t1)
      //    => __PP __PP ...  __PP  __PP
      __ vpmullw(tmp, tmp, kScratchDoubleReg);

      // s = left * 256
      __ vpsllw(kScratchDoubleReg, left, 8);

      // dst = I16x8Mul(left * 256, right)
      //    => pp__ pp__ ...  pp__  pp__
      __ vpmullw(dst, kScratchDoubleReg, right);

      // dst = I16x8Shr(dst, 8)
      //    => 00pp 00pp ...  00pp  00pp
      __ vpsrlw(dst, dst, 8);

      // t = I16x8Shl(t, 8)
      //    => PP00 PP00 ...  PP00  PP00
      __ vpsllw(tmp, tmp, 8);

      // dst = I16x8Or(dst, t)
      //    => PPpp PPpp ...  PPpp  PPpp
      __ vpor(dst, dst, tmp);
      break;
    }
    case kSSEI8x16MinS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pminsb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16MinS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpminsb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI8x16MaxS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      __ pmaxsb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16MaxS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmaxsb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI8x16Eq: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpeqb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16Eq: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI8x16Ne: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpeqb(i.OutputSimd128Register(), i.InputOperand(1));
      __ pcmpeqb(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(i.OutputSimd128Register(), kScratchDoubleReg);
      break;
    }
    case kAVXI8x16Ne: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      __ vpcmpeqb(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(i.OutputSimd128Register(), i.OutputSimd128Register(),
               kScratchDoubleReg);
      break;
    }
    case kSSEI8x16GtS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pcmpgtb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16GtS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpgtb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI8x16GeS: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pminsb(dst, src);
      __ pcmpeqb(dst, src);
      break;
    }
    case kAVXI8x16GeS: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpminsb(kScratchDoubleReg, src1, src2);
      __ vpcmpeqb(i.OutputSimd128Register(), kScratchDoubleReg, src2);
      break;
    }
    case kSSEI8x16UConvertI16x8: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      __ packuswb(dst, i.InputOperand(1));
      break;
    }
    case kAVXI8x16UConvertI16x8: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      __ vpackuswb(dst, dst, i.InputOperand(1));
      break;
    }
    case kSSEI8x16AddSatU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ paddusb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16AddSatU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpaddusb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kSSEI8x16SubSatU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ psubusb(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16SubSatU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpsubusb(i.OutputSimd128Register(), i.InputSimd128Register(0),
                  i.InputOperand(1));
      break;
    }
    case kIA32I8x16ShrU: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      Register tmp = i.ToRegister(instr->TempAt(0));
      XMMRegister tmp_simd = i.TempSimd128Register(1);

      if (HasImmediateInput(instr, 1)) {
        // Perform 16-bit shift, then mask away high bits.
        uint8_t shift = i.InputInt3(1);
        __ Psrlw(dst, dst, byte{shift});

        uint8_t bmask = 0xff >> shift;
        uint32_t mask = bmask << 24 | bmask << 16 | bmask << 8 | bmask;
        __ mov(tmp, mask);
        __ Movd(tmp_simd, tmp);
        __ Pshufd(tmp_simd, tmp_simd, 0);
        __ Pand(dst, tmp_simd);
      } else {
        // Unpack the bytes into words, do logical shifts, and repack.
        __ Punpckhbw(kScratchDoubleReg, dst);
        __ Punpcklbw(dst, dst);
        __ mov(tmp, i.InputRegister(1));
        // Take shift value modulo 8.
        __ and_(tmp, 7);
        __ add(tmp, Immediate(8));
        __ Movd(tmp_simd, tmp);
        __ Psrlw(kScratchDoubleReg, kScratchDoubleReg, tmp_simd);
        __ Psrlw(dst, dst, tmp_simd);
        __ Packuswb(dst, kScratchDoubleReg);
      }
      break;
    }
    case kSSEI8x16MinU: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ pminub(dst, i.InputOperand(1));
      break;
    }
    case kAVXI8x16MinU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpminub(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI8x16MaxU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pmaxub(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXI8x16MaxU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpmaxub(i.OutputSimd128Register(), i.InputSimd128Register(0),
                 i.InputOperand(1));
      break;
    }
    case kSSEI8x16GtU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pmaxub(dst, src);
      __ pcmpeqb(dst, src);
      __ pcmpeqb(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(dst, kScratchDoubleReg);
      break;
    }
    case kAVXI8x16GtU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpmaxub(kScratchDoubleReg, src1, src2);
      __ vpcmpeqb(dst, kScratchDoubleReg, src2);
      __ vpcmpeqb(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kSSEI8x16GeU: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(1);
      __ pminub(dst, src);
      __ pcmpeqb(dst, src);
      break;
    }
    case kAVXI8x16GeU: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister src1 = i.InputSimd128Register(0);
      Operand src2 = i.InputOperand(1);
      __ vpminub(kScratchDoubleReg, src1, src2);
      __ vpcmpeqb(i.OutputSimd128Register(), kScratchDoubleReg, src2);
      break;
    }
    case kIA32I8x16RoundingAverageU: {
      __ Pavgb(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputOperand(1));
      break;
    }
    case kIA32I8x16Abs: {
      __ Pabsb(i.OutputSimd128Register(), i.InputSimd128Register(0));
      break;
    }
    case kIA32I8x16BitMask: {
      __ Pmovmskb(i.OutputRegister(), i.InputSimd128Register(0));
      break;
    }
    case kIA32S128Const: {
      XMMRegister dst = i.OutputSimd128Register();
      Register tmp = i.TempRegister(0);
      uint64_t low_qword = make_uint64(i.InputUint32(1), i.InputUint32(0));
      __ Move(dst, low_qword);
      __ Move(tmp, Immediate(i.InputUint32(2)));
      __ Pinsrd(dst, tmp, 2);
      __ Move(tmp, Immediate(i.InputUint32(3)));
      __ Pinsrd(dst, tmp, 3);
      break;
    }
    case kIA32S128Zero: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Pxor(dst, dst);
      break;
    }
    case kIA32S128AllOnes: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Pcmpeqd(dst, dst);
      break;
    }
    case kSSES128Not: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ pcmpeqd(kScratchDoubleReg, kScratchDoubleReg);
      __ pxor(dst, kScratchDoubleReg);
      break;
    }
    case kAVXS128Not: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpcmpeqd(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      __ vpxor(i.OutputSimd128Register(), kScratchDoubleReg, i.InputOperand(0));
      break;
    }
    case kSSES128And: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pand(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXS128And: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpand(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputOperand(1));
      break;
    }
    case kSSES128Or: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ por(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXS128Or: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpor(i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputOperand(1));
      break;
    }
    case kSSES128Xor: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      __ pxor(i.OutputSimd128Register(), i.InputOperand(1));
      break;
    }
    case kAVXS128Xor: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      __ vpxor(i.OutputSimd128Register(), i.InputSimd128Register(0),
               i.InputOperand(1));
      break;
    }
    case kSSES128Select: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      // Mask used here is stored in dst.
      XMMRegister dst = i.OutputSimd128Register();
      __ movaps(kScratchDoubleReg, i.InputSimd128Register(1));
      __ xorps(kScratchDoubleReg, i.InputSimd128Register(2));
      __ andps(dst, kScratchDoubleReg);
      __ xorps(dst, i.InputSimd128Register(2));
      break;
    }
    case kAVXS128Select: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      __ vxorps(kScratchDoubleReg, i.InputSimd128Register(2),
                i.InputOperand(1));
      __ vandps(kScratchDoubleReg, kScratchDoubleReg, i.InputOperand(0));
      __ vxorps(dst, kScratchDoubleReg, i.InputSimd128Register(2));
      break;
    }
    case kIA32S128AndNot: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      // The inputs have been inverted by instruction selector, so we can call
      // andnps here without any modifications.
      XMMRegister src1 = i.InputSimd128Register(1);
      __ Andnps(dst, src1);
      break;
    }
    case kIA32I8x16Swizzle: {
      DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister mask = i.TempSimd128Register(0);

      // Out-of-range indices should return 0, add 112 so that any value > 15
      // saturates to 128 (top bit set), so pshufb will zero that lane.
      __ Move(mask, uint32_t{0x70707070});
      __ Pshufd(mask, mask, 0x0);
      __ Paddusb(mask, i.InputSimd128Register(1));
      __ Pshufb(dst, mask);
      break;
    }
    case kIA32I8x16Shuffle: {
      XMMRegister dst = i.OutputSimd128Register();
      Operand src0 = i.InputOperand(0);
      Register tmp = i.TempRegister(0);
      // Prepare 16 byte aligned buffer for shuffle control mask
      __ mov(tmp, esp);
      __ and_(esp, -16);
      if (instr->InputCount() == 5) {  // only one input operand
        DCHECK_EQ(i.OutputSimd128Register(), i.InputSimd128Register(0));
        for (int j = 4; j > 0; j--) {
          uint32_t mask = i.InputUint32(j);
          __ push(Immediate(mask));
        }
        __ Pshufb(dst, Operand(esp, 0));
      } else {  // two input operands
        DCHECK_EQ(6, instr->InputCount());
        __ Movups(kScratchDoubleReg, src0);
        for (int j = 5; j > 1; j--) {
          uint32_t lanes = i.InputUint32(j);
          uint32_t mask = 0;
          for (int k = 0; k < 32; k += 8) {
            uint8_t lane = lanes >> k;
            mask |= (lane < kSimd128Size ? lane : 0x80) << k;
          }
          __ push(Immediate(mask));
        }
        __ Pshufb(kScratchDoubleReg, Operand(esp, 0));
        Operand src1 = i.InputOperand(1);
        if (!src1.is_reg(dst)) __ Movups(dst, src1);
        for (int j = 5; j > 1; j--) {
          uint32_t lanes = i.InputUint32(j);
          uint32_t mask = 0;
          for (int k = 0; k < 32; k += 8) {
            uint8_t lane = lanes >> k;
            mask |= (lane >= kSimd128Size ? (lane & 0xF) : 0x80) << k;
          }
          __ push(Immediate(mask));
        }
        __ Pshufb(dst, Operand(esp, 0));
        __ por(dst, kScratchDoubleReg);
      }
      __ mov(esp, tmp);
      break;
    }
    case kIA32S128Load8Splat: {
      __ Pinsrb(i.OutputSimd128Register(), i.MemoryOperand(), 0);
      __ Pxor(kScratchDoubleReg, kScratchDoubleReg);
      __ Pshufb(i.OutputSimd128Register(), kScratchDoubleReg);
      break;
    }
    case kIA32S128Load16Splat: {
      __ Pinsrw(i.OutputSimd128Register(), i.MemoryOperand(), 0);
      __ Pshuflw(i.OutputSimd128Register(), i.OutputSimd128Register(),
                 uint8_t{0});
      __ Punpcklqdq(i.OutputSimd128Register(), i.OutputSimd128Register());
      break;
    }
    case kIA32S128Load32Splat: {
      __ Vbroadcastss(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load64Splat: {
      __ Movddup(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load8x8S: {
      __ Pmovsxbw(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load8x8U: {
      __ Pmovzxbw(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load16x4S: {
      __ Pmovsxwd(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load16x4U: {
      __ Pmovzxwd(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load32x2S: {
      __ Pmovsxdq(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S128Load32x2U: {
      __ Pmovzxdq(i.OutputSimd128Register(), i.MemoryOperand());
      break;
    }
    case kIA32S32x4Swizzle: {
      DCHECK_EQ(2, instr->InputCount());
      __ Pshufd(i.OutputSimd128Register(), i.InputOperand(0), i.InputInt8(1));
      break;
    }
    case kIA32S32x4Shuffle: {
      DCHECK_EQ(4, instr->InputCount());  // Swizzles should be handled above.
      int8_t shuffle = i.InputInt8(2);
      DCHECK_NE(0xe4, shuffle);  // A simple blend should be handled below.
      __ Pshufd(kScratchDoubleReg, i.InputOperand(1), shuffle);
      __ Pshufd(i.OutputSimd128Register(), i.InputOperand(0), shuffle);
      __ Pblendw(i.OutputSimd128Register(), kScratchDoubleReg, i.InputInt8(3));
      break;
    }
    case kIA32S16x8Blend:
      ASSEMBLE_SIMD_IMM_SHUFFLE(pblendw, SSE4_1, i.InputInt8(2));
      break;
    case kIA32S16x8HalfShuffle1: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Pshuflw(dst, i.InputOperand(0), i.InputInt8(1));
      __ Pshufhw(dst, dst, i.InputInt8(2));
      break;
    }
    case kIA32S16x8HalfShuffle2: {
      XMMRegister dst = i.OutputSimd128Register();
      __ Pshuflw(kScratchDoubleReg, i.InputOperand(1), i.InputInt8(2));
      __ Pshufhw(kScratchDoubleReg, kScratchDoubleReg, i.InputInt8(3));
      __ Pshuflw(dst, i.InputOperand(0), i.InputInt8(2));
      __ Pshufhw(dst, dst, i.InputInt8(3));
      __ Pblendw(dst, kScratchDoubleReg, i.InputInt8(4));
      break;
    }
    case kIA32S8x16Alignr:
      ASSEMBLE_SIMD_IMM_SHUFFLE(palignr, SSSE3, i.InputInt8(2));
      break;
    case kIA32S16x8Dup: {
      XMMRegister dst = i.OutputSimd128Register();
      Operand src = i.InputOperand(0);
      int8_t lane = i.InputInt8(1) & 0x7;
      int8_t lane4 = lane & 0x3;
      int8_t half_dup = lane4 | (lane4 << 2) | (lane4 << 4) | (lane4 << 6);
      if (lane < 4) {
        __ Pshuflw(dst, src, half_dup);
        __ Pshufd(dst, dst, 0);
      } else {
        __ Pshufhw(dst, src, half_dup);
        __ Pshufd(dst, dst, 0xaa);
      }
      break;
    }
    case kIA32S8x16Dup: {
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src = i.InputSimd128Register(0);
      int8_t lane = i.InputInt8(1) & 0xf;
      if (CpuFeatures::IsSupported(AVX)) {
        CpuFeatureScope avx_scope(tasm(), AVX);
        if (lane < 8) {
          __ vpunpcklbw(dst, src, src);
        } else {
          __ vpunpckhbw(dst, src, src);
        }
      } else {
        DCHECK_EQ(dst, src);
        if (lane < 8) {
          __ punpcklbw(dst, dst);
        } else {
          __ punpckhbw(dst, dst);
        }
      }
      lane &= 0x7;
      int8_t lane4 = lane & 0x3;
      int8_t half_dup = lane4 | (lane4 << 2) | (lane4 << 4) | (lane4 << 6);
      if (lane < 4) {
        __ Pshuflw(dst, dst, half_dup);
        __ Pshufd(dst, dst, 0);
      } else {
        __ Pshufhw(dst, dst, half_dup);
        __ Pshufd(dst, dst, 0xaa);
      }
      break;
    }
    case kIA32S64x2UnpackHigh:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpckhqdq);
      break;
    case kIA32S32x4UnpackHigh:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpckhdq);
      break;
    case kIA32S16x8UnpackHigh:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpckhwd);
      break;
    case kIA32S8x16UnpackHigh:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpckhbw);
      break;
    case kIA32S64x2UnpackLow:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpcklqdq);
      break;
    case kIA32S32x4UnpackLow:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpckldq);
      break;
    case kIA32S16x8UnpackLow:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpcklwd);
      break;
    case kIA32S8x16UnpackLow:
      ASSEMBLE_SIMD_PUNPCK_SHUFFLE(punpcklbw);
      break;
    case kSSES16x8UnzipHigh: {
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      if (instr->InputCount() == 2) {
        __ movups(kScratchDoubleReg, i.InputOperand(1));
        __ psrld(kScratchDoubleReg, 16);
        src2 = kScratchDoubleReg;
      }
      __ psrld(dst, 16);
      __ packusdw(dst, src2);
      break;
    }
    case kAVXS16x8UnzipHigh: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      if (instr->InputCount() == 2) {
        __ vpsrld(kScratchDoubleReg, i.InputSimd128Register(1), 16);
        src2 = kScratchDoubleReg;
      }
      __ vpsrld(dst, i.InputSimd128Register(0), 16);
      __ vpackusdw(dst, dst, src2);
      break;
    }
    case kSSES16x8UnzipLow: {
      CpuFeatureScope sse_scope(tasm(), SSE4_1);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ pxor(kScratchDoubleReg, kScratchDoubleReg);
      if (instr->InputCount() == 2) {
        __ pblendw(kScratchDoubleReg, i.InputOperand(1), 0x55);
        src2 = kScratchDoubleReg;
      }
      __ pblendw(dst, kScratchDoubleReg, 0xaa);
      __ packusdw(dst, src2);
      break;
    }
    case kAVXS16x8UnzipLow: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      __ vpxor(kScratchDoubleReg, kScratchDoubleReg, kScratchDoubleReg);
      if (instr->InputCount() == 2) {
        __ vpblendw(kScratchDoubleReg, kScratchDoubleReg, i.InputOperand(1),
                    0x55);
        src2 = kScratchDoubleReg;
      }
      __ vpblendw(dst, kScratchDoubleReg, i.InputSimd128Register(0), 0x55);
      __ vpackusdw(dst, dst, src2);
      break;
    }
    case kSSES8x16UnzipHigh: {
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      if (instr->InputCount() == 2) {
        __ movups(kScratchDoubleReg, i.InputOperand(1));
        __ psrlw(kScratchDoubleReg, 8);
        src2 = kScratchDoubleReg;
      }
      __ psrlw(dst, 8);
      __ packuswb(dst, src2);
      break;
    }
    case kAVXS8x16UnzipHigh: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      if (instr->InputCount() == 2) {
        __ vpsrlw(kScratchDoubleReg, i.InputSimd128Register(1), 8);
        src2 = kScratchDoubleReg;
      }
      __ vpsrlw(dst, i.InputSimd128Register(0), 8);
      __ vpackuswb(dst, dst, src2);
      break;
    }
    case kSSES8x16UnzipLow: {
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      if (instr->InputCount() == 2) {
        __ movups(kScratchDoubleReg, i.InputOperand(1));
        __ psllw(kScratchDoubleReg, 8);
        __ psrlw(kScratchDoubleReg, 8);
        src2 = kScratchDoubleReg;
      }
      __ psllw(dst, 8);
      __ psrlw(dst, 8);
      __ packuswb(dst, src2);
      break;
    }
    case kAVXS8x16UnzipLow: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src2 = dst;
      if (instr->InputCount() == 2) {
        __ vpsllw(kScratchDoubleReg, i.InputSimd128Register(1), 8);
        __ vpsrlw(kScratchDoubleReg, kScratchDoubleReg, 8);
        src2 = kScratchDoubleReg;
      }
      __ vpsllw(dst, i.InputSimd128Register(0), 8);
      __ vpsrlw(dst, dst, 8);
      __ vpackuswb(dst, dst, src2);
      break;
    }
    case kSSES8x16TransposeLow: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ psllw(dst, 8);
      if (instr->InputCount() == 1) {
        __ movups(kScratchDoubleReg, dst);
      } else {
        DCHECK_EQ(2, instr->InputCount());
        __ movups(kScratchDoubleReg, i.InputOperand(1));
        __ psllw(kScratchDoubleReg, 8);
      }
      __ psrlw(dst, 8);
      __ por(dst, kScratchDoubleReg);
      break;
    }
    case kAVXS8x16TransposeLow: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      if (instr->InputCount() == 1) {
        __ vpsllw(kScratchDoubleReg, i.InputSimd128Register(0), 8);
        __ vpsrlw(dst, kScratchDoubleReg, 8);
      } else {
        DCHECK_EQ(2, instr->InputCount());
        __ vpsllw(kScratchDoubleReg, i.InputSimd128Register(1), 8);
        __ vpsllw(dst, i.InputSimd128Register(0), 8);
        __ vpsrlw(dst, dst, 8);
      }
      __ vpor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kSSES8x16TransposeHigh: {
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      __ psrlw(dst, 8);
      if (instr->InputCount() == 1) {
        __ movups(kScratchDoubleReg, dst);
      } else {
        DCHECK_EQ(2, instr->InputCount());
        __ movups(kScratchDoubleReg, i.InputOperand(1));
        __ psrlw(kScratchDoubleReg, 8);
      }
      __ psllw(kScratchDoubleReg, 8);
      __ por(dst, kScratchDoubleReg);
      break;
    }
    case kAVXS8x16TransposeHigh: {
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      if (instr->InputCount() == 1) {
        __ vpsrlw(dst, i.InputSimd128Register(0), 8);
        __ vpsllw(kScratchDoubleReg, dst, 8);
      } else {
        DCHECK_EQ(2, instr->InputCount());
        __ vpsrlw(kScratchDoubleReg, i.InputSimd128Register(1), 8);
        __ vpsrlw(dst, i.InputSimd128Register(0), 8);
        __ vpsllw(kScratchDoubleReg, kScratchDoubleReg, 8);
      }
      __ vpor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kSSES8x8Reverse:
    case kSSES8x4Reverse:
    case kSSES8x2Reverse: {
      DCHECK_EQ(1, instr->InputCount());
      XMMRegister dst = i.OutputSimd128Register();
      DCHECK_EQ(dst, i.InputSimd128Register(0));
      if (arch_opcode != kSSES8x2Reverse) {
        // First shuffle words into position.
        int8_t shuffle_mask = arch_opcode == kSSES8x4Reverse ? 0xB1 : 0x1B;
        __ pshuflw(dst, dst, shuffle_mask);
        __ pshufhw(dst, dst, shuffle_mask);
      }
      __ movaps(kScratchDoubleReg, dst);
      __ psrlw(kScratchDoubleReg, 8);
      __ psllw(dst, 8);
      __ por(dst, kScratchDoubleReg);
      break;
    }
    case kAVXS8x2Reverse:
    case kAVXS8x4Reverse:
    case kAVXS8x8Reverse: {
      DCHECK_EQ(1, instr->InputCount());
      CpuFeatureScope avx_scope(tasm(), AVX);
      XMMRegister dst = i.OutputSimd128Register();
      XMMRegister src = dst;
      if (arch_opcode != kAVXS8x2Reverse) {
        // First shuffle words into position.
        int8_t shuffle_mask = arch_opcode == kAVXS8x4Reverse ? 0xB1 : 0x1B;
        __ vpshuflw(dst, i.InputOperand(0), shuffle_mask);
        __ vpshufhw(dst, dst, shuffle_mask);
      } else {
        src = i.InputSimd128Register(0);
      }
      // Reverse each 16 bit lane.
      __ vpsrlw(kScratchDoubleReg, src, 8);
      __ vpsllw(dst, src, 8);
      __ vpor(dst, dst, kScratchDoubleReg);
      break;
    }
    case kIA32V32x4AnyTrue:
    case kIA32V16x8AnyTrue:
    case kIA32V8x16AnyTrue: {
      Register dst = i.OutputRegister();
      XMMRegister src = i.InputSimd128Register(0);
      Register tmp = i.TempRegister(0);
      __ xor_(tmp, tmp);
      __ mov(dst, Immediate(1));
      __ Ptest(src, src);
      __ cmov(zero, dst, tmp);
      break;
    }
    // Need to split up all the different lane structures because the
    // comparison instruction used matters, e.g. given 0xff00, pcmpeqb returns
    // 0x0011, pcmpeqw returns 0x0000, ptest will set ZF to 0 and 1
    // respectively.
    case kIA32V32x4AllTrue:
      ASSEMBLE_SIMD_ALL_TRUE(Pcmpeqd);
      break;
    case kIA32V16x8AllTrue:
      ASSEMBLE_SIMD_ALL_TRUE(pcmpeqw);
      break;
    case kIA32V8x16AllTrue: {
      ASSEMBLE_SIMD_ALL_TRUE(pcmpeqb);
      break;
    }
    case kIA32Word32AtomicPairLoad: {
      XMMRegister tmp = i.ToDoubleRegister(instr->TempAt(0));
      __ movq(tmp, i.MemoryOperand());
      __ Pextrd(i.OutputRegister(0), tmp, 0);
      __ Pextrd(i.OutputRegister(1), tmp, 1);
      break;
    }
    case kIA32Word32AtomicPairStore: {
      Label store;
      __ bind(&store);
      __ mov(i.TempRegister(0), i.MemoryOperand(2));
      __ mov(i.TempRegister(1), i.NextMemoryOperand(2));
      __ push(ebx);
      frame_access_state()->IncreaseSPDelta(1);
      i.MoveInstructionOperandToRegister(ebx, instr->InputAt(0));
      __ lock();
      __ cmpxchg8b(i.MemoryOperand(2));
      __ pop(ebx);
      frame_access_state()->IncreaseSPDelta(-1);
      __ j(not_equal, &store);
      break;
    }
    case kWord32AtomicExchangeInt8: {
      __ xchg_b(i.InputRegister(0), i.MemoryOperand(1));
      __ movsx_b(i.InputRegister(0), i.InputRegister(0));
      break;
    }
    case kWord32AtomicExchangeUint8: {
      __ xchg_b(i.InputRegister(0), i.MemoryOperand(1));
      __ movzx_b(i.InputRegister(0), i.InputRegister(0));
      break;
    }
    case kWord32AtomicExchangeInt16: {
      __ xchg_w(i.InputRegister(0), i.MemoryOperand(1));
      __ movsx_w(i.InputRegister(0), i.InputRegister(0));
      break;
    }
    case kWord32AtomicExchangeUint16: {
      __ xchg_w(i.InputRegister(0), i.MemoryOperand(1));
      __ movzx_w(i.InputRegister(0), i.InputRegister(0));
      break;
    }
    case kWord32AtomicExchangeWord32: {
      __ xchg(i.InputRegister(0), i.MemoryOperand(1));
      break;
    }
    case kIA32Word32AtomicPairExchange: {
      DCHECK(VerifyOutputOfAtomicPairInstr(&i, instr));
      Label exchange;
      __ bind(&exchange);
      __ mov(eax, i.MemoryOperand(2));
      __ mov(edx, i.NextMemoryOperand(2));
      __ push(ebx);
      frame_access_state()->IncreaseSPDelta(1);
      i.MoveInstructionOperandToRegister(ebx, instr->InputAt(0));
      __ lock();
      __ cmpxchg8b(i.MemoryOperand(2));
      __ pop(ebx);
      frame_access_state()->IncreaseSPDelta(-1);
      __ j(not_equal, &exchange);
      break;
    }
    case kWord32AtomicCompareExchangeInt8: {
      __ lock();
      __ cmpxchg_b(i.MemoryOperand(2), i.InputRegister(1));
      __ movsx_b(eax, eax);
      break;
    }
    case kWord32AtomicCompareExchangeUint8: {
      __ lock();
      __ cmpxchg_b(i.MemoryOperand(2), i.InputRegister(1));
      __ movzx_b(eax, eax);
      break;
    }
    case kWord32AtomicCompareExchangeInt16: {
      __ lock();
      __ cmpxchg_w(i.MemoryOperand(2), i.InputRegister(1));
      __ movsx_w(eax, eax);
      break;
    }
    case kWord32AtomicCompareExchangeUint16: {
      __ lock();
      __ cmpxchg_w(i.MemoryOperand(2), i.InputRegister(1));
      __ movzx_w(eax, eax);
      break;
    }
    case kWord32AtomicCompareExchangeWord32: {
      __ lock();
      __ cmpxchg(i.MemoryOperand(2), i.InputRegister(1));
      break;
    }
    case kIA32Word32AtomicPairCompareExchange: {
      __ push(ebx);
      frame_access_state()->IncreaseSPDelta(1);
      i.MoveInstructionOperandToRegister(ebx, instr->InputAt(2));
      __ lock();
      __ cmpxchg8b(i.MemoryOperand(4));
      __ pop(ebx);
      frame_access_state()->IncreaseSPDelta(-1);
      break;
    }
#define ATOMIC_BINOP_CASE(op, inst)                \
  case kWord32Atomic##op##Int8: {                  \
    ASSEMBLE_ATOMIC_BINOP(inst, mov_b, cmpxchg_b); \
    __ movsx_b(eax, eax);                          \
    break;                                         \
  }                                                \
  case kWord32Atomic##op##Uint8: {                 \
    ASSEMBLE_ATOMIC_BINOP(inst, mov_b, cmpxchg_b); \
    __ movzx_b(eax, eax);                          \
    break;                                         \
  }                                                \
  case kWord32Atomic##op##Int16: {                 \
    ASSEMBLE_ATOMIC_BINOP(inst, mov_w, cmpxchg_w); \
    __ movsx_w(eax, eax);                          \
    break;                                         \
  }                                                \
  case kWord32Atomic##op##Uint16: {                \
    ASSEMBLE_ATOMIC_BINOP(inst, mov_w, cmpxchg_w); \
    __ movzx_w(eax, eax);                          \
    break;                                         \
  }                                                \
  case kWord32Atomic##op##Word32: {                \
    ASSEMBLE_ATOMIC_BINOP(inst, mov, cmpxchg);     \
    break;                                         \
  }
      ATOMIC_BINOP_CASE(Add, add)
      ATOMIC_BINOP_CASE(Sub, sub)
      ATOMIC_BINOP_CASE(And, and_)
      ATOMIC_BINOP_CASE(Or, or_)
      ATOMIC_BINOP_CASE(Xor, xor_)
#undef ATOMIC_BINOP_CASE
#define ATOMIC_BINOP_CASE(op, instr1, instr2)         \
  case kIA32Word32AtomicPair##op: {                   \
    DCHECK(VerifyOutputOfAtomicPairInstr(&i, instr)); \
    ASSEMBLE_I64ATOMIC_BINOP(instr1, instr2)          \
    break;                                            \
  }
      ATOMIC_BINOP_CASE(Add, add, adc)
      ATOMIC_BINOP_CASE(And, and_, and_)
      ATOMIC_BINOP_CASE(Or, or_, or_)
      ATOMIC_BINOP_CASE(Xor, xor_, xor_)
#undef ATOMIC_BINOP_CASE
    case kIA32Word32AtomicPairSub: {
      DCHECK(VerifyOutputOfAtomicPairInstr(&i, instr));
      Label binop;
      __ bind(&binop);
      // Move memory operand into edx:eax
      __ mov(eax, i.MemoryOperand(2));
      __ mov(edx, i.NextMemoryOperand(2));
      // Save input registers temporarily on the stack.
      __ push(ebx);
      frame_access_state()->IncreaseSPDelta(1);
      i.MoveInstructionOperandToRegister(ebx, instr->InputAt(0));
      __ push(i.InputRegister(1));
      // Negate input in place
      __ neg(ebx);
      __ adc(i.InputRegister(1), 0);
      __ neg(i.InputRegister(1));
      // Add memory operand, negated input.
      __ add(ebx, eax);
      __ adc(i.InputRegister(1), edx);
      __ lock();
      __ cmpxchg8b(i.MemoryOperand(2));
      // Restore input registers
      __ pop(i.InputRegister(1));
      __ pop(ebx);
      frame_access_state()->IncreaseSPDelta(-1);
      __ j(not_equal, &binop);
      break;
    }
    case kWord32AtomicLoadInt8:
    case kWord32AtomicLoadUint8:
    case kWord32AtomicLoadInt16:
    case kWord32AtomicLoadUint16:
    case kWord32AtomicLoadWord32:
    case kWord32AtomicStoreWord8:
    case kWord32AtomicStoreWord16:
    case kWord32AtomicStoreWord32:
      UNREACHABLE();  // Won't be generated by instruction selector.
      break;
  }
  return kSuccess;
}  // NOLINT(readability/fn_size)

static Condition FlagsConditionToCondition(FlagsCondition condition) {
  switch (condition) {
    case kUnorderedEqual:
    case kEqual:
      return equal;
      break;
    case kUnorderedNotEqual:
    case kNotEqual:
      return not_equal;
      break;
    case kSignedLessThan:
      return less;
      break;
    case kSignedGreaterThanOrEqual:
      return greater_equal;
      break;
    case kSignedLessThanOrEqual:
      return less_equal;
      break;
    case kSignedGreaterThan:
      return greater;
      break;
    case kUnsignedLessThan:
      return below;
      break;
    case kUnsignedGreaterThanOrEqual:
      return above_equal;
      break;
    case kUnsignedLessThanOrEqual:
      return below_equal;
      break;
    case kUnsignedGreaterThan:
      return above;
      break;
    case kOverflow:
      return overflow;
      break;
    case kNotOverflow:
      return no_overflow;
      break;
    default:
      UNREACHABLE();
  }
}

// Assembles a branch after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr, BranchInfo* branch) {
  Label::Distance flabel_distance =
      branch->fallthru ? Label::kNear : Label::kFar;
  Label* tlabel = branch->true_label;
  Label* flabel = branch->false_label;
  if (branch->condition == kUnorderedEqual) {
    __ j(parity_even, flabel, flabel_distance);
  } else if (branch->condition == kUnorderedNotEqual) {
    __ j(parity_even, tlabel);
  }
  __ j(FlagsConditionToCondition(branch->condition), tlabel);

  // Add a jump if not falling through to the next block.
  if (!branch->fallthru) __ jmp(flabel);
}

void CodeGenerator::AssembleBranchPoisoning(FlagsCondition condition,
                                            Instruction* instr) {
  // TODO(860429): Remove remaining poisoning infrastructure on ia32.
  UNREACHABLE();
}

void CodeGenerator::AssembleArchDeoptBranch(Instruction* instr,
                                            BranchInfo* branch) {
  AssembleArchBranch(instr, branch);
}

void CodeGenerator::AssembleArchJump(RpoNumber target) {
  if (!IsNextInAssemblyOrder(target)) __ jmp(GetLabel(target));
}

void CodeGenerator::AssembleArchTrap(Instruction* instr,
                                     FlagsCondition condition) {
  class OutOfLineTrap final : public OutOfLineCode {
   public:
    OutOfLineTrap(CodeGenerator* gen, Instruction* instr)
        : OutOfLineCode(gen), instr_(instr), gen_(gen) {}

    void Generate() final {
      IA32OperandConverter i(gen_, instr_);
      TrapId trap_id =
          static_cast<TrapId>(i.InputInt32(instr_->InputCount() - 1));
      GenerateCallToTrap(trap_id);
    }

   private:
    void GenerateCallToTrap(TrapId trap_id) {
      if (trap_id == TrapId::kInvalid) {
        // We cannot test calls to the runtime in cctest/test-run-wasm.
        // Therefore we emit a call to C here instead of a call to the runtime.
        __ PrepareCallCFunction(0, esi);
        __ CallCFunction(
            ExternalReference::wasm_call_trap_callback_for_testing(), 0);
        __ LeaveFrame(StackFrame::WASM);
        auto call_descriptor = gen_->linkage()->GetIncomingDescriptor();
        size_t pop_size =
            call_descriptor->StackParameterCount() * kSystemPointerSize;
        // Use ecx as a scratch register, we return anyways immediately.
        __ Ret(static_cast<int>(pop_size), ecx);
      } else {
        gen_->AssembleSourcePosition(instr_);
        // A direct call to a wasm runtime stub defined in this module.
        // Just encode the stub index. This will be patched when the code
        // is added to the native module and copied into wasm code space.
        __ wasm_call(static_cast<Address>(trap_id), RelocInfo::WASM_STUB_CALL);
        ReferenceMap* reference_map =
            gen_->zone()->New<ReferenceMap>(gen_->zone());
        gen_->RecordSafepoint(reference_map, Safepoint::kNoLazyDeopt);
        __ AssertUnreachable(AbortReason::kUnexpectedReturnFromWasmTrap);
      }
    }

    Instruction* instr_;
    CodeGenerator* gen_;
  };
  auto ool = zone()->New<OutOfLineTrap>(this, instr);
  Label* tlabel = ool->entry();
  Label end;
  if (condition == kUnorderedEqual) {
    __ j(parity_even, &end, Label::kNear);
  } else if (condition == kUnorderedNotEqual) {
    __ j(parity_even, tlabel);
  }
  __ j(FlagsConditionToCondition(condition), tlabel);
  __ bind(&end);
}

// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  IA32OperandConverter i(this, instr);
  Label done;

  // Materialize a full 32-bit 1 or 0 value. The result register is always the
  // last output of the instruction.
  Label check;
  DCHECK_NE(0u, instr->OutputCount());
  Register reg = i.OutputRegister(instr->OutputCount() - 1);
  if (condition == kUnorderedEqual) {
    __ j(parity_odd, &check, Label::kNear);
    __ Move(reg, Immediate(0));
    __ jmp(&done, Label::kNear);
  } else if (condition == kUnorderedNotEqual) {
    __ j(parity_odd, &check, Label::kNear);
    __ mov(reg, Immediate(1));
    __ jmp(&done, Label::kNear);
  }
  Condition cc = FlagsConditionToCondition(condition);

  __ bind(&check);
  if (reg.is_byte_register()) {
    // setcc for byte registers (al, bl, cl, dl).
    __ setcc(cc, reg);
    __ movzx_b(reg, reg);
  } else {
    // Emit a branch to set a register to either 1 or 0.
    Label set;
    __ j(cc, &set, Label::kNear);
    __ Move(reg, Immediate(0));
    __ jmp(&done, Label::kNear);
    __ bind(&set);
    __ mov(reg, Immediate(1));
  }
  __ bind(&done);
}

void CodeGenerator::AssembleArchBinarySearchSwitch(Instruction* instr) {
  IA32OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  std::vector<std::pair<int32_t, Label*>> cases;
  for (size_t index = 2; index < instr->InputCount(); index += 2) {
    cases.push_back({i.InputInt32(index + 0), GetLabel(i.InputRpo(index + 1))});
  }
  AssembleArchBinarySearchSwitchRange(input, i.InputRpo(1), cases.data(),
                                      cases.data() + cases.size());
}

void CodeGenerator::AssembleArchTableSwitch(Instruction* instr) {
  IA32OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  size_t const case_count = instr->InputCount() - 2;
  Label** cases = zone()->NewArray<Label*>(case_count);
  for (size_t index = 0; index < case_count; ++index) {
    cases[index] = GetLabel(i.InputRpo(index + 2));
  }
  Label* const table = AddJumpTable(cases, case_count);
  __ cmp(input, Immediate(case_count));
  __ j(above_equal, GetLabel(i.InputRpo(1)));
  __ jmp(Operand::JumpTable(input, times_system_pointer_size, table));
}

// The calling convention for JSFunctions on IA32 passes arguments on the
// stack and the JSFunction and context in EDI and ESI, respectively, thus
// the steps of the call look as follows:

// --{ before the call instruction }--------------------------------------------
//                                                         |  caller frame |
//                                                         ^ esp           ^ ebp

// --{ push arguments and setup ESI, EDI }--------------------------------------
//                                       | args + receiver |  caller frame |
//                                       ^ esp                             ^ ebp
//                 [edi = JSFunction, esi = context]

// --{ call [edi + kCodeEntryOffset] }------------------------------------------
//                                 | RET | args + receiver |  caller frame |
//                                 ^ esp                                   ^ ebp

// =={ prologue of called function }============================================
// --{ push ebp }---------------------------------------------------------------
//                            | FP | RET | args + receiver |  caller frame |
//                            ^ esp                                        ^ ebp

// --{ mov ebp, esp }-----------------------------------------------------------
//                            | FP | RET | args + receiver |  caller frame |
//                            ^ ebp,esp

// --{ push esi }---------------------------------------------------------------
//                      | CTX | FP | RET | args + receiver |  caller frame |
//                      ^esp  ^ ebp

// --{ push edi }---------------------------------------------------------------
//                | FNC | CTX | FP | RET | args + receiver |  caller frame |
//                ^esp        ^ ebp

// --{ subi esp, #N }-----------------------------------------------------------
// | callee frame | FNC | CTX | FP | RET | args + receiver |  caller frame |
// ^esp                       ^ ebp

// =={ body of called function }================================================

// =={ epilogue of called function }============================================
// --{ mov esp, ebp }-----------------------------------------------------------
//                            | FP | RET | args + receiver |  caller frame |
//                            ^ esp,ebp

// --{ pop ebp }-----------------------------------------------------------
// |                               | RET | args + receiver |  caller frame |
//                                 ^ esp                                   ^ ebp

// --{ ret #A+1 }-----------------------------------------------------------
// |                                                       |  caller frame |
//                                                         ^ esp           ^ ebp

// Runtime function calls are accomplished by doing a stub call to the
// CEntry (a real code object). On IA32 passes arguments on the
// stack, the number of arguments in EAX, the address of the runtime function
// in EBX, and the context in ESI.

// --{ before the call instruction }--------------------------------------------
//                                                         |  caller frame |
//                                                         ^ esp           ^ ebp

// --{ push arguments and setup EAX, EBX, and ESI }-----------------------------
//                                       | args + receiver |  caller frame |
//                                       ^ esp                             ^ ebp
//              [eax = #args, ebx = runtime function, esi = context]

// --{ call #CEntry }-----------------------------------------------------------
//                                 | RET | args + receiver |  caller frame |
//                                 ^ esp                                   ^ ebp

// =={ body of runtime function }===============================================

// --{ runtime returns }--------------------------------------------------------
//                                                         |  caller frame |
//                                                         ^ esp           ^ ebp

// Other custom linkages (e.g. for calling directly into and out of C++) may
// need to save callee-saved registers on the stack, which is done in the
// function prologue of generated code.

// --{ before the call instruction }--------------------------------------------
//                                                         |  caller frame |
//                                                         ^ esp           ^ ebp

// --{ set up arguments in registers on stack }---------------------------------
//                                                  | args |  caller frame |
//                                                  ^ esp                  ^ ebp
//                  [r0 = arg0, r1 = arg1, ...]

// --{ call code }--------------------------------------------------------------
//                                            | RET | args |  caller frame |
//                                            ^ esp                        ^ ebp

// =={ prologue of called function }============================================
// --{ push ebp }---------------------------------------------------------------
//                                       | FP | RET | args |  caller frame |
//                                       ^ esp                             ^ ebp

// --{ mov ebp, esp }-----------------------------------------------------------
//                                       | FP | RET | args |  caller frame |
//                                       ^ ebp,esp

// --{ save registers }---------------------------------------------------------
//                                | regs | FP | RET | args |  caller frame |
//                                ^ esp  ^ ebp

// --{ subi esp, #N }-----------------------------------------------------------
//                 | callee frame | regs | FP | RET | args |  caller frame |
//                 ^esp                  ^ ebp

// =={ body of called function }================================================

// =={ epilogue of called function }============================================
// --{ restore registers }------------------------------------------------------
//                                | regs | FP | RET | args |  caller frame |
//                                ^ esp  ^ ebp

// --{ mov esp, ebp }-----------------------------------------------------------
//                                       | FP | RET | args |  caller frame |
//                                       ^ esp,ebp

// --{ pop ebp }----------------------------------------------------------------
//                                            | RET | args |  caller frame |
//                                            ^ esp                        ^ ebp

void CodeGenerator::FinishFrame(Frame* frame) {
  auto call_descriptor = linkage()->GetIncomingDescriptor();
  const RegList saves = call_descriptor->CalleeSavedRegisters();
  if (saves != 0) {  // Save callee-saved registers.
    DCHECK(!info()->is_osr());
    int pushed = 0;
    for (int i = Register::kNumRegisters - 1; i >= 0; i--) {
      if (!((1 << i) & saves)) continue;
      ++pushed;
    }
    frame->AllocateSavedCalleeRegisterSlots(pushed);
  }
}

void CodeGenerator::AssembleConstructFrame() {
  auto call_descriptor = linkage()->GetIncomingDescriptor();
  if (frame_access_state()->has_frame()) {
    if (call_descriptor->IsCFunctionCall()) {
      __ push(ebp);
      __ mov(ebp, esp);
      if (info()->GetOutputStackFrameType() == StackFrame::C_WASM_ENTRY) {
        __ Push(Immediate(StackFrame::TypeToMarker(StackFrame::C_WASM_ENTRY)));
        // Reserve stack space for saving the c_entry_fp later.
        __ AllocateStackSpace(kSystemPointerSize);
      }
    } else if (call_descriptor->IsJSFunctionCall()) {
      __ Prologue();
    } else {
      __ StubPrologue(info()->GetOutputStackFrameType());
      if (call_descriptor->IsWasmFunctionCall()) {
        __ push(kWasmInstanceRegister);
      } else if (call_descriptor->IsWasmImportWrapper() ||
                 call_descriptor->IsWasmCapiFunction()) {
        // Wasm import wrappers are passed a tuple in the place of the instance.
        // Unpack the tuple into the instance and the target callable.
        // This must be done here in the codegen because it cannot be expressed
        // properly in the graph.
        __ mov(kJSFunctionRegister,
               Operand(kWasmInstanceRegister,
                       Tuple2::kValue2Offset - kHeapObjectTag));
        __ mov(kWasmInstanceRegister,
               Operand(kWasmInstanceRegister,
                       Tuple2::kValue1Offset - kHeapObjectTag));
        __ push(kWasmInstanceRegister);
        if (call_descriptor->IsWasmCapiFunction()) {
          // Reserve space for saving the PC later.
          __ AllocateStackSpace(kSystemPointerSize);
        }
      }
    }
  }

  int required_slots =
      frame()->GetTotalFrameSlotCount() - frame()->GetFixedSlotCount();

  if (info()->is_osr()) {
    // TurboFan OSR-compiled functions cannot be entered directly.
    __ Abort(AbortReason::kShouldNotDirectlyEnterOsrFunction);

    // Unoptimized code jumps directly to this entrypoint while the unoptimized
    // frame is still on the stack. Optimized code uses OSR values directly from
    // the unoptimized frame. Thus, all that needs to be done is to allocate the
    // remaining stack slots.
    if (FLAG_code_comments) __ RecordComment("-- OSR entrypoint --");
    osr_pc_offset_ = __ pc_offset();
    required_slots -= osr_helper()->UnoptimizedFrameSlots();
  }

  const RegList saves = call_descriptor->CalleeSavedRegisters();
  if (required_slots > 0) {
    DCHECK(frame_access_state()->has_frame());
    if (info()->IsWasm() && required_slots > 128) {
      // For WebAssembly functions with big frames we have to do the stack
      // overflow check before we construct the frame. Otherwise we may not
      // have enough space on the stack to call the runtime for the stack
      // overflow.
      Label done;

      // If the frame is bigger than the stack, we throw the stack overflow
      // exception unconditionally. Thereby we can avoid the integer overflow
      // check in the condition code.
      if (required_slots * kSystemPointerSize < FLAG_stack_size * 1024) {
        Register scratch = esi;
        __ push(scratch);
        __ mov(scratch,
               FieldOperand(kWasmInstanceRegister,
                            WasmInstanceObject::kRealStackLimitAddressOffset));
        __ mov(scratch, Operand(scratch, 0));
        __ add(scratch, Immediate(required_slots * kSystemPointerSize));
        __ cmp(esp, scratch);
        __ pop(scratch);
        __ j(above_equal, &done, Label::kNear);
      }

      __ wasm_call(wasm::WasmCode::kWasmStackOverflow,
                   RelocInfo::WASM_STUB_CALL);
      ReferenceMap* reference_map = zone()->New<ReferenceMap>(zone());
      RecordSafepoint(reference_map, Safepoint::kNoLazyDeopt);
      __ AssertUnreachable(AbortReason::kUnexpectedReturnFromWasmTrap);
      __ bind(&done);
    }

    // Skip callee-saved and return slots, which are created below.
    required_slots -= base::bits::CountPopulation(saves);
    required_slots -= frame()->GetReturnSlotCount();
    if (required_slots > 0) {
      __ AllocateStackSpace(required_slots * kSystemPointerSize);
    }
  }

  if (saves != 0) {  // Save callee-saved registers.
    DCHECK(!info()->is_osr());
    for (int i = Register::kNumRegisters - 1; i >= 0; i--) {
      if (((1 << i) & saves)) __ push(Register::from_code(i));
    }
  }

  // Allocate return slots (located after callee-saved).
  if (frame()->GetReturnSlotCount() > 0) {
    __ AllocateStackSpace(frame()->GetReturnSlotCount() * kSystemPointerSize);
  }
}

void CodeGenerator::AssembleReturn(InstructionOperand* additional_pop_count) {
  auto call_descriptor = linkage()->GetIncomingDescriptor();

  const RegList saves = call_descriptor->CalleeSavedRegisters();
  // Restore registers.
  if (saves != 0) {
    const int returns = frame()->GetReturnSlotCount();
    if (returns != 0) {
      __ add(esp, Immediate(returns * kSystemPointerSize));
    }
    for (int i = 0; i < Register::kNumRegisters; i++) {
      if (!((1 << i) & saves)) continue;
      __ pop(Register::from_code(i));
    }
  }

  // We might need ecx and edx for scratch.
  DCHECK_EQ(0u, call_descriptor->CalleeSavedRegisters() & edx.bit());
  DCHECK_EQ(0u, call_descriptor->CalleeSavedRegisters() & ecx.bit());
  IA32OperandConverter g(this, nullptr);
  int parameter_count =
      static_cast<int>(call_descriptor->StackParameterCount());

  // {aditional_pop_count} is only greater than zero if {parameter_count = 0}.
  // Check RawMachineAssembler::PopAndReturn.
  if (parameter_count != 0) {
    if (additional_pop_count->IsImmediate()) {
      DCHECK_EQ(g.ToConstant(additional_pop_count).ToInt32(), 0);
    } else if (__ emit_debug_code()) {
      __ cmp(g.ToRegister(additional_pop_count), Immediate(0));
      __ Assert(equal, AbortReason::kUnexpectedAdditionalPopValue);
    }
  }

  Register argc_reg = ecx;
#ifdef V8_NO_ARGUMENTS_ADAPTOR
  // Functions with JS linkage have at least one parameter (the receiver).
  // If {parameter_count} == 0, it means it is a builtin with
  // kDontAdaptArgumentsSentinel, which takes care of JS arguments popping
  // itself.
  const bool drop_jsargs = frame_access_state()->has_frame() &&
                           call_descriptor->IsJSFunctionCall() &&
                           parameter_count != 0;
#else
  const bool drop_jsargs = false;
#endif
  if (call_descriptor->IsCFunctionCall()) {
    AssembleDeconstructFrame();
  } else if (frame_access_state()->has_frame()) {
    // Canonicalize JSFunction return sites for now if they always have the same
    // number of return args.
    if (additional_pop_count->IsImmediate() &&
        g.ToConstant(additional_pop_count).ToInt32() == 0) {
      if (return_label_.is_bound()) {
        __ jmp(&return_label_);
        return;
      } else {
        __ bind(&return_label_);
      }
    }
    if (drop_jsargs) {
      // Get the actual argument count.
      __ mov(argc_reg, Operand(ebp, StandardFrameConstants::kArgCOffset));
    }
    AssembleDeconstructFrame();
  }

  if (drop_jsargs) {
    // We must pop all arguments from the stack (including the receiver). This
    // number of arguments is given by max(1 + argc_reg, parameter_count).
    int parameter_count_without_receiver =
        parameter_count - 1;  // Exclude the receiver to simplify the
                              // computation. We'll account for it at the end.
    Label mismatch_return;
    Register scratch_reg = edx;
    DCHECK_NE(argc_reg, scratch_reg);
    __ cmp(argc_reg, Immediate(parameter_count_without_receiver));
    __ j(greater, &mismatch_return, Label::kNear);
    __ Ret(parameter_count * kSystemPointerSize, scratch_reg);
    __ bind(&mismatch_return);
    __ PopReturnAddressTo(scratch_reg);
    __ lea(esp, Operand(esp, argc_reg, times_system_pointer_size,
                        kSystemPointerSize));  // Also pop the receiver.
    // We use a return instead of a jump for better return address prediction.
    __ PushReturnAddressFrom(scratch_reg);
    __ Ret();
  } else if (additional_pop_count->IsImmediate()) {
    Register scratch_reg = ecx;
    int additional_count = g.ToConstant(additional_pop_count).ToInt32();
    size_t pop_size = (parameter_count + additional_count) * kSystemPointerSize;
    CHECK_LE(pop_size, static_cast<size_t>(std::numeric_limits<int>::max()));
    __ Ret(static_cast<int>(pop_size), scratch_reg);
  } else {
    Register pop_reg = g.ToRegister(additional_pop_count);
    Register scratch_reg = pop_reg == ecx ? edx : ecx;
    int pop_size = static_cast<int>(parameter_count * kSystemPointerSize);
    __ PopReturnAddressTo(scratch_reg);
    __ lea(esp, Operand(esp, pop_reg, times_system_pointer_size,
                        static_cast<int>(pop_size)));
    __ PushReturnAddressFrom(scratch_reg);
    __ Ret();
  }
}

void CodeGenerator::FinishCode() {}

void CodeGenerator::PrepareForDeoptimizationExits(
    ZoneDeque<DeoptimizationExit*>* exits) {}

void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  IA32OperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.
  switch (MoveType::InferMove(source, destination)) {
    case MoveType::kRegisterToRegister:
      if (source->IsRegister()) {
        __ mov(g.ToRegister(destination), g.ToRegister(source));
      } else {
        DCHECK(source->IsFPRegister());
        __ Movaps(g.ToDoubleRegister(destination), g.ToDoubleRegister(source));
      }
      return;
    case MoveType::kRegisterToStack: {
      Operand dst = g.ToOperand(destination);
      if (source->IsRegister()) {
        __ mov(dst, g.ToRegister(source));
      } else {
        DCHECK(source->IsFPRegister());
        XMMRegister src = g.ToDoubleRegister(source);
        MachineRepresentation rep =
            LocationOperand::cast(source)->representation();
        if (rep == MachineRepresentation::kFloat32) {
          __ Movss(dst, src);
        } else if (rep == MachineRepresentation::kFloat64) {
          __ Movsd(dst, src);
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, rep);
          __ Movups(dst, src);
        }
      }
      return;
    }
    case MoveType::kStackToRegister: {
      Operand src = g.ToOperand(source);
      if (source->IsStackSlot()) {
        __ mov(g.ToRegister(destination), src);
      } else {
        DCHECK(source->IsFPStackSlot());
        XMMRegister dst = g.ToDoubleRegister(destination);
        MachineRepresentation rep =
            LocationOperand::cast(source)->representation();
        if (rep == MachineRepresentation::kFloat32) {
          __ Movss(dst, src);
        } else if (rep == MachineRepresentation::kFloat64) {
          __ Movsd(dst, src);
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, rep);
          __ Movups(dst, src);
        }
      }
      return;
    }
    case MoveType::kStackToStack: {
      Operand src = g.ToOperand(source);
      Operand dst = g.ToOperand(destination);
      if (source->IsStackSlot()) {
        __ push(src);
        __ pop(dst);
      } else {
        MachineRepresentation rep =
            LocationOperand::cast(source)->representation();
        if (rep == MachineRepresentation::kFloat32) {
          __ Movss(kScratchDoubleReg, src);
          __ Movss(dst, kScratchDoubleReg);
        } else if (rep == MachineRepresentation::kFloat64) {
          __ Movsd(kScratchDoubleReg, src);
          __ Movsd(dst, kScratchDoubleReg);
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, rep);
          __ Movups(kScratchDoubleReg, src);
          __ Movups(dst, kScratchDoubleReg);
        }
      }
      return;
    }
    case MoveType::kConstantToRegister: {
      Constant src = g.ToConstant(source);
      if (destination->IsRegister()) {
        Register dst = g.ToRegister(destination);
        if (src.type() == Constant::kHeapObject) {
          __ Move(dst, src.ToHeapObject());
        } else {
          __ Move(dst, g.ToImmediate(source));
        }
      } else {
        DCHECK(destination->IsFPRegister());
        XMMRegister dst = g.ToDoubleRegister(destination);
        if (src.type() == Constant::kFloat32) {
          // TODO(turbofan): Can we do better here?
          __ Move(dst, src.ToFloat32AsInt());
        } else {
          DCHECK_EQ(src.type(), Constant::kFloat64);
          __ Move(dst, src.ToFloat64().AsUint64());
        }
      }
      return;
    }
    case MoveType::kConstantToStack: {
      Constant src = g.ToConstant(source);
      Operand dst = g.ToOperand(destination);
      if (destination->IsStackSlot()) {
        __ Move(dst, g.ToImmediate(source));
      } else {
        DCHECK(destination->IsFPStackSlot());
        if (src.type() == Constant::kFloat32) {
          __ Move(dst, Immediate(src.ToFloat32AsInt()));
        } else {
          DCHECK_EQ(src.type(), Constant::kFloat64);
          uint64_t constant_value = src.ToFloat64().AsUint64();
          uint32_t lower = static_cast<uint32_t>(constant_value);
          uint32_t upper = static_cast<uint32_t>(constant_value >> 32);
          Operand dst0 = dst;
          Operand dst1 = g.ToOperand(destination, kSystemPointerSize);
          __ Move(dst0, Immediate(lower));
          __ Move(dst1, Immediate(upper));
        }
      }
      return;
    }
  }
  UNREACHABLE();
}

void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  IA32OperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  switch (MoveType::InferSwap(source, destination)) {
    case MoveType::kRegisterToRegister: {
      if (source->IsRegister()) {
        Register src = g.ToRegister(source);
        Register dst = g.ToRegister(destination);
        __ push(src);
        __ mov(src, dst);
        __ pop(dst);
      } else {
        DCHECK(source->IsFPRegister());
        XMMRegister src = g.ToDoubleRegister(source);
        XMMRegister dst = g.ToDoubleRegister(destination);
        __ Movaps(kScratchDoubleReg, src);
        __ Movaps(src, dst);
        __ Movaps(dst, kScratchDoubleReg);
      }
      return;
    }
    case MoveType::kRegisterToStack: {
      if (source->IsRegister()) {
        Register src = g.ToRegister(source);
        __ push(src);
        frame_access_state()->IncreaseSPDelta(1);
        Operand dst = g.ToOperand(destination);
        __ mov(src, dst);
        frame_access_state()->IncreaseSPDelta(-1);
        dst = g.ToOperand(destination);
        __ pop(dst);
      } else {
        DCHECK(source->IsFPRegister());
        XMMRegister src = g.ToDoubleRegister(source);
        Operand dst = g.ToOperand(destination);
        MachineRepresentation rep =
            LocationOperand::cast(source)->representation();
        if (rep == MachineRepresentation::kFloat32) {
          __ Movss(kScratchDoubleReg, dst);
          __ Movss(dst, src);
          __ Movaps(src, kScratchDoubleReg);
        } else if (rep == MachineRepresentation::kFloat64) {
          __ Movsd(kScratchDoubleReg, dst);
          __ Movsd(dst, src);
          __ Movaps(src, kScratchDoubleReg);
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, rep);
          __ Movups(kScratchDoubleReg, dst);
          __ Movups(dst, src);
          __ Movups(src, kScratchDoubleReg);
        }
      }
      return;
    }
    case MoveType::kStackToStack: {
      if (source->IsStackSlot()) {
        Operand dst1 = g.ToOperand(destination);
        __ push(dst1);
        frame_access_state()->IncreaseSPDelta(1);
        Operand src1 = g.ToOperand(source);
        __ push(src1);
        Operand dst2 = g.ToOperand(destination);
        __ pop(dst2);
        frame_access_state()->IncreaseSPDelta(-1);
        Operand src2 = g.ToOperand(source);
        __ pop(src2);
      } else {
        DCHECK(source->IsFPStackSlot());
        Operand src0 = g.ToOperand(source);
        Operand dst0 = g.ToOperand(destination);
        MachineRepresentation rep =
            LocationOperand::cast(source)->representation();
        if (rep == MachineRepresentation::kFloat32) {
          __ Movss(kScratchDoubleReg, dst0);  // Save dst in scratch register.
          __ push(src0);  // Then use stack to copy src to destination.
          __ pop(dst0);
          __ Movss(src0, kScratchDoubleReg);
        } else if (rep == MachineRepresentation::kFloat64) {
          __ Movsd(kScratchDoubleReg, dst0);  // Save dst in scratch register.
          __ push(src0);  // Then use stack to copy src to destination.
          __ pop(dst0);
          __ push(g.ToOperand(source, kSystemPointerSize));
          __ pop(g.ToOperand(destination, kSystemPointerSize));
          __ Movsd(src0, kScratchDoubleReg);
        } else {
          DCHECK_EQ(MachineRepresentation::kSimd128, rep);
          __ Movups(kScratchDoubleReg, dst0);  // Save dst in scratch register.
          __ push(src0);  // Then use stack to copy src to destination.
          __ pop(dst0);
          __ push(g.ToOperand(source, kSystemPointerSize));
          __ pop(g.ToOperand(destination, kSystemPointerSize));
          __ push(g.ToOperand(source, 2 * kSystemPointerSize));
          __ pop(g.ToOperand(destination, 2 * kSystemPointerSize));
          __ push(g.ToOperand(source, 3 * kSystemPointerSize));
          __ pop(g.ToOperand(destination, 3 * kSystemPointerSize));
          __ Movups(src0, kScratchDoubleReg);
        }
      }
      return;
    }
    default:
      UNREACHABLE();
  }
}

void CodeGenerator::AssembleJumpTable(Label** targets, size_t target_count) {
  for (size_t index = 0; index < target_count; ++index) {
    __ dd(targets[index]);
  }
}

#undef __
#undef kScratchDoubleReg
#undef ASSEMBLE_COMPARE
#undef ASSEMBLE_IEEE754_BINOP
#undef ASSEMBLE_IEEE754_UNOP
#undef ASSEMBLE_BINOP
#undef ASSEMBLE_ATOMIC_BINOP
#undef ASSEMBLE_I64ATOMIC_BINOP
#undef ASSEMBLE_MOVX
#undef ASSEMBLE_SIMD_PUNPCK_SHUFFLE
#undef ASSEMBLE_SIMD_IMM_SHUFFLE
#undef ASSEMBLE_SIMD_ALL_TRUE
#undef ASSEMBLE_SIMD_SHIFT

}  // namespace compiler
}  // namespace internal
}  // namespace v8
