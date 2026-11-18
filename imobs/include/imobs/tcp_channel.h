#pragma once
// 默认 TCP 上报通道（实施计划 P2.5 收尾）：长度前缀帧 + batch_id 回执。
//
// 形态决策：
//   - 自带发送线程：Send() 拷贝入队即返回，绝不阻塞消费线程（打点方之后的
//     第二条"无害"防线——网络的慢永远只堵通道自己）；
//   - 通道只管"一次尝试"：失败经回调如实上报，退避/重发/换端点由 UploadPipeline
//     与宿主容灾策略负责；
//   - 断线自动重连（下一个任务前）；回执 = 8 字节 batch_id 回显（收集器协议）。
// 生产注记：TLS、多端点容灾、蜂窝网络策略在宿主通道实现；本实现面向
// loopback/内网收集器（e2e 资产 + 内网部署形态）。
//
// 帧协议：上行 [u32 len LE][batch bytes]；下行 [u64 batch_id LE]（ACK）。

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "imobs/pipeline.h"

namespace imobs {

class TcpChannel final : public IChannel {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    int io_timeout_ms = 2'000;  // 单次收发超时（含 connect）
  };

  explicit TcpChannel(Options opt);
  ~TcpChannel() override;  // 停线程；未决任务以失败回调收尾（恰好一次的契约不破）

  void Send(Priority pri, uint64_t batch_id, std::string_view payload,
            SendCallback cb) override;

  struct Stats {
    uint64_t sent_ok = 0;
    uint64_t sent_fail = 0;
    uint64_t reconnects = 0;
  };
  Stats stats() const;

 private:
  struct Task {
    Priority pri;
    uint64_t batch_id;
    std::string payload;
    SendCallback cb;
  };

  void Run();
  bool EnsureConnected();
  void CloseFd();
  bool SendOne(const Task& task);

  Options opt_;
  int fd_ = -1;  // 仅发送线程触碰

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  bool stop_ = false;
  Stats stats_;

  std::thread worker_;
};

}  // namespace imobs
