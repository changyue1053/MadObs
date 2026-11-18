#pragma once
// 打包压缩编解码（架构 §9 上报域）：zlib（系统库，双端零引入成本）。
// 选型记录：zstd 压得更小但需引第三方；打包批的主体是高重复 TLV 记录，
// deflate 已能拿到大头收益；接口按字节向量封装，未来换 zstd 只动本文件。
//
// 线程契约：无状态纯函数，任意线程可用；仅消费线程/服务端调用（非热路径）。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace imobs {
enum class CodecResult { kOk, kIncompressible, kMalformed, kResourceFailure };
CodecResult DeflateBytesDetailed(const uint8_t* data, size_t len, std::vector<uint8_t>* out);
CodecResult InflateBytesDetailed(const uint8_t* data, size_t len, size_t raw_len, std::vector<uint8_t>* out);


// 压缩 [data,len)。仅当压缩成功**且**产物更小才返回 true（out 有效）；
// 否则返回 false（调用方按原文落批——"压不动就不压"是格式契约的一部分）。
bool DeflateBytes(const uint8_t* data, size_t len, std::vector<uint8_t>* out);

// 解压 [data,len) → 恰好 raw_len 字节。长度不符 / 数据损坏返回 false。
bool InflateBytes(const uint8_t* data, size_t len, size_t raw_len, std::vector<uint8_t>* out);

}  // namespace imobs
