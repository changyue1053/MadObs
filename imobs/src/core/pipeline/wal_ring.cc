#include "imobs/wal_ring.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include "imobs/crc32.h"

namespace imobs {
namespace {

constexpr uint32_t kFileMagic = 0x494D574Cu;  // "IMWL"
constexpr uint32_t kSegMagic = 0x494D5347u;   // "IMSG"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFileHdrSize = 4096;  // 头页：参数 + 消费水位 + crc
constexpr uint32_t kSegHdrSize = 16;     // 段头：magic + epoch
constexpr uint32_t kRecHdrSize = 8;      // 记录头：len + crc32c(payload)

uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

uint32_t Ld32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t Ld64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
void St32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void St64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

}  // namespace

std::unique_ptr<WalRing> WalRing::Open(const std::string& path, const WalOptions& opt,
                                       std::string* err) {
  if (opt.segment_count == 0 || opt.segment_size < kSegHdrSize + kRecHdrSize + 8) {
    if (err) *err = "非法分段参数";
    return nullptr;
  }
  std::unique_ptr<WalRing> w(new WalRing());
  w->opt_ = opt;
  if (!w->MapFile(path, err)) return nullptr;

  const uint8_t* h = w->map_;
  const uint32_t magic = Ld32(h);
  if (magic == 0) {  // 全新文件（ftruncate 后全零）
    w->InitFresh();
    return w;
  }
  if (magic != kFileMagic || Ld32(h + 4) != kVersion) {
    if (err) *err = "文件魔数/版本不符";
    return nullptr;
  }
  if (Ld32(h + 8) != opt.segment_size || Ld32(h + 12) != opt.segment_count) {
    if (err) *err = "分段参数与既有文件不符（参数变更需迁移，不做静默重建）";
    return nullptr;
  }
  if (Crc32c(h, 32) == Ld32(h + 32)) {
    w->watermark_ = Cursor{Ld64(h + 16), Ld32(h + 24)};
  } else {
    // 头页撕裂：回退零水位并重写。方向安全——最坏重复上传，由上游 batch_id 去重。
    w->watermark_ = Cursor{};
    w->PersistHeader();
  }

  uint64_t max_epoch = 0;
  uint32_t cur = 0;
  for (uint32_t s = 0; s < opt.segment_count; ++s) {
    const uint64_t e = w->SegEpoch(s);
    if (e > max_epoch) {
      max_epoch = e;
      cur = s;
    }
  }
  if (max_epoch == 0) {
    w->WipeSegment(0, 1);
    w->cur_seg_ = 0;
    w->max_epoch_ = 1;
    w->write_off_ = kSegHdrSize;
  } else {
    w->cur_seg_ = cur;
    w->max_epoch_ = max_epoch;
    w->RecoverWriteSegment();
  }
  return w;
}

WalRing::~WalRing() {
  if (map_ != nullptr) {
    ::msync(map_, map_len_, MS_ASYNC);
    ::munmap(map_, map_len_);
  }
  if (fd_ >= 0) ::close(fd_);
}

bool WalRing::MapFile(const std::string& path, std::string* err) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    if (err) *err = std::string("open: ") + std::strerror(errno);
    return false;
  }
  map_len_ = kFileHdrSize + static_cast<size_t>(opt_.segment_size) * opt_.segment_count;
  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    if (err) *err = std::string("fstat: ") + std::strerror(errno);
    return false;
  }
  if (static_cast<size_t>(st.st_size) != map_len_) {
    if (::ftruncate(fd_, static_cast<off_t>(map_len_)) != 0) {
      if (err) *err = std::string("ftruncate: ") + std::strerror(errno);
      return false;
    }
  }
  void* m = ::mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (m == MAP_FAILED) {
    if (err) *err = std::string("mmap: ") + std::strerror(errno);
    return false;
  }
  map_ = static_cast<uint8_t*>(m);
  return true;
}

void WalRing::InitFresh() {
  watermark_ = Cursor{};
  PersistHeader();
  WipeSegment(0, 1);
  cur_seg_ = 0;
  max_epoch_ = 1;
  write_off_ = kSegHdrSize;
}

void WalRing::RecoverWriteSegment() {
  uint8_t* base = SegBase(cur_seg_);
  uint32_t off = kSegHdrSize;
  for (;;) {
    if (off + kRecHdrSize > opt_.segment_size) break;
    const uint32_t len = Ld32(base + off);
    if (len == 0) break;  // 干净链尾
    const uint32_t max_payload = opt_.segment_size - off - kRecHdrSize;
    if (len > max_payload || Crc32c(base + off + kRecHdrSize, len) != Ld32(base + off + 4)) {
      // 撕裂尾部：丢弃并清零到段尾，保证后续追加的记录链干净、不会误拼陈旧字节。
      ++stats_.torn_dropped;
      std::memset(base + off, 0, opt_.segment_size - off);
      break;
    }
    off += kRecHdrSize + Align8(len);
  }
  write_off_ = off;
}

