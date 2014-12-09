/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL
#define GRANARY_ARCH_INTERNAL

#include "granary/cfg/instruction.h"

#include "granary/code/fragment.h"
#include "granary/code/ssa.h"

#include "granary/code/assemble/8_schedule_registers.h"

#include "granary/util.h"

enum : bool {
  SHARE_SPILL_SLOTS = true
};

namespace granary {
namespace arch {

// Returns a valid `SSAOperand` pointer to the operand being copied if this
// instruction is a copy instruction, otherwise returns `nullptr`.
//
// Note: This has an architecture-specific implementation.
extern SSAOperand *GetCopiedOperand(const NativeInstruction *instr,
                                    SSAInstruction *ssa_instr);

// Create an instruction to copy a GPR to a spill slot.
//
// Note: This has an architecture-specific implementation.
extern granary::Instruction *SaveGPRToSlot(VirtualRegister gpr,
                                           VirtualRegister slot);

// Create an instruction to copy the value of a spill slot to a GPR.
//
// Note: This has an architecture-specific implementation.
extern granary::Instruction *RestoreGPRFromSlot(VirtualRegister gpr,
                                                VirtualRegister slot);

// Swaps the value of one GPR with another.
//
// Note: This has an architecture-specific implementation.
extern granary::Instruction *SwapGPRWithGPR(VirtualRegister gpr1,
                                            VirtualRegister gpr2);

// Swaps the value of one GPR with a slot.
//
// Note: This has an architecture-specific implementation.
extern granary::Instruction *SwapGPRWithSlot(VirtualRegister gpr1,
                                             VirtualRegister slot);

}  // namespace arch
namespace {

// Free up all SSA-related data structures stored in the fragment instructions.
static void FreeSSAData(FragmentList *frags) {
  for (auto frag : FragmentListIterator(frags)) {
    for (auto instr : InstructionListIterator(frag->instrs)) {
      if (IsA<NativeInstruction *>(instr)) {
        if (auto ssa_instr = GetMetaData<SSAInstruction *>(instr)) {
          delete ssa_instr;
          ClearMetaData(instr);
        }
      } else if (auto ainstr = DynamicCast<AnnotationInstruction *>(instr)) {
        if (kAnnotSSANodeOwner == ainstr->annotation ||
            kAnnotSSARestoreRegister == ainstr->annotation) {
          // Note: `kAnnotSaveRegister`s will have corresponding
          //       `kAnnotSSANodeOwner`s if the scope of the save/restore is
          //       partition-local.
          delete GetMetaData<SSANode *>(instr);
          ClearMetaData(instr);
        } else if (kAnnotSSAElidedInstruction == ainstr->annotation) {
          delete GetMetaData<SSAInstruction *>(instr);
          ClearMetaData(instr);
        }
      }
    }
  }
}

// Frees all flag zone data structures.
static void FreeFlagZones(FragmentList *frags) {
  for (auto frag : FragmentListIterator(frags)) {
    auto &flag_zone(frag->flag_zone.Value());
    if (flag_zone) {
      delete flag_zone;
      flag_zone = nullptr;
    }
  }
}

// Return the Nth architectural GPR.
static VirtualRegister NthArchGPR(int n) {
  return VirtualRegister(VR_KIND_ARCH_GPR, arch::GPR_WIDTH_BYTES,
                         static_cast<uint16_t>(n));
}

// Return the Nth spill slot.
static VirtualRegister NthSpillSlot(int n) {
  return VirtualRegister(VR_KIND_VIRTUAL_SLOT, arch::GPR_WIDTH_BYTES,
                         static_cast<uint16_t>(n));
}

// Register scheduling is a bottom-to-top process, therefore everything should
// be observed from the perspective of where we are now, what we've previously
// done (which corresponds to later instructions), and how we're going to adapt
// our current state to handle the current instruction or earlier instructions.
//
// The first part of register scheduling is to schedule the partition-local
// registers. Partition-local registers are registers that are used in two or
// more fragments. When scheduling a VR, the partition-local scheduler attempts
// to maintain invariants about where the VR's value is located between
// fragments. This invariants-based approach works because earlier steps ensure
// that if a VR is live on exit from a fragment F, then the VR will be live
// on entry to all of F's successors. The key to maintaining this liveness
// property is the compensation fragments added in by step 6.
//
// The second step is to schedule fragment-local registers, which is treated
// as a special case of partition-local.
//
// Algorithm SchedulePartitionLocalRegisters:
// ------------------------------------------
//    While there are still partition local registers to be scheduled:
//      Choose a spill slot number `SLOT`.
//      For each partition P:
//        Find an unscheduled partition-local virtual register VR in P.
//        Find a preferred GPR for VR.
//          Note: A preferred GPR for VR will be a GPR that will ideally store
//                VR in all fragments using VR. This GPR will represent the
//                canonical way of "communicating" VR when VR is live across
//                two fragments. If no preferred GPR exists, then VR must be
//                homed to a slot between fragments.
//
//        For each fragment F in P:
//          Let Loc(VR) = SLOT
//          Let Loc(PGPR) = PGPR
//
//          If VR is live on exit from F:
//            If VR has a PGPR:
//              Let Loc(VR) = PGPR
//              Let Loc(PGPR) = SLOT
//            Else:
//              Let Loc(VR) = LIVE_SLOT
//
//          For each instruction I in F in reverse order:
//            If Loc(VR) is used or defined in I:
//              Find an alternative GPR, AGPR, for VR.
//              Apply Case 1.
//              Let Loc(Loc(VR)) = Loc(VR).
//              Let Loc(VR) = AGPR.
//              Let Loc(AGPR) = SLOT.
//
//            If VR is used or defined in I:
//              If Loc(VR) is SLOT or Loc(VR) is LIVE_SLOT:
//                Find a usable GPR, UGPR, for VR in I.
//                  Note: We give preference to UGPR == PGPR if PGPR is not
//                        used in I.
//                If Loc(VR) is SLOT:
//                  Apply Case 2.1.
//                Else:  # LIVE_SLOT
//                  Apply Case 2.2.
//                Let Loc(UGPR) = SLOT
//                Let Loc(VR) = UGPR
//
//              Replace all instances of VR in I with Loc(VR).
//
//            If VR was defined by I:
//              Assert1 Loc(VR) is a GPR
//              Assert2 VR is not live on entry to F.
//              Apply Case 3.
//              Let Loc(Loc(VR)) = Loc(VR).
//              Let Loc(VR) = SLOT.
//
//          If VR is live on entry to F:
//            If PGPR is valid:
//              Assert3 Loc(VR) != SLOT
//                Note: Failure of this assertion most likely indicates a
//                      missing compensation fragment, as otherwise `Assert1`
//                      would have triggered the issue in advance. This
//                      shouldn't be possible because compensation fragments
//                      have `kAnnotSSANodeKill` instructions to kill VRs.
//              If Loc(VR) != PGPR:
//                Apply Case 4.
//                Let Loc(Loc(VR)) = Loc(VR).
//                Let Loc(VR) = PGPR.
//                Let Loc(PGPR) = SLOT.
//
//            Else If Loc(VR) != LIVE_SLOT:
//              Assert4 Loc(VR) != SLOT
//                Note: Failure of this assertion most likely means a later
//                      instruction killed VR (by defining it).
//              Apply Case 2.2.
//              Let Loc(Loc(VR)) = Loc(VR).
//              Let Lov(VR) LIVE_SLOT.
//          Else:
//            Assert5 VR is live on exit from F
//            Assert6 Loc(VR) != LIVE_SLOT
//            If Loc(VR) != SLOT:
//              Apply Case 5.
//              Let Loc(Loc(VR)) = Loc(VR).
//              Let Loc(VR) = SLOT.
//
// Case 1:  Scheduling a VR where PGPR == r1. When going through the instruction
//          list in reverse order, we see a native use of PGPR, so we need to
//          change where the register is located. We'll change it to be stored
//          in r2.
//
//          save r2 --> slot  (assumed)
//          ...
//          native use of r1  (current position)
//          swap r2 <-> r1  # Move the VR's value (stored in r2) to r1,
//                          # and save r1's value to r2.
//          swap r2 <-> slot  # Restore r2 and save r1 (stored in r2) to
//                            # the slot.
//
//            Note: This is equivalent to:
//              swap r2 <-> slot  # Restore r2, but keep VR's value in slot.
//              swap r1 <-> slot  # Save r1 to slot, and put VR's value into r1.
//          ...
//          restore slot --> r1 (assumed)
//
// Case 2.1:Scheduling a VR where the VR has not previously been spilled in
//          this fragment. We will use r1 as the register that is scheduled to
//          hold VR, and we'll inject a restore of r1.
//
//          save r1 --> slot  (assumed)
//          ...
//          native use of VR  (current position)
//          restore slot --> r1  # Restore r1.
//
// Case 2.2:Scheduling a VR where the VR has not previously been spilled in
//          this fragment, but where it is assumed to be live, and where the
//          normal invariant could not be maintained, and so VR's value is
//          actually located in the slot. We will use r1 as the register that
//          is scheduled to hold VR, and we'll inject a restore of r1.
//
//          save r1 --> slot  (assumed)
//          ...
//          native use of VR  (current position)
//          swap slot <-> r1  # Restore r1, but keep the value of the VR alive.
//
// Case 3:  We've found the definition of a VR, so we just need to spill its
//          GPR before the instruction defining the GPR.
//
//          save r1 --> slot  # Save r1.
//          native def of VR  (current position)
//          ...
//          restore slot --> r1  (assumed)
//
// Case 4:  We're at the entrypoint to a fragment, VR is live on entry to the
//          fragment (and therefore live on exit from F's predecessor), and
//          PGPR is valid, but the VR is not homed to its PGPR. Therefore, we
//          want to maintain the invariant that across these boundaries, VR is
//          homed to PGPR. We will assume that VR is currently homed to r1.
//
//          This case is fundamentally very similar to Case 1.
//
//          <VR live on entry>
//          swap PGPR <-> r1  # Move the VR's value (stored in PGPR) to r1,
//                            # and save r1's value to PGRP.
//          swap PGPR <-> slot  # Restore PGPR and save r1 (stored in PGPR) to
//                              # the slot.
//          ...
//          native use of VR homed to r1  (assumed)
//          ...
//          restore slot --> r1  (assumed)
//
// Case 5:  We're at the entrypoint to a fragment, VR is not live on entry
//          to the fragment, and is currently homed to a register. Therefore
//          we need to add in the original save of the register to the spill
//          slot.
//
//          save r1 --> slot  # Save the GPR used by VR to a slot.
//          ...
//          restore slot --> r1  (assumed)

enum class RegLocationType {
  GPR,
  SLOT,
  LIVE_SLOT
};

struct RegLocation {
  VirtualRegister loc;
  RegLocationType type;
};

// Used for scheduling registers in a partition.
struct PartitionScheduler {
  PartitionScheduler(VirtualRegister vr_, SSANodeId node_id_, int slot_,
                     VirtualRegister preferred_gpr_)
    : vr(vr_),
      spill_slot(NthSpillSlot(slot_)),
      node_id(node_id_),
      vr_location{spill_slot, RegLocationType::SLOT},
      invalid_location{VirtualRegister(), RegLocationType::GPR},
      preferred_gpr(preferred_gpr_) {
    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      gpr_locations[i] = {NthArchGPR(i), RegLocationType::GPR};
    }
  }

