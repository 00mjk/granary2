/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL
#define GRANARY_ARCH_INTERNAL

#include "granary/base/base.h"
#include "granary/base/bitset.h"
#include "granary/base/list.h"
#include "granary/base/new.h"

#include "granary/cfg/instruction.h"
#include "granary/cfg/iterator.h"

#include "granary/code/assemble/fragment.h"
#include "granary/code/assemble/ssa.h"

#include "granary/util.h"

namespace granary {

// Returns true if this instruction is a copy instruction.
//
// Note: This has an architecture-specific implementation.
extern bool IsCopyInstruction(const NativeInstruction *instr);

// Speculates on whether or not a particular instruction selection exists for
// some set of explicit operands. Returns true if we thing the selection does
// exist.
//
// Note: This has an architecture-specific implementation.
bool TryReplaceOperand(const NativeInstruction *ninstr,
                       const Operand *op, Operand *repl_op);

// Create an instruction to copy a GPR to a spill slot.
//
// Note: This has an architecture-specific implementation.
std::unique_ptr<Instruction> SaveGPRToSlot(VirtualRegister gpr,
                                           VirtualRegister slot);

// Create an instruction to copy the value of a spill slot to a GPR.
//
// Note: This has an architecture-specific implementation.
std::unique_ptr<Instruction> RestoreGPRFromSlot(VirtualRegister gpr,
                                                VirtualRegister slot);

// Represents a storage location for a virtual register. Uses the union-find
// algorithm to settle on a canonical storage location for all virtual registers
// used/defined within a given register web.
class RegisterLocation {
 public:
  inline explicit RegisterLocation(RegisterLocation *next_)
      : next(next_),
        spill_slot(-1),
        reg(),
        parent(this),
        rank(0) {}

  // Find the canonical storage location associated with the current storage
  // location.
  RegisterLocation *Find(void) {
    if (parent != this) {
      parent = parent->Find();
    }
    return parent;
  }

  // Union together two storage locations.
  void UnionWith(RegisterLocation *that) {
    auto this_root = Find();
    auto that_root = that->Find();
    if (this_root != that_root) {
      if (this_root->rank < that_root->rank) {
        this_root->parent = that_root;
      } else if (this_root->rank > that_root->rank) {
        that_root->parent = this_root;
      } else {
        that_root->parent = this_root;
        this_root->rank = that_root->rank + 1;
      }
    }
  }

  GRANARY_DEFINE_NEW_ALLOCATOR(RegisterLocation, {
    SHARED = false,
    ALIGNMENT = 1
  })

  // All storage locations are chained together into a simple linked list for
  // later memory reclamation.
  RegisterLocation *next;

  // To what spill slot is this storage location assigned?
  int spill_slot;

  VirtualRegister reg;

 private:
  RegisterLocation(void) = delete;

  RegisterLocation *parent;
  int rank;

  GRANARY_DISALLOW_COPY_AND_ASSIGN(RegisterLocation);
};

namespace {

// Returns the Nth architectural virtual register.
static VirtualRegister NthArchGPR(int n) {
  return VirtualRegister(VR_KIND_ARCH_VIRTUAL, arch::GPR_WIDTH_BYTES,
                         static_cast<uint16_t>(n));
}

// Locations of where GPRs are currently stored.
struct GPRLocations {
  GPRLocations(void) {
    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      location[i].reg = NthArchGPR(i);
      location[i].stolen_by = nullptr;
      location[i].is_live = true;  // TODO(pag): Integrate liveness info.
    }
  }

  struct {
    // Current location of this native GPR. This will either be the GPR itself
    // or a spill slot VR.
    VirtualRegister reg;

    // The storage location associated with the virtual register web that stole
    // the GPR (assuming it was indeed stolen).
    RegisterLocation *stolen_by;

