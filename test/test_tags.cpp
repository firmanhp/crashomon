// test/test_tags.cpp — unit tests for crashomon_set_tag and
//                     crashomon_set_abort_message.
//
// DoInit() (called by AutoInit() at library load) tries to connect to watcherd.
// When no watcherd is running it returns early, leaving CrashpadInfo's
// simple_annotations() pointer null.  Each test's SetUp() mimics what DoInit()
// does on a successful connection: create a fresh SimpleStringDictionary and
// register it via set_simple_annotations().  TearDown() removes the registration
// so tests are fully isolated.

#include "lib/crashomon.h"

#include <memory>
#include <string>

#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "gtest/gtest.h"

namespace {

class TagsTest : public ::testing::Test {
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

// ── crashomon_set_tag ─────────────────────────────────────────────────────────

TEST_F(TagsTest, StoresKeyValue) {
  crashomon_set_tag("version", "1.2.3");
  EXPECT_STREQ(dict_->GetValueForKey("version"), "1.2.3");
}

TEST_F(TagsTest, OverwritesSameKey) {
  crashomon_set_tag("env", "staging");
  crashomon_set_tag("env", "production");
  EXPECT_STREQ(dict_->GetValueForKey("env"), "production");
}

TEST_F(TagsTest, MultipleDistinctKeys) {
  crashomon_set_tag("k1", "v1");
  crashomon_set_tag("k2", "v2");
  EXPECT_STREQ(dict_->GetValueForKey("k1"), "v1");
  EXPECT_STREQ(dict_->GetValueForKey("k2"), "v2");
}

TEST_F(TagsTest, NullKeyIsNoOp) {
  // key/value are easily-swappable-parameters; using distinct string literals
  // makes the intent clear.  NOLINTNEXTLINE: first arg is intentionally null.
  crashomon_set_tag(nullptr, "value");
  EXPECT_EQ(dict_->GetCount(), 0u);
}

TEST_F(TagsTest, NullValueIsNoOp) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — second arg is intentionally null.
  crashomon_set_tag("key", nullptr);
  EXPECT_EQ(dict_->GetCount(), 0u);
}

TEST_F(TagsTest, NullAnnotationsIsNoOp) {
  // Simulate state before a successful DoInit(): no dictionary registered.
  crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  // Must not dereference null — verified by the test completing without SIGSEGV.
  crashomon_set_tag("key", "value");
}

// SimpleStringDictionary::kValueMaxLength == 255: values longer than 255 chars
// are silently truncated.
TEST_F(TagsTest, LongValueTruncatedAt255Chars) {
  const std::string long_value(300, 'x');
  crashomon_set_tag("k", long_value.c_str());
  const char* stored = dict_->GetValueForKey("k");
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(std::string_view{stored}.size(), 255u);
}

// ── crashomon_set_abort_message ───────────────────────────────────────────────

TEST_F(TagsTest, AbortMessageStoredUnderAbortMessageKey) {
  crashomon_set_abort_message("invariant violated: count >= 0");
  EXPECT_STREQ(dict_->GetValueForKey("abort_message"), "invariant violated: count >= 0");
}

TEST_F(TagsTest, AbortMessageOverwritesPreviousMessage) {
  crashomon_set_abort_message("first message");
  crashomon_set_abort_message("second message");
  EXPECT_STREQ(dict_->GetValueForKey("abort_message"), "second message");
}

TEST_F(TagsTest, AbortMessageNullIsNoOp) {
  crashomon_set_abort_message(nullptr);
  EXPECT_EQ(dict_->GetCount(), 0u);
}

TEST_F(TagsTest, AbortMessageNullAnnotationsIsNoOp) {
  crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(nullptr);
  // Must not dereference null.
  crashomon_set_abort_message("msg");
}

}  // namespace
