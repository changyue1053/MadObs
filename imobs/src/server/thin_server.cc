#include "imobs/thin_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "imobs/batch_view.h"

namespace imobs {
namespace {

// —— 极简 JSON 组装（查询面只输出，不解析；文本字段逐字符转义）——
void JsonEscape(const std::string& s, std::string* out) {
  for (const char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          *out += c;
        }
    }
  }
}

// 查询参数（k=v&k=v，薄形态只取用到的键；不做百分号解码——参数全是数字/标识符）。
std::string QueryParam(const std::string& path, const std::string& key) {
  const size_t q = path.find('?');
  if (q == std::string::npos) return {};
  size_t pos = q + 1;
  while (pos < path.size()) {
    size_t amp = path.find('&', pos);
    if (amp == std::string::npos) amp = path.size();
    const std::string kv = path.substr(pos, amp - pos);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
    pos = amp + 1;
  }
  return {};
}

uint64_t ParseU64(const std::string& s, uint64_t fallback) {
  if (s.empty()) return fallback;
  uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return fallback;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  return v;
}

bool ReadRequest(int fd, std::string* req) {
  char buf[2048];
  req->clear();
  while (req->size() < 8192) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    req->append(buf, static_cast<size_t>(n));
    if (req->find("\r\n\r\n") != std::string::npos) return true;
  }
  return false;
}

