/* Copyright 2014 Peter Goodman, all rights reserved. */

#include <granary.h>

GRANARY_USING_NAMESPACE granary;

// Simple tool decoding all blocks in a function.
class WholeFunctionDecoder : public InstrumentationTool {
 public:
  virtual ~WholeFunctionDecoder(void) = default;
  virtual void InstrumentControlFlow(BlockFactory *factory,
                                     Trace *cfg) {
    for (auto block : cfg->NewBlocks()) {
      for (auto succ : block->Successors()) {
        if (succ.cfi->IsSystemCall() || succ.cfi->IsInterruptCall()) {
          break;  // System calls don't always return to the next instruction.
        } else if (!succ.cfi->IsFunctionCall()) {
          factory->RequestBlock(succ.block);
        }
      }
    }
  }
};

// Initialize the `whole_func` tool.
GRANARY_ON_CLIENT_INIT() {
  AddInstrumentationTool<WholeFunctionDecoder>("whole_func");
}
