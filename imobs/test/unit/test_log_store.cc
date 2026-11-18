#include "imobs/log_store.h"

#include <unistd.h>

#include <cstdio>
#include <string>

#include "imobs/crypto.h"
#include "imobs/schema.h"
#include "imobs_test.h"

using imobs::Fnv1a;
using imobs::LogStore;

namespace {

std::string TmpLogPath(const char* tag) {
  char tmpl[] = "/tmp/imobs_log_XXXXXX";
  char* dir = mkdtemp(tmpl);
  return std::string(dir) + "/" + tag + ".log";
}

constexpr uint64_t kModLink = Fnv1a("imlink");
constexpr uint64_t kModDb = Fnv1a("imdb");

}  // namespace

TEST(LogStore, ExtractByTimeWindowAndModule) {
  const std::string path = TmpLogPath("win");
  std::string err;
  auto ls = LogStore::Open(path, 8192, 4, &err);
  ASSERT_TRUE(ls != nullptr);

  ls->Append(1'000, 1, kModLink, "connect start");
  ls->Append(2'000, 2, kModDb, "slow query 12ms");
  ls->Append(3'000, 3, kModLink, "handshake timeout");
  ls->Append(4'000, 1, kModDb, "checkpoint done");

  // 时间窗 [1500, 3500]：两条
  auto win = ls->Extract(1'500, 3'500);
  ASSERT_EQ(win.size(), size_t{2});
  EXPECT_TRUE(win[0].text == "slow query 12ms");
  EXPECT_TRUE(win[1].text == "handshake timeout");
  EXPECT_EQ(win[1].level, 3);

  // 模块过滤：回捞指令常见形态 "只要 imlink 的"
  auto only_link = ls->Extract(0, 10'000, kModLink);
  ASSERT_EQ(only_link.size(), size_t{2});
  EXPECT_TRUE(only_link[0].text == "connect start");
  EXPECT_TRUE(only_link[1].text == "handshake timeout");
}

TEST(LogStore, OverwriteOldestKeepsNewestWindow) {
  const std::string path = TmpLogPath("wrap");
  std::string err;
  auto ls = LogStore::Open(path, 512, 3, &err);  // 小段：快速环回
  ASSERT_TRUE(ls != nullptr);

  const std::string line(80, 'x');  // 每条 ~100B，每段 ~4 条
  for (int i = 0; i < 40; ++i) {
    ls->Append(1'000 + i, 1, kModLink, line);
  }
  auto all = ls->Extract(0, 10'000);
  ASSERT_TRUE(all.size() < 40);      // 最旧被覆盖
  ASSERT_TRUE(all.size() >= 8);      // 但保住了最新窗口
  EXPECT_EQ(all.back().wall_ms, 1'039);  // 尾部必是最新一条
  // 存活区间必须是连续尾部（环形覆盖不产生空洞）
  for (size_t i = 1; i < all.size(); ++i) {
    EXPECT_EQ(all[i].wall_ms, all[i - 1].wall_ms + 1);
  }
}

TEST(LogStore, SurvivesReopenAndTruncatesOversize) {
  const std::string path = TmpLogPath("reopen");
  std::string err;
  {
    auto ls = LogStore::Open(path, 8192, 2, &err);
    ASSERT_TRUE(ls != nullptr);
    ls->Append(500, 2, kModDb, "before reopen");
    const std::string huge(10'000, 'y');  // 超长截断到 kMaxText
    ls->Append(600, 3, kModDb, huge);
  }
  auto ls = LogStore::Open(path, 8192, 2, &err);
  ASSERT_TRUE(ls != nullptr);
  auto all = ls->Extract(0, 1'000);
  ASSERT_EQ(all.size(), size_t{2});
  EXPECT_TRUE(all[0].text == "before reopen");
  EXPECT_EQ(all[1].text.size(), LogStore::kMaxText);
  EXPECT_EQ(ls->wal_stats().torn_dropped, 0u);
}

TEST(LogStore, EncryptedRoundTripPlaintextNeverTouchesDisk) {
  if (!imobs::crypto::Available()) {
    std::printf("        [skip] 本构建无强加密提供方\n");
    return;
  }
  const std::string path = TmpLogPath("enc");
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(0xA0 + i);
  std::string err;
  {
    auto ls = LogStore::Open(path, 8192, 2, &err, key);
    ASSERT_TRUE(ls != nullptr);
    ls->Append(100, 2, kModLink, "secret-conn-reset uid=42");
    ls->Append(200, 3, kModDb, "secret-sql-slow 812ms");
    // 同 store 内可读回。
    auto got = ls->Extract(0, 1'000);
    ASSERT_EQ(got.size(), size_t{2});
    EXPECT_TRUE(got[0].text == "secret-conn-reset uid=42");
  }
  // 磁盘字节里不允许出现任何明文子串（加密的最低语义）。
  {
    FILE* f = fopen(path.c_str(), "rb");
    ASSERT_TRUE(f != nullptr);
    std::string raw;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    EXPECT_TRUE(raw.find("secret-") == std::string::npos);
    EXPECT_TRUE(raw.find("conn-reset") == std::string::npos);
  }
  // 正确密钥重开：全部可读。错误密钥重开：解不开 → 跳过（回捞得 0 条，不崩不假读）。
  {
    auto ls = LogStore::Open(path, 8192, 2, &err, key);
    ASSERT_TRUE(ls != nullptr);
    EXPECT_EQ(ls->Extract(0, 1'000).size(), size_t{2});
  }
  {
    uint8_t bad[32];
    for (int i = 0; i < 32; ++i) bad[i] = static_cast<uint8_t>(i);
    auto ls = LogStore::Open(path, 8192, 2, &err, bad);
    ASSERT_TRUE(ls != nullptr);
    EXPECT_EQ(ls->Extract(0, 1'000).size(), size_t{0});
  }
}

IMOBS_TEST_MAIN
