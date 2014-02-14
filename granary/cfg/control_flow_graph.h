/* Copyright 2014 Peter Goodman, all rights reserved. */

#ifndef GRANARY_CFG_CONTROL_FLOW_GRAPH_H_
#define GRANARY_CFG_CONTROL_FLOW_GRAPH_H_

#include "granary/base/base.h"
#include "granary/base/types.h"
#include "granary/cfg/iterator.h"

namespace granary {

// Forward declarations.
class BasicBlock;
class DecodedBasicBlock;
class LocalControlFlowGraph;
class BlockFactory;

// A control flow graph of basic blocks to instrument.
class LocalControlFlowGraph final {
 public:
  GRANARY_INTERNAL_DEFINITION
  inline LocalControlFlowGraph(void)
      : first_block(nullptr),
        last_block(nullptr),
        first_new_block(nullptr) {}

  // Destroy the CFG and all basic blocks in the CFG.
  ~LocalControlFlowGraph(void);

  // Return the entry basic block of this control-flow graph.
  DecodedBasicBlock *EntryBlock(void) const;

  // Returns an iterable that can be used inside of a range-based for loop. For
  // example:
  //
  //    for(auto block : cfg.Blocks())
  //      ...
  BasicBlockIterator Blocks(void) const;

  // Returns an iterable that can be used inside of a range-based for loop. For
  // example:
  //
  //    for(auto block : cfg.NewBlocks())
  //      ...
  //
  // The distinction between `Blocks` and `NewBlocks` is relevant to
  // block materialization passes, where `Blocks` is the list of all basic
  // blocks, and `NewBlocks` is the list of newly materialized basic blocks.
  BasicBlockIterator NewBlocks(void) const;

  // Add a block to the CFG. If the block has successors that haven't yet been
  // added, then add those too.
  GRANARY_INTERNAL_DEFINITION void AddBlock(BasicBlock *block);

 private:
  friend class BlockFactory;  // For `first_new_block`.

  // List of basic blocks known to this control-flow graph.
  GRANARY_INTERNAL_DEFINITION BasicBlock *first_block;
  GRANARY_INTERNAL_DEFINITION BasicBlock *last_block;
  GRANARY_INTERNAL_DEFINITION BasicBlock *first_new_block;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(LocalControlFlowGraph);
};

}  // namespace granary

#endif  // GRANARY_CFG_CONTROL_FLOW_GRAPH_H_