  // Current location of a register.
  RegLocation &Loc(VirtualRegister reg) {
    if (reg.IsNative()) {
      if (reg.IsGeneralPurpose()) {
        return gpr_locations[reg.Number()];
      }
    } else if (reg.IsVirtual()) {
      if (reg == vr) {
        return vr_location;
      }
    }

    GRANARY_ASSERT(false);
    // Fallback case for arch regs that aren't GPRs, and for virtual registers
    // that we aren't looking at.
    return invalid_location;
  }

  // Virtual register being scheduled in this partition.
  const VirtualRegister vr;
  const VirtualRegister spill_slot;
  const SSANodeId node_id;

  // Current locations of arch GPRs.
  RegLocation gpr_locations[arch::NUM_GENERAL_PURPOSE_REGISTERS];

  // Current location of the VR.
  RegLocation vr_location;

  // Dummy.
  RegLocation invalid_location;

  // Should we try to enforce the invariant that if a VR is live on entry/exit
  // from a fragment then it should be located in its preferred GPR?
  VirtualRegister preferred_gpr;
};

// Try to find an as-of-yet unscheduled SSA node in a node map.
static bool GetUnscheduledVR(SSANodeMap &nodes, VirtualRegister *reg,
                             SSANodeId *node_id) {
  for (auto node : nodes.Values()) {
    auto &scheduled_status(node->id.Value());
    if (NODE_UNSCHEDULED == scheduled_status && node->reg.IsVirtual()) {
      scheduled_status = NODE_SCHEDULED;
      *reg = node->reg;
      *node_id = node->id;
      return true;
    }
  }
  return false;
}

// Try to find an as-of-yet unscheduled SSA node in a fragment.
static bool GetUnscheduledVR(SSAFragment *frag, VirtualRegister *reg,
                             SSANodeId *node_id) {
  if (!GetUnscheduledVR(frag->ssa.exit_nodes, reg, node_id)) {
    return GetUnscheduledVR(frag->ssa.entry_nodes, reg, node_id);
  }
  return true;
}

struct GPRScheduler {
  GPRScheduler(void)
      : reg_counts(),
        used_regs(),
        restricted_regs() {}

  // Recounts the number of times each arch GPR is used within a partition.
  void RecountGPRUsage(const PartitionInfo *partition,
                       Fragment *first, Fragment *last) {
    reg_counts.ClearGPRUseCounters();
    for (auto frag : FragmentListIterator(first)) {
      if (partition != frag->partition.Value()) continue;
      reg_counts.CountGPRUses(frag);
      if (frag == last) break;
    }
  }

  // Try to get a preferred GPR for use by some VR. This will modify `*reg` and
  // return `true` if a preferred GPR is found. Also, if a preferred GPR is
  // found then the GPR will be marked as live in `min_gpr_num`, thus preventing
  // it from being a preferred GPR again.
  VirtualRegister GetPreferredGPR(UsedRegisterSet *preferred_gprs) {
    auto ret = false;
    auto min_gpr_num = static_cast<int>(arch::NUM_GENERAL_PURPOSE_REGISTERS);
    auto min_num_uses = std::numeric_limits<int>::max();
    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      if (preferred_gprs->IsLive(i)) continue;
      if (reg_counts.num_uses_of_gpr[i] <= min_num_uses) {
        ret = true;
        min_gpr_num = i;
        min_num_uses = reg_counts.num_uses_of_gpr[i];
      }
    }
    if (ret) {
      preferred_gprs->Revive(min_gpr_num);
      return NthArchGPR(min_gpr_num);
    } else {
      return VirtualRegister();
    }
  }

