#include "imobs/tdigest.h"

#include <algorithm>
#include <cmath>

#include "imobs/wire.h"

namespace imobs {
namespace {
constexpr size_t kBufferCap = 512;
constexpr double kPi = 3.14159265358979323846;
}  // namespace

TDigest::TDigest(double compression) : compression_(compression < 20.0 ? 20.0 : compression) {
  buffer_.reserve(kBufferCap);
}

void TDigest::Add(double x, uint64_t w) {
  if (w == 0) return;
  if (total_ == 0) {
    min_ = x;
    max_ = x;
  } else {
    min_ = std::min(min_, x);
    max_ = std::max(max_, x);
  }
  total_ += w;
  buffer_.push_back({x, w});
  if (buffer_.size() >= kBufferCap) Flush();
}

void TDigest::Merge(const TDigest& other) {
  other.Flush();
  if (other.total_ == 0) return;
  if (total_ == 0) {
    min_ = other.min_;
    max_ = other.max_;
  } else {
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }
  for (const auto& c : other.centroids_) {
    total_ += c.weight;
    buffer_.push_back(c);
    if (buffer_.size() >= kBufferCap) Flush();
  }
}

double TDigest::ScaleK(double q) const {
  q = std::clamp(q, 0.0, 1.0);
  return compression_ / (2.0 * kPi) * std::asin(2.0 * q - 1.0);
}

void TDigest::Flush() const {
  if (buffer_.empty()) return;
  std::sort(buffer_.begin(), buffer_.end(),
            [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });

  // 归并两个有序序列，同时做压缩扫描：相邻质心若合并后 k 跨度 ≤ 1 则并为一个。
  std::vector<Centroid> out;  // audit-allow: amortized 质心归并：每 ≥512 样本或快照触发一次（消费线程）；零分配化见上游反馈
  out.reserve(centroids_.size() + buffer_.size());
  const double w_total = static_cast<double>(total_);

  size_t i = 0;
  size_t j = 0;
  auto has_next = [&] { return i < centroids_.size() || j < buffer_.size(); };
  auto take_next = [&]() -> Centroid {
    if (i < centroids_.size() &&
        (j >= buffer_.size() || centroids_[i].mean <= buffer_[j].mean)) {
      return centroids_[i++];
    }
    return buffer_[j++];
  };

  Centroid cur = take_next();
  double cum_before_cur = 0.0;  // cur 之前已定质心的累计权重
  while (has_next()) {
    const Centroid nxt = take_next();
    const double q0 = cum_before_cur / w_total;
    const double q2 =
        (cum_before_cur + static_cast<double>(cur.weight) + static_cast<double>(nxt.weight)) /
        w_total;
    if (ScaleK(q2) - ScaleK(q0) <= 1.0) {
      const double wsum = static_cast<double>(cur.weight) + static_cast<double>(nxt.weight);
      cur.mean += (nxt.mean - cur.mean) * (static_cast<double>(nxt.weight) / wsum);
      cur.weight += nxt.weight;
    } else {
      out.push_back(cur);
      cum_before_cur += static_cast<double>(cur.weight);
      cur = nxt;
    }
  }
  out.push_back(cur);

  centroids_ = std::move(out);
  buffer_.clear();
}

size_t TDigest::CentroidCount() const {
  Flush();
  return centroids_.size();
}

double TDigest::Quantile(double q) const {
  Flush();
  if (centroids_.empty()) return 0.0;
  if (q <= 0.0) return min_;
  if (q >= 1.0) return max_;
  if (centroids_.size() == 1) return centroids_[0].mean;

  const double target = q * static_cast<double>(total_);
  double cum = 0.0;
  for (size_t i = 0; i < centroids_.size(); ++i) {
    const Centroid& c = centroids_[i];
    const double mid = cum + static_cast<double>(c.weight) / 2.0;
    if (target < mid) {
      if (i == 0) {  // min 与首质心中点之间插值
        const double t = target / mid;
        return min_ + t * (c.mean - min_);
      }
      const Centroid& p = centroids_[i - 1];
      const double pmid = cum - static_cast<double>(p.weight) / 2.0;
      const double t = (target - pmid) / (mid - pmid);
      return p.mean + t * (c.mean - p.mean);
    }
    cum += static_cast<double>(c.weight);
  }
  const Centroid& last = centroids_.back();
  const double lmid = static_cast<double>(total_) - static_cast<double>(last.weight) / 2.0;
  const double denom = static_cast<double>(total_) - lmid;
  const double t = denom > 0.0 ? (target - lmid) / denom : 1.0;
  return last.mean + t * (max_ - last.mean);
}

void TDigest::Serialize(std::vector<uint8_t>* out) const {  // audit-allow: cold-path 快照序列化（30s 批级）
  Flush();
  WireWriter w(out);
  w.Fixed64(DoubleBits(compression_));
  w.Fixed64(DoubleBits(min_));
  w.Fixed64(DoubleBits(max_));
  w.Varint(centroids_.size());
  for (const auto& c : centroids_) {
    w.Fixed64(DoubleBits(c.mean));
    w.Varint(c.weight);
  }
}

bool TDigest::Deserialize(const uint8_t* data, size_t len, TDigest* out) {
  WireReader r(data, len);
  uint64_t comp_bits = 0;
  uint64_t min_bits = 0;
  uint64_t max_bits = 0;
  uint64_t n = 0;
  if (!r.Fixed64(&comp_bits) || !r.Fixed64(&min_bits) || !r.Fixed64(&max_bits) ||
      !r.Varint(&n)) {
    return false;
  }
  if (n > 1'000'000) return false;  // 体积上限（防御恶意输入）
  TDigest d(BitsDouble(comp_bits));
  d.min_ = BitsDouble(min_bits);
  d.max_ = BitsDouble(max_bits);
  d.centroids_.reserve(n);
  uint64_t total = 0;
  double prev_mean = -std::numeric_limits<double>::infinity();
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t mean_bits = 0;
    uint64_t weight = 0;
    if (!r.Fixed64(&mean_bits) || !r.Varint(&weight) || weight == 0) return false;
    const double mean = BitsDouble(mean_bits);
    if (!(mean >= prev_mean)) return false;  // 必须有序（同时挡 NaN）
    prev_mean = mean;
    d.centroids_.push_back({mean, weight});
    total += weight;
  }
  d.total_ = total;
  *out = std::move(d);
  return true;
}

}  // namespace imobs