    // Whether or not this GPR is live at the current instruction.
    bool is_live;
  } location[arch::NUM_GENERAL_PURPOSE_REGISTERS];
};

// Tracks whether or not an instruction using a specific architectural GPR.
typedef BitSet<arch::NUM_GENERAL_PURPOSE_REGISTERS> RegUseTracker;

// Returns the current storage location for an SSAVariable.
inline RegisterLocation *LocationOf(SSAVariable *var) {
  return var->loc->Find();
}

// Assign a storage location to a variable.
static RegisterLocation *AssignRegisterLocation(SSAVariable *var,
                                              RegisterLocation *next) {
  auto def = DefinitionOf(var);
  if (!def->loc) {
    next = new RegisterLocation(next);
    def->loc = next;
  }
  if (var != def) {
    var->loc = def->loc;
  }
  return next;
}

// Apply a function to each SSA variable defined by an instruction.
static void ForEachDefinitionImpl(NativeInstruction *instr,
                                  std::function<void(SSAVariable *)> &func) {
  if (auto var = GetMetaData<SSAVariable *>(instr)) {
    while (auto forward_def = DynamicCast<SSAForward *>(var)) {
      func(var);
      var = forward_def->next_instr_def;
    }
    if (var) {
      func(var);
    }
  }
}

// Apply a function to each SSA variable defined by an instruction.
template <typename T>
static inline void ForEachDefinition(NativeInstruction *instr, T clos) {
  std::function<void(SSAVariable *)> func(std::cref(clos));
  ForEachDefinitionImpl(instr, func);
}

// Assign storage locations to all variables defined by this instruction.
static RegisterLocation *AssignInstrDefRegisterLocations(NativeInstruction *instr,
                                                       RegisterLocation *next) {
  ForEachDefinition(instr, [&] (SSAVariable *var) {
    next = AssignRegisterLocation(var, next);
  });
  return next;
}

// Assign a storage location to every PHI node, then try to assign and union
// with the storage locations of all operands of the PHI nodes.
static RegisterLocation *AssignPhiRegisterLocations(Fragment * const frag,
                                                  RegisterLocation *next) {
  for (auto entry_def : frag->ssa_vars->EntryDefs()) {
    if (auto var = DefinitionOf(entry_def.var)) {
      next = AssignRegisterLocation(var, next);
      auto loc = var->loc;
      if (auto phi = DynamicCast<SSAPhi *>(var)) {
        for (auto op : phi->Operands()) {
          auto op_var = op->Variable();
          next = AssignRegisterLocation(op_var, next);
          loc->UnionWith(op_var->loc);
        }
      }
    }
  }
  return next;
}

// Assign storage locations to all variables.
static RegisterLocation *AssignRegisterLocations(Fragment * const frags) {
  RegisterLocation *next(nullptr);
  // Assign a storage location to every "bare" definition.
  for (auto frag : FragmentIterator(frags)) {
    for (auto instr : ForwardInstructionIterator(frag->first)) {
      if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
        next = AssignInstrDefRegisterLocations(ninstr, next);
      }
    }
  }
  for (auto frag : FragmentIterator(frags)) {
    if (frag->ssa_vars) {
      next = AssignPhiRegisterLocations(frag, next);
    }
  }
  return next;
}

// Find the local definition of a particular register by scanning the
// instruction list of a fragment in reverse order, starting at
// `instr_using_reg`.
static SSAVariable *FindLocalInstrDefinitionOf(Instruction *instr_using_reg,
                                               VirtualRegister reg) {
  SSAVariable *def(nullptr);
  for (auto instr : BackwardInstructionIterator(instr_using_reg)) {
    if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
      if ((def = DefinitionOf(ninstr, reg))) {
        return def;
      }
    }
  }
  return nullptr;
}

// Returns true if the definition `def` of the register `def_reg` within the
// fragment `frag` escapes the fragment.
static bool DefinitionEscapesFragment(Fragment *frag, SSAVariable *def,
                                      VirtualRegister def_reg) {
  if (auto exit_def = frag->ssa_vars->ExitDefinitionOf(def_reg)) {
    return LocationOf(def) == LocationOf(exit_def);  // Escapes.
  }
  return false;
}

// Find the definitions of the registers used by a particular instruction.
static SSAVariable *LocalDefinitionOf(Fragment *frag,
                                      Instruction *instr_using_reg,
                                      VirtualRegister reg) {
  // If we've got a definition, then make sure that definition doesn't escape.
  if (auto def = FindLocalInstrDefinitionOf(instr_using_reg, reg)) {
    if (!DefinitionEscapesFragment(frag, def, reg)) {
      return def;  // No exit definition, or different exit definition.
    }
  }
  return nullptr;
}

// Returns the virtual spill slot register associated with a storage location
// for a register, regardless of if the backing storage of the location is
// a spill slot or a native GPR.
static VirtualRegister VirtualSpillOf(RegisterLocation *loc) {
  GRANARY_ASSERT(-1 != loc->spill_slot);
  return VirtualRegister(VR_KIND_VIRTUAL_SLOT, arch::GPR_WIDTH_BYTES,
                         static_cast<uint16_t>(loc->spill_slot));
}

