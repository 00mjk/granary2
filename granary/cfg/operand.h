/* Copyright 2014 Peter Goodman, all rights reserved. */

#ifndef GRANARY_CFG_OPERAND_H_
#define GRANARY_CFG_OPERAND_H_

#include "granary/base/cast.h"
#include "granary/base/container.h"
#include "granary/base/string.h"
#include "granary/base/type_trait.h"

#include "granary/code/register.h"

namespace granary {

// Forward declarations.
class AnnotationInstruction;
class DecodedBlock;
class NativeInstruction;
class Operand;
class MemoryOperand;
class RegisterOperand;
class ImmediateOperand;
class LabelOperand;
class LabelInstruction;

namespace arch {
class Operand;
}  // namespace arch

// Type of a string that can be used to convert an operand to a string.
typedef FixedLengthString<31> OperandString;

// A generic operand from a native instruction. A generic interface is provided
// so that operands can be iterated.
class Operand {
 public:
  inline Operand(void)
      : op(),
        op_ptr(nullptr) {}

  Operand(const Operand &that);

  // Move semantics transfers referenceability.
  Operand(Operand &&that) = default;

  // Initialize this operand.
  GRANARY_INTERNAL_DEFINITION Operand(arch::Operand *op_);

  // Initialize this operand.
  GRANARY_INTERNAL_DEFINITION Operand(const arch::Operand *op_);

  bool IsValid(void) const;
  bool IsRead(void) const;
  bool IsWrite(void) const;
  bool IsSemanticDefinition(void) const;
  bool IsConditionalRead(void) const;
  bool IsConditionalWrite(void) const;

  GRANARY_INTERNAL_DEFINITION bool IsRegister(void) const;
  GRANARY_INTERNAL_DEFINITION bool IsMemory(void) const;
  GRANARY_INTERNAL_DEFINITION bool IsImmediate(void) const;
  GRANARY_INTERNAL_DEFINITION bool IsLabel(void) const;

  // Conveniences.
  inline bool IsReadWrite(void) const {
    return IsRead() && IsWrite();
  }

  // Returns whether or not this operand can be replaced / modified.
  //
  // Note: This has a architecture-specific implementation.
  bool IsModifiable(void) const;

  // Returns whether or not this operand is explicit.
  //
  // Note: This is only valid on operands matched from instructions and not on
  //       manually created operands.
  //
  // Note: This has a architecture-specific implementation.
  bool IsExplicit(void) const;

  // Return the width (in bits) of this operand, or 0 if its width is not
  // known.
  //
  // Note: This has a architecture-specific implementation.
  size_t BitWidth(void) const;

  // Return the width (in bytes) of this operand, or 0 if its width is not
  // known.
  //
  // Note: This has a architecture-specific implementation.
  size_t ByteWidth(void) const;

  // Convert this operand into a string.
  //
  // Note: This has a architecture-specific implementation.
  void EncodeToString(OperandString *str) const;

  // Replace the internal operand memory. This method is "unsafe" insofar
  // as it assumes the caller is maintaining the invariant that the current
  // operand is being replaced with one that has the correct type.
  GRANARY_INTERNAL_DEFINITION void UnsafeReplace(arch::Operand *op_);
  GRANARY_INTERNAL_DEFINITION void UnsafeReplace(const arch::Operand *op_);

  // Returns a pointer to the internal, arch-specific memory operand that is
  // *internal* to this `Operand`.
  GRANARY_INTERNAL_DEFINITION
  const arch::Operand *Extract(void) const;

  // Returns a pointer to the internal, arch-specific memory operand that is
  // *referenced* by this `Operand`.
  GRANARY_INTERNAL_DEFINITION
  arch::Operand *UnsafeExtract(void) const;

  // Try to replace the current operand.
  //
  // Note: This has a architecture-specific implementation.
  GRANARY_INTERNAL_DEFINITION
  bool UnsafeTryReplaceOperand(const Operand &op);

 GRANARY_PROTECTED:
  GRANARY_CONST OpaqueContainer<arch::Operand, 32, 16> op;

  friend class OperandRef;

