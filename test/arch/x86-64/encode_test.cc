/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL
#define GRANARY_ARCH_INTERNAL
#define GRANARY_TEST

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "arch/driver.h"

#include "granary/base/cast.h"

#include "granary/exit.h"
#include "granary/init.h"

extern "C" {
  extern void TestDecode_Instructions(void);
  extern void TestDecode_Instructions_End(void);
}

TEST(EncodeTest, EncodeCommonInstructions) {
  using namespace granary;
  Init(kInitAttach);

  auto begin = UnsafeCast<AppPC>(TestDecode_Instructions);
  auto end = UnsafeCast<AppPC>(TestDecode_Instructions_End);

  arch::InstructionEncoder staged_encoder(arch::InstructionEncodeKind::STAGED);
  arch::InstructionEncoder commit_encoder(arch::InstructionEncodeKind::COMMIT);
  arch::Instruction instr;

  while (begin < end) {
    auto ret = arch::InstructionDecoder::DecodeNext(&instr, &begin);
    if (!ret) break;

    uint8_t mem[XED_MAX_INSTRUCTION_BYTES] = {0};
    ret = staged_encoder.Encode(&instr, &(mem[0]));
    EXPECT_TRUE(ret);
    if (!ret) break;

    ret = commit_encoder.Encode(&instr, &(mem[0]));
    EXPECT_TRUE(ret);
    if (!ret) break;
  }
  Exit(kExitDetach);
}
