/* Copyright 2014 Peter Goodman, all rights reserved. */

#include <gmock/gmock.h>

#define GRANARY_INTERNAL
#define GRANARY_ARCH_INTERNAL
#define GRANARY_TEST

#include "granary/base/lock.h"

#include "test/util/isolated_function.h"

using namespace granary;

extern "C" {
extern void RunFunctionInContext(void *func, IsolatedRegState *inout);

// Useful debugging aid to "break" on the first difference using a hardware
// watchpoint.
int watchpoint = 0;

}  // extern "C"

namespace {
static SpinLock regs_lock;
static IsolatedRegState regs1, regs2, regs3;
}  // namespace

// Runs a function and an instrumented function in an "isolated" context
// (almost full machine state), and then
void RunIsolatedFunction(std::function<void(IsolatedRegState *)> &setup_state,
                         void *func,
                         void *instrumented_func) {
  SpinLockedRegion locker(&regs_lock);

  memset(&regs1, 0, sizeof regs1);
  regs1.RSP = reinterpret_cast<uintptr_t>(&(regs1.redzone_high));
  setup_state(&regs1);
  RunFunctionInContext(reinterpret_cast<void *>(func), &regs1);
  VALGRIND_MAKE_MEM_DEFINED(&regs1, sizeof regs1);
  memcpy(&regs2, &regs1, sizeof regs1);

  // Make the initial values of everything on the stack different so that we
  // can eventually distinguish what changes and what stays the same.
  memset(&regs1, 0, sizeof regs1);
  memset(&(regs1.stack), 0xAB, sizeof regs1.stack);
  regs1.RSP = reinterpret_cast<uintptr_t>(&(regs1.redzone_high));
  setup_state(&regs1);
  RunFunctionInContext(reinterpret_cast<void *>(func), &regs1);
  VALGRIND_MAKE_MEM_DEFINED(&regs1, sizeof regs1);
  memcpy(&regs3, &regs1, sizeof regs1);

  memset(&regs1, 0, sizeof regs1);
  regs1.RSP = reinterpret_cast<uintptr_t>(&(regs1.redzone_high));
  setup_state(&regs1);
  watchpoint = 1;
  RunFunctionInContext(reinterpret_cast<void *>(instrumented_func), &regs1);
  VALGRIND_MAKE_MEM_DEFINED(&regs1, sizeof regs1);

  // Compare bytes that are the same across the two native runs. This ensures
  // that stuff in regs3 that falls outside of the redzone is not part of the
  // comparison.
  auto regs1_bytes = reinterpret_cast<uint8_t *>(&regs1);
  auto regs2_bytes = reinterpret_cast<uint8_t *>(&regs2);
  auto regs3_bytes = reinterpret_cast<uint8_t *>(&regs3);
  for (auto i = sizeof regs1; i-- > 0; ) {
    if (regs2_bytes[i] == regs3_bytes[i]) {
      if (regs1_bytes[i] != regs2_bytes[i]) {
        watchpoint = 0;
        EXPECT_EQ(regs1_bytes[i], regs2_bytes[i]);
        break;
      }
    }
  }
}

