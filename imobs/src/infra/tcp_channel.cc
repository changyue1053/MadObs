#include "imobs/tcp_channel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <utility>

namespace imobs {
namespace {

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

}  // namespace

TcpChannel::TcpChannel(Options opt) : opt_(std::move(opt)) {
  worker_ = std::thread([this] { Run(); });
}

TcpChannel::~TcpChannel() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  worker_.join();
  CloseFd();
  // 未决任务以失败收尾：SendCallback"恰好一次"的契约在关停路径同样成立
  for (Task& t : queue_) t.cb({false});
  queue_.clear();
}

void TcpChannel::Send(Priority pri, uint64_t batch_id, std::string_view payload,
                      SendCallback cb) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!stop_) {
      queue_.push_back(Task{pri, batch_id, std::string(payload), std::move(cb)});
      cv_.notify_one();
      return;
    }
  }
  cb({false});  // 已关停：立即如实失败
}

TcpChannel::Stats TcpChannel::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

void TcpChannel::CloseFd() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool TcpChannel::EnsureConnected() {
  if (fd_ >= 0) return true;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

  timeval tv{};
  tv.tv_sec = opt_.io_timeout_ms / 1000;
  tv.tv_usec = (opt_.io_timeout_ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // 库不改全局信号处置
#endif
  const int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt_.port);
  if (::inet_pton(AF_INET, opt_.host.c_str(), &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.reconnects;
  }
  return true;
}

bool TcpChannel::SendOne(const Task& task) {
  if (!EnsureConnected()) return false;
  uint8_t hdr[4];
  const auto len = static_cast<uint32_t>(task.payload.size());
  std::memcpy(hdr, &len, 4);
  if (!WriteAll(fd_, hdr, 4) || !WriteAll(fd_, task.payload.data(), task.payload.size())) {
    CloseFd();  // 写失败：断开重连留给下一个任务
    return false;
  }
  uint64_t ack = 0;
  if (!ReadAll(fd_, &ack, 8)) {
    CloseFd();
    return false;
  }
  return ack == task.batch_id;  // 回执必须回显 batch_id：错单即失败
}

void TcpChannel::Run() {
  for (;;) {
    Task task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;  // 剩余任务由析构以失败收尾
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    const bool ok = SendOne(task);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (ok) {
        ++stats_.sent_ok;
      } else {
        ++stats_.sent_fail;
      }
    }
    task.cb({ok});  // 任意线程回调：pipeline 的完成队列负责转交消费线程
  }
}

}  // namespace imobs
