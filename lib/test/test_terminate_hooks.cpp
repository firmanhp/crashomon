// lib/test/test_terminate_hooks.cpp — unit tests for WriteAssertAnnotation and
//                                     WriteTerminateAnnotation.

#include <memory>
#include <stdexcept>
#include <typeinfo>

#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "gtest/gtest.h"
#include "lib/crashomon.h"
#include "lib/crashomon_internal.h"

namespace {

class TerminateHooksTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dict_ = std::make_unique<crashpad::SimpleStringDictionary>();
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(dict_.get());
  }
  void TearDown() override {
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  }
  std::unique_ptr<crashpad::SimpleStringDictionary> dict_;
};

// ── WriteAssertAnnotation ─────────────────────────────────────────────────────

TEST_F(TerminateHooksTest, AssertAnnotation_FormatsExprFileLineFuncCorrectly) {
  crashomon::WriteAssertAnnotation("x > 0", "src/main.cpp", 42, "foo()");
  EXPECT_STREQ(dict_->GetValueForKey("abort_message"),
               "assertion failed: 'x > 0' (src/main.cpp:42, foo())");
}

TEST_F(TerminateHooksTest, AssertAnnotation_NullAnnotationsIsNoOp) {
  crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  crashomon::WriteAssertAnnotation("x > 0", "f.cpp", 1, "g()");
}

TEST_F(TerminateHooksTest, AssertAnnotation_NullArgsDoNotCrash) {
  crashomon::WriteAssertAnnotation(nullptr, nullptr, 0, nullptr);
  EXPECT_NE(dict_->GetValueForKey("abort_message"), nullptr);
}

}  // namespace
