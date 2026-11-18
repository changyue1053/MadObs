#pragma once
// 结构化环形日志（实施计划 P5.1，Mars xlog 定式的最小可信实现）：
//   - "全都记、都不传、按需取"：本地环形覆盖最旧，回捞时按 wall 时间窗 + 模块过滤提取；
//   - 复用 WalRing 的三件资产：覆盖最旧的分段环、crc 撕裂防护、kill -9 零丢已完成记录；
//   - 结构化是回捞效率的源头：level/module/关联字段进字段而非拼进文本；
//   - 静态加密：AES-256-GCM 逐条封印（crypto 提供方）。fail-closed：给了密钥但
//     本构建无提供方 → Open 直接失败，绝不静默落明文（与 IMDB §14 同一纪律）。
//     nonce = 每次 Open 随机 4B salt || 8B 递增计数——同 key 跨重启绝不复用。
// 线程契约：仅消费线程调用（业务日志经打点事件进环后由消费线程落盘——本层是落盘终点）。

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imobs/wal_ring.h"

namespace imobs {

class LogStore {
 public:
  struct Entry {
    int64_t wall_ms = 0;
    uint8_t level = 0;  // 0=debug 1=info 2=warn 3=error
    uint64_t module_hash = 0;
    std::string text;
  };

  // key32：非空 = 启用逐条 AES-256-GCM（32 字节，宿主从 Keychain/Keystore 注入）。
  static std::unique_ptr<LogStore> Open(const std::string& path, uint32_t segment_size,
                                        uint32_t segment_count, std::string* err,
                                        const uint8_t* key32 = nullptr);

  // 超长文本截断到 kMaxText（截断也要留头部——日志头部信息密度最高）。
  void Append(int64_t wall_ms, uint8_t level, uint64_t module_hash, std::string_view text);

  // 回捞窗口提取（kFetchLogs 命令的执行体）：[start, end] 闭区间；module=0 不过滤。
  std::vector<Entry> Extract(int64_t start_wall_ms, int64_t end_wall_ms,
                             uint64_t module_hash_filter = 0) const;

  WalRing::Stats wal_stats() const { return wal_->stats(); }

  static constexpr size_t kMaxText = 2048;

 private:
  LogStore() = default;
  std::unique_ptr<WalRing> wal_;
  bool encrypted_ = false;
  std::array<uint8_t, 32> key_{};
  std::array<uint8_t, 4> nonce_salt_{};  // 每次 Open 随机：跨重启的 nonce 唯一性
  uint64_t nonce_counter_ = 0;
};

}  // namespace imobs
