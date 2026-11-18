#include "imobs/metrics.h"

#include "imobs/schema.h"
#include "imobs_test.h"

using imobs::Event;
using imobs::EventKind;
using imobs::LabelDef;
using imobs::MetricsTable;
using imobs::SchemaRegistry;

namespace {
Event Ev(uint64_t key, uint16_t idx, uint64_t value) {
  Event e{};
  e.kind = static_cast<uint8_t>(EventKind::kMetric);
  e.key = key;
  e.label_idx = idx;
  e.value = value;
  return e;
}
}  // namespace

TEST(Metrics, CounterAggregateAndReset) {
  SchemaRegistry reg;
  const uint64_t key = reg.RegisterCounter("c", {LabelDef{"net", {"wifi", "cell"}}});
  ASSERT_TRUE(key != 0);
  MetricsTable table(&reg);
  table.Apply(Ev(key, 0, 2));
  table.Apply(Ev(key, 0, 3));
  table.Apply(Ev(key, 1, 5));

  auto rows = table.SnapshotAndReset();
  ASSERT_EQ(rows.size(), size_t{2});  // 两个 label 组合各一行——预聚合的本质
  EXPECT_EQ(rows[0].label_idx, 0);
  EXPECT_EQ(rows[0].count, 2u);
  EXPECT_EQ(rows[0].sum, 5u);
  EXPECT_EQ(rows[1].label_idx, 1);
  EXPECT_EQ(rows[1].count, 1u);
  EXPECT_EQ(rows[1].sum, 5u);
  EXPECT_TRUE(reg.RenderLabels(rows[0].key, rows[0].label_idx) == std::string("net=wifi"));

  EXPECT_EQ(table.SnapshotAndReset().size(), size_t{0});  // 快照即窗口清零
}

TEST(Metrics, HistogramQuantileWithin1Percent) {
  SchemaRegistry reg;
  const uint64_t key = reg.RegisterHistogram("h", {});
  ASSERT_TRUE(key != 0);
  MetricsTable table(&reg);
  for (uint64_t v = 1; v <= 1000; ++v) table.Apply(Ev(key, 0, v));

  auto rows = table.SnapshotAndReset();
  ASSERT_EQ(rows.size(), size_t{1});
  EXPECT_EQ(rows[0].count, 1000u);
  // t-digest（P1.3）：真值 p50=500.5、p99=990，容差 1%
  const double p50 = rows[0].digest.Quantile(0.50);
  const double p99 = rows[0].digest.Quantile(0.99);
  EXPECT_TRUE(p50 >= 495.0 && p50 <= 506.0);
  EXPECT_TRUE(p99 >= 980.0 && p99 <= 1000.0);
  EXPECT_EQ(rows[0].digest.Count(), 1000u);
}

TEST(Metrics, UnknownKeyDroppedAndCounted) {
  SchemaRegistry reg;
  MetricsTable table(&reg);
  table.Apply(Ev(0xdeadbeef, 0, 1));  // 绕过门面的打点：纵深防御第二道
  EXPECT_EQ(table.unknown_key_dropped(), 1u);
  EXPECT_EQ(table.SnapshotAndReset().size(), size_t{0});
}

IMOBS_TEST_MAIN
