// test/test_register_extract.cpp — unit tests for AMD64 and ARM64 GPR extraction.
//
// These tests construct raw context structs with known values and verify the
// output without needing a minidump file or a running daemon.

// breakpad_types.h defines uint128_struct; must precede the CPU-specific
// headers that reference it.
#include "google_breakpad/common/breakpad_types.h"
#include "google_breakpad/common/minidump_cpu_amd64.h"
#include "google_breakpad/common/minidump_cpu_arm64.h"
#include "google_breakpad/processor/minidump.h"  // IWYU pragma: keep
#include "gtest/gtest.h"
#include "tombstone/register_extract.h"

namespace crashomon {
namespace {

// ── AMD64 ────────────────────────────────────────────────────────────────────

constexpr uint64_t kAmd64Rax = 0xAAAA'0001ULL;
constexpr uint64_t kAmd64Rsp = 0xAAAA'0002ULL;
constexpr uint64_t kAmd64Rip = 0xAAAA'0003ULL;

TEST(RegisterExtractTest, AMD64_AllRegistersPresent) {
  MDRawContextAMD64 ctx{};
  auto regs = ExtractAMD64Regs(ctx);
  EXPECT_EQ(regs.size(), 17U);
}

TEST(RegisterExtractTest, AMD64_CorrectNames) {
  MDRawContextAMD64 ctx{};
  auto regs = ExtractAMD64Regs(ctx);
  ASSERT_GE(regs.size(), 17U);
  // Spot-check names in display order.
  EXPECT_EQ(regs[0].first, "rax");
  EXPECT_EQ(regs[7].first, "rsp");
  EXPECT_EQ(regs[16].first, "rip");
}

TEST(RegisterExtractTest, AMD64_CorrectValues) {
  MDRawContextAMD64 ctx{};
  ctx.rax = kAmd64Rax;
  ctx.rsp = kAmd64Rsp;
  ctx.rip = kAmd64Rip;

  auto regs = ExtractAMD64Regs(ctx);
  ASSERT_GE(regs.size(), 17U);

  for (const auto& [name, val] : regs) {
    if (name == "rax") {
      EXPECT_EQ(val, kAmd64Rax);
    }
    if (name == "rsp") {
      EXPECT_EQ(val, kAmd64Rsp);
    }
    if (name == "rip") {
      EXPECT_EQ(val, kAmd64Rip);
    }
  }
}

// ── ARM64 ────────────────────────────────────────────────────────────────────

constexpr uint64_t kArm64X0 = 0xBBBB'0001ULL;
constexpr uint64_t kArm64Fp = 0xBBBB'0002ULL;
constexpr uint64_t kArm64Sp = 0xBBBB'0003ULL;
constexpr uint64_t kArm64Pc = 0xBBBB'0004ULL;

TEST(RegisterExtractTest, ARM64_AllRegistersPresent) {
  MDRawContextARM64 ctx{};
  auto regs = ExtractARM64Regs(ctx);
  EXPECT_EQ(regs.size(), 33U);
}

TEST(RegisterExtractTest, ARM64_CorrectNames) {
  MDRawContextARM64 ctx{};
  auto regs = ExtractARM64Regs(ctx);
  ASSERT_EQ(regs.size(), 33U);
  // x0-x28 are the first 29 entries.
  EXPECT_EQ(regs[0].first, "x0");
  EXPECT_EQ(regs[28].first, "x28");
  // Named registers follow in order.
  EXPECT_EQ(regs[29].first, "fp");
  EXPECT_EQ(regs[30].first, "lr");
  EXPECT_EQ(regs[31].first, "sp");
  EXPECT_EQ(regs[32].first, "pc");
}

TEST(RegisterExtractTest, ARM64_CorrectValues) {
  MDRawContextARM64 ctx{};
  ctx.iregs[0] = kArm64X0;
  ctx.iregs[MD_CONTEXT_ARM64_REG_FP] = kArm64Fp;
  ctx.iregs[MD_CONTEXT_ARM64_REG_SP] = kArm64Sp;
  ctx.iregs[MD_CONTEXT_ARM64_REG_PC] = kArm64Pc;

  auto regs = ExtractARM64Regs(ctx);
  ASSERT_EQ(regs.size(), 33U);

  for (const auto& [name, val] : regs) {
    if (name == "x0") {
      EXPECT_EQ(val, kArm64X0);
    }
    if (name == "fp") {
      EXPECT_EQ(val, kArm64Fp);
    }
    if (name == "sp") {
      EXPECT_EQ(val, kArm64Sp);
    }
    if (name == "pc") {
      EXPECT_EQ(val, kArm64Pc);
    }
  }
}

TEST(RegisterExtractTest, ARM64_ZeroContext) {
  MDRawContextARM64 ctx{};
  auto regs = ExtractARM64Regs(ctx);
  ASSERT_EQ(regs.size(), 33U);
  for (const auto& [name, val] : regs) {
    EXPECT_EQ(val, 0U) << "non-zero for register " << name;
  }
}

}  // namespace
}  // namespace crashomon
