// lib/test/test_terminate_hooks.cpp — unit tests for WriteAssertAnnotation and
//                                     WriteTerminateAnnotation.

#include <memory>
#include <stdexcept>

#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "gtest/gtest.h"
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
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes,
  // misc-non-private-member-variables-in-classes, readability-identifier-naming)
  std::unique_ptr<crashpad::SimpleStringDictionary> dict_;
};

// ── WriteAssertAnnotation ─────────────────────────────────────────────────────

TEST_F(TerminateHooksTest, AssertAnnotation_FormatsExprFileLineFuncCorrectly) {
  constexpr unsigned int test_line_number = 42;
  crashomon::WriteAssertAnnotation("x > 0", "src/main.cpp", test_line_number, "foo()");
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

// ── WriteTerminateAnnotation ──────────────────────────────────────────────────

TEST_F(TerminateHooksTest, TerminateAnnotation_WithExceptionTypeSetsTypeAndMessage) {
  crashomon::WriteTerminateAnnotation(&typeid(std::runtime_error));
  EXPECT_STREQ(dict_->GetValueForKey("abort_message"), "unhandled C++ exception");
  EXPECT_STREQ(dict_->GetValueForKey("terminate_type"), "std::runtime_error");
}

TEST_F(TerminateHooksTest, TerminateAnnotation_UnknownTypeSetsNonEmptyTypeName) {
  crashomon::WriteTerminateAnnotation(&typeid(int));
  EXPECT_STREQ(dict_->GetValueForKey("abort_message"), "unhandled C++ exception");
  EXPECT_STREQ(dict_->GetValueForKey("terminate_type"), "int");
}

TEST_F(TerminateHooksTest, TerminateAnnotation_NoActiveExceptionWritesFallback) {
  crashomon::WriteTerminateAnnotation(nullptr);
  EXPECT_STREQ(dict_->GetValueForKey("abort_message"), "terminate called without active exception");
  EXPECT_EQ(dict_->GetValueForKey("terminate_type"), nullptr);
}

TEST_F(TerminateHooksTest, TerminateAnnotation_NullAnnotationsIsNoOp) {
  crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  crashomon::WriteTerminateAnnotation(&typeid(std::logic_error));
  // Must not crash.
}

}  // namespace
