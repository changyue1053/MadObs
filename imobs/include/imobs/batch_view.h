#pragma once
// 批视图解析（服务端/收集器/测试共用）：与 UploadPipeline 的批格式同源。
//
// 明文批  "IMB1"：[fixed64 batch_id][magic][u8 pri][fixed64 anchor_wall_ms]
//                 [fixed64 anchor_mono_ns][varint n][记录帧 × n（varint len + bytes）]
// 压缩批  "IMBZ"：头部同上，n 之后为 [varint raw_len][zlib(记录帧 × n)]。
//                 P0 永远明文（单条即发，压缩无收益且崩溃路径求简）；
//                 P1/P2 压不动（产物不小于原文）时回退明文——两种 magic 都是合法线上格式。
//
// batch_id = Fnv1a64(batch_id 之后的全部字节)：对**落盘/发送的字节**取哈希，
// 压缩批重发的是同一段压缩字节 → 同 id，幂等语义与明文一致。

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace imobs {

struct BatchView {
  uint64_t batch_id = 0;
  uint8_t pri = 0;
  bool compressed = false;
  uint64_t anchor_wall_ms = 0;
  uint64_t anchor_mono_ns = 0;
  std::vector<std::string_view> records;  // 明文批借用输入缓冲；压缩批借用 owned
  std::vector<uint8_t> owned;             // 压缩批的解压产物（records 指向它）

  BatchView() = default;
  BatchView(const BatchView&) = delete;  // records 是借用视图：拷贝会悬垂，move 才安全
  BatchView& operator=(const BatchView&) = delete;
  BatchView(BatchView&&) = default;
  BatchView& operator=(BatchView&&) = default;
};

// 解析一个完整批。false = 任何形式的损坏（magic/长度/解压/记录帧越界）。
bool ParseBatchViewDetailed(const void* data, size_t len, BatchView* out, bool* resource_failure);
bool ParseBatchView(const void* data, size_t len, BatchView* out);

}  // namespace imobs
