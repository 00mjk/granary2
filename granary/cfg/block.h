/* Copyright 2014 Peter Goodman, all rights reserved. */

#ifndef GRANARY_CFG_BLOCK_H_
#define GRANARY_CFG_BLOCK_H_

#include "arch/base.h"

#include "granary/base/base.h"
#include "granary/base/cast.h"
#include "granary/base/list.h"
#include "granary/base/new.h"
#include "granary/base/pc.h"
#include "granary/base/type_trait.h"

#include "granary/cfg/iterator.h"

#ifdef GRANARY_INTERNAL
# include "granary/code/register.h"
#endif

namespace granary {

class Instruction;

// Forward declarations.
class Block;
class CachedBlock;
class DecodedBlock;
class BlockMetaData;
class Trace;
class ControlFlowInstruction;
class BlockFactory;
class BlockIterator;
class ReverseBlockIterator;
GRANARY_INTERNAL_DEFINITION class Fragment;

namespace detail {
enum {
  kMaxNumFuncOperands = 6
};

class SuccessorBlockIterator;

// A successor of a basic block. A successor is a pair defined as a control-flow
// instruction and the basic block that it targets.
class BlockSuccessor {
 public:
  BlockSuccessor(void) = delete;
  BlockSuccessor(const BlockSuccessor &) = default;

  // Control-flow instruction leading to the target basic block.
  //
  // `const`-qualified so that `cfi` isn't unlinked from an instruction list
  // while the successors are being iterated.
  ControlFlowInstruction * const cfi;

  // The basic block targeted by `cfi`.
  Block * const block;

 private:
  friend class Trace;
  friend class SuccessorBlockIterator;

  inline BlockSuccessor(ControlFlowInstruction *cfi_,
                             Block *block_)
      : cfi(cfi_),
        block(block_) {}

  GRANARY_DISALLOW_ASSIGN(BlockSuccessor);
};

// Iterator to find the successors of a basic block.
class SuccessorBlockIterator {
 public:
  inline SuccessorBlockIterator begin(void) const {
    return *this;
  }

  inline SuccessorBlockIterator end(void) const {
    return SuccessorBlockIterator();
  }

  inline bool operator!=(const SuccessorBlockIterator &that) const {
    return cursor != that.cursor;
  }

  BlockSuccessor operator*(void) const;
  void operator++(void);

 private:
  friend class granary::Block;
  friend class granary::DecodedBlock;

  inline SuccessorBlockIterator(void)
      : cursor(nullptr),
        next_cursor(nullptr) {}

  GRANARY_INTERNAL_DEFINITION
  explicit SuccessorBlockIterator(Instruction *instr_);

  // The next instruction that we will look at for
  GRANARY_POINTER(Instruction) *cursor;
  GRANARY_POINTER(Instruction) *next_cursor;
};

}  // namespace detail

enum {
  kBlockSuccessorFallThrough = 1,
  kBlockSuccessorBranch = 1
};

// Abstract basic block of instructions.
class Block {
 public:
  virtual ~Block(void) = default;

  GRANARY_INTERNAL_DEFINITION Block(void);

  // Find the successors of this basic block. This can be used as follows:
  //
  //    for (auto succ : block->Successors()) {
  //      succ.block
  //      succ.cti
  //    }
  //
  // Note: This method is only usefully defined for `DecodedBlock`. All
  //       other basic block types are treated as having no successors.
  virtual detail::SuccessorBlockIterator Successors(void) const;

  // Returns the starting PC of this basic block in the (native) application.
  virtual AppPC StartAppPC(void) const = 0;

  // Returns the starting PC of this basic block in the (instrumented) code
  // cache.
  virtual CachePC StartCachePC(void) const = 0;

  // Retunrs a unique ID for this basic block within the trace. This can be
  // useful for client tools to implement data flow passes.
  int Id(void) const;

  GRANARY_DECLARE_BASE_CLASS(Block)

 protected:
  template <typename> friend class ListHead;
  template <typename> friend class ListOfListHead;
  friend class BlockIterator;
  friend class ReverseBlockIterator;
  friend class ControlFlowInstruction;
  friend class Trace;  // For `list` and `id`.
  friend class BlockFactory;

  GRANARY_IF_EXTERNAL( Block(void) = delete; )

  // Connects together lists of basic blocks in the trace.
  GRANARY_INTERNAL_DEFINITION ListHead<Block> list;

  // Unique ID for this block within its local control-flow graph. Defaults to
  // `-1` if the block does not belong to an trace.
  GRANARY_INTERNAL_DEFINITION int id;

  // The generation number for where this block was materialized.
  GRANARY_INTERNAL_DEFINITION int generation;

  // Is this block reachable from the entry node of the trace?
  GRANARY_INTERNAL_DEFINITION bool is_reachable;

  // Successor blocks.
  Block *successors[2];

 GRANARY_PROTECTED:

  // The fragment associated with the entrypoint of this block.
  GRANARY_INTERNAL_DEFINITION Fragment *fragment;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(Block);
};

// An instrumented basic block, i.e. a basic block that has been instrumented,
// is in the process of being instrumented, or will (likely) be instrumented.
class InstrumentedBlock : public Block {
 public:
  GRANARY_INTERNAL_DEFINITION
  InstrumentedBlock(Trace *cfg_, BlockMetaData *meta_);

  virtual ~InstrumentedBlock(void);

  // Return this basic block's meta-data.
  virtual BlockMetaData *MetaData(void);

  // Return this basic block's meta-data.
  BlockMetaData *UnsafeMetaData(void);

  // Returns the starting PC of this basic block in the (native) application.
  virtual AppPC StartAppPC(void) const override;

  // Returns the starting PC of this basic block in the (instrumented) code
  // cache.
  virtual CachePC StartCachePC(void) const override;

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, InstrumentedBlock)

 GRANARY_PROTECTED:

  // The local control-flow graph to which this block belongs.
  GRANARY_INTERNAL_DEFINITION Trace * const cfg;

  // The meta-data associated with this basic block. Points to some (usually)
  // interned meta-data that is valid on entry to this basic block.
  GRANARY_INTERNAL_DEFINITION BlockMetaData *meta;

 private:
  friend class BlockFactory;

  InstrumentedBlock(void) = delete;

  // The starting PC of this basic block, if any.
  GRANARY_INTERNAL_DEFINITION AppPC native_pc;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(InstrumentedBlock);
};

// A basic block that has already been committed to the code cache.
class CachedBlock final : public InstrumentedBlock {
 public:
  GRANARY_INTERNAL_DEFINITION
  CachedBlock(Trace *cfg_, const BlockMetaData *meta_);

  virtual ~CachedBlock(void) = default;

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, CachedBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(CachedBlock, {
    kAlignment = 1
  })

 private:
  CachedBlock(void) = delete;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(CachedBlock);
};

// A basic block that has been decoded but not yet committed to the code cache.
class DecodedBlock : public InstrumentedBlock {
 public:
  virtual ~DecodedBlock(void);

  GRANARY_INTERNAL_DEFINITION
  DecodedBlock(Trace *cfg_, BlockMetaData *meta_);

  // Return an iterator of the successor blocks of this basic block.
  virtual detail::SuccessorBlockIterator Successors(void) const override;

  // Allocates a new temporary virtual register for use by instructions within
  // this basic block.
  VirtualRegister AllocateVirtualRegister(
      size_t num_bytes=arch::GPR_WIDTH_BYTES);

  // Allocates a new temporary virtual register for use by instructions within
  // this basic block.
  GRANARY_INTERNAL_DEFINITION
  VirtualRegister AllocateTemporaryRegister(
      size_t num_bytes=arch::GPR_WIDTH_BYTES);

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, DecodedBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(DecodedBlock, {
    kAlignment = arch::CACHE_LINE_SIZE_BYTES
  })

  // Return the first instruction in the basic block.
  Instruction *FirstInstruction(void) const;

  // Return the last instruction in the basic block.
  Instruction *LastInstruction(void) const;

  // Return an iterator for the instructions of the block.
  InstructionIterator Instructions(void) const;

  // Return a reverse iterator for the instructions of the block.
  ReverseInstructionIterator ReversedInstructions(void) const;

  // Return an iterator for the application instructions of a basic block.
  AppInstructionIterator AppInstructions(void) const;

  // Return a reverse iterator for the application instructions of the block.
  ReverseAppInstructionIterator ReversedAppInstructions(void) const;

  // Add a new instruction to the beginning of the instruction list.
  void PrependInstruction(std::unique_ptr<Instruction> instr);
  void PrependInstruction(Instruction *instr);

  // Add a new instruction to the end of the instruction list.
  void AppendInstruction(std::unique_ptr<Instruction> instr);
  void AppendInstruction(Instruction *instr);

  // Mark the code of this block as being cold.
  void MarkAsColdCode(void);

  // Is this cold code?
  bool IsColdCode(void) const;

  // Remove and return single instruction. Some special kinds of instructions
  // can't be removed.
  static std::unique_ptr<Instruction> Unlink(Instruction *instr);

  // Truncate a decoded basic block. This removes `instr` up until the end of
  // the instruction list. In some cases, certain special instructions are not
  // allowed to be truncated. This will not remove such special cases.
  static void Truncate(Instruction *instr);

  // Returns the Nth argument register for use by a lir function call.
  GRANARY_INTERNAL_DEFINITION
  VirtualRegister NthArgumentRegister(size_t arg_num) const;

 private:
  friend class BlockFactory;
  friend class Trace;

  DecodedBlock(void) = delete;

  // List of instructions in this basic block. Basic blocks have sole ownership
  // over their instructions.
  //
  // Note: These fields are marked `GRANARY_CONST`, which is only externally
  //       resolved to `cont`, despite being internal-only fields. This is to
  //       document that they are effectively `const`, but that they can indeed
  //       change (e.g. `InsertBefore` and `InsertAfter` the first/last
  //       instructions).
  GRANARY_INTERNAL_DEFINITION Instruction * GRANARY_CONST first;
  GRANARY_INTERNAL_DEFINITION Instruction * GRANARY_CONST last;