  // Get some GPR for use, so long as the GPR is not part of the
  // `avoid_reg_set`.
  VirtualRegister GetGPR(void) {
    GRANARY_IF_DEBUG( auto found_reg = false; )
    auto min_gpr_num = static_cast<int>(arch::NUM_GENERAL_PURPOSE_REGISTERS);
    auto min_num_uses = std::numeric_limits<int>::max();
    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      if (used_regs.IsLive(i) || restricted_regs.IsLive(i)) continue;
      if (reg_counts.num_uses_of_gpr[i] <= min_num_uses) {
        GRANARY_IF_DEBUG( found_reg = true; )
        min_gpr_num = i;
        min_num_uses = reg_counts.num_uses_of_gpr[i];
      }
    }
    GRANARY_ASSERT(found_reg);
    return NthArchGPR(min_gpr_num);
  }

  // Counts of the number of uses of each register.
  RegisterUsageCounter reg_counts;

  // Registers being used by an instruction.
  UsedRegisterSet used_regs;

  // Registers that an instruction is not allowed to use.
  UsedRegisterSet restricted_regs;
};

// Returns true if the virtual register associated with a particular SSA node
// is live within a node map.
static bool IsLive(SSANodeMap &nodes, VirtualRegister reg, SSANodeId node_id) {
  if (nodes.Exists(reg)) {
    const auto node = nodes[reg];
    return node->id == node_id;
  }
  return false;
}

// Try to get a GPR for use by an instruction.
static VirtualRegister GetGPR(GPRScheduler *reg_sched, VirtualRegister pgpr) {
  VirtualRegister agpr;
  if (pgpr.IsValid() && reg_sched->used_regs.IsDead(pgpr) &&
      reg_sched->restricted_regs.IsDead(pgpr)) {
    return pgpr;  // Try to use the preferred GPR if possible.
  } else {
    return reg_sched->GetGPR();
  }
}

// Looks through an instruction to see if some node is defined or used.
static void FindDefUse(const SSAInstruction *instr, SSANodeId node_id,
                       bool *is_defined, bool *is_used) {
  for (const auto &op : instr->defs) {
    GRANARY_ASSERT(op.is_reg);
    if (op.nodes[0]->id == node_id) {
      *is_defined = true;
      break;
    }
  }
  for (const auto &op : instr->uses) {
    for (const auto node : op.nodes) {
      if (node->id == node_id) {
        *is_used = true;
        return;
      }
    }
  }
}

// Replace a use of a virtual register
static void ReplaceOperandReg(SSAOperand &op, VirtualRegister replacement_reg) {
  GRANARY_ASSERT(1 == op.nodes.Size());
  GRANARY_IF_DEBUG( auto node = op.nodes[0]; )
  GRANARY_ASSERT(node->reg.IsVirtual());
  GRANARY_ASSERT(replacement_reg.IsNative());
  Operand existing_op(op.operand);
  if (op.is_reg) {
    replacement_reg.Widen(op.operand->ByteWidth());
    RegisterOperand repl_op(replacement_reg);
    GRANARY_IF_DEBUG( auto replaced = ) existing_op.Ref().ReplaceWith(repl_op);
    GRANARY_ASSERT(replaced);
  } else {
    replacement_reg.Widen(arch::ADDRESS_WIDTH_BYTES);
    MemoryOperand repl_op(replacement_reg, op.operand->ByteWidth());
    GRANARY_IF_DEBUG( auto replaced = ) existing_op.Ref().ReplaceWith(repl_op);
    GRANARY_ASSERT(replaced);
  }
}

// Replace all uses of a virtual register associated with a specific SSA node
// with a GPR.
static void ReplaceUsesOfVR(SSAInstruction *instr, SSANodeId node_id,
                            VirtualRegister replacement_reg) {
  for (auto &op : instr->defs) {
    if (op.nodes[0]->id == node_id) {
      GRANARY_ASSERT(op.is_reg);
      ReplaceOperandReg(op, replacement_reg);
    }
  }
  for (auto &op : instr->uses) {
    if (op.nodes[0]->id == node_id) {
      ReplaceOperandReg(op, replacement_reg);
    }
  }
}

// Gets a spill slot to be used by the partition-local register scheduler.
static int GetSpillSlot(const SpillSlotSet &used_slots, int *num_slots) {
  auto ret = *num_slots;
  if (SHARE_SPILL_SLOTS) {
    GRANARY_IF_DEBUG( auto found_slot = false; )
    for (auto i = 0; i < arch::MAX_NUM_SPILL_SLOTS; ++i) {
      if (!used_slots.Get(i)) {
        ret = i;
        GRANARY_IF_DEBUG( found_slot = true; )
        break;
      }
    }
    GRANARY_ASSERT(found_slot);
  }
  *num_slots = std::max(ret + 1, *num_slots);
  return ret;
}

// Finds a spill slot that can hold the virtual register. We need to make sure
// that this spill slot does not interfere with any other spill slots being
// used within the current partition.
static int FindSlotForVR(PartitionInfo * const partition,
                         Fragment *first, Fragment *last,
                         VirtualRegister vr, SSANodeId node_id) {
  SpillSlotSet used_slots;

  for (auto frag : FragmentListIterator(first)) {
    if (partition != frag->partition.Value()) continue;

    auto ssa_frag = DynamicCast<SSAFragment *>(frag);
    if (!ssa_frag) continue;

    auto is_live_on_exit = IsLive(ssa_frag->ssa.exit_nodes, vr, node_id);
    auto is_live_on_entry = IsLive(ssa_frag->ssa.entry_nodes, vr, node_id);

    if (is_live_on_exit || is_live_on_entry) {
      used_slots.Union(ssa_frag->spill.used_slots);
    }

    if (frag == last) break;
  }
  return GetSpillSlot(used_slots, &(partition->num_slots));
}

// The register we want to use for scheduling `vr` is used in this instruction,
// therefore we need to re-home the register.
static void HomeUsedReg(PartitionScheduler *part_sched,
                        GPRScheduler *reg_sched,
                        SSAFragment *frag,
                        Instruction *instr,
                        RegLocation *vr_home) {
  if (!vr_home->loc.IsNative() ||
      !reg_sched->used_regs.IsLive(vr_home->loc)) {
    return; // Not used in this instruction.
  }

  const auto slot = part_sched->spill_slot;
  const auto pgpr = part_sched->preferred_gpr;

  GRANARY_ASSERT(RegLocationType::GPR == vr_home->type);
  auto agpr = GetGPR(reg_sched, pgpr);
  GRANARY_ASSERT(vr_home->loc != agpr);

  frag->instrs.InsertAfter(instr, arch::SwapGPRWithSlot(agpr, slot));
  frag->instrs.InsertAfter(instr, arch::SwapGPRWithGPR(vr_home->loc, agpr));

  part_sched->Loc(vr_home->loc) = {vr_home->loc, RegLocationType::GPR};
  part_sched->Loc(agpr) = {slot, RegLocationType::SLOT};

  vr_home->loc = agpr;  // Updates `Loc` by ref.
}

