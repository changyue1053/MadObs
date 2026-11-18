#include "imobs/tdigest.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "imobs_test.h"

using imobs::TDigest;

namespace {

double ExactQuantile(std::vector<double>& sorted, double q) {
  return sorted[static_cast<size_t>(q * static_cast<double>(sorted.size() - 1))];
}

double RelErr(double est, double exact) { return std::fabs(est - exact) / std::fabs(exact); }

}  // namespace

TEST(TDigest, UniformAccuracyWithin1Percent) {
  std::minstd_rand rng(42);
  std::uniform_real_distribution<double> dist(0.0, 1'000'000.0);
  TDigest d;
  std::vector<double> all;
  all.reserve(1'000'000);
  for (int i = 0; i < 1'000'000; ++i) {
    const double x = dist(rng);
    d.Add(x);
    all.push_back(x);
  }
  std::sort(all.begin(), all.end());
  // DoD（实施计划 P1.3）：1e6 样本 P50/P99 相对误差 < 1%
  EXPECT_TRUE(RelErr(d.Quantile(0.50), ExactQuantile(all, 0.50)) < 0.01);
  EXPECT_TRUE(RelErr(d.Quantile(0.99), ExactQuantile(all, 0.99)) < 0.01);
  EXPECT_TRUE(RelErr(d.Quantile(0.999), ExactQuantile(all, 0.999)) < 0.01);
  EXPECT_EQ(d.Count(), 1'000'000u);
  EXPECT_TRUE(d.CentroidCount() < 500);  // δ=200：压实后质心数有界，线上体积可控
}

TEST(TDigest, LognormalTailAccuracy) {
  // 时延分布的真实形态是重尾——对数正态是最贴近的检验负载
  std::minstd_rand rng(7);
  std::normal_distribution<double> normal(0.0, 1.0);
  TDigest d;
  std::vector<double> all;
  all.reserve(200'000);
  for (int i = 0; i < 200'000; ++i) {
    const double x = std::exp(normal(rng));
    d.Add(x);
    all.push_back(x);
  }
  std::sort(all.begin(), all.end());
  EXPECT_TRUE(RelErr(d.Quantile(0.50), ExactQuantile(all, 0.50)) < 0.01);
  EXPECT_TRUE(RelErr(d.Quantile(0.99), ExactQuantile(all, 0.99)) < 0.01);
}

TEST(TDigest, MergeMatchesWholeAndSerializeRoundtrip) {
  std::minstd_rand rng(9);
  std::uniform_real_distribution<double> dist(0.0, 10'000.0);
  TDigest whole;
  TDigest half_a;
  TDigest half_b;
  std::vector<double> all;
  for (int i = 0; i < 100'000; ++i) {
    const double x = dist(rng);
    whole.Add(x);
    ((i & 1) != 0 ? half_a : half_b).Add(x);
    all.push_back(x);
  }
  std::sort(all.begin(), all.end());

  // 归并（端侧两窗口 / 服务端两端源）后精度不塌
  half_a.Merge(half_b);
  EXPECT_EQ(half_a.Count(), 100'000u);
  EXPECT_TRUE(RelErr(half_a.Quantile(0.50), ExactQuantile(all, 0.50)) < 0.01);
  EXPECT_TRUE(RelErr(half_a.Quantile(0.99), ExactQuantile(all, 0.99)) < 0.01);

  // 序列化往返：质心逐一还原，分位完全一致
  std::vector<uint8_t> buf;
  whole.Serialize(&buf);
  TDigest back;
  ASSERT_TRUE(TDigest::Deserialize(buf.data(), buf.size(), &back));
  EXPECT_EQ(back.Count(), whole.Count());
  EXPECT_TRUE(std::fabs(back.Quantile(0.99) - whole.Quantile(0.99)) < 1e-9);
  EXPECT_TRUE(std::fabs(back.Min() - whole.Min()) < 1e-9);

  // 防御：截断输入必须被拒绝
  TDigest junk;
  EXPECT_FALSE(TDigest::Deserialize(buf.data(), buf.size() / 2, &junk));
}

TEST(TDigest, WeightedAddEquivalence) {
  std::minstd_rand rng(11);
  std::uniform_real_distribution<double> dist(0.0, 1000.0);
  TDigest weighted;
  TDigest singles;
  for (int i = 0; i < 500; ++i) {
    const double x = dist(rng);
    weighted.Add(x, 20);
    for (int k = 0; k < 20; ++k) singles.Add(x);
  }
  EXPECT_EQ(weighted.Count(), singles.Count());
  EXPECT_TRUE(std::fabs(weighted.Quantile(0.50) - singles.Quantile(0.50)) /
                  singles.Quantile(0.50) <
              0.015);
  EXPECT_TRUE(std::fabs(weighted.Quantile(0.99) - singles.Quantile(0.99)) /
                  singles.Quantile(0.99) <
              0.015);
}

IMOBS_TEST_MAIN
