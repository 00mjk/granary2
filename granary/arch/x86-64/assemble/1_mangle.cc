/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL
#define GRANARY_ARCH_INTERNAL

#include "granary/arch/util.h"

#include "granary/base/base.h"
#include "granary/base/new.h"

#include "granary/cfg/basic_block.h"
#include "granary/cfg/instruction.h"

#include "granary/arch/x86-64/builder.h"
#include "granary/arch/x86-64/xed.h"

#include "granary/cache.h"
#include "granary/breakpoint.h"

namespace granary {
namespace arch {
namespace {

// Instruction iclass reversers for conditional branches, indexed by
// `instr->iclass - XED_ICLASS_JB`.
static const xed_iclass_enum_t kReversedConditionalCFIs[] = {
  XED_ICLASS_JNB,
  XED_ICLASS_JNBE,
  XED_ICLASS_JNL,
  XED_ICLASS_JNLE,
  XED_ICLASS_INVALID,
  XED_ICLASS_INVALID,
  XED_ICLASS_JB,
  XED_ICLASS_JBE,
  XED_ICLASS_JL,
  XED_ICLASS_JLE,
  XED_ICLASS_JO,
  XED_ICLASS_JP,
  XED_ICLASS_JS,
  XED_ICLASS_JZ,
  XED_ICLASS_JNO,
  XED_ICLASS_JNP,
  XED_ICLASS_INVALID,
  XED_ICLASS_JNS,
  XED_ICLASS_JNZ,
};

// Instruction builders for conditional branches, indexed by
// `instr->iclass - XED_ICLASS_JB`.
typedef void (CFIBuilder)(Instruction *, PC);
static CFIBuilder * const kConditionalCFIBuilders[] = {
  JB_RELBRd<PC>,
  JBE_RELBRd<PC>,
  JL_RELBRd<PC>,
  JLE_RELBRd<PC>,
  nullptr,
  nullptr,
  JNB_RELBRd<PC>,
  JNBE_RELBRd<PC>,
  JNL_RELBRd<PC>,
  JNLE_RELBRd<PC>,
  JNO_RELBRd<PC>,
  JNP_RELBRd<PC>,
  JNS_RELBRd<PC>,
  JNZ_RELBRd<PC>,
  JO_RELBRd<PC>,
  JP_RELBRd<PC>,
  nullptr,
  JS_RELBRd<PC>,
  JZ_RELBRd<PC>
};

// Inserts a `UD2` instruction after a CFI. The idea here is that if we're
// mangling jumps to native code, then we don't want the (first) predicted
// target of the indirect jump to be the next instruction, so we hint at the
// processor to stop prefetching via the `UD2`.
static void InsertUD2AfterCFI(ControlFlowInstruction *cfi) {
  Instruction ni;
  UD2(&ni);
  cfi->UnsafeInsertAfter(new NativeInstruction(&ni));
}

// Relativize a conditional branch by turning it into an indirect jump through
// a `NativeAddress`, then add instructions around the new indirect jump to
// jump around the indirect jump when the original condition is not satisfied.
static void RelativizeConditionalBranch(CacheMetaData *meta,
                                        ControlFlowInstruction *cfi,
                                        Instruction *instr,
                                        PC target_pc) {
  auto iclass = kReversedConditionalCFIs[instr->iclass - XED_ICLASS_JB];
  auto iclass_builder = kConditionalCFIBuilders[iclass - XED_ICLASS_JB];

  Instruction neg_bri;
  iclass_builder(&neg_bri, static_cast<PC>(nullptr));

  auto label = new LabelInstruction;
  auto neg_br = new BranchInstruction(&neg_bri, label);

  instr->iclass = XED_ICLASS_JMP;
  instr->category = XED_CATEGORY_UNCOND_BR;

  // Have a negated conditional branch jump around the old conditional branch.
  cfi->UnsafeInsertBefore(neg_br);
  cfi->UnsafeInsertAfter(label);

  // Overwrite the conditional branch with an indirect JMP.
  auto addr_mloc = new NativeAddress(target_pc, meta->native_addresses);
  JMP_MEMv(instr, &(addr_mloc->addr));
  instr->is_sticky = true;

  InsertUD2AfterCFI(cfi);
}

// Returns `true` if this is an looping instruction.
static bool IsLoopInstruction(xed_iclass_enum_t iclass) {
  return XED_ICLASS_JRCXZ == iclass ||
         (XED_ICLASS_LOOP <= iclass && XED_ICLASS_LOOPNE >= iclass);
}

// Relativize a loop instruction. This turns an instruction like `jecxz <foo>`
// or `loop <foo>` into:
//                    jmp   <try_loop>
//        do_loop:    jmp   <foo>
//        try_loop:   loop  <do_loop>
static void RelativizeLoop(CacheMetaData *meta, ControlFlowInstruction *cfi,
                           Instruction *instr,
                           PC target_pc, bool target_is_far_away) {
  Instruction jmp_try_loop;
  Instruction loop_do_loop;

  memcpy(&loop_do_loop, instr, sizeof loop_do_loop);

  JMP_RELBRz<PC>(&jmp_try_loop, nullptr);
  if (target_is_far_away) {
    auto addr_mloc = new NativeAddress(target_pc, meta->native_addresses);
    JMP_MEMv(instr, &(addr_mloc->addr));
    instr->is_sticky = true;

    InsertUD2AfterCFI(cfi);
  } else {
    JMP_RELBRd<PC>(instr, target_pc);
  }

  auto do_loop = new LabelInstruction;
  auto try_loop = new LabelInstruction;

  loop_do_loop.SetBranchTarget(do_loop);

  cfi->UnsafeInsertBefore(new BranchInstruction(&jmp_try_loop, try_loop));
  cfi->UnsafeInsertBefore(do_loop);

  cfi->UnsafeInsertAfter(new BranchInstruction(&loop_do_loop, do_loop));
  cfi->UnsafeInsertAfter(try_loop);
}

}  // namespace

// Relativize a direct control-flow instruction.
void RelativizeDirectCFI(CacheMetaData *meta, ControlFlowInstruction *cfi,
                         Instruction *instr, PC target_pc,
                         bool target_is_far_away) {
  auto iclass = instr->iclass;
  if (XED_ICLASS_CALL_NEAR == iclass) {
    if (target_is_far_away) {
      auto addr_mloc = new NativeAddress(target_pc, meta->native_addresses);
      CALL_NEAR_MEMv(instr, &(addr_mloc->addr));
      instr->is_sticky = true;
    }
  } else if (XED_ICLASS_JMP == iclass) {
    if (target_is_far_away) {
      auto addr_mloc = new NativeAddress(target_pc, meta->native_addresses);
      JMP_MEMv(instr, &(addr_mloc->addr));
      instr->is_sticky = true;

      InsertUD2AfterCFI(cfi);
    }

  // Always need to mangle this.
  } else if (IsLoopInstruction(iclass)) {
    RelativizeLoop(meta, cfi, instr, target_pc, target_is_far_away);

  // Conditional jumps. We translate these by converting them into a negated
  // conditional jump around an indirect jump to the far-away instruction.
  } else if (instr->IsConditionalJump()) {
    if (target_is_far_away) {
      RelativizeConditionalBranch(meta, cfi, instr, target_pc);
    }
  } else {
    GRANARY_ASSERT(false);
  }
}

// Mangle a specialized indirect return into an indirect jump.
static void MangleIndirectReturn(DecodedBasicBlock *block,
                                 ControlFlowInstruction *cfi) {
  auto target = block->AllocateVirtualRegister();
  Instruction ni;

  auto shift = cfi->instruction.StackPointerShiftAmount();
  if (ADDRESS_WIDTH_BYTES == shift) {
    POP_GPRv_51(&ni, target);
  } else {
    MOV_GPRv_MEMv(&ni, target, BaseDispMemOp(0, XED_REG_RSP,
                                             ADDRESS_WIDTH_BITS));
    cfi->UnsafeInsertBefore(new NativeInstruction(&ni));
    LEA_GPRv_AGEN(&ni, XED_REG_RSP, BaseDispMemOp(shift, XED_REG_RSP,
                                                  ADDRESS_WIDTH_BITS));
  }
  cfi->UnsafeInsertBefore(new NativeInstruction(&ni));

  // Convert the `RET_NEAR` into an indirect jump.
  JMP_GPRv(&(cfi->instruction), target);
}

// Mangle an indirect function call.
static void MangleIndirectCall(DecodedBasicBlock *block,
                               ControlFlowInstruction *cfi,
                               AnnotationInstruction *ret_address) {
  Instruction ni;
  Operand op;

  auto ret_address_reg = block->AllocateVirtualRegister();
  auto decoded_pc = cfi->instruction.decoded_pc;
  op.type = XED_ENCODER_OPERAND_TYPE_PTR;
  op.is_effective_address = true;
  op.is_annot_encoded_pc = true;
  op.ret_address = ret_address;
  op.width = ADDRESS_WIDTH_BITS;
  LEA_GPRv_AGEN(&ni, ret_address_reg, op);
  cfi->UnsafeInsertBefore(new NativeInstruction(&ni));
  PUSH_GPRv_50(&ni, ret_address_reg);
  ni.decoded_pc = decoded_pc;  // Mark as application.
  ni.effective_operand_width = ADDRESS_WIDTH_BITS;
  ni.AnalyzeStackUsage();
  cfi->UnsafeInsertBefore(new NativeInstruction(&ni));
}

// Performs mangling of an indirect CFI instruction. This ensures that the
// target of any specialized indirect CFI instruction is stored in a register.
void MangleIndirectCFI(DecodedBasicBlock *block, ControlFlowInstruction *cfi,
                       AnnotationInstruction *ret_address) {
  if (cfi->IsFunctionReturn()) {
    auto target_block = cfi->TargetBlock();
    if (auto return_block = DynamicCast<ReturnBasicBlock *>(target_block)) {
      if (return_block->UnsafeMetaData()) {
        MangleIndirectReturn(block, cfi);
      }
    }
    return;

  } else if (cfi->IsFunctionCall()) {
    Instruction ni;
    auto &orig_target_op(cfi->instruction.ops[0]);
    MangleIndirectCall(block, cfi, ret_address);
    if (orig_target_op.IsMemory()) {
      auto new_target_reg = block->AllocateVirtualRegister();
      MOV_GPRv_MEMv(&ni, new_target_reg, orig_target_op);
      cfi->UnsafeInsertBefore(new NativeInstruction(&ni));
      CALL_NEAR_GPRv(&(cfi->instruction), new_target_reg);
    }

  } else if (cfi->IsUnconditionalJump()) {
    Instruction ni;
    auto &orig_target_op(cfi->instruction.ops[0]);
    if (orig_target_op.IsMemory()) {
      auto new_target_reg = block->AllocateVirtualRegister();
      MOV_GPRv_MEMv(&ni, new_target_reg, orig_target_op);
      cfi->UnsafeInsertBefore(new NativeInstruction(&ni));
      JMP_GPRv(&(cfi->instruction), new_target_reg);
    }
  } else {
    // System call/return, interrupt call/return.
  }

  // Note: The final mangling of indirect calls and indirect jumps happens in
  //       `9_allocate_slots.cc` in the function `RemoveIndirectCallsAndJumps`.
}

// Performs mangling of an direct CFI instruction.
//
// Note: This has an architecture-specific implementation.
void MangleDirectCFI(DecodedBasicBlock *, ControlFlowInstruction *cfi,
                     AppPC target_pc) {
  auto &instr(cfi->instruction);
  if (IsLoopInstruction(instr.iclass)) {
    RelativizeLoop(nullptr, cfi, &instr, target_pc, false);
  }
}

// Returns true if an address needs to be relativized.
bool AddressNeedsRelativizing(const void *ptr) {
  return 32 < ImmediateWidthBits(reinterpret_cast<uintptr_t>(ptr));
}

// Relativize a instruction with a memory operand, where the operand loads some
// value from `mem_addr`
void RelativizeMemOp(DecodedBasicBlock *block, NativeInstruction *ninstr,
                     const MemoryOperand &mloc, const void *mem_addr) {
  auto op = mloc.UnsafeExtract();
  if (XED_REG_DS != op->segment && XED_REG_INVALID != op->segment) return;

  Instruction ni;
  auto addr_reg = block->AllocateVirtualRegister(ADDRESS_WIDTH_BYTES);
  MOV_GPRv_IMMv(&ni, addr_reg, reinterpret_cast<uintptr_t>(mem_addr));
  ni.effective_operand_width = ADDRESS_WIDTH_BITS;
  ninstr->UnsafeInsertBefore(new NativeInstruction(&ni));

  GRANARY_ASSERT(!op->is_sticky && op->is_explicit && !op->is_compound);
  op->type = XED_ENCODER_OPERAND_TYPE_MEM;
  op->reg = addr_reg;
}

}  // namespace arch
}  // namespace granary