// Schedule all a partition-local register within a specific fragment of the
// partition.
static void SchedulePartitionLocalRegs(PartitionScheduler *part_sched,
                                       GPRScheduler *reg_sched,
                                       SSAFragment *frag) {
  const auto vr = part_sched->vr;
  const auto node_id = part_sched->node_id;
  const auto slot = part_sched->spill_slot;
  const auto pgpr = part_sched->preferred_gpr;
  const auto is_live_on_exit = IsLive(frag->ssa.exit_nodes, vr, node_id);
  const auto is_live_on_entry = IsLive(frag->ssa.entry_nodes, vr, node_id);

  // Mark this slot as used in every fragment where it appears as either
  // live on entry or live on exit. Because of the way that live on entry/
  // exit was built (via data flow), we should get a natural cover of the
  // transitive closure of all potentially simultaneously live reg (at
  // the fragment granularity).
  if (is_live_on_exit || is_live_on_entry) {
    frag->spill.used_slots.Set(slot.Number(), true);
  } else {
    return;  // Nothing to schedule!
  }

  if (is_live_on_exit) {
    if (pgpr.IsValid()) {
      part_sched->Loc(vr) = {pgpr, RegLocationType::GPR};
      part_sched->Loc(pgpr) = {slot, RegLocationType::SLOT};
    } else {
      part_sched->Loc(vr) = {slot, RegLocationType::LIVE_SLOT};
    }
  }

  Instruction *first_instr(nullptr);

  auto instr = frag->instrs.Last();
  for (Instruction *prev_instr(nullptr); instr; instr = prev_instr) {
    prev_instr = instr->Previous();

    SSAInstruction *ssa_instr(nullptr);
    auto is_defined = false;
    auto is_used = false;
    auto &vr_home(part_sched->Loc(vr));

    reg_sched->used_regs.KillAll();
    reg_sched->restricted_regs.KillAll();

    // Annotation instructions can define/kill VRs.
    if (auto ainstr = DynamicCast<AnnotationInstruction *>(instr)) {
      auto node = GetMetaData<const SSANode *>(ainstr);
      // Note: `kAnnotSSANodeOwner` is not considered definitions because they
      //       are added by the local value numbering stage of SSA construction
      //       for the sake of making it easier to reclaim `SSANode` objects.
      if (kAnnotSSANodeKill == ainstr->annotation) {
        if (node->id == node_id) {
          // This node can't be homed to a register because the meaning of
          // this is to say that the node was live in a predecessor, but is not
          // live in a successor (of this compensation fragment), and therefore
          // we should not expect and uses of the node to follow this
          // instruction.
          GRANARY_ASSERT(RegLocationType::GPR != vr_home.type);

          // Fake a kill as a use. The meaning here is that we expect that this
          // register will start being used, and in fact it's exported from
          // a predecessor fragment's `exit_nodes` into this frag's
          // `entry_nodes`, so we need to have it as a use so that the slot
          // matching happens.
          is_used = true;
        }

      // Don't allow save/restore regs to span more than one fragment.
      } else if (kAnnotSSASaveRegister == ainstr->annotation ||
                 kAnnotSSARestoreRegister == ainstr->annotation) {
        GRANARY_ASSERT(node->id != node_id);
        reg_sched->used_regs.Revive(ainstr->Data<VirtualRegister>());
        HomeUsedReg(part_sched, reg_sched, frag, instr, &vr_home);
        continue;

      } else if (kAnnotSSAReviveRegisters == ainstr->annotation) {
        reg_sched->used_regs = ainstr->Data<UsedRegisterSet>();
        HomeUsedReg(part_sched, reg_sched, frag, instr, &vr_home);
        continue;

      // We can stop here.
      } else if (kAnnotSSAPartitionLocalBegin == ainstr->annotation) {
        first_instr = instr;
        break;

      } else {
        continue;
      }

    // Its a native instruction, need to look to see if the VR is used and/or
    // defined. We also need to see if the current location of the VR is used.
    } else if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
      ssa_instr = GetMetaData<SSAInstruction *>(ninstr);
      if (!ssa_instr) continue;
      reg_sched->used_regs.Visit(ninstr);

      // If this instruction has no explicit operands, then we can't possibly
      // schedule a VR in, and so we don't need to add the constraint to the
      // system that restricted registers should not be used.
      if (ninstr->NumExplicitOperands()) {
        reg_sched->restricted_regs.ReviveRestrictedRegisters(ninstr);
      }

      HomeUsedReg(part_sched, reg_sched, frag, instr, &vr_home);
      FindDefUse(ssa_instr, node_id, &is_defined, &is_used);
    }

    if (!is_used && !is_defined) continue;

    // Inject a fill for this instruction. Filling might restore a GPR if this
    // VR has a preferred GPR, or if there is no preferred GPR, then filling
    // will swap the VR value modified and/or read by this instruction into the
    // spill slot.
    if (RegLocationType::SLOT == vr_home.type ||
        RegLocationType::LIVE_SLOT == vr_home.type) {

      auto agpr = GetGPR(reg_sched, pgpr);
      if (RegLocationType::SLOT == vr_home.type) {
        frag->instrs.InsertAfter(instr, arch::RestoreGPRFromSlot(agpr, slot));

      } else {  // `RegLocationType::LIVE_SLOT`.
        frag->instrs.InsertAfter(instr, arch::SwapGPRWithSlot(agpr, slot));
      }

      part_sched->Loc(agpr) = {slot, RegLocationType::SLOT};
      vr_home.loc = agpr;  // Updates `Loc` by ref.
      vr_home.type = RegLocationType::GPR;
    }

    if (ssa_instr) ReplaceUsesOfVR(ssa_instr, node_id, vr_home.loc);

    // Inject the spill for this definition.
    if (is_defined) {
      GRANARY_ASSERT(RegLocationType::GPR == vr_home.type);
      GRANARY_ASSERT(!is_live_on_entry);
      frag->instrs.InsertBefore(instr, arch::SaveGPRToSlot(vr_home.loc, slot));
      part_sched->Loc(vr_home.loc) = {vr_home.loc, RegLocationType::GPR};
      vr_home.loc = slot;
      vr_home.type = RegLocationType::SLOT;
    }
  }
  const auto vr_home = part_sched->Loc(vr);
  if (is_live_on_entry) {
    GRANARY_ASSERT(RegLocationType::SLOT != vr_home.type);

    // Need to make sure that the VR is homed to its preferred GPR for
    // transitions between fragments.
    if (pgpr.IsValid()) {
      GRANARY_ASSERT(vr_home.loc.IsNative());
      if (vr_home.loc != pgpr) {
        frag->instrs.InsertBefore(first_instr,
                                  arch::SwapGPRWithGPR(vr_home.loc, pgpr));
        frag->instrs.InsertBefore(first_instr,
                                  arch::SwapGPRWithSlot(pgpr, slot));
      }

    // Need to put the VR into its spill slot for transitions between fragments.
    } else if (RegLocationType::LIVE_SLOT != vr_home.type) {
      GRANARY_ASSERT(vr_home.loc.IsNative());
      frag->instrs.InsertBefore(first_instr,
                                arch::SwapGPRWithSlot(vr_home.loc, slot));

    } else {
      GRANARY_ASSERT(vr_home.loc == slot);
    }

  // Not live on entry, i.e. this is one of the first defs of this VR, so we
  // need to add the initial reg spill.
  } else {
    GRANARY_ASSERT(is_live_on_exit);
    if (RegLocationType::GPR == vr_home.type) {
      frag->instrs.InsertBefore(first_instr,
                                arch::SaveGPRToSlot(vr_home.loc, slot));
    } else {
      GRANARY_ASSERT(RegLocationType::LIVE_SLOT != vr_home.type);
      GRANARY_ASSERT(vr_home.loc == slot);
    }
  }
}

