#include <gtest/gtest.h>

#include <string>

#include "core/status.h"
#include "core/types.h"

namespace kv {
namespace {

TEST(ValidateKeyValue, LimitsMatchDesign) {
    EXPECT_EQ(kMaxKeyLength, 64u * 1024u);
    EXPECT_EQ(kMaxValueLength, 16u * 1024u * 1024u);
}

TEST(ValidateKeyValue, EmptyKeyAndValueAreValid) {
    EXPECT_TRUE(ValidateKeyValue("", "").ok());
}

TEST(ValidateKeyValue, EmbeddedZeroBytesAreValid) {
    const std::string key("a\0b", 3);
    const std::string value("\0\0", 2);
    EXPECT_TRUE(ValidateKeyValue(key, value).ok());
}

TEST(ValidateKeyValue, KeyAtMaximumLengthIsValid) {
    const std::string key(kMaxKeyLength, 'k');
    const Status s = ValidateKeyValue(key, "v");
    EXPECT_TRUE(s.ok()) << s.ToString();
}

TEST(ValidateKeyValue, KeyOneByteOverMaximumIsInvalid) {
    const std::string key(kMaxKeyLength + 1, 'k');
    const Status s = ValidateKeyValue(key, "v");
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
}

TEST(ValidateKeyValue, ValueAtMaximumLengthIsValid) {
    const std::string value(kMaxValueLength, 'v');
    const Status s = ValidateKeyValue("k", value);
    EXPECT_TRUE(s.ok()) << s.ToString();
}

TEST(ValidateKeyValue, ValueOneByteOverMaximumIsInvalid) {
    const std::string value(kMaxValueLength + 1, 'v');
    const Status s = ValidateKeyValue("k", value);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
}

TEST(ValidateKeyValue, BothAtMaximumLengthIsValid) {
    const std::string key(kMaxKeyLength, 'k');
    const std::string value(kMaxValueLength, 'v');
    const Status s = ValidateKeyValue(key, value);
    EXPECT_TRUE(s.ok()) << s.ToString();
}

TEST(ValidateKeyValue, BothOverMaximumIsInvalid) {
    const std::string key(kMaxKeyLength + 1, 'k');
    const std::string value(kMaxValueLength + 1, 'v');
    const Status s = ValidateKeyValue(key, value);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
}

}  // namespace
}  // namespace kv