  // Pointer to the native operand that backs `op`. We don't allow any of the
  // `Operand`-derived classes to manipulate or use this backing pointer, nor do
  // we actually use this pointer to derive properties of the backing operand.
  // Instead, it is used only to make a generic `OperandRef`, with which one can
  // replace the backing operand.
  //
  // This has three states:
  //
  //    valid     - It points to an `arch::Operand` inside of an
  //                `arch::Instruction` that we can access and modify. In this
  //                case, `op` is a point-in-time copy of `*op_ptr`.
  //
  //    tombstone - `op` is either a point-in-time copy of a constant
  //                `arch::Operand` within an `arch::Instruction`, or it is a
  //                copy of another `Operand`, or was manually constructed.
  //
  //    nullptr   - `op` does not represent any kind of operand.
  //
  GRANARY_POINTER(arch::Operand) * GRANARY_CONST op_ptr;
};

// Represents a memory operand. Memory operands are either pointers (i.e.
// addresses to some location in memory) or register operands containing an
// address.
class MemoryOperand : public Operand {
 public:
  using Operand::Operand;
  inline MemoryOperand(void)
      : Operand() {}

  // Initialize a new memory operand from a virtual register, where the
  // referenced memory has a width of `num_bytes`.
  //
  // Note: This has a architecture-specific implementation.
  MemoryOperand(VirtualRegister ptr_reg, size_t num_bytes);

  // Generic initializer for a pointer to some data.
  template <typename T>
  inline explicit MemoryOperand(const T *ptr)
      : MemoryOperand(reinterpret_cast<const void *>(ptr),
                      GRANARY_MIN(8UL, sizeof(T))) {}

  // Initialize a new memory operand from a pointer, where the
  // referenced memory has a width of `num_bytes`.
  //
  // Note: This has a architecture-specific implementation.
  MemoryOperand(const void *ptr, size_t num_bytes);

  // Returns true if this is a compound memory operation. Compound memory
  // operations can have multiple smaller operands (e.g. registers) inside of
  // them. An example of a compound memory operand is a `base + index * scale`
  // (i.e. base/displacement) operand on x86.
  //
  // Note: This has a architecture-specific implementation.
  bool IsCompound(void) const;

  // Is this an effective address (instead of being an actual memory access).
  //
  // Note: This has a architecture-specific implementation.
  bool IsEffectiveAddress(void) const;

  // Try to match this memory operand as a pointer value.
  //
  // Note: This has a architecture-specific implementation.
  bool IsPointer(void) const;

  // Try to match this memory operand as a pointer value.
  //
  // Note: This has a architecture-specific implementation.
  bool MatchPointer(const void *&ptr) const;

  // Try to match this memory operand as a register value. That is, the address
  // is stored in the matched register.
  //
  // Note: This does not match segment registers.
  //
  // Note: This has a architecture-specific implementation.
  bool MatchRegister(VirtualRegister &reg) const;

  // Try to match a segment register.
  //
  // Note: This has a architecture-specific implementation.
  bool MatchSegmentRegister(VirtualRegister &reg) const;

  // Try to match several registers from the memory operand. This is applicable
  // when this is a compound memory operand, e.g. `base + index * scale`. This
  // also works when the memory operand is not compound.
  //
  // Note: This has a architecture-specific implementation.
  template <typename... VRs>
  inline size_t CountMatchedRegisters(VRs&... regs) const {
    return CountMatchedRegisters({&regs...});
  }

  // Try to match several registers from the memory operand. This is applicable
  // when this is a compound memory operand, e.g. `base + index * scale`. This
  // also works when the memory operand is not compound.
  //
  // Note: This has a architecture-specific implementation.
  size_t CountMatchedRegisters(
      std::initializer_list<VirtualRegister *> regs) const;

  // Tries to replace the memory operand.
  //
  // Note: This has a architecture-specific implementation.
  bool TryReplaceWith(const MemoryOperand &repl_op);
};

static_assert(sizeof(MemoryOperand) == sizeof(Operand),
              "`MemoryOperand`s must have the same size as `Operand`s.");

// Represents a register operand. This might be a general-purpose register, a
// non-general purpose architectural register, or a virtual register.
class RegisterOperand : public Operand {
 public:
  using Operand::Operand;
  inline RegisterOperand(void)
      : Operand() {}