// Schedule all partition-local virtual registers within the fragments of a
// given partition.
static void SchedulePartitionLocalRegs(FragmentList *frags,
                                       PartitionInfo *partition) {
  Container<PartitionScheduler> sched;
  GPRScheduler gpr_sched;
  VirtualRegister reg;
  VirtualRegister preferred_gpr;
  SSANodeId node_id;

  auto slot_num = 0;

  // Used to bound the iterating through the fragment list after the first
  // register has been scheduled.
  Fragment *first_frag(frags->First());
  Fragment *last_frag(nullptr);
  for (auto frag : ReverseFragmentListIterator(frags)) {
    if (frag->partition.Value() == partition) {
      if (!last_frag) last_frag = frag;
      first_frag = frag;
    }
  }

  UsedRegisterSet preferred_gprs;

  do {
    reg = VirtualRegister();
    GRANARY_IF_DEBUG( auto found_reg = false; )
    for (auto frag : ReverseFragmentListIterator(last_frag)) {

      // Filter on only a specific partition.
      if (frag->partition.Value() != partition) continue;

      auto ssa_frag = DynamicCast<SSAFragment *>(frag);
      if (!ssa_frag) continue;

      // Go find the register to schedule if we don't have one yet.
      if (!reg.IsValid()) {
        GRANARY_ASSERT(!found_reg);
        last_frag = frag;
        if (!GetUnscheduledVR(ssa_frag, &reg, &node_id)) continue;

        gpr_sched.RecountGPRUsage(partition, first_frag, last_frag);

        preferred_gpr = gpr_sched.GetPreferredGPR(&preferred_gprs);
        slot_num = FindSlotForVR(partition, first_frag, last_frag,
                                 reg, node_id);
        GRANARY_IF_DEBUG( found_reg = true; )
      } else {
        GRANARY_ASSERT(found_reg);
      }

      sched.Construct(reg, node_id, slot_num, preferred_gpr);
      SchedulePartitionLocalRegs(sched.AddressOf(), &gpr_sched, ssa_frag);

      if (frag == first_frag) break;
    }

  } while (reg.IsValid());
}

// Schedule fragment-local registers. We start by doing things one partition at
// at time. Identifying partitions is simple because every partition has a
// single entrypoint: its PartitionEntryFragment. There are technically some
// partitions with no such fragment, but those don't use virtual registers.
static void SchedulePartitionLocalRegs(FragmentList *frags) {
  for (auto frag : FragmentListIterator(frags)) {
    if (auto part_frag = DynamicCast<PartitionEntryFragment *>(frag)) {
      SchedulePartitionLocalRegs(frags, part_frag->partition.Value());
    }
  }
}

// Now that the partition-local registers have been scheduled, what remains is
// to schedule the fragment-local registers. Fragment-local register scheduling
// is also a bottom-to-top process (it processes instructions in a fragment in
// reverse order), and is simpler in some respects but more complicated in
// others.
//
// One thing that makes fragment-local scheduling easier is that it is a linear
// process, and we don't need to worry about maintaining any invariants across
// fragments. Another thing that is simpler is that multiple registers can be
// simultaneously scheduled. Finally, calculating interference is simpler
// because a given register by definition interferes with all others we are
// currently scheduling. This last property of interference is unlike the
// partition-local case, where the argument is more intuitive and depends on
// pre-existing invariants about liveness across fragments.
//
// One thing that makes fragment-local scheduling hard is that to do a good
// job, we need to consider register liveness when making spill/fill decisions,
// and we also need to arrange for spill-sharing, i.e. allowing two non-
// interfering registers to share the same spill slot.
//
// For the sake of exposition, the below algorithm will not show how liveness
// affects the various cases.
//
// Algorithm ScheduleFragmentLocalRegisters:
// -----------------------------------------
//  For each fragment F:
//    For each GPR:
//      Let Loc(GPR) = GPR.
//      Let InverseLoc(GPR) = GPR.
//
//      Invariant1: Loc(InverseLoc(GPR)) == GPR.
//
//    For each instruction I in reverse order:
//      For each GPR appearing as a use/def in I:
//        If Loc(GPR) != GPR:  # Some VR is homed to this GPR after I.
//          If Loc(GPR) == LIVE_SLOT:  # Spill-sharing.
//            Apply Case 1.
//            Let Loc(GPR) = GPR.
//            Let InverseLoc(GPR) = GPR.
//          Else:  # SLOT
//            Assert1 InverseLoc(GPR) is a VR.
//            Assert2 `Invariant1` holds.
//            Let AGPR be some available GPR for use by VR, such that AGPR does
//              not appear in I. Require that Loc(AGPR) == AGPR.
//            Apply Case 2.
//            Let VROccupyingGPR = InverseLoc(GPR).
//            Let Loc(VROccupyingGPR) = AGPR.
//            Let Loc(GPR) = GPR.
//            Let Loc(AGPR) = SLOT.
//            Let InverseLoc(GPR) = GPR.
//            Let InverseLoc(AGPR) = VROccupyingGPR.
//
//      For each VR appearing as a use/def in I:
//        Assert3 Loc(VR) != LIVE_SLOT.
//        If Loc(VR) == SLOT:
//          Let AGPR be some available GPR for use by VR, such that AGPR does
//            not appear in I. Give precedence to Loc(AGPR) == LIVE_SLOT.
//
//          If Loc(AGPR) == LIVE_SLOT:  # Slot sharing.
//            Apply Case 3.
//          Else:
//            Assert4 InveseLocation(AGPR) == AGPR.
//            Apply Case 4.
//          Let InverseLoc(AGPR) = VR.
//          Let Loc(VR) = AGPR.
//          Let Loc(AGPR) = SLOT.
//
//        If VR is defined by I:
//          Assert5 Loc(VR) is a GPR.
//          Let GPRHomedByVR = Loc(VR).
//          Assert6 Loc(GPRHomedByVR) == SLOT.
//          Assert7 InverseLoc(GPRHomedByVR) == VR.
//            Note: Assert6 and Assert7 Verify that Invariant1 holds.
//          Apply Case 5.
//          Delete Loc(VR).
//          Let Loc(GPRHomedByVR) = LIVE_SLOT.
//
//          Note: Invariant1 no longer holds because of slot-sharing.
//
//    For each GPR:
//      If Loc(GPR) != GPR:
//        Apply Case 6.
//
// Case 1:  Some later virtual register VR spilled a GPR r1 to a slot,
//          then when we saw the definition of VR, we decided to leave r1 in
//          the slot instead of injecting a save so that we could potentially
//          use the already spilled r1 for another VR. Unfortunately, we've
//          just found an instruction using r1, and so we need to inject the
//          instruction that saves r1 to the slot after the new instruction.
//
//          native use of r1
//          save r1 --> slot  # Injected save.
//          ...
//          restore slot --> r1  (assumed)
//
// Case 2:  Some virtual register VR is occupying GPR r1, but the current
//          instruction I wants to use r1. Therefore, we need to ensure that
//          r1's native value is saved to the slot after I (to agree with the
//          later assumption that r1 can be restored from the slot). We also
//          need to keep VR alive on the assumption that some instruction
//          before I might use and/or define it. Therefore, we get some
//          alternative register r2 that we will assume holds the value of r2
//          in instructions before/including I, and and
//
//          save r2 --> slot  (assumed)
//          ...
//          native use of r1
//          swap r2 <--> r1  # r2 now holds native r1 value, r1 holds VR value.
//          swap r2 <--> slot  # Save native r1 value (stored in r2) to slot
//                             # and restore r2's native value (assumed to be
//                             # stored in the slot).
//          ...
//          restore slot --> r1  (assumed)
//
// Case 3:  We have found the last use of a VR. Normally, in this case we'd
//          assume VR is homed to some register r1, and them home that register,
//          but what we've got is that r1 has (by later instructions) assumed
//          to be spilled.
//
//          native use of VR  # Will replace VR with r1 eventually.
//          # Nothing to inject.
//          ...
//          restore slot --> r1  (assumed)
//
// Case 4:  We have found the last use of a VR, and we want to home VR to r1,
//          so after this use of VR, we need to inject an instruction to
//          restore r1 to its native value.
//
//          native use of VR  # VR will be replaced by r1.
//          restore slot --> r1  # Injected.
//
// Case 5:  We have found the definition of a VR in I, where VR is homed to r1.
//          Normally, this would be where we would think of injecting the save
//          of r1 to the slot, but because we want to share spill slots, we
//          won't inject any saves.
//
//          # Nothing to inject.
//          native def of VR  # VR will be replaced by r1.
//          ...
//          restore slot --> r1  (assumed)
//
// Case 6:  We're at the beginning of a fragment, and some registers are
//          assumed to be spilled by later instructions, but no-one (as of yet)
//          does the spilling. Therefore, we need to do the spilling.
//
//          save r1 --> slot.  # Injected.
//          ...
//          restore slot --> r1  (assumed)

