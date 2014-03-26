/* Copyright 2014 Peter Goodman, all rights reserved. */

#ifndef GRANARY_CODE_ASSEMBLE_FRAGMENT_H_
#define GRANARY_CODE_ASSEMBLE_FRAGMENT_H_

#ifndef GRANARY_INTERNAL
# error "This code is internal to Granary."
#endif

#include "granary/arch/base.h"

#include "granary/base/bitset.h"
#include "granary/base/list.h"
#include "granary/base/new.h"

#include "granary/code/register.h"

namespace granary {

// Forward declarations.
class Instruction;
class LabelInstruction;
class NativeInstruction;
class DecodedBasicBlock;
class LocalControlFlowGraph;
class BlockMetaData;
class FragmentBuilder;

// Represents a basic block in the true sense. Granary basic blocks can contain
// local control flow, so they need to be split into fragments of instructions
// that more closely represent the actual run-time control flow. This lower
// level model is needed for register allocation, etc.
class Fragment {
 public:

  // Next fragment in the fragment list. This is always associated with an
  // implicit control-flow instruction between two fragments.
  Fragment *fall_through_target;

  // Conditional branch target. This always associated with an explicit
  // control-flow instruction between two fragments.
  Fragment *branch_target;
  NativeInstruction *branch_instr;

  // All fragments are chained together into a list for simple iteration,
  // freeing, etc.
  Fragment *next;

  // Unique ID of this fragment.
  const int id;

  // Is this block the first fragment in a decoded basic block?
  bool is_decoded_block_head:1;

  // Is this a future basic block?
  bool is_future_block_head:1;

  // Is this an exit block? An exit block is a future block, or a block that
  // ends in some kind of return, or a native block.
  bool is_exit:1;

  // Does the last instruction in this fragment change the stack pointer? If so,
  // the we consider the stack to be valid in this fragment if the stack pointer
  // is also read during the operation. Otherwise, it's treated as a strict
  // stack switch, where the stack might not be valid.
  bool writes_to_stack_pointer:1;
  bool reads_from_stack_pointer:1;

  // Identifier of a "stack region". This is a very coarse grained concept,
  // where we color fragments according to:
  //    -N:   The stack pointer doesn't point to a valid stack.
  //    0:    We don't know yet.
  //    N:    The stack pointer points to some valid stack.
  //
  // The numbering partitions fragments into two coarse grained groups:
  // invalid code execution on an unsafe stack (negative id), or code executing
  // on a safe stack (positive id). The numbering sub-divides fragments into
  // finer-grained colors based, where two or more fragments have the same
  // color if they are connected through control flow, and if there are no
  // changes to the stack pointer within the basic blocks.
  int partition_id;

  // Source basic block info.
  BlockMetaData *block_meta;

  // Instruction list.
  Instruction *first;
  Instruction *last;

  // Which physical registers are live on entry to and exit from this block.
  RegisterUsageTracker entry_regs_live;

  // Live registers on exit, but where we're being conservative about the set
  // of live registers (i.e. if a register is live in any successor, then treat
  // it as live).
  RegisterUsageTracker exit_regs_live;

  // Live registers on exit, but where we're being conservative about the set
  // of dead registers (i.e. if a register is dead in any successor, then treat
  // it as dead).
  RegisterUsageTracker exit_regs_dead;

 private:
  friend class FragmentBuilder;

  Fragment(void) = delete;

  // Initialize the fragment from a basic block.
  explicit Fragment(int id_);

  GRANARY_DEFINE_NEW_ALLOCATOR(Fragment, {
    SHARED = true,
    ALIGNMENT = 1
  })

  // Append an instruction into the fragment.
  void AppendInstruction(std::unique_ptr<Instruction> instr);

  // Remove an instruction.
  std::unique_ptr<Instruction> RemoveInstruction(Instruction *instr);
};

typedef LinkedListIterator<Fragment> FragmentIterator;

}  // namespace granary

#endif  // GRANARY_CODE_ASSEMBLE_FRAGMENT_H_