void WriteResponse(int fd, int code, const std::string& body) {
  std::string resp = "HTTP/1.0 ";
  resp += code == 200 ? "200 OK" : (code == 404 ? "404 Not Found" : "400 Bad Request");
  resp += "\r\nContent-Type: application/json\r\nContent-Length: ";
  resp += std::to_string(body.size());
  resp += "\r\nConnection: close\r\n\r\n";
  resp += body;
  size_t off = 0;
  while (off < resp.size()) {
    const ssize_t n = ::send(fd, resp.data() + off, resp.size() - off, 0);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

}  // namespace

bool ThinServer::Start(const Options& opt, std::string* err) {
  opt_ = opt;
  // ingest：观察者必须先于 Start 挂上（Collector 契约）。
  collector_.SetBatchObserver([this](const void* d, size_t n) { OnBatch(d, n); });
  if (!collector_.Start(opt.ingest_port, err)) return false;

  http_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (http_fd_ < 0) {
    if (err) *err = "http socket failed";
    collector_.Stop();
    return false;
  }
  const int one = 1;
  ::setsockopt(http_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(opt.http_port);
  if (::bind(http_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(http_fd_, 16) != 0) {
    if (err) *err = "http bind/listen failed";
    ::close(http_fd_);
    http_fd_ = -1;
    collector_.Stop();
    return false;
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  ::getsockname(http_fd_, reinterpret_cast<sockaddr*>(&bound), &blen);
  http_port_ = ntohs(bound.sin_port);
  http_thread_ = std::thread([this, fd = http_fd_] { HttpLoop(fd); });
  return true;
}

void ThinServer::Stop() {
  if (http_fd_ >= 0) {
    ::shutdown(http_fd_, SHUT_RDWR);  // 打断 accept
    ::close(http_fd_);
    http_fd_ = -1;
    if (http_thread_.joinable()) http_thread_.join();
  }
  collector_.Stop();
}

// ingest 聚合：与端侧 Encode* 同一套 Decode*（协议单一事实源）。
void ThinServer::OnBatch(const void* data, size_t len) {
  BatchView v;
  if (!ParseBatchView(data, len, &v)) return;  // 坏批：Collector 已有帧级防御，这里静默
  std::lock_guard<std::mutex> lk(agg_mu_);
  for (const auto& rec : v.records) {
    if (rec.empty()) continue;
    const auto* p = reinterpret_cast<const uint8_t*>(rec.data());
    switch (static_cast<uint8_t>(rec[0])) {
      case kRecMetrics: {
        std::vector<MetricsRowView> rows;
        if (!DecodeMetricsChunk(p, rec.size(), &rows)) break;
        for (auto& r : rows) {
          MetricRow& agg = metrics_[{r.key, r.label_idx}];
          agg.kind = r.kind;
          agg.key = r.key;
          agg.label_idx = r.label_idx;
          agg.count += r.count;
          agg.sum += r.sum;
          if (r.kind == 1) agg.digest.Merge(r.digest);  // t-digest 可合并性=聚合正确性
        }
        break;
      }
      case kRecEvent: {
        EventRecordWire e;
        if (!DecodeEventRecord(p, rec.size(), &e)) break;
        events_.push_back(e);
        if (events_.size() > opt_.event_cap) events_.pop_front();
        break;
      }
      case kRecHeal: {
        HealRecordWire hr;
        if (!DecodeHealRecord(p, rec.size(), &hr)) break;
        heals_.push_back(std::move(hr));
        if (heals_.size() > opt_.event_cap) heals_.pop_front();
        break;
      }
      case kRecLogs: {
        uint64_t cmd = 0;
        std::vector<LogEntryWire> entries;
        if (!DecodeLogChunk(p, rec.size(), &cmd, &entries)) break;
        auto& dst = logs_[cmd];
        dst.insert(dst.end(), std::make_move_iterator(entries.begin()),
                   std::make_move_iterator(entries.end()));
        break;
      }
      default:
        break;  // 'T' 由 Collector 内的拼接器消费；未知类型向前兼容跳过
    }
  }
}

std::vector<ThinServer::MetricRow> ThinServer::MetricsSnapshot() const {
  std::lock_guard<std::mutex> lk(agg_mu_);
  std::vector<MetricRow> out;
  out.reserve(metrics_.size());
  for (const auto& [k, row] : metrics_) out.push_back(row);
  return out;
}

std::vector<LogEntryWire> ThinServer::LogsOf(uint64_t command_id) const {
  std::lock_guard<std::mutex> lk(agg_mu_);
  const auto it = logs_.find(command_id);
  return it == logs_.end() ? std::vector<LogEntryWire>{} : it->second;
}

void ThinServer::HttpLoop(int listen_fd) {
  for (;;) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) return;  // Stop() 关闭监听后退出
    std::string req;
    if (ReadRequest(fd, &req)) {
      // "GET <path> HTTP/1.x"
      std::string path;
      if (req.rfind("GET ", 0) == 0) {
        const size_t sp = req.find(' ', 4);
        if (sp != std::string::npos) path = req.substr(4, sp - 4);
      }
      if (path.empty()) {
        WriteResponse(fd, 400, "{\"error\":\"bad-request\"}");
      } else {
        const std::string body = Handle(path);
        WriteResponse(fd, body.empty() ? 404 : 200,
                      body.empty() ? "{\"error\":\"not-found\"}" : body);
      }
    }
    ::close(fd);
  }
}

std::string ThinServer::Handle(const std::string& path) const {
  const std::string route = path.substr(0, path.find('?'));

  if (route == "/healthz") {
    const auto st = collector_.stats();
    std::string b = "{\"ok\":true,\"batches\":" + std::to_string(st.batches) +
                    ",\"duplicates\":" + std::to_string(st.duplicates) +
                    ",\"trace_events\":" + std::to_string(st.trace_events) +
                    ",\"journeys\":" + std::to_string(collector_.JourneyCount()) + "}";
    return b;
  }

  if (route == "/metrics") {
    std::string b = "[";
    bool first = true;
    std::lock_guard<std::mutex> lk(agg_mu_);
    for (const auto& [k, row] : metrics_) {
      if (!first) b += ',';
      first = false;
      b += "{\"key\":" + std::to_string(row.key) +
           ",\"label_idx\":" + std::to_string(row.label_idx) +
           ",\"kind\":" + std::to_string(row.kind) +
           ",\"count\":" + std::to_string(row.count) + ",\"sum\":" + std::to_string(row.sum);
      if (row.kind == 1 && row.count > 0) {
        b += ",\"p50\":" + std::to_string(row.digest.Quantile(0.5)) +
             ",\"p99\":" + std::to_string(row.digest.Quantile(0.99));
      }
      b += '}';
    }
    b += ']';
    return b;
  }

  if (route == "/journeys") {
    const uint64_t limit = ParseU64(QueryParam(path, "limit"), 100);
    std::string b = "[";
    bool first = true;
    for (const uint64_t id : collector_.JourneyIds(limit)) {
      if (!first) b += ',';
      first = false;
      b += "{\"msg_id\":" + std::to_string(id) + ",\"stages\":[";
      const auto stages = collector_.StagesOf(id);
      for (size_t i = 0; i < stages.size(); ++i) {
        if (i != 0) b += ',';
        b += std::to_string(stages[i]);
      }
      b += "]}";
    }
    b += ']';
    return b;
  }

  if (route == "/journey") {
    const uint64_t id = ParseU64(QueryParam(path, "msg"), 0);
    const auto stages = collector_.StagesOf(id);
    if (stages.empty()) return {};
    std::string b = "{\"msg_id\":" + std::to_string(id) + ",\"stages\":[";
    for (size_t i = 0; i < stages.size(); ++i) {
      if (i != 0) b += ',';
      b += std::to_string(stages[i]);
    }
    b += "]}";
    return b;
  }

  if (route == "/heals") {
    const uint64_t limit = ParseU64(QueryParam(path, "limit"), 100);
    std::string b = "[";
    std::lock_guard<std::mutex> lk(agg_mu_);
    size_t n = 0;
    for (const auto& hr : heals_) {
      if (n++ >= limit) break;
      if (n > 1) b += ',';
      b += "{\"scene\":" + std::to_string(hr.scene_id) + ",\"from\":" + std::to_string(hr.from) +
           ",\"to\":" + std::to_string(hr.to) + ",\"reason\":" + std::to_string(hr.reason) +
           ",\"action\":\"";
      JsonEscape(hr.action, &b);
      b += "\"}";
    }
    b += ']';
    return b;
  }

  if (route == "/events") {
    const uint64_t limit = ParseU64(QueryParam(path, "limit"), 100);
    std::string b = "[";
    std::lock_guard<std::mutex> lk(agg_mu_);
    size_t n = 0;
    for (const auto& e : events_) {
      if (n++ >= limit) break;
      if (n > 1) b += ',';
      b += "{\"key\":" + std::to_string(e.key) + ",\"level\":" + std::to_string(e.level) +
           ",\"a\":" + std::to_string(e.a) + ",\"b\":" + std::to_string(e.b) + "}";
    }
    b += ']';
    return b;
  }

  if (route == "/logs") {
    const uint64_t cmd = ParseU64(QueryParam(path, "cmd"), 0);
    std::lock_guard<std::mutex> lk(agg_mu_);
    const auto it = logs_.find(cmd);
    if (it == logs_.end()) return {};
    std::string b = "[";
    for (size_t i = 0; i < it->second.size(); ++i) {
      const auto& e = it->second[i];
      if (i != 0) b += ',';
      b += "{\"wall_ms\":" + std::to_string(e.wall_ms) +
           ",\"level\":" + std::to_string(e.level) +
           ",\"module\":" + std::to_string(e.module_hash) + ",\"text\":\"";
      JsonEscape(e.text, &b);
      b += "\"}";
    }
    b += ']';
    return b;
  }

  return {};
}

}  // namespace imobs