// Used for scheduling fragment-local registers.
struct FragmentScheduler {
  typedef TinyMap<VirtualRegister, RegLocation,
                  arch::NUM_GENERAL_PURPOSE_REGISTERS>
          RegLocationMap;

  typedef TinyMap<VirtualRegister, VirtualRegister, 4> SaveRestorLocationMap;

  FragmentScheduler(void) = delete;

  explicit FragmentScheduler(SSAFragment *frag_)
      : vr_locations(),
        save_restore_slots(),
        invalid_location{VirtualRegister(), RegLocationType::GPR},
        frag(frag_),
        reg_counts() {

    reg_counts.CountGPRUses(frag);
    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      auto gpr = NthArchGPR(i);
      gpr_locations[i] = {gpr, RegLocationType::GPR};
      inverse_gpr_locations[i] = {gpr, RegLocationType::GPR};
    }
    for (auto &slot : slot_is_available) slot = true;
  }

  ~FragmentScheduler(void) {
    auto partition = frag->partition.Value();
    partition->num_slots = std::max(partition->num_slots,
                                    frag->spill.num_slots);
  }

  // Allocates a spill slot for use.
  VirtualRegister AllocateSlot(void) {
    auto slot = 0;
    for (auto i = 0; i < arch::MAX_NUM_SPILL_SLOTS; ++i) {
      if (slot_is_available[i]) {
        slot_is_available[i] = false;
        slot = i;
        break;
      }
    }
    auto slot_id = frag->spill.num_partition_slots + slot;
    frag->spill.num_slots = std::max(frag->spill.num_slots, slot_id + 1);
    return NthSpillSlot(slot_id);
  }

  // Current location of a GPR (arch GPR or VR).
  RegLocation &Loc(VirtualRegister reg) {
    if (reg.IsNative()) {
      if (reg.IsGeneralPurpose()) {
        return gpr_locations[reg.Number()];
      }
    } else if (reg.IsVirtual()) {
      auto &vr(vr_locations[reg]);
      if (!vr.loc.IsValid()) {  // Never seen before.
        vr.type = RegLocationType::SLOT;
      }
      return vr;
    }
    GRANARY_ASSERT(false);
    return invalid_location;
  }

  // Delete a location. This is only used for virtual registers.
  void DeleteLoc(VirtualRegister reg) {
    GRANARY_ASSERT(reg.IsVirtual());
    vr_locations.Remove(reg);
  }

  // The inverse location of an arch GPR. That is, this tells us *who* is
  // currently located in `reg`. In practice, this is used to figure out what
  // virtual register is homed to a specific GPR.
  RegLocation &InverseLoc(VirtualRegister reg) {
    GRANARY_ASSERT(reg.IsNative() && reg.IsGeneralPurpose());
    return inverse_gpr_locations[reg.Number()];
  }

  // Tries to get a GPR that can be spill shared and isn't used by the current
  // instruction. Returns `true` if a register is found and updates `reg`,
  // otherwise returns `false`.
  bool TryGetSharedGPR(const UsedRegisterSet &avoid_reg_set,
                       VirtualRegister *reg) {
    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      auto &loc(gpr_locations[i]);
      if (RegLocationType::LIVE_SLOT == loc.type && avoid_reg_set.IsDead(i)) {
        *reg = NthArchGPR(i);
        return true;
      }
    }
    return false;
  }

  // Get some GPR for use, so long as the GPR is not part of the
  // `avoid_reg_set`.
  VirtualRegister GetGPR(const UsedRegisterSet &avoid_reg_set) {
    GRANARY_IF_DEBUG( auto found_reg = false; )
    auto min_gpr_num = static_cast<int>(arch::NUM_GENERAL_PURPOSE_REGISTERS);
    auto min_num_uses = std::numeric_limits<int>::max();

    for (auto i = 0; i < arch::NUM_GENERAL_PURPOSE_REGISTERS; ++i) {
      if (avoid_reg_set.IsLive(i)) continue;
      if (!gpr_locations[i].loc.IsNative()) continue;
      if (reg_counts.num_uses_of_gpr[i] <= min_num_uses) {
        GRANARY_IF_DEBUG( found_reg = true; )
        min_gpr_num = i;
        min_num_uses = reg_counts.num_uses_of_gpr[i];
      }
    }
    GRANARY_ASSERT(found_reg);
    return NthArchGPR(min_gpr_num);
  }

  // Location of all arch GPRs.
  RegLocation gpr_locations[arch::NUM_GENERAL_PURPOSE_REGISTERS];
  RegLocation inverse_gpr_locations[arch::NUM_GENERAL_PURPOSE_REGISTERS];

  // Location of all currently live / interfering virtual registers.
  RegLocationMap vr_locations;

  // Slots for register save/restore.
  SaveRestorLocationMap save_restore_slots;

  // Dummy location.
  RegLocation invalid_location;

  // Fragment being scheduled.
  SSAFragment *frag;

  // Counts of the usage of registers in `frag`.
  RegisterUsageCounter reg_counts;

  // Used for finding spill slots.
  bool slot_is_available[arch::MAX_NUM_SPILL_SLOTS];
};