bool WalRing::Append(const void* data, uint32_t len) {
  if (len == 0) {
    ++stats_.overflow_dropped;
    return false;
  }
  const uint32_t need = kRecHdrSize + Align8(len);
  if (need > opt_.segment_size - kSegHdrSize) {  // 单条超过段容量
    ++stats_.overflow_dropped;
    return false;
  }
  if (write_off_ + need > opt_.segment_size) {
    if (!AdvanceSegment()) {
      ++stats_.overflow_dropped;
      return false;
    }
  }
  uint8_t* base = SegBase(cur_seg_);
  uint8_t* rec = base + write_off_;
  std::memcpy(rec + kRecHdrSize, data, len);
  const uint32_t padded = Align8(len);
  if (padded != len) std::memset(rec + kRecHdrSize + len, 0, padded - len);
  St32(rec + 4, Crc32c(data, len));
  St32(rec, len);  // len 最后落位：len==0 是链尾标记，扫描器永远看不到半条记录
  write_off_ += need;
  ++stats_.appended;
  if (opt_.sync_every_append) {
    // P0 分区语义：逐条 msync 兜掉电（kill -9 本就不丢——脏页由内核回写）。
    const uintptr_t page = reinterpret_cast<uintptr_t>(rec) & ~static_cast<uintptr_t>(4095);
    const size_t n = static_cast<size_t>(reinterpret_cast<uintptr_t>(rec + need) - page);
    ::msync(reinterpret_cast<void*>(page), n, MS_SYNC);
  }
  return true;
}

bool WalRing::AdvanceSegment() {
  const uint32_t next = (cur_seg_ + 1) % opt_.segment_count;
  if (!opt_.overwrite_oldest && SegmentHasPending(next)) return false;
  WipeSegment(next, ++max_epoch_);
  cur_seg_ = next;
  write_off_ = kSegHdrSize;
  return true;
}

bool WalRing::SegmentHasPending(uint32_t seg) const {
  const uint64_t e = SegEpoch(seg);
  if (e == 0 || e < watermark_.epoch) return false;
  const uint32_t from =
      (e == watermark_.epoch) ? std::max(watermark_.offset, kSegHdrSize) : kSegHdrSize;
  bool pending = false;
  WalkSegment(seg, kSegHdrSize, [&](uint32_t end, std::string_view) {
    if (end > from) {
      pending = true;
      return false;
    }
    return true;
  });
  return pending;
}

void WalRing::WipeSegment(uint32_t seg, uint64_t epoch) {
  uint8_t* base = SegBase(seg);
  std::memset(base, 0, opt_.segment_size);
  St32(base, kSegMagic);
  St64(base + 8, epoch);
}

void WalRing::PersistHeader() {
  uint8_t* h = map_;
  St32(h, kFileMagic);
  St32(h + 4, kVersion);
  St32(h + 8, opt_.segment_size);
  St32(h + 12, opt_.segment_count);
  St64(h + 16, watermark_.epoch);
  St32(h + 24, watermark_.offset);
  St32(h + 28, 0);
  St32(h + 32, Crc32c(h, 32));
  ::msync(h, 4096, MS_ASYNC);  // 头页撕裂已有零水位回退兜底，允许异步
}

uint8_t* WalRing::SegBase(uint32_t seg) const {
  return map_ + kFileHdrSize + static_cast<size_t>(seg) * opt_.segment_size;
}

uint64_t WalRing::SegEpoch(uint32_t seg) const {
  const uint8_t* base = SegBase(seg);
  if (Ld32(base) != kSegMagic) return 0;
  return Ld64(base + 8);
}

void WalRing::WalkSegment(
    uint32_t seg, uint32_t start,
    const std::function<bool(uint32_t end_off, std::string_view payload)>& cb) const {
  const uint8_t* base = SegBase(seg);
  uint32_t off = start;
  for (;;) {
    if (off + kRecHdrSize > opt_.segment_size) return;
    const uint32_t len = Ld32(base + off);
    if (len == 0) return;
    const uint32_t max_payload = opt_.segment_size - off - kRecHdrSize;
    if (len > max_payload) return;
    if (Crc32c(base + off + kRecHdrSize, len) != Ld32(base + off + 4)) return;
    const uint32_t end = off + kRecHdrSize + Align8(len);
    if (!cb(end, std::string_view(reinterpret_cast<const char*>(base + off + kRecHdrSize), len))) {
      return;
    }
    off = end;
  }
}

void WalRing::ScanPending(const std::function<bool(const Record&)>& cb) const {
  std::vector<std::pair<uint64_t, uint32_t>> segs;  // (epoch, seg) 升序 = 全局顺序
  for (uint32_t s = 0; s < opt_.segment_count; ++s) {
    const uint64_t e = SegEpoch(s);
    if (e == 0 || e < watermark_.epoch) continue;
    segs.emplace_back(e, s);
  }
  std::sort(segs.begin(), segs.end());
  for (const auto& seg_pair : segs) {
    // 不用结构化绑定：lambda 捕获 structured binding 需 P1091（C++20），
    // OHOS 侧 LLVM 尚未实现——三端交叉编译兼容（imsdk 集成期发现）。
    const uint64_t e = seg_pair.first;
    const uint32_t s = seg_pair.second;
    const uint32_t from =
        (e == watermark_.epoch) ? std::max(watermark_.offset, kSegHdrSize) : kSegHdrSize;
    bool keep_going = true;
    WalkSegment(s, kSegHdrSize, [&](uint32_t end, std::string_view payload) {
      if (end <= from) return true;  // 已消费
      Record r{Cursor{e, end}, payload};
      keep_going = cb(r);
      return keep_going;
    });
    if (!keep_going) return;
  }
}

bool WalRing::MarkConsumed(const Cursor& upto) {
  if (upto.epoch < watermark_.epoch ||
      (upto.epoch == watermark_.epoch && upto.offset <= watermark_.offset)) {
    return true;  // 幂等：水位只前进不回退
  }
  watermark_ = upto;
  PersistHeader();
  return true;
}

}  // namespace imobs
