// tombstone/register_extract.cpp — AMD64 and ARM64 GPR extraction.

#include "daemon/tombstone/register_extract.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// breakpad_types.h defines uint128_struct; must be included before the
// CPU-specific headers that reference it.
#include "google_breakpad/common/breakpad_types.h"
#include "google_breakpad/common/minidump_cpu_amd64.h"
#include "google_breakpad/common/minidump_cpu_arm64.h"
#include "google_breakpad/processor/minidump.h"

namespace crashomon {

std::vector<std::pair<std::string, uint64_t>> ExtractAMD64Regs(const MDRawContextAMD64& ctx) {
  return {
      {"rax", ctx.rax}, {"rbx", ctx.rbx}, {"rcx", ctx.rcx}, {"rdx", ctx.rdx}, {"rsi", ctx.rsi},
      {"rdi", ctx.rdi}, {"rbp", ctx.rbp}, {"rsp", ctx.rsp}, {"r8", ctx.r8},   {"r9", ctx.r9},
      {"r10", ctx.r10}, {"r11", ctx.r11}, {"r12", ctx.r12}, {"r13", ctx.r13}, {"r14", ctx.r14},
      {"r15", ctx.r15}, {"rip", ctx.rip},
  };
}

std::vector<std::pair<std::string, uint64_t>> ExtractARM64Regs(const MDRawContextARM64& ctx) {
  // ARM64 has x0-x28 (29 regs) + fp, lr, sp, pc = 33 total.
  constexpr int arm64_last_gp_reg = 28;
  constexpr size_t arm64_total_regs = 33;
  std::vector<std::pair<std::string, uint64_t>> regs;
  regs.reserve(arm64_total_regs);
  for (int idx = 0; idx <= arm64_last_gp_reg; ++idx) {
    regs.emplace_back("x" + std::to_string(idx),
                      // ctx.iregs is a Breakpad C array; loop bounds (0-28) are verified;
                      // no GSL available to use gsl::at().
                      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                      ctx.iregs[static_cast<size_t>(idx)]);
  }
  regs.emplace_back("fp", ctx.iregs[MD_CONTEXT_ARM64_REG_FP]);
  regs.emplace_back("lr", ctx.iregs[MD_CONTEXT_ARM64_REG_LR]);
  regs.emplace_back("sp", ctx.iregs[MD_CONTEXT_ARM64_REG_SP]);
  regs.emplace_back("pc", ctx.iregs[MD_CONTEXT_ARM64_REG_PC]);
  return regs;
}

}  // namespace crashomon