  // Initialize a new register operand from a virtual register.
  //
  // Note: This has a architecture-specific implementation.
  explicit RegisterOperand(VirtualRegister reg);

  // Driver-specific implementations.
  bool IsNative(void) const;
  bool IsVirtual(void) const;
  bool IsStackPointer(void) const;
  bool IsStackPointerAlias(void) const;

  // Extract the register.
  //
  // Note: This has a architecture-specific implementation.
  VirtualRegister Register(void) const;

  // Tries to replace the register operand.
  //
  // Note: This has a architecture-specific implementation.
  bool TryReplaceWith(const RegisterOperand &repl_op);
};

static_assert(sizeof(RegisterOperand) == sizeof(Operand),
              "`RegisterOperand`s must have the same size as `Operand`s.");

// Represents an immediate integer operand.
class ImmediateOperand : public Operand {
 public:
  using Operand::Operand;
  inline ImmediateOperand(void)
      : Operand() {}

  // Initialize a immediate operand from a signed integer, where the value has
  // a width of `width_bytes`.
  //
  // Note: This has a architecture-specific implementation.
  ImmediateOperand(intptr_t imm, size_t width_bytes);

  // Initialize a immediate operand from a unsigned integer, where the value
  // has a width of `width_bytes`.
  //
  // Note: This has a architecture-specific implementation.
  ImmediateOperand(uintptr_t imm, size_t width_bytes);


  template <typename T, typename EnableIf<IsPointer<T>::RESULT>::Type=0>
  inline explicit ImmediateOperand(T ptr)
      : ImmediateOperand(reinterpret_cast<uintptr_t>(ptr),
                         arch::ADDRESS_WIDTH_BYTES) {}

  template <typename T, typename EnableIf<IsSignedInteger<T>::RESULT>::Type=0>
  inline explicit ImmediateOperand(T imm)
      : ImmediateOperand(static_cast<intptr_t>(imm), sizeof(T)) {}

  template <typename T, typename EnableIf<IsUnsignedInteger<T>::RESULT>::Type=0>
  inline explicit ImmediateOperand(T imm)
      : ImmediateOperand(static_cast<uintptr_t>(imm), sizeof(T)) {}

  // Extract the value as an unsigned integer.
  //
  // Note: This has a architecture-specific implementation.
  uint64_t UInt(void);

  // Extract the value as a signed integer.
  //
  // Note: This has a architecture-specific implementation.
  int64_t Int(void);

  // Tries to replace the immediate operand.
  //
  // Note: This has a architecture-specific implementation.
  bool TryReplaceWith(const ImmediateOperand &repl_op);
};

static_assert(sizeof(ImmediateOperand) == sizeof(Operand),
              "`ImmediateOperand`s must have the same size as `Operand`s.");

// Represents a label that can be used as the target of a branch.
class LabelOperand : public Operand {
 public:
  using Operand::Operand;

  // Initialize a label operand from a non-null pointer to a label.
  //
  // Note: This has a architecture-specific implementation.
  explicit LabelOperand(LabelInstruction *label);

  // Target of a label operand.
  //
  // Note: This has a architecture-specific implementation.
  AnnotationInstruction *Target(void) const;