// Schedule a fragment-local register use.
static void ScheduleFragmentLocalUse(FragmentScheduler *sched,
                                     SSAOperand &op,
                                     NativeInstruction *instr,
                                     const UsedRegisterSet &used_regs) {
  auto node = op.nodes[0];
  auto vr = node->reg;
  auto frag = sched->frag;
  if (!vr.IsVirtual()) return;  // Ignore arch GPRs.
  if (NODE_SCHEDULED == node->id.Value()) return;
  GRANARY_ASSERT(!IsLive(frag->ssa.entry_nodes, vr, node->id));
  GRANARY_ASSERT(!IsLive(frag->ssa.exit_nodes, vr, node->id));

  auto &vr_home(sched->Loc(vr));
  GRANARY_ASSERT(RegLocationType::LIVE_SLOT != vr_home.type);
  if (RegLocationType::SLOT == vr_home.type) {
    VirtualRegister agpr;
    VirtualRegister slot;

    // Case 3. We can share this VR's spill slot with another.
    if (sched->TryGetSharedGPR(used_regs, &agpr)) {
      auto &agpr_home(sched->Loc(agpr));
      GRANARY_ASSERT(RegLocationType::LIVE_SLOT == agpr_home.type);
      slot = agpr_home.loc;
      GRANARY_ASSERT(slot.IsVirtualSlot());

    // Case 4. We have to allocate a new slot, and arrange for the register
    // that `vr` will occupy to be filled.
    } else {
      agpr = sched->GetGPR(used_regs);
      slot = sched->AllocateSlot();
      GRANARY_ASSERT(sched->InverseLoc(agpr).loc == agpr);
      frag->instrs.InsertAfter(instr, arch::RestoreGPRFromSlot(agpr, slot));
    }

    sched->Loc(agpr) = {slot, RegLocationType::SLOT};
    vr_home = {agpr, RegLocationType::GPR};  // `vr_home == sched->Loc(vr)`.
    sched->InverseLoc(agpr) = {vr, RegLocationType::GPR};
  }

  GRANARY_ASSERT(RegLocationType::GPR == vr_home.type);
  ReplaceOperandReg(op, vr_home.loc);
}

// Case 5: Schedule a fragment-local register definition. This enables slot
// sharing within the fragment.
static void ScheduleFragmentLocalDef(FragmentScheduler *sched, SSAOperand &op) {
  auto node = op.nodes[0];
  auto vr = node->reg;
  if (!vr.IsVirtual()) return;  // Ignore arch GPRs.
  if (NODE_SCHEDULED == node->id.Value()) return;
  GRANARY_IF_DEBUG( auto frag = sched->frag; )
  GRANARY_ASSERT(!IsLive(frag->ssa.entry_nodes, vr, node->id));
  GRANARY_ASSERT(!IsLive(frag->ssa.exit_nodes, vr, node->id));

  auto &vr_home(sched->Loc(vr));
  GRANARY_ASSERT(RegLocationType::GPR == vr_home.type);

  auto gpr_homed_by_vr = vr_home.loc;
  GRANARY_ASSERT(gpr_homed_by_vr.IsNative());

  auto &gpr_home(sched->Loc(gpr_homed_by_vr));
  GRANARY_ASSERT(RegLocationType::SLOT == gpr_home.type);
  GRANARY_ASSERT(gpr_home.loc.IsVirtualSlot());

  auto &inv_gpr_home(sched->InverseLoc(gpr_homed_by_vr));
  GRANARY_ASSERT(inv_gpr_home.loc == vr);

  vr_home = {VirtualRegister(), RegLocationType::GPR}; // Reset.
  sched->DeleteLoc(vr);  // Kill `vr_home`.

  // Enable slot sharing (default).
  gpr_home.type = RegLocationType::LIVE_SLOT;  // Enables slot-sharing.
  inv_gpr_home.loc = VirtualRegister();  // Will help signal an error.
  inv_gpr_home.type = RegLocationType::GPR;
}

// Home a specific used register.
static void HomeUsedReg(FragmentScheduler *sched, Instruction *instr,
                        VirtualRegister gpr,
                        const UsedRegisterSet &avoid_regs) {
  auto frag = sched->frag;
  auto &gpr_home(sched->Loc(gpr));
  auto &inv_gpr_home(sched->InverseLoc(gpr));

  if (RegLocationType::GPR == gpr_home.type) {
    GRANARY_ASSERT(gpr_home.loc == gpr);
    return;

  // Case 1. After this instruction, the VR that occupied this GPR is
  // dead, but we left the GPR alive in its spill slot because we want to
  // do slot sharing. Now we need to insert the initial save of the GPR
  // to its slot.
  } else if (RegLocationType::LIVE_SLOT == gpr_home.type) {
    frag->instrs.InsertAfter(instr, arch::SaveGPRToSlot(gpr, gpr_home.loc));
    gpr_home.loc = gpr;
    gpr_home.type = RegLocationType::GPR;
    inv_gpr_home = {gpr, RegLocationType::GPR};

  // Case 2: After this instruction, the GPR is located in a spill slot,
  // and the VR occupying this GPR after this instruction is live at this
  // instruction.
  } else {
    auto vr_occupying_gpr = inv_gpr_home.loc;
    GRANARY_ASSERT(vr_occupying_gpr.IsVirtual());

    auto &vr_home(sched->Loc(vr_occupying_gpr));
    GRANARY_ASSERT(vr_home.loc == gpr);

    auto agpr = sched->GetGPR(avoid_regs);
    auto slot = gpr_home.loc;

    GRANARY_ASSERT(agpr != gpr);
    frag->instrs.InsertAfter(instr, arch::SwapGPRWithSlot(agpr, slot)); // 2.
    frag->instrs.InsertAfter(instr, arch::SwapGPRWithGPR(agpr, gpr));  // 1.

    vr_home = {agpr, RegLocationType::GPR};  // Loc(vr_occupying_gpr).
    gpr_home = {gpr, RegLocationType::GPR};  // Loc(gpr).
    sched->Loc(agpr) = {slot, RegLocationType::SLOT};

    inv_gpr_home = {gpr, RegLocationType::GPR};  // InverseLoc(gpr).
    sched->InverseLoc(agpr) = {vr_occupying_gpr, RegLocationType::GPR};
  }
}

// Home the GPRs from the set `used_set`, but ensure that none of the registers
// from the set `avoid_set` are used as part of that homing.
//
// Note: This assumes that `used_regs` is a subset of `avoid_regs`.
static void HomeUsedRegs(FragmentScheduler *sched, Instruction *instr,
                         const UsedRegisterSet &used_regs,
                         const UsedRegisterSet &avoid_regs) {
  for (auto gpr : used_regs) {
    HomeUsedReg(sched, instr, gpr, avoid_regs);
  }
}

// Try to eliminate a redundant copy instruction.
static bool TryRemoveCopyInstruction(FragmentScheduler *sched,
                                     NativeInstruction *instr,
                                     SSAInstruction *ssa_instr) {
  if (!arch::GetCopiedOperand(instr, ssa_instr)) return false;

  // Ignore nodes that were scheduled by the partition-local scheduler.
  auto dest_node = ssa_instr->defs[0].nodes[0];
  if (NODE_SCHEDULED == dest_node->id.Value()) return false;

  // For transparency, we'll only remove copies that we've likely introduced.
  auto dest_reg = dest_node->reg;
  if (!dest_reg.IsVirtual()) return false;

  if (sched->vr_locations.Exists(dest_reg)) {
    auto vr_home = sched->vr_locations[dest_reg];
    if (RegLocationType::SLOT != vr_home.type) return false;
    GRANARY_ASSERT(RegLocationType::LIVE_SLOT != vr_home.type);
  }

  auto ainstr = new AnnotationInstruction(kAnnotSSAElidedInstruction, dest_reg);
  SetMetaData(ainstr, ssa_instr);
  sched->frag->instrs.InsertAfter(instr, ainstr);

  Instruction::Unlink(instr);  // Will self-destruct.
  return true;
}

