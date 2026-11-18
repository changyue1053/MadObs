#include "imobs/wal_ring.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imobs_test.h"

using imobs::WalOptions;
using imobs::WalRing;

namespace {

std::string TmpWalPath(const char* tag) {
  char tmpl[] = "/tmp/imobs_wal_XXXXXX";
  char* dir = mkdtemp(tmpl);
  return std::string(dir) + "/" + tag + ".wal";
}

std::vector<std::string> Pending(const WalRing& w, std::vector<WalRing::Cursor>* ends = nullptr) {
  std::vector<std::string> out;
  w.ScanPending([&](const WalRing::Record& r) {
    out.emplace_back(r.payload);
    if (ends != nullptr) ends->push_back(r.end);
    return true;
  });
  return out;
}

}  // namespace

TEST(WalRing, AppendScanRoundtrip) {
  const std::string path = TmpWalPath("roundtrip");
  WalOptions opt;
  opt.segment_size = 1024;
  opt.segment_count = 4;
  std::string err;
  auto w = WalRing::Open(path, opt, &err);
  ASSERT_TRUE(w != nullptr);
  for (int i = 0; i < 10; ++i) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "rec-%03d", i);
    ASSERT_TRUE(w->Append(buf, static_cast<uint32_t>(n)));
  }
  const auto got = Pending(*w);
  ASSERT_EQ(got.size(), size_t{10});
  EXPECT_TRUE(got.front() == std::string("rec-000"));
  EXPECT_TRUE(got.back() == std::string("rec-009"));
  EXPECT_EQ(w->stats().appended, 10u);
  EXPECT_EQ(w->stats().torn_dropped, 0u);
}

TEST(WalRing, WatermarkPersistsAcrossReopen) {
  const std::string path = TmpWalPath("wm");
  WalOptions opt;
  opt.segment_size = 1024;
  opt.segment_count = 4;
  std::string err;
  {
    auto w = WalRing::Open(path, opt, &err);
    ASSERT_TRUE(w != nullptr);
    for (int i = 0; i < 6; ++i) {
      char b[16];
      const int n = std::snprintf(b, sizeof(b), "r%d", i);
      ASSERT_TRUE(w->Append(b, static_cast<uint32_t>(n)));
    }
    std::vector<WalRing::Cursor> ends;
    const auto got = Pending(*w, &ends);
    ASSERT_EQ(got.size(), size_t{6});
    ASSERT_TRUE(w->MarkConsumed(ends[3]));  // 消费到第 4 条（r3）
  }
  auto w2 = WalRing::Open(path, opt, &err);
  ASSERT_TRUE(w2 != nullptr);
  const auto got = Pending(*w2);
  ASSERT_EQ(got.size(), size_t{2});
  EXPECT_TRUE(got[0] == std::string("r4"));
  EXPECT_TRUE(got[1] == std::string("r5"));
}

TEST(WalRing, TornTailDroppedOnRecover) {
  const std::string path = TmpWalPath("torn");
  WalOptions opt;
  opt.segment_size = 1024;
  opt.segment_count = 2;
  std::string err;
  {
    auto w = WalRing::Open(path, opt, &err);
    ASSERT_TRUE(w != nullptr);
    ASSERT_TRUE(w->Append("aaaaaaaa", 8));
    ASSERT_TRUE(w->Append("bbbbbbbb", 8));
    ASSERT_TRUE(w->Append("cccccccc", 8));
  }
  {
    // 翻转第三条 payload 的一个字节，模拟写坏/撕裂。
    // 布局：4096 头页 + 16 段头 + 每条 (8 头 + 8 载荷)；第三条 payload 文件偏移：
    FILE* f = std::fopen(path.c_str(), "r+b");
    ASSERT_TRUE(f != nullptr);
    const long off = 4096 + 16 + 2 * 16 + 8;
    ASSERT_EQ(std::fseek(f, off, SEEK_SET), 0);
    const int ch = std::fgetc(f);
    ASSERT_TRUE(ch != EOF);
    ASSERT_EQ(std::fseek(f, off, SEEK_SET), 0);
    std::fputc(ch ^ 0xFF, f);
    ASSERT_EQ(std::fclose(f), 0);
  }
  auto w = WalRing::Open(path, opt, &err);
  ASSERT_TRUE(w != nullptr);
  EXPECT_EQ(w->stats().torn_dropped, 1u);
  const auto got = Pending(*w);
  ASSERT_EQ(got.size(), size_t{2});  // 撕裂的第三条被丢弃，绝不吐半条记录
  EXPECT_TRUE(got[1] == std::string("bbbbbbbb"));

  // 撕裂点被清零，之后继续追加不受影响
  ASSERT_TRUE(w->Append("dddddddd", 8));
  EXPECT_EQ(Pending(*w).size(), size_t{3});
}

TEST(WalRing, OverwriteOldestKeepsNewestWindow) {
  const std::string path = TmpWalPath("wrap");
  WalOptions opt;
  opt.segment_size = 256;  // 可用 240 = 5 条 × (8+40)
  opt.segment_count = 3;
  opt.overwrite_oldest = true;  // P1/P2 语义
  std::string err;
  auto w = WalRing::Open(path, opt, &err);
  ASSERT_TRUE(w != nullptr);
  char payload[40];
  for (int i = 0; i < 25; ++i) {
    std::memset(payload, 'a' + (i % 26), sizeof(payload));
    ASSERT_TRUE(w->Append(payload, sizeof(payload)));
  }
  const auto got = Pending(*w);
  ASSERT_EQ(got.size(), size_t{15});  // 3 段 × 5 条：只保最新窗口
  std::memset(payload, 'a' + (10 % 26), sizeof(payload));
  EXPECT_TRUE(got.front() == std::string(payload, sizeof(payload)));  // 最旧存活 = 第 11 条
  std::memset(payload, 'a' + (24 % 26), sizeof(payload));
  EXPECT_TRUE(got.back() == std::string(payload, sizeof(payload)));
  EXPECT_EQ(w->stats().overflow_dropped, 0u);
}

TEST(WalRing, NoOverwritePolicyRejectsWhenFull) {
  const std::string path = TmpWalPath("p0");
  WalOptions opt;
  opt.segment_size = 256;
  opt.segment_count = 2;
  opt.overwrite_oldest = false;  // P0 语义：永不覆盖
  std::string err;
  auto w = WalRing::Open(path, opt, &err);
  ASSERT_TRUE(w != nullptr);
  char payload[40];
  std::memset(payload, 'x', sizeof(payload));
  uint64_t ok = 0;
  for (int i = 0; i < 30; ++i) {
    if (w->Append(payload, sizeof(payload))) ++ok;
  }
  EXPECT_EQ(ok, 10u);  // 2 段 × 5 条
  EXPECT_EQ(w->stats().overflow_dropped, 20u);  // 满溢有账（本身是强信号）

  // ACK 之后段可回收，继续可写
  std::vector<WalRing::Cursor> ends;
  Pending(*w, &ends);
  ASSERT_TRUE(!ends.empty());
  ASSERT_TRUE(w->MarkConsumed(ends.back()));
  EXPECT_TRUE(w->Append(payload, sizeof(payload)));
}

IMOBS_TEST_MAIN