 private:
  LabelOperand(void) = delete;
};

static_assert(sizeof(LabelOperand) == sizeof(Operand),
              "`LabelOperand`s must have the same size as `Operand`s.");

// High-level operand actions. Underneath these high-level actions we can
// specialize to different types of reads and write with:
//
//    Read        -> Conditional Read (IsConditionalRead)
//    Write       -> Conditional Write (IsConditionalWrite)
//    Read/Write  -> Read and conditionally written (IsConditionalWrite)
//    Read/Write  -> Conditionally read, always written (IsConditionalRead)
//
// To prevent ambiguities when matching, e.g. attempting to match the same
// Read/Write operand with two separate match operands, we make Read/Write
// operands explicit, such that a Read(...) can't match against a Read/Write
// operand.
enum OperandAction : uint8_t {
  kOperandActionAny,
  kOperandActionRead,
  kOperandActionWrite,
  kOperandActionReadOnly,
  kOperandActionWriteOnly,
  kOperandActionReadWrite
};

enum OperandConstraint : uint8_t {
  kOperandConstraintMatch,
  kOperandConstraintBind
};

enum OperandType : uint8_t {
  kOperandTypeRegister,
  kOperandTypeMemory,
  kOperandTypeImmediate
};

// Figure out the type of this operand.
//
// Note: We don't allow binding or matching with `LabelOperand`s.
namespace detail {
template <typename T> struct OperandTypeGetter;

template <>
struct OperandTypeGetter<RegisterOperand> {
  static constexpr OperandType kType = kOperandTypeRegister;
};

template <>
struct OperandTypeGetter<MemoryOperand> {
  static constexpr OperandType kType = kOperandTypeMemory;
};

template <>
struct OperandTypeGetter<ImmediateOperand> {
  static constexpr OperandType kType = kOperandTypeImmediate;
};
}  // namespace detail

// Generic operand matcher.
class OperandMatcher {
 public:
  Operand * GRANARY_CONST op;
  const OperandAction action;
  const OperandConstraint constraint;
  const OperandType type;
};

// Returns an operand matcher against an operand that is read.
template <typename T>
inline static OperandMatcher ReadFrom(T &op) {
  return {&op, kOperandActionRead, kOperandConstraintBind,
          detail::OperandTypeGetter<T>::kType};
}

// Returns an operand matcher against an operand that is only read.
template <typename T>
inline static OperandMatcher ReadOnlyFrom(T &op) {
  return {&op, kOperandActionReadOnly, kOperandConstraintBind,
          detail::OperandTypeGetter<T>::kType};
}

// Returns an operand matcher against an operand that is written.
template <typename T>
inline static OperandMatcher WriteTo(T &op) {
  return {&op, kOperandActionWrite, kOperandConstraintBind,
          detail::OperandTypeGetter<T>::kType};
}

// Returns an operand matcher against an operand that is only written.
template <typename T>
inline static OperandMatcher WriteOnlyTo(T &op) {
  return {&op, kOperandActionWriteOnly, kOperandConstraintBind,
          detail::OperandTypeGetter<T>::kType};
}

// Returns an operand matcher against an operand that is read and written.
template <typename T>
inline static OperandMatcher ReadAndWriteTo(T &op) {
  return {&op, kOperandActionReadWrite, kOperandConstraintBind,
          detail::OperandTypeGetter<T>::kType};
}

// Returns an operand matcher against an operand that is read and written.
template <typename T>
inline static OperandMatcher ReadOrWriteTo(T &op) {
  return {&op, kOperandActionAny, kOperandConstraintBind,
          detail::OperandTypeGetter<T>::kType};
}

// TODO(pag): Only doing exact matching on register operands.

// Returns an operand matcher against an operand that is read.
inline static OperandMatcher ExactReadFrom(RegisterOperand &op) {
  return {&op, kOperandActionRead, kOperandConstraintMatch,
          kOperandTypeRegister};
}

// Returns an operand matcher against an operand that is only read.
inline static OperandMatcher ExactReadOnlyFrom(RegisterOperand &op) {
  return {&op, kOperandActionReadOnly, kOperandConstraintMatch,
          kOperandTypeRegister};
}

// Returns an operand matcher against an operand that is written.
inline static OperandMatcher ExactWriteTo(RegisterOperand &op) {
  return {&op, kOperandActionWrite, kOperandConstraintMatch,
          kOperandTypeRegister};
}

// Returns an operand matcher against an operand that is only written.
inline static OperandMatcher ExactWriteOnlyTo(RegisterOperand &op) {
  return {&op, kOperandActionWriteOnly, kOperandConstraintMatch,
          kOperandTypeRegister};
}

// Returns an operand matcher against an operand that is read and written.
inline static OperandMatcher ExactReadAndWriteTo(RegisterOperand &op) {
  return {&op, kOperandActionReadWrite, kOperandConstraintMatch,
          kOperandTypeRegister};
}

// Returns an operand matcher against an operand that is read and written.
inline static OperandMatcher ExactReadOrWriteTo(RegisterOperand &op) {
  return {&op, kOperandActionAny, kOperandConstraintMatch,
          kOperandTypeRegister};
}

}  // namespace granary

#endif  // GRANARY_CFG_OPERAND_H_
