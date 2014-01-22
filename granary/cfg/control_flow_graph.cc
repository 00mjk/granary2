/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL

#include "granary/arch/base.h"
#include "granary/base/list.h"
#include "granary/base/new.h"
#include "granary/base/types.h"
#include "granary/cfg/control_flow_graph.h"
#include "granary/cfg/basic_block.h"
#include "granary/cfg/instruction.h"
#include "granary/driver.h"
#include "granary/environment.h"
#include "granary/mir.h"

namespace granary {
namespace detail {

// Defines a list of a basic blocks within a control-flow graph.
class BasicBlockList {
 public:
  ListHead list;
  std::unique_ptr<BasicBlock> block;

  inline explicit BasicBlockList(BasicBlock *block_)
      : block(block_) {}

  ~BasicBlockList(void) = default;

  // Basic block lists are allocated from a global memory pool using the
  // `new` and `delete` operators.
  GRANARY_DEFINE_NEW_ALLOCATOR(BasicBlockList, {
    SHARED = true,
    ALIGNMENT = 16
  })

 private:
  GRANARY_DISALLOW_COPY_AND_ASSIGN(BasicBlockList);
};

// Move the iterator to the next basic block.
void BasicBlockIterator::operator++(void) {
  BasicBlockList *next(blocks->list.GetNext(blocks));
  do {
    blocks = next;
    next = nullptr;

    // Auto-clean up blocks while iterating over them.
    if (GRANARY_UNLIKELY(blocks->block->CanRelease())) {
      next = blocks->list.GetNext(blocks);
      blocks->list.Unlink();
      delete blocks;
    }
  } while (GRANARY_UNLIKELY(nullptr != next));
}

// Get a basic block out of the iterator.
BasicBlock *BasicBlockIterator::operator*(void) const {
  return blocks->block.get();
}

}  // namespace detail

ControlFlowGraph::ControlFlowGraph(Environment *environment_,
                                   AppProgramCounter pc,
                                   BasicBlockMetaData *meta)
      : environment(environment_),
        blocks(new detail::BasicBlockList(new InFlightBasicBlock(pc, meta))) {

}

// Destroy the CFG.
ControlFlowGraph::~ControlFlowGraph(void) {
  for (detail::BasicBlockList *curr(blocks), *next(nullptr); curr; curr = next) {
    next = curr->list.GetNext(curr);
    delete curr;
  }
  blocks = nullptr;
}

void ControlFlowGraph::Materialize(const detail::BasicBlockSuccessor &target,
                                   const BasicBlockMetaData *meta) {
  GRANARY_UNUSED(target);
  GRANARY_UNUSED(meta);
}

void ControlFlowGraph::Materialize(const ControlFlowInstruction *instruction,
                                   const BasicBlockMetaData *meta) {
  GRANARY_UNUSED(instruction);
  GRANARY_UNUSED(meta);
}

namespace {

// A basic block that has not yet been decoded, and which we don't know about
// at this time because it's the target of an indirect jump/call.
static UnknownBasicBlock UNKNOWN_BLOCK;

// Return a control-flow instruction that targets a future basic block.
static Instruction *MakeDirectCTI(driver::DecodedInstruction *instr,
                                  AppProgramCounter target) {
  return new ControlFlowInstruction(
      instr, new FutureBasicBlock(target, nullptr));
}

// Convert a decoded instruction into the internal Granary instruction IR.
static Instruction *RaiseInstruction(
    driver::DecodedInstruction *instr) {
  if (instr->IsConditionalJump()) {
    return MakeDirectCTI(instr, instr->BranchTarget());

  } else if (instr->HasIndirectTarget()) {
    return new ControlFlowInstruction(instr, &UNKNOWN_BLOCK);

  } else if (instr->IsFunctionCall() || instr->IsJump()) {
    return MakeDirectCTI(instr, instr->BranchTarget());

  } else {
    return new NativeInstruction(instr);
  }
}

// Decode the list of instructions and appends them to the first instruction in
// a basic block. The last decoded instruction is returned.
static ControlFlowInstruction *DecodeInstructionList(
    Instruction *instr, AppProgramCounter *pc) {
  driver::InstructionDecoder decoder;
  driver::DecodedInstruction dinstr;

  for (AppProgramCounter decoded_pc(*pc);
       !IsA<ControlFlowInstruction *>(instr) && decoder.DecodeNext(&dinstr, pc);
       decoded_pc = *pc) {
    instr = instr->InsertAfter(
        std::unique_ptr<Instruction>(RaiseInstruction(dinstr.Copy())));
  }
  return DynamicCast<ControlFlowInstruction *>(instr);
}

#if 0

// Decode and return an in-flight basic block.
static std::unique_ptr<Instruction> DecodeInstructionList(
    AppProgramCounter start_pc) {

  driver::InstructionDecoder decoder;
  driver::DecodedInstruction instr;



  Instruction *curr_instr(block->first);
  Instruction *next_instr(nullptr);

  for (AppProgramCounter next_pc(block->app_start_pc), decoded_pc(next_pc);
       decoder.DecodeNext(&instr, &next_pc);
       decoded_pc = next_pc, curr_instr = next_instr) {

    // Decode and annotate the instruction.
    curr_instr = RaiseInstruction(instr.Copy());
    next_instr = cfg->environment->AnnotateInstruction(curr_instr);

    if (IsA<ControlFlowInstruction *>(curr_instr)) {
      break;
    }

    // Add a synthesized jump to the next basic block.
    const auto cti = DynamicCast<ControlFlowInstruction *>(curr_instr);
    if (cti) {
      if (cti->IsConditionalJump() || cti->IsFunctionCall()) {
        next_instr->InsertAfter(mir::Jump(cfg, next_pc));
      }
      break;
    }
  }
}
#endif
}  // namespace

// Materialize an in-flight basic block by decoding native instructions,
// annotating those instructions, and updating the CFG with new successor
// basic blocks.
void ControlFlowGraph::Materialize(InFlightBasicBlock *block,
                                   detail::BasicBlockList *block_list) {
  auto pc = block->app_start_pc;
  auto cti = DecodeInstructionList(block->FirstInstruction(), &pc);
  if (cti && (cti->IsFunctionCall() || cti->IsConditionalJump())) {
    cti->InsertAfter(mir::Jump(this, pc));
  }

  for (auto instr : block->Instructions()) {
    environment->AnnotateInstruction(instr);
    cti = DynamicCast<ControlFlowInstruction *>(instr);

    if (!cti) {
      continue;
    }

    // Add the nodes into the control-flow graph in pre-order.
    auto next_block_list = new detail::BasicBlockList(cti->target);
    block_list->list.SetNext(block_list, next_block_list);
    block_list = next_block_list;
  }
}

}  // namespace granary
