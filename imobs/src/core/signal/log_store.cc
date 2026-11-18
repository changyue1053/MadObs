#include "imobs/log_store.h"

#include <random>

#include "imobs/crypto.h"
#include "imobs/wire.h"

namespace imobs {
namespace {

// WAL 载荷首字节 = 格式版本：明文 / GCM 封印。未知格式跳过（向前兼容）。
constexpr uint8_t kFmtPlain = 0;
constexpr uint8_t kFmtGcm = 1;

// 明文记录体：[fixed64 wall_ms][u8 level][fixed64 module][varint len][text]
void EncodeBody(int64_t wall_ms, uint8_t level, uint64_t module_hash, std::string_view text,
                std::vector<uint8_t>* out) {  // audit-allow: cold-path 日志落盘编码（消费线程，日志行级），非打点 per-event 路径
  WireWriter w(out);
  w.Fixed64(static_cast<uint64_t>(wall_ms));
  w.U8(level);
  w.Fixed64(module_hash);
  w.Varint(text.size());
  w.Bytes(text.data(), text.size());
}

bool DecodeBody(const uint8_t* data, size_t len, LogStore::Entry* e) {
  WireReader rd(data, len);
  uint64_t wall = 0;
  uint64_t tlen = 0;
  const uint8_t* text = nullptr;
  if (!rd.Fixed64(&wall) || !rd.U8(&e->level) || !rd.Fixed64(&e->module_hash) ||
      !rd.Varint(&tlen) || tlen > LogStore::kMaxText || !rd.Bytes(&text, tlen)) {
    return false;
  }
  e->wall_ms = static_cast<int64_t>(wall);
  e->text.assign(reinterpret_cast<const char*>(text), tlen);
  return true;
}

}  // namespace

std::unique_ptr<LogStore> LogStore::Open(const std::string& path, uint32_t segment_size,  // audit-allow: cold-path Open 启动一次
                                         uint32_t segment_count, std::string* err,  // audit-allow: cold-path Open 启动一次
                                         const uint8_t* key32) {
  if (key32 != nullptr && !crypto::Available()) {
    // fail-closed：要求加密但本构建无提供方——宁可开不了，绝不静默落明文。
    if (err) *err = "log encryption requested but no crypto provider linked";
    return nullptr;
  }
  WalOptions opt;
  opt.segment_size = segment_size;
  opt.segment_count = segment_count;
  opt.overwrite_oldest = true;    // 日志天然可再生：环形覆盖，永不拒绝写入
  opt.sync_every_append = false;  // 崩溃口径：kill -9 零丢；掉电丢尾部可接受
  std::unique_ptr<LogStore> ls(new LogStore());  // audit-allow: cold-path Open 启动一次（私有构造）
  ls->wal_ = WalRing::Open(path, opt, err);
  if (!ls->wal_) return nullptr;
  if (key32 != nullptr) {
    ls->encrypted_ = true;
    std::copy(key32, key32 + 32, ls->key_.begin());
    std::random_device rd;  // 非确定性熵源仅用于 nonce salt（非密钥材料）
    for (auto& b : ls->nonce_salt_) b = static_cast<uint8_t>(rd());
  }
  return ls;
}

void LogStore::Append(int64_t wall_ms, uint8_t level, uint64_t module_hash,
                      std::string_view text) {
  if (text.size() > kMaxText) text = text.substr(0, kMaxText);
  std::vector<uint8_t> body;  // audit-allow: cold-path 日志落盘（消费线程，日志行级频率）
  body.reserve(24 + text.size());
  EncodeBody(wall_ms, level, module_hash, text, &body);

  std::vector<uint8_t> rec;  // audit-allow: cold-path 日志落盘（消费线程，日志行级频率）
  if (!encrypted_) {
    rec.resize(1 + body.size());
    rec[0] = kFmtPlain;
    std::copy(body.begin(), body.end(), rec.begin() + 1);
  } else {
    // nonce = salt(4B, 每次 Open 随机) || counter(8B, 单调)：同 key 绝不复用。
    uint8_t nonce[crypto::kGcmNonceLen];
    std::copy(nonce_salt_.begin(), nonce_salt_.end(), nonce);
    const uint64_t c = nonce_counter_++;
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(c >> (i * 8));
    const uint8_t aad[1] = {kFmtGcm};  // 格式字节参与认证：改标即篡改
    std::vector<uint8_t> sealed;  // audit-allow: cold-path GCM 封印产物（日志行级频率）
    if (!crypto::AesGcmSeal(key_.data(), nonce, aad, 1, body.data(), body.size(), &sealed)) {
      return;  // 提供方失败（理论不可达：Open 已 fail-closed）——丢弃胜于明文落盘
    }
    rec.resize(1 + sizeof(nonce) + sealed.size());
    rec[0] = kFmtGcm;
    std::copy(nonce, nonce + sizeof(nonce), rec.begin() + 1);
    std::copy(sealed.begin(), sealed.end(), rec.begin() + 1 + sizeof(nonce));
  }
  wal_->Append(rec.data(), static_cast<uint32_t>(rec.size()));
}

std::vector<LogStore::Entry> LogStore::Extract(int64_t start_wall_ms, int64_t end_wall_ms,  // audit-allow: cold-path 回捞提取（指令级频率）
                                               uint64_t module_hash_filter) const {
  std::vector<Entry> out;  // audit-allow: cold-path 回捞提取（指令级频率）
  wal_->ScanPending([&](const WalRing::Record& r) {
    if (r.payload.empty()) return true;
    const auto* p = reinterpret_cast<const uint8_t*>(r.payload.data());
    const size_t n = r.payload.size();
    Entry e;
    bool ok = false;
    if (p[0] == kFmtPlain) {
      ok = DecodeBody(p + 1, n - 1, &e);
    } else if (p[0] == kFmtGcm && encrypted_ && n > 1 + crypto::kGcmNonceLen) {
      const uint8_t aad[1] = {kFmtGcm};
      std::vector<uint8_t> body;  // audit-allow: cold-path 回捞解密缓冲（指令级频率）
      ok = crypto::AesGcmOpen(key_.data(), p + 1, aad, 1, p + 1 + crypto::kGcmNonceLen,
                              n - 1 - crypto::kGcmNonceLen, &body) &&
           DecodeBody(body.data(), body.size(), &e);
    }
    // 解不开（错钥/篡改/未知格式）与坏记录同一处置：跳过——回捞不因脏数据整体失败。
    if (!ok) return true;
    if (e.wall_ms < start_wall_ms || e.wall_ms > end_wall_ms) return true;
    if (module_hash_filter != 0 && e.module_hash != module_hash_filter) return true;
    out.push_back(std::move(e));
    return true;
  });
  return out;
}

}  // namespace imobs
