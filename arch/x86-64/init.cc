/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL

#include "granary/base/base.h"

#include "arch/x86-64/xed.h"
#include "arch/x86-64/operand.h"
#include "arch/x86-64/builder.h"

#include "granary/breakpoint.h"

#include "os/memory.h"

namespace granary {
namespace arch {

// Decoder state that sets the mode to 64-bit.
xed_state_t XED_STATE;

// Table of all implicit operands.
const Operand *IMPLICIT_OPERANDS[XED_MAX_INST_TABLE_NODES] = {nullptr};

// Number of implicit operands for each iform.
int NUM_IMPLICIT_OPERANDS[XED_MAX_INST_TABLE_NODES] = {0};

// Categories of every iclass.
xed_category_enum_t ICLASS_CATEGORIES[XED_ICLASS_LAST] = {XED_CATEGORY_INVALID};

// Table to find the instruction selections for each iclass.
const xed_inst_t *ICLASS_SELECTIONS[XED_ICLASS_LAST] = {nullptr};
const xed_inst_t *LAST_ICLASS_SELECTION = nullptr;

// Table mapping each iclass/iform to the set of read and written flags by *any*
// selection of that iclass/iform.
FlagsSet ICLASS_FLAGS[XED_ICLASS_LAST];
FlagsSet IFORM_FLAGS[XED_IFORM_LAST];

namespace {

// Number of pages allocates to hold the table of implicit operands.
static int num_implicit_operand_pages = 0;

// Initialize the table of iclass categories.
static void InitIclassTables(void) {
  LAST_ICLASS_SELECTION = xed_inst_table_base() + XED_MAX_INST_TABLE_NODES;
  for (auto sel = 0; sel < XED_MAX_INST_TABLE_NODES; ++sel) {
    auto instr = xed_inst_table_base() + sel;
    auto iclass = xed_inst_iclass(instr);
    if (!ICLASS_SELECTIONS[iclass]) {
      auto category = xed_inst_category(instr);
      ICLASS_SELECTIONS[iclass] = instr;
      ICLASS_CATEGORIES[iclass] = category;
    }
  }
}

// Initialize the table of iclass flags.
static void InitIclassFlags(void) {
  xed_decoded_inst_t xedd;
  for (auto sel = 0; sel < XED_MAX_INST_TABLE_NODES; ++sel) {
    auto xedi = xed_inst_table_base() + sel;
    auto iclass = xed_inst_iclass(xedi);
    memset(&xedd, 0, sizeof xedd);
    xedd._inst = xedi;
    if (auto flags = xed_decoded_inst_get_rflags_info(&xedd)) {
      auto &iclass_flags(ICLASS_FLAGS[iclass]);
      iclass_flags.read.flat |= flags->read.flat;
      iclass_flags.written.flat |= flags->written.flat;

      // Turns conditionally written flags into read flags.
      if (flags->may_write) {
        iclass_flags.read.flat |= flags->written.flat;
      }
    }
  }
}

// Initialize the table of iform flags.
static void InitIformFlags(void) {
  xed_decoded_inst_t xedd;
  for (auto sel = 0; sel < XED_MAX_INST_TABLE_NODES; ++sel) {
    auto xedi = xed_inst_table_base() + sel;
    auto iform = xed_inst_iform_enum(xedi);
    auto &iform_flags(IFORM_FLAGS[iform]);
    memset(&xedd, 0, sizeof xedd);
    xedd._inst = xedi;
    if (auto flags = xed_decoded_inst_get_rflags_info(&xedd)) {
      iform_flags.read.flat |= flags->read.flat;
      iform_flags.written.flat |= flags->written.flat;

      // Turns conditionally written flags into read flags.
      if (flags->may_write) {
        iform_flags.read.flat |= flags->written.flat;
      }
    }
#if 0
    // The little hack above doesn't tend to work for instructions that use the
    // `_flag_complex` field of `xed_inst_t`, is it can lead to doing an actual
    // lookup of the decoded operands (of which none are provided!). We fall
    // back on hoping that we've managed to get some simple flags via the iclass
    // in a prior step.
    if (auto num_ops = xed_inst_noperands(xedi)) {
      auto last_op = xed_inst_operand(xedi, num_ops - 1);
      auto last_op_type = xed_operand_type(last_op);
      auto nt_name = xed_operand_nonterminal_name(last_op);

      // Make sure that conditional writes of the flags are treated as reads.
      if (XED_OPERAND_TYPE_NT_LOOKUP_FN == last_op_type &&
          XED_NONTERMINAL_RFLAGS == nt_name) {
        iform_flags = ICLASS_FLAGS[xed_inst_iclass(xedi)];
        auto rw = xed_operand_rw(last_op);
        if (XED_OPERAND_ACTION_CW == rw || XED_OPERAND_ACTION_RCW == rw) {
          iform_flags.read.flat |= iform_flags.written.flat;
        }
      }
    }
#endif
  }
}

// Invoke a function one every implicit operand of each iclass.
static void ForEachImplicitOperand(
    const std::function<void(const xed_inst_t *, const xed_operand_t *,
                             unsigned, unsigned)> &cb) {
  for (auto isel = 0U; isel < XED_MAX_INST_TABLE_NODES; ++isel) {
    auto instr = xed_inst_table_base() + isel;
    auto iform = xed_inst_iform_enum(instr);
    if (XED_IFORM_INVALID == iform) continue;

    auto iclass = xed_inst_iclass(instr);
    auto num_ops = xed_inst_noperands(instr);
    for (auto i = 0U; i < num_ops; ++i) {
      auto op = xed_inst_operand(instr, i);
      if (XED_OPVIS_EXPLICIT != xed_operand_operand_visibility(op) &&
          !IsAmbiguousOperand(iclass, iform, i)) {
        cb(instr, op, i, isel);
      }
    }
  }
}

// Returns the total number of implicit operands.
static size_t CountImplicitOperands(void) {
  size_t num_implicit_ops(0);
  auto func = [&] (const xed_inst_t *, const xed_operand_t *, unsigned,
                   unsigned isel) {
    GRANARY_IF_DEBUG( auto new_num_ops = ) ++NUM_IMPLICIT_OPERANDS[isel];
    GRANARY_ASSERT(11 >= new_num_ops);  // Max case is `PUSHAD`.
    ++num_implicit_ops;
  };
  ForEachImplicitOperand(std::cref(func));
  return num_implicit_ops;
}

// Allocate the implicit operands.
static Operand *AllocateImplicitOperands(void) {
  auto num_implicit_ops = CountImplicitOperands();
  auto ops_mem_size = num_implicit_ops * sizeof(Operand);
  auto aligned_ops_mem_size = GRANARY_ALIGN_TO(ops_mem_size,
                                               arch::PAGE_SIZE_BYTES);
  num_implicit_operand_pages = static_cast<int>(
      aligned_ops_mem_size / arch::PAGE_SIZE_BYTES);
  auto ops_mem_raw = os::AllocatePages(num_implicit_operand_pages);
  return reinterpret_cast<Operand *>(ops_mem_raw);
}

// Fill in an operand as if it's a register operand.
static void FillRegisterOperand(Operand *instr_op, xed_reg_enum_t reg) {
  instr_op->type = XED_ENCODER_OPERAND_TYPE_REG;
  instr_op->reg.DecodeFromNative(reg);
  instr_op->width = static_cast<int16_t>(instr_op->reg.BitWidth());
  instr_op->is_sticky = true;
}

// Address operands are usually used either directly as register operands (REG0
// or REG1), or as the register component of a separate memory operand (BASE0,
// BASE1).
static void FillAddressOperand(Operand *instr_op, xed_reg_enum_t reg) {
  FillRegisterOperand(instr_op, reg);
}

// Convert a non-terminal operand into a Granary operand. This will sometimes
// cheat by converting non-terminal operands into a close-enough representation
// that benefits other parts of Granary (e.g. the virtual register system). Not
// all non-terminal operands have a decoding that Granary cares about.
static bool ConvertNonTerminalOperand(Operand *instr_op,
                                      const xed_operand_t *op) {
  switch (xed_operand_nonterminal_name(op)) {
    case XED_NONTERMINAL_AR10:
      FillAddressOperand(instr_op, XED_REG_R10); return true;
    case XED_NONTERMINAL_AR11:
      FillAddressOperand(instr_op, XED_REG_R11); return true;
    case XED_NONTERMINAL_AR12:
      FillAddressOperand(instr_op, XED_REG_R12); return true;
    case XED_NONTERMINAL_AR13:
      FillAddressOperand(instr_op, XED_REG_R13); return true;
    case XED_NONTERMINAL_AR14:
      FillAddressOperand(instr_op, XED_REG_R14); return true;
    case XED_NONTERMINAL_AR15:
      FillAddressOperand(instr_op, XED_REG_R15); return true;
    case XED_NONTERMINAL_AR8:
      FillAddressOperand(instr_op, XED_REG_R8); return true;
    case XED_NONTERMINAL_AR9:
      FillAddressOperand(instr_op, XED_REG_R9); return true;
    case XED_NONTERMINAL_ARAX:
      FillAddressOperand(instr_op, XED_REG_RAX); return true;
    case XED_NONTERMINAL_ARBP:
      FillAddressOperand(instr_op, XED_REG_RBP); return true;
    case XED_NONTERMINAL_ARBX:
      FillAddressOperand(instr_op, XED_REG_RBX); return true;
    case XED_NONTERMINAL_ARCX:
      FillAddressOperand(instr_op, XED_REG_RCX); return true;
    case XED_NONTERMINAL_ARDI:
      FillAddressOperand(instr_op, XED_REG_RDI); return true;
    case XED_NONTERMINAL_ARDX:
      FillAddressOperand(instr_op, XED_REG_RDX); return true;
    case XED_NONTERMINAL_ARSI:
      FillAddressOperand(instr_op, XED_REG_RSI); return true;
    case XED_NONTERMINAL_ARSP:  // Address with RSP.
      FillAddressOperand(instr_op, XED_REG_RSP); return true;
    case XED_NONTERMINAL_OEAX:
      FillRegisterOperand(instr_op, XED_REG_EAX); return true;
    case XED_NONTERMINAL_ORAX:
      FillRegisterOperand(instr_op, XED_REG_RAX); return true;
    case XED_NONTERMINAL_ORBP:
      FillRegisterOperand(instr_op, XED_REG_RBP); return true;
    case XED_NONTERMINAL_ORDX:  // Output to RDX, e.g. in MUL_GPRv.
      FillRegisterOperand(instr_op, XED_REG_RDX); return true;
    case XED_NONTERMINAL_ORSP:  // Output to RSP.
      FillRegisterOperand(instr_op, XED_REG_RSP); return true;
    case XED_NONTERMINAL_RIP:
      FillRegisterOperand(instr_op, XED_REG_RIP); return true;
    case XED_NONTERMINAL_SRBP:
      FillRegisterOperand(instr_op, XED_REG_RBP); return true;
    case XED_NONTERMINAL_SRSP:  // Shift RSP?
      FillRegisterOperand(instr_op, XED_REG_RSP); return true;
    case XED_NONTERMINAL_RFLAGS:
      FillRegisterOperand(instr_op, XED_REG_RFLAGS); return true;
    default:
      GRANARY_ASSERT(false);
      return false;
  }
}

// Initializes an implicit operand.
static void InitImplicitOperand(const xed_inst_t *instr,
                                const xed_operand_t *op, Operand *instr_op,
                                unsigned i) {
  auto op_name = xed_operand_name(op);
  auto op_type = xed_operand_type(op);
  memset(instr_op, 0, sizeof *instr_op);
  if (XED_OPERAND_TYPE_NT_LOOKUP_FN == op_type) {
    ConvertNonTerminalOperand(instr_op, op);
  } else if (xed_operand_is_register(op_name)) {
    FillRegisterOperand(instr_op, xed_operand_reg(op));
  } else if (XED_OPERAND_MEM0 == op_name || XED_OPERAND_MEM1 == op_name) {
    auto base01_op = xed_inst_operand(instr, i + 1);
    ConvertNonTerminalOperand(instr_op, base01_op);
    instr_op->type = XED_ENCODER_OPERAND_TYPE_MEM;
  } else {
    GRANARY_ASSERT(false);
  }
  instr_op->is_sticky = true;
  instr_op->rw = xed_operand_rw(op);
}

// Initializes the implicit operands in the table.
static void InitImplicitOperands(Operand *op) {
  auto func = [&] (const xed_inst_t *instr, const xed_operand_t *instr_op,
                   unsigned i, unsigned isel) {
    InitImplicitOperand(instr, instr_op, op, i);

    // Initialize the first implicit operand for this iform and move to
    // initialize the next operand.
    if (!IMPLICIT_OPERANDS[isel]) {
      IMPLICIT_OPERANDS[isel] = op;
    }
    op++;
  };
  ForEachImplicitOperand(std::cref(func));
}

// Initialize a table of implicit operands.
//
// TODO(pag): These tables could likely be compressed by quite a bit.
static void InitOperandTables(void) {
  auto ops = AllocateImplicitOperands();
  InitImplicitOperands(ops);
  os::ProtectPages(ops, num_implicit_operand_pages,
                   os::MemoryProtection::READ_ONLY);
}

static bool arch_is_initialized = false;

}  // namespace

// Initialize the driver (instruction encoder/decoder).
void Init(void) {
  if (!arch_is_initialized) {
    arch_is_initialized = true;
    xed_tables_init();
    xed_state_zero(&XED_STATE);
    xed_state_init(&XED_STATE, XED_MACHINE_MODE_LONG_64,
                   XED_ADDRESS_WIDTH_64b, XED_ADDRESS_WIDTH_64b);
    InitIclassTables();
    InitIclassFlags();
    InitIformFlags();
    InitOperandTables();
  }
}

}  // namespace arch
}  // namespace granary
