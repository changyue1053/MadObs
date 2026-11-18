#include "imobs/schema.h"

#include <string>

#include "imobs_test.h"

using imobs::Fnv1a;
using imobs::LabelDef;
using imobs::SchemaError;
using imobs::SchemaRegistry;

static_assert(Fnv1a("link.race.fallback") != 0, "编译期哈希可用");
static_assert(Fnv1a("a") != Fnv1a("b"), "编译期哈希可区分");

TEST(Schema, RegisterAndResolve) {
  SchemaRegistry reg;
  const uint64_t key = reg.RegisterCounter(
      "link.race.fallback", {LabelDef{"net", {"wifi", "cell"}}, LabelDef{"fg", {"0", "1"}}});
  ASSERT_TRUE(key != 0);
  EXPECT_TRUE(reg.Known(key));

  uint16_t idx = 0;
  ASSERT_TRUE(reg.ResolveLabelIndex(key, {{"net", "cell"}, {"fg", "1"}}, &idx));
  EXPECT_EQ(idx, 3);  // 混合进制：1*2+1
  EXPECT_TRUE(reg.RenderLabels(key, idx) == std::string("net=cell,fg=1"));

  uint16_t idx0 = 0;
  ASSERT_TRUE(reg.ResolveLabelIndex(key, {{"net", "wifi"}, {"fg", "0"}}, &idx0));
  EXPECT_EQ(idx0, 0);
}

TEST(Schema, ForbiddenLabelKeyRejected) {
  SchemaRegistry reg;
  EXPECT_EQ(reg.RegisterCounter("bad.metric", {LabelDef{"uid", {"x"}}}), 0ull);
  ASSERT_EQ(reg.rejected().size(), size_t{1});
  EXPECT_TRUE(reg.rejected()[0].reason == SchemaError::kForbiddenLabelKey);
}

TEST(Schema, UnboundedLabelRejected) {
  SchemaRegistry reg;
  EXPECT_EQ(reg.RegisterCounter("bad.metric2", {LabelDef{"region", {}}}), 0ull);
  ASSERT_EQ(reg.rejected().size(), size_t{1});
  EXPECT_TRUE(reg.rejected()[0].reason == SchemaError::kUnboundedLabel);
}

TEST(Schema, DuplicateNameRejected) {
  SchemaRegistry reg;
  ASSERT_TRUE(reg.RegisterCounter("dup", {}) != 0);
  EXPECT_EQ(reg.RegisterCounter("dup", {}), 0ull);
  ASSERT_EQ(reg.rejected().size(), size_t{1});
  EXPECT_TRUE(reg.rejected()[0].reason == SchemaError::kDuplicateName);
}

TEST(Schema, UnknownValueOrKeyRejected) {
  SchemaRegistry reg;
  const uint64_t key = reg.RegisterCounter("m", {LabelDef{"net", {"wifi"}}});
  ASSERT_TRUE(key != 0);
  uint16_t idx = 0;
  EXPECT_FALSE(reg.ResolveLabelIndex(key, {{"net", "5g"}}, &idx));    // 未注册取值
  EXPECT_FALSE(reg.ResolveLabelIndex(key, {{"nett", "wifi"}}, &idx)); // key 不匹配
  EXPECT_FALSE(reg.ResolveLabelIndex(key, {}, &idx));                 // 组合长度不符
}

IMOBS_TEST_MAIN
