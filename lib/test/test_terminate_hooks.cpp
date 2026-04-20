// lib/test/test_terminate_hooks.cpp — unit tests for WriteAssertAnnotation and
//                                     WriteTerminateAnnotation.

#include <memory>

#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "gtest/gtest.h"
#include "lib/crashomon_internal.h"

namespace {

class TerminateHooksTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dict = std::make_unique<crashpad::SimpleStringDictionary>();
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(dict.get());
  }
  void TearDown() override {
    crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes, misc-non-private-member-variables-in-classes)
  std::unique_ptr<crashpad::SimpleStringDictionary> dict;
};

// ── WriteAssertAnnotation ─────────────────────────────────────────────────────

TEST_F(TerminateHooksTest, AssertAnnotation_FormatsExprFileLineFuncCorrectly) {
  constexpr unsigned int test_line_number = 42;
  crashomon::WriteAssertAnnotation("x > 0", "src/main.cpp", test_line_number, "foo()");
  EXPECT_STREQ(dict->GetValueForKey("abort_message"),
               "assertion failed: 'x > 0' (src/main.cpp:42, foo())");
}

TEST_F(TerminateHooksTest, AssertAnnotation_NullAnnotationsIsNoOp) {
  crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  crashomon::WriteAssertAnnotation("x > 0", "f.cpp", 1, "g()");
}

TEST_F(TerminateHooksTest, AssertAnnotation_NullArgsDoNotCrash) {
  crashomon::WriteAssertAnnotation(nullptr, nullptr, 0, nullptr);
  EXPECT_NE(dict->GetValueForKey("abort_message"), nullptr);
}

}  // namespace
