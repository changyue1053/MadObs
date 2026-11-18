#include "imobs/collector.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "imobs/wire.h"

namespace imobs {
namespace {

bool ReadAll(int fd, void* data, size_t len) {
  auto* p = static_cast<uint8_t*>(data);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::recv(fd, p + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool WriteAll(int fd, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(fd, p + off, len - off, 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

constexpr uint32_t kMaxBatchLen = 4 * 1024 * 1024;

}  // namespace

bool Collector::Start(uint16_t port, std::string* err) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    if (err) *err = "socket failed";
    return false;
  }
  const int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    if (err) *err = "bind/listen failed";
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen);
  port_ = ntohs(bound.sin_port);
  stop_.store(false);
  // fd 按值交给循环线程：Stop() 对成员的写与循环的读从此无共享（TSan 干净）。
  accept_thread_ = std::thread([this, fd = listen_fd_] { AcceptLoop(fd); });
  return true;
}

void Collector::Stop() {
  if (listen_fd_ < 0) return;
  stop_.store(true);
  ::shutdown(listen_fd_, SHUT_RDWR);  // 打断 accept
  ::close(listen_fd_);
  listen_fd_ = -1;
  if (accept_thread_.joinable()) accept_thread_.join();  // 先停 accept：不再产生新连接
  std::vector<std::thread> conns;
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    // 对端（通道）可能仍持有连接：必须主动 shutdown 解除连接线程的 recv 阻塞，
    // 否则 join 会等一个永远不来的 EOF——服务停机不能指望客户端配合。
    for (const int fd : conn_fds_) ::shutdown(fd, SHUT_RDWR);
    conns.swap(conn_threads_);
  }
  for (auto& t : conns) {
    if (t.joinable()) t.join();
  }
}

void Collector::AcceptLoop(int listen_fd) {
  for (;;) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) return;  // Stop() 关闭监听后退出
    std::lock_guard<std::mutex> lk(conn_mu_);
    conn_fds_.insert(fd);
    conn_threads_.emplace_back([this, fd] { ServeConnection(fd); });
  }
}

void Collector::ServeConnection(int fd) {
  std::vector<uint8_t> payload;
  for (;;) {
    uint8_t hdr[4];
    if (!ReadAll(fd, hdr, 4)) break;
    uint32_t len = 0;
    std::memcpy(&len, hdr, 4);
    if (len == 0 || len > kMaxBatchLen) break;  // 坏帧：断开（网关会记坏包账）
    payload.resize(len);
    if (!ReadAll(fd, payload.data(), len)) break;

    WireReader r(payload.data(), payload.size());
    uint64_t batch_id = 0;
    if (!r.Fixed64(&batch_id)) break;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!seen_batch_ids_.insert(batch_id).second) {
        ++stats_.duplicates;  // 幂等：重复批不入账——但照样 ACK（见下）
      } else {
        fresh = true;
        ++stats_.batches;
        stats_.trace_events += stitcher_.IngestBatch(payload.data(), payload.size());
        if (sample_batch_.empty()) sample_batch_ = payload;
      }
    }
    // 观察者在锁外回调：观察者自己加锁，与本类查询路径没有锁序耦合（防死锁）。
    if (fresh && observer_) observer_(payload.data(), payload.size());
    // 重复批也要 ACK：端侧只需要"服务端已收到"这一个事实（at-least-once + 去重）
    if (!WriteAll(fd, &batch_id, 8)) break;
  }
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    conn_fds_.erase(fd);  // 先出册再关闭：Stop 的 shutdown 绝不触碰已复用的 fd
  }
  ::close(fd);
}

Collector::Stats Collector::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

size_t Collector::JourneyCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stitcher_.JourneyCount();
}

std::vector<uint8_t> Collector::StagesOf(uint64_t msg_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return stitcher_.StagesOf(msg_id);
}

bool Collector::JourneyComplete(uint64_t msg_id, const std::vector<uint8_t>& required) const {
  std::lock_guard<std::mutex> lk(mu_);
  return stitcher_.Complete(msg_id, required);
}

std::vector<uint64_t> Collector::JourneyIds(size_t limit) const {
  std::lock_guard<std::mutex> lk(mu_);
  return stitcher_.JourneyIds(limit);
}

std::vector<uint8_t> Collector::SampleBatch() const {
  std::lock_guard<std::mutex> lk(mu_);
  return sample_batch_;
}

}  // namespace imobs
