/* Copyright 2014 Peter Goodman, all rights reserved. */

#define GRANARY_INTERNAL
#define GRANARY_ARCH_INTERNAL

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "arch/driver.h"

#include "granary/base/cast.h"

extern "C" {
  extern void TestDecode_Instructions(void);
  extern void TestDecode_Instructions_End(void);
}

TEST(DecodeTest, DecodeCommonInstructions) {
  using namespace granary;
  arch::Init();

  auto begin = UnsafeCast<AppPC>(TestDecode_Instructions);
  auto end = UnsafeCast<AppPC>(TestDecode_Instructions_End);

  arch::InstructionDecoder decoder;
  arch::Instruction instr;

  while (begin < end) {
    auto old_begin = begin;
    auto ret = decoder.DecodeNext(&instr, &begin);
    EXPECT_TRUE(ret);
    if (!ret) break;
    EXPECT_TRUE(old_begin < begin);
    if (old_begin >= begin) break;
    EXPECT_TRUE(XED_ICLASS_INVALID != instr.iclass);
    if (XED_ICLASS_INVALID == instr.iclass) break;
    EXPECT_TRUE(XED_IFORM_INVALID != instr.iform);
    if (XED_IFORM_INVALID == instr.iform) break;
  }
}