// Allocate a spill slot from this fragment for a fragment-local virtual
// register.
static VirtualRegister AllocateVirtualSlot(Fragment *frag,
                                           RegisterLocation *loc) {
  // Note: `spill_slot_allocated_mask` is `uint32_t`, hence the maximum of 32
  //       simultaneously live fragment-local registers.
  for (uint32_t i(0); i < 32; ++i) {
    const auto mask = 1U << i;
    if (!(frag->spill_slot_allocated_mask & mask)) {
      frag->spill_slot_allocated_mask |= mask;
      frag->num_allocated_spill_slots = std::max(
          i, frag->num_allocated_spill_slots);
      loc->spill_slot = static_cast<int>(i);
      loc->reg = VirtualSpillOf(loc);
      return loc->reg;
    }
  }
  GRANARY_ASSERT(false);
  return VirtualRegister();
}

// Get the backing storage location for a given fragment-local virtual register.
static VirtualRegister BackingStorageOf(Fragment *frag, RegisterLocation *loc) {
  if (!loc->reg.IsValid()) {
    loc->reg = AllocateVirtualSlot(frag, loc);
  }
  return loc->reg;
}

// Ensure that a GPR is homed for its use in `using_instr`. If the GPR is not
// homed, then it's located in a spill slot. What we mean by it's located in
// a spill slot is that we assume that it's located there, but nothing has
// actually placed it there. Therefore, the act of homing a register actually
// involves copying the register to the spill slot after `using_instr`, so that
// later code that restores the register from a spill slot is restoring the
// right value. This might seem funny, but recall that we are visiting
// instructions in reverse order. Read the following diagram from bottom-to-top:
//
//          using_instr <-- instruction using `gpr`
//          spill = gpr <-- what we want to inject, it steals `gpr`.
//          ...
//          X           <-- instruction using VR, where VR is stored in `gpr`.
//          gpr = spill <-- restore of `gpr`, which can now be assumed stolen.
static void FillGPRForDefUse(Fragment *frag, Instruction *using_instr,
                             GPRLocations *gpr_locs, VirtualRegister gpr,
                             bool is_live) {
  if (gpr.IsNative() && gpr.IsGeneralPurpose()) {
    auto &gpr_loc(gpr_locs->location[gpr.Number()]);
    if (!gpr_loc.stolen_by || !gpr_loc.is_live) {
      GRANARY_ASSERT(gpr_loc.reg == gpr);
      return;  // It's homed, or it wasn't live after this instruction.
    }
    frag->InsertAfter(using_instr, SaveGPRToSlot(gpr, gpr_loc.reg));
    gpr_loc.reg = gpr;
    gpr_loc.stolen_by->reg = VirtualSpillOf(gpr_loc.stolen_by);
    gpr_loc.stolen_by = nullptr;
    gpr_loc.is_live = is_live;
  }
}

// Ensure that a GPR is homed for its use in `using_instr`.
static void FillGPRForUse(Fragment *frag, Instruction *using_instr,
                          GPRLocations *gpr_locs, VirtualRegister gpr) {
  FillGPRForDefUse(frag, using_instr, gpr_locs, gpr, true);
}

// Ensure that a GPR is homed for its def in `def_instr`.
static void FillGPRForDef(Fragment *frag, Instruction *def_instr,
                          GPRLocations *gpr_locs, VirtualRegister gpr) {
  FillGPRForDefUse(frag, def_instr, gpr_locs, gpr, false);
}

// Free a spill slot for use by another fragment-local register within the
// current fragment.
static void FreeVirtualSlot(Fragment *frag, Instruction *instr_before_var_kill,
                            GPRLocations *gpr_locs, RegisterLocation *var_loc) {
  auto &var_reg(var_loc->reg);
  if (var_reg.IsValid()) {
    if (var_reg.IsNative()) {
      FillGPRForUse(frag, instr_before_var_kill, gpr_locs, var_reg);
    }
    const auto mask = 1U << var_loc->spill_slot;
    GRANARY_ASSERT(0 != (frag->spill_slot_allocated_mask & mask));
    frag->spill_slot_allocated_mask &= ~mask;
    var_reg = VirtualRegister();
  }
}

// Try to steal a dead register. Returns `true` if a dead register was stolen,
// and updates `out_reg` to be the stolen register, otherwise returns `false`
// and leaves `out_reg` as-is.
static bool TryFindStealableDeadRegister(GPRLocations *locs,
                                         RegUseTracker *uses,
                                         VirtualRegister *out_reg) {
  for (auto i = arch::NUM_GENERAL_PURPOSE_REGISTERS; i--; ) {
    auto &loc(locs->location[i]);
    if (!loc.is_live && !loc.stolen_by && !uses->Get(loc.reg.Number())) {
      *out_reg = loc.reg;
      return true;
    }
  }
  return false;
}