// Schedule registers defs/uses that are local to a fragment and contained
// in a specific native instruction.
static void ScheduleFragmentLocalRegs(FragmentScheduler *sched,
                                      NativeInstruction *instr) {
  auto ssa_instr = GetMetaData<SSAInstruction *>(instr);
  if (!ssa_instr) return;
  if (TryRemoveCopyInstruction(sched, instr, ssa_instr)) return;

  UsedRegisterSet used_regs;
  used_regs.Visit(instr);

  UsedRegisterSet avoid_regs(used_regs);
  if (instr->NumExplicitOperands()) {
    avoid_regs.ReviveRestrictedRegisters(instr);
  }

  HomeUsedRegs(sched, instr, used_regs, avoid_regs);

  for (auto &use_op : ssa_instr->uses) {
    ScheduleFragmentLocalUse(sched, use_op, instr, used_regs);
  }
  for (auto &def_op : ssa_instr->defs) {
    ScheduleFragmentLocalUse(sched, def_op, instr, used_regs);
  }
  for (auto &def_op : ssa_instr->defs) {
    ScheduleFragmentLocalDef(sched, def_op);
  }
}

// Schedule fragment local registers for a register save-to-spill or
// restore-from-spill instruction.
static void ScheduleFragmentLocalRegs(FragmentScheduler *sched,
                                      AnnotationInstruction *instr,
                                      bool is_save) {
  auto node = GetMetaData<SSANode *>(instr);
  auto gpr = node->reg;
  auto frag = sched->frag;
  if (NODE_SCHEDULED == node->id.Value()) return;
  GRANARY_ASSERT(gpr.IsNative());

  UsedRegisterSet used_regs;
  used_regs.Revive(gpr);
  auto &slot(sched->save_restore_slots[gpr]);

  if (is_save) {
    GRANARY_ASSERT(slot.IsVirtualSlot());
    auto &gpr_home(sched->Loc(gpr));
    auto &inv_gpr_home(sched->InverseLoc(gpr));

    // Enable slot sharing for register restores.
    gpr_home.type = RegLocationType::LIVE_SLOT;  // Enables slot-sharing.
    gpr_home.loc = slot;
    inv_gpr_home.loc = VirtualRegister();  // Will help signal an error.
    inv_gpr_home.type = RegLocationType::GPR;

    sched->save_restore_slots.Remove(gpr);

  } else {
    GRANARY_ASSERT(!slot.IsValid());

    // First, home the GPR *after* `instr` in case a later instruction had it
    // in a spill slot.
    HomeUsedRegs(sched, instr, used_regs, used_regs);

    // Now, insert a restore instruction.
    slot = sched->AllocateSlot();
    frag->instrs.InsertAfter(instr, arch::RestoreGPRFromSlot(gpr, slot));
  }
}

// Schedule registers defs/uses that are local to a fragment.
static void ScheduleFragmentLocalRegs(SSAFragment *frag) {
  FragmentScheduler sched(frag);
  Instruction *first_instr(nullptr);

  auto instr = frag->instrs.Last();
  for (Instruction *prev_instr(nullptr); instr; instr = prev_instr) {
    prev_instr = instr->Previous();

    if (auto ainstr = DynamicCast<AnnotationInstruction *>(instr)) {
      if (kAnnotSSAFragLocalBegin == ainstr->annotation) {
        first_instr = instr;
        break;
      } else if (kAnnotSSASaveRegister == ainstr->annotation) {
        ScheduleFragmentLocalRegs(&sched, ainstr, true);
      } else if (kAnnotSSARestoreRegister == ainstr->annotation) {
        ScheduleFragmentLocalRegs(&sched, ainstr, false);
      } else if (kAnnotSSAReviveRegisters == ainstr->annotation) {
        auto used_regs = ainstr->Data<UsedRegisterSet>();
        HomeUsedRegs(&sched, ainstr, used_regs, used_regs);
      }
    } else if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
      ScheduleFragmentLocalRegs(&sched, ninstr);
    }
  }

  UsedRegisterSet all_regs;
  all_regs.ReviveAll();
  for (auto gpr : all_regs) {
    auto &gpr_home(sched.Loc(gpr));
    if (RegLocationType::GPR == gpr_home.type) {
      GRANARY_ASSERT(gpr == gpr_home.loc);
      continue;
    }

    GRANARY_ASSERT(gpr_home.loc.IsVirtualSlot());

    // Case 6: Inject the original saves of the registers.
    frag->instrs.InsertBefore(first_instr,
                              arch::SaveGPRToSlot(gpr, gpr_home.loc));
  }
}

static void ScheduleFragmentLocalRegs(FragmentList *frags) {
  for (auto frag : FragmentListIterator(frags)) {
    if (auto ssa_frag = DynamicCast<SSAFragment *>(frag)) {
      if (auto comp_frag = DynamicCast<CodeFragment *>(frag)) {
        // Skip compensation fragments.
        if (comp_frag->attr.is_compensation_code) continue;
      }
      ScheduleFragmentLocalRegs(ssa_frag);
    }
  }
}

// Updates all `SSAFragment`s so that they know the slot counts of their
// respective partitions.
static void UpdateFragmentsWithSlotCounts(FragmentList *frags) {
  for (auto frag : FragmentListIterator(frags)) {
    if (auto ssa_frag = DynamicCast<SSAFragment *>(frag)) {
      auto partition = ssa_frag->partition.Value();
      ssa_frag->spill.num_slots = partition->num_slots;
      ssa_frag->spill.num_partition_slots = partition->num_slots;
    }
  }
}

// Returns the first potential SSA-related instruction of a fragment.
static Instruction *FirstSSAInstruction(SSAFragment *frag) {
  Instruction *candidate(nullptr);
  for (auto instr : InstructionListIterator(frag->instrs)) {
    if (auto ninstr = DynamicCast<NativeInstruction *>(instr)) {
      candidate = ninstr;

      // Implies that this instruction has an associated `SSAInstruction`.
      if (ninstr->MetaData()) {
        return ninstr;
      }
    } else if (auto ainstr = DynamicCast<AnnotationInstruction *>(instr)) {
      switch (ainstr->annotation) {
        case kAnnotSSANodeOwner:
        case kAnnotSSANodeKill:
        case kAnnotSSASaveRegister:
        case kAnnotSSARestoreRegister:
          return ainstr;
        default:
          break;
      }
    }
  }
  return candidate;
}

// Add annotations to the fragment that marks the "beginning" of the fragment
// to the register scheduler. This is so that we can know where the first
// spills need to be placed, as well as knowing how to order local and global
// spills.
static void AddFragBeginAnnotations(FragmentList *frags) {
  for (auto frag : FragmentListIterator(frags)) {
    if (auto ssa_frag = DynamicCast<SSAFragment *>(frag)) {
      auto local_annot = new AnnotationInstruction(kAnnotSSAFragLocalBegin);
      auto global_annot = new AnnotationInstruction(kAnnotSSAPartitionLocalBegin);
      if (auto first = FirstSSAInstruction(ssa_frag)) {
        frag->instrs.InsertBefore(first, global_annot);
        frag->instrs.InsertBefore(first, local_annot);
      } else {
        ssa_frag->instrs.Prepend(local_annot);
        ssa_frag->instrs.Prepend(global_annot);
      }
    }
  }
}

}  // namespace

// Schedule virtual registers.
void ScheduleRegisters(FragmentList *frags) {
  AddFragBeginAnnotations(frags);
  SchedulePartitionLocalRegs(frags);
  UpdateFragmentsWithSlotCounts(frags);
  ScheduleFragmentLocalRegs(frags);
  FreeSSAData(frags);
  FreeFlagZones(frags);
}

}  // namespace granary