  // Does this block contain cold code?
  GRANARY_INTERNAL_DEFINITION bool is_cold_code;

  // Virtual registers used within function calls injected into a basic block.
  // We share these regs across all calls.
  GRANARY_INTERNAL_DEFINITION
  VirtualRegister arg_regs[detail::kMaxNumFuncOperands];

  GRANARY_DISALLOW_COPY_AND_ASSIGN(DecodedBlock);
};

// Represents a decoded basic block that is meant as compensation code that
// points to an existing block.
class CompensationBlock : public DecodedBlock {
 public:
  GRANARY_INTERNAL_DEFINITION
  CompensationBlock(Trace *cfg_, BlockMetaData *meta_);


  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, CompensationBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(CompensationBlock, {
    kAlignment = arch::CACHE_LINE_SIZE_BYTES
  })

 protected:
  friend class BlockFactory;

  // Should we be allowed to try to compare this block with another one?
  GRANARY_INTERNAL_DEFINITION bool is_comparable;
};

// Forward declaration.
enum BlockRequestKind : uint8_t;

// A basic block that has not yet been decoded, and might eventually be decoded.
class DirectBlock final : public InstrumentedBlock {
 public:
  virtual ~DirectBlock(void);
  GRANARY_INTERNAL_DEFINITION DirectBlock(
      Trace *cfg_, BlockMetaData *meta_);

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, DirectBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(DirectBlock, {
    kAlignment = 1
  })

 private:
  friend class BlockFactory;

  DirectBlock(void) = delete;

  // How should we materialize this block, and if what block resulted from the
  // materialization?
  GRANARY_INTERNAL_DEFINITION Block *materialized_block;
  GRANARY_INTERNAL_DEFINITION BlockRequestKind materialize_strategy;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(DirectBlock);
};

// A basic block that has not yet been decoded, and which we don't know about
// at this time because it's the target of an indirect jump/call.
class IndirectBlock final : public InstrumentedBlock {
 public:
  virtual ~IndirectBlock(void) = default;

  GRANARY_INTERNAL_DEFINITION
  using InstrumentedBlock::InstrumentedBlock;

  // Returns the starting PC of this basic block in the (native) application.
  virtual AppPC StartAppPC(void) const override final;

  // Returns the starting PC of this basic block in the (instrumented) code
  // cache.
  virtual CachePC StartCachePC(void) const override final;

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, IndirectBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(IndirectBlock, {
    kAlignment = 1
  })

 private:
  IndirectBlock(void) = delete;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(IndirectBlock);
};

// A basic block that has not yet been decoded, and which we don't know about
// at this time because it's the target of an indirect jump/call.
class ReturnBlock final : public InstrumentedBlock {
 public:
  GRANARY_INTERNAL_DEFINITION ReturnBlock(Trace *cfg_,
                                               BlockMetaData *meta_);
  virtual ~ReturnBlock(void);

  // Returns true if this return basic block has meta-data. If it has meta-data
  // then the way that the branch is resolved is slightly more complicated.
  bool UsesMetaData(void) const;

  // Return this basic block's meta-data. Accessing a return basic block's meta-
  // data will "create" it for the block.
  virtual BlockMetaData *MetaData(void) override final;

  // Returns the starting PC of this basic block in the (native) application.
  virtual AppPC StartAppPC(void) const override final;

  // Returns the starting PC of this basic block in the (instrumented) code
  // cache.
  virtual CachePC StartCachePC(void) const override final;

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, ReturnBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(ReturnBlock, {
    kAlignment = 1
  })

 private:
  ReturnBlock(void) = delete;

  // The meta-data of this block, but where we only assign the `lazy_meta` to
  // `Block::meta` when a request of `MetaData` is made. This is so that
  // the default behavior is to not propagate meta-data through function
  // returns.
  GRANARY_INTERNAL_DEFINITION BlockMetaData *lazy_meta;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(ReturnBlock);
};

// A native basic block, i.e. this points to either native code, or some stub
// code that leads to native code.
class NativeBlock final : public Block {
 public:
  virtual ~NativeBlock(void) = default;

  GRANARY_INTERNAL_DEFINITION
  explicit NativeBlock(AppPC native_pc_);

  // Returns the starting PC of this basic block in the (native) application.
  virtual AppPC StartAppPC(void) const override final;

  // Returns the starting PC of this basic block in the (instrumented) code
  // cache.
  virtual CachePC StartCachePC(void) const override final;

  GRANARY_DECLARE_DERIVED_CLASS_OF(Block, NativeBlock)
  GRANARY_DECLARE_INTERNAL_NEW_ALLOCATOR(NativeBlock, {
    kAlignment = 1
  })

 private:
  NativeBlock(void) = delete;

  GRANARY_INTERNAL_DEFINITION const AppPC native_pc;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(NativeBlock);
};

}  // namespace granary

#endif  // GRANARY_CFG_BLOCK_H_