// Spill a register that isn't being used by the current instruction and is
// not already stolen. This updates `out_reg` in place.
static VirtualRegister FindSpillableRegister(GPRLocations *locs,
                                             RegUseTracker *uses) {
  for (auto i = arch::NUM_GENERAL_PURPOSE_REGISTERS; i--; ) {
    auto &ith_gpr_loc(locs->location[i]);
    if (!ith_gpr_loc.stolen_by && !uses->Get(i)) {
      return ith_gpr_loc.reg;
    }
  }
  GRANARY_ASSERT(false);
  return VirtualRegister();
}

// Force a virtual register into a general-purpose register. This will try to
// get the virtual register into a general purpose register by stealing a
// physical register, and if none can be stolen, then it will spill a physical
// register.
static void ForceVRToGPR(Fragment *frag, NativeInstruction * const instr,
                         GPRLocations *locs, RegUseTracker *uses,
                         RegisterLocation *reg_loc) {
  VirtualRegister gpr;
  if (!TryFindStealableDeadRegister(locs, uses, &gpr)) {
    gpr = FindSpillableRegister(locs, uses);
    frag->InsertAfter(instr, RestoreGPRFromSlot(gpr, reg_loc->reg));
  }

  GRANARY_ASSERT(gpr.IsNative());
  auto &gpr_loc(locs->location[gpr.Number()]);
  GRANARY_ASSERT(!gpr_loc.stolen_by);
  gpr_loc.stolen_by = reg_loc;
  gpr_loc.reg = reg_loc->reg;
  reg_loc->reg = gpr;
}

// Schedule a fragment-local virtual register operand that is a register use.
// In this case we, try to keep fragment-local virtual registers in spill slot-
// based memory locations, without having to spill/fill native registers for
// those virtual registers.
static void ScheduleFragLocalRegOp(Fragment *frag,
                                   NativeInstruction * const instr,
                                   GPRLocations *locs, RegUseTracker *uses,
                                   RegisterOperand *op, SSAVariable *var) {
  auto reg = op->Register();
  auto reg_loc = LocationOf(var);
  auto reg_storage = BackingStorageOf(frag, reg_loc);

  // If the current storage for `reg` is a spill slot, then force it into a
  // register.
  if (reg_storage.IsVirtualSlot()) {
    ForceVRToGPR(frag, instr, locs, uses, reg_loc);
    reg_storage = reg_loc->reg;
  }

  GRANARY_ASSERT(reg_storage.IsNative() && reg_storage.IsGeneralPurpose());
  reg_storage.Widen(reg.ByteWidth());
  RegisterOperand repl_op(reg_storage);

  GRANARY_IF_DEBUG(auto replaced = ) op->Ref().ReplaceWith(repl_op);
  GRANARY_ASSERT(replaced);
}

// Schedule a fragment-local register that might be used in this operand. We
// choose different spill/fill strategies based on whether or not a virtual
// register is used as a memory operand or as a register operand.
static void ScheduleFragLocalReg(Fragment *frag,
                                 NativeInstruction * const instr,
                                 GPRLocations *locs, RegUseTracker *uses,
                                 Operand *op) {
  if (auto reg_op = DynamicCast<RegisterOperand *>(op)) {
    auto reg = reg_op->Register();
    if (reg.IsVirtual()) {
      // Make sure we start looking for the definition in the right place.
      Instruction *def_search_instr = reg_op->IsRead() ? instr->Previous()
                                                       : instr;
      if (auto def = LocalDefinitionOf(frag, def_search_instr, reg)) {
        ScheduleFragLocalRegOp(frag, instr, locs, uses, reg_op, def);
      }
    }
  }
}

// Ensure that any native GPRs used by a particular operand are homed.
static void HomeNativeGPRs(Fragment *frag, NativeInstruction *instr,
                           GPRLocations *locs, Operand *op) {
  if (auto reg_op = DynamicCast<RegisterOperand *>(op)) {
    auto reg = reg_op->Register();
    if (op->IsWrite() &&
        !(reg_op->IsRead() || reg_op->IsConditionalWrite() ||
          reg.PreservesBytesOnWrite())) {
      FillGPRForDef(frag, instr, locs, reg);
    } else {
      FillGPRForUse(frag, instr, locs, reg);
    }
  } else if (auto mem_op = DynamicCast<MemoryOperand *>(op)) {
    VirtualRegister r1, r2, r3;
    if (mem_op->CountMatchedRegisters({&r1, &r2, &r3})) {
      FillGPRForUse(frag, instr, locs, r1);
      FillGPRForUse(frag, instr, locs, r2);
      FillGPRForUse(frag, instr, locs, r3);
    }
  }
}

