/* Copyright 2014 Peter Goodman, all rights reserved. */


#ifndef ARCH_OPERAND_H_
#define ARCH_OPERAND_H_

#ifndef GRANARY_INTERNAL
# error "This code is internal to Granary."
#endif

#include "granary/cfg/operand.h"

namespace granary {
namespace arch {

// Documents the methods that must be provided by driver operands. This
// interface class cannot be used as-is as the methods don't exist.
class alignas(16) OperandInterface {
 public:
  bool IsValid(void) const;
  bool IsRead(void) const;
  bool IsWrite(void) const;
  bool IsSemanticDefinition(void) const;
  bool IsConditionalRead(void) const;
  bool IsConditionalWrite(void) const;
  bool IsRegister(void) const;
  bool IsMemory(void) const;
  bool IsCompoundMemory(void) const;
  bool IsEffectiveAddress(void) const;
  bool IsPointer(void) const;
  bool IsImmediate(void) const;
  bool IsBranchTarget(void) const;
  bool IsExplicit(void) const;

  size_t ByteWidth(void) const;
  size_t BitWidth(void) const;

  void EncodeToString(OperandString *str) const;
};

}  // namespace arch
}  // namespace granary

#endif  // ARCH_OPERAND_H_
