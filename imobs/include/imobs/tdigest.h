#pragma once
// merging t-digest（Dunning）：分位摘要，跨端可归并——端侧聚合、服务端 Flink
// 合并用同一套结构（实施计划 P1.3，替换 log2 桶 v0）。
// 精度画像：k1 尺度函数在分布两端分辨率最高（P99/P999 正是观测的主战场）；
// δ=200 时 1e6 样本 P50/P99 相对误差 < 1%（DoD，见 test_tdigest 对拍）。
// 线程契约：仅消费线程使用；Quantile/Serialize 前会先压实缓冲（mutable）。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace imobs {

class TDigest {
 public:
  explicit TDigest(double compression = 200.0);

  void Add(double x, uint64_t w = 1);
  void Merge(const TDigest& other);

  double Quantile(double q) const;
  uint64_t Count() const { return total_; }
  double Min() const { return total_ == 0 ? 0.0 : min_; }
  double Max() const { return total_ == 0 ? 0.0 : max_; }
  size_t CentroidCount() const;  // 压实后质心数（线上体积观测用，δ=200 时 ~几十）

  void Serialize(std::vector<uint8_t>* out) const;
  static bool Deserialize(const uint8_t* data, size_t len, TDigest* out);

 private:
  struct Centroid {
    double mean;
    uint64_t weight;
  };

  void Flush() const;              // 缓冲并入质心：一次归并 + 压缩扫描
  double ScaleK(double q) const;   // k1(q) = δ/(2π)·asin(2q−1)

  double compression_;
  mutable std::vector<Centroid> centroids_;  // 按 mean 有序（压实后）
  mutable std::vector<Centroid> buffer_;     // 未压实增量
  uint64_t total_ = 0;
  double min_ = 0.0;
  double max_ = 0.0;
};

}  // namespace imobs