// Mark a register as being used.
static void MarkRegAsUsed(RegUseTracker *uses, VirtualRegister reg) {
  if (reg.IsNative() && reg.IsGeneralPurpose()) {
    uses->Set(reg.Number(), true);
  }
}

// Mark all GPRs used by a particular instruction as being used (regardless of
// if they are read or write operands).
static void FindUsedNativeGPRs(RegUseTracker *uses, Operand *op) {
  if (auto reg_op = DynamicCast<RegisterOperand *>(op)) {
    MarkRegAsUsed(uses, reg_op->Register());
  } else if (auto mem_op = DynamicCast<MemoryOperand *>(op)) {
    VirtualRegister r1, r2, r3;
    if (mem_op->CountMatchedRegisters({&r1, &r2, &r3})) {
      MarkRegAsUsed(uses, r1);
      MarkRegAsUsed(uses, r2);
      MarkRegAsUsed(uses, r3);
    }
  }
}

// Schedule fragment-local virtual registers. A virtual register is fragment-
// local if it is defined with the fragment, and doesn't share a definition
static void ScheduleFragLocalRegs(Fragment *frag, GPRLocations *gpr_locs) {

  //for (auto instr : BackwardInstructionIterator(frag->last)) {
  for (auto instr = frag->last; instr;) {
    const auto prev_instr = instr->Previous();
    if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
      RegUseTracker uses;

      // Note: Should visit defs before uses. Should have the effect of
      //       homing+killing defs, and homing+reviving uses.
      ninstr->ForEachOperand([&] (Operand *op) {
        HomeNativeGPRs(frag, ninstr, gpr_locs, op);
        FindUsedNativeGPRs(&uses, op);
      });

      ninstr->ForEachOperand([&] (Operand *op) {
        ScheduleFragLocalReg(frag, ninstr, gpr_locs, &uses, op);
      });

      // For each defined virtual instruction, free up the spill slot associated
      // with this virtual register if the virtual register is fragment-local
      // and if the virtual register is definitely defind by this instruction.
      ForEachDefinition(ninstr, [&] (SSAVariable *def) {
        auto def_reg = RegisterOf(def);
        if (def_reg.IsVirtual() && IsA<SSARegister *>(def) &&
            !DefinitionEscapesFragment(frag, def, def_reg)) {
          FreeVirtualSlot(frag, prev_instr, gpr_locs, LocationOf(def));
        }
      });
    }
    instr = prev_instr;
  }
}

// Schedule virtual registers that are only used an defined within their
// containing fragments.
static void ScheduleLocalRegs(Fragment * const frags) {
  for (auto frag : FragmentIterator(frags)) {
    frag->num_allocated_spill_slots = 0;
    frag->spill_slot_allocated_mask = 0;
    if (frag->ssa_vars) {
      GPRLocations gpr_locs;
      ScheduleFragLocalRegs(frag, &gpr_locs);

      continue;  // TODO(pag): Remove me.

      for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
        auto &loc(gpr_locs.location[i]);
        if (loc.stolen_by) {
          GRANARY_ASSERT(loc.reg.IsVirtualSlot());
          frag->InsertBefore(frag->first,
                             SaveGPRToSlot(NthArchGPR(i), loc.reg));
        }
      }
    }
  }
}

// Clean up all allocated storage locations.
static void CleanUpRegisterLocations(RegisterLocation *loc) {
  for (RegisterLocation *next_loc(nullptr); loc; loc = next_loc) {
    next_loc = loc->next;
    delete loc;
  }
}

// Remove SSA definitions from an instruction.
static void CleanUpSSADefs(NativeInstruction *instr) {
  if (auto var = GetMetaData<SSAVariable *>(instr)) {
    while (auto forward_def = DynamicCast<SSAForward *>(var)) {
      var = forward_def->next_instr_def;
      DestroySSAObj(forward_def);
    }
    if (var) {
      DestroySSAObj(var);
    }
    ClearMetaData(instr);
  }
}

// Clean up all SSA variable definitions.
static void CleanUpSSAVars(Fragment * const frags) {
  for (auto frag : FragmentIterator(frags)) {
    if (frag->ssa_vars) {
      for (auto instr : ForwardInstructionIterator(frag->first)) {
        if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
          CleanUpSSADefs(ninstr);
        }
      }
      delete frag->ssa_vars;
      frag->ssa_vars = nullptr;
    }
  }
}

}  // namespace

// Schedule virtual registers.
void ScheduleRegisters(Fragment * const frags) {
  auto loc = AssignRegisterLocations(frags);
  ScheduleLocalRegs(frags);
  CleanUpRegisterLocations(loc);
  CleanUpSSAVars(frags);
}

}  // namespace granary
