// 回放骨架 runner（架构 §18 test/replay/；W6 残项 5）：
// 脱敏轨迹样本（trace_sample_v1.txt，语法见该文件头）→ 按 batch_view.h 契约
// 构造线上明文批字节 → 走生产解析/拼接路径（ParseBatchView + TraceStitcher）
// → 汇聚报告与 golden 逐字节对照，差一字节即红。
//
// 用法：
//   test_replay_stitcher <sample.txt> <golden>   CTest 形态：不匹配即非零退出
//   test_replay_stitcher <sample.txt> --emit     再生成 golden：仅输出到 stdout，
//       落盘须人工重定向——golden 变更必须是显式、可评审的动作，不给静默覆盖入口。
#include "imobs/stitcher.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "imobs/records.h"
#include "imobs/wire.h"

namespace {

using imobs::TraceEventWire;
using imobs::TraceStitcher;
using imobs::WireWriter;

struct Chunk {
  uint64_t base_mono_ns = 0;
  std::vector<TraceEventWire> events;
};

struct Batch {
  uint64_t anchor_wall_ms = 0;
  uint64_t anchor_mono_ns = 0;
  std::vector<std::vector<uint8_t>> records;  // 已编码记录帧（轨迹块与 junk 同列）
};

struct Sample {
  std::vector<uint8_t> required;  // require 行：完整性判定的必经节点
  std::vector<Batch> batches;
};

// 与 UploadPipeline::MaybeFlush 的明文批同构（契约见 batch_view.h 头注释）：
//   body = [magic "IMB1"][u8 pri][fixed64 anchor_wall_ms][fixed64 anchor_mono_ns]
//          [varint n_records][n × (varint len + bytes)]
//   full = [fixed64 batch_id = Fnv1a64(body)] + body
// 回放样本走明文形态（压缩批 "IMBZ" 留给后续样本版本）；构造正确性由拼接器
// 入账路径上的生产解析器 ParseBatchView 校验——构错批直接 ingested=0 红灯。
std::vector<uint8_t> BuildBatchBytes(const Batch& b) {
  std::vector<uint8_t> body;
  WireWriter w(&body);
  w.Bytes("IMB1", 4);
  w.U8(1);  // pri=P1：轨迹块的常规上报优先级
  w.Fixed64(b.anchor_wall_ms);
  w.Fixed64(b.anchor_mono_ns);
  w.Varint(b.records.size());
  for (const auto& rec : b.records) {
    w.Varint(rec.size());
    w.Bytes(rec.data(), rec.size());
  }
  std::vector<uint8_t> full;
  full.reserve(body.size() + 8);
  WireWriter w2(&full);
  w2.Fixed64(imobs::Fnv1a64(body.data(), body.size()));
  w2.Bytes(body.data(), body.size());
  return full;
}

void FlushChunk(Batch* batch, Chunk* chunk) {
  if (chunk->events.empty()) return;
  std::vector<uint8_t> rec;
  EncodeTraceChunk(chunk->base_mono_ns, chunk->events, &rec);
  batch->records.push_back(std::move(rec));
  chunk->events.clear();
}

bool HexToBytes(const std::string& hex, std::vector<uint8_t>* out) {
  if (hex.size() % 2 != 0) return false;
  for (size_t i = 0; i < hex.size(); i += 2) {
    char* end = nullptr;
    const std::string byte = hex.substr(i, 2);
    const unsigned long v = std::strtoul(byte.c_str(), &end, 16);
    if (end == nullptr || *end != '\0') return false;
    out->push_back(static_cast<uint8_t>(v));
  }
  return true;
}

// 样本解析：语法错误一律硬失败（exit 2）——样本是契约文件，坏样本不准静默跳过。
bool ParseSample(const std::string& path, Sample* out, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    *err = "样本打开失败：" + path;
    return false;
  }
  Batch cur_batch;
  Chunk cur_chunk;
  bool in_batch = false;
  bool in_chunk = false;
  std::string line;
  int ln = 0;
  auto fail = [&](const std::string& msg) {
    *err = path + ":" + std::to_string(ln) + ": " + msg;
    return false;
  };
  while (std::getline(in, line)) {
    ++ln;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream ss(line);
    std::string kw;
    if (!(ss >> kw)) continue;  // 空行
    if (kw == "require") {
      unsigned s = 0;
      while (ss >> s) out->required.push_back(static_cast<uint8_t>(s));
    } else if (kw == "batch") {
      if (in_batch) {
        if (in_chunk) FlushChunk(&cur_batch, &cur_chunk);
        out->batches.push_back(std::move(cur_batch));
        cur_batch = Batch{};
      }
      in_batch = true;
      in_chunk = false;
      if (!(ss >> cur_batch.anchor_wall_ms >> cur_batch.anchor_mono_ns)) {
        return fail("batch 需要 <anchor_wall_ms> <anchor_mono_ns>");
      }
    } else if (kw == "chunk") {
      if (!in_batch) return fail("chunk 出现在 batch 之外");
      if (in_chunk) FlushChunk(&cur_batch, &cur_chunk);
      in_chunk = true;
      if (!(ss >> cur_chunk.base_mono_ns)) return fail("chunk 需要 <base_mono_ns>");
    } else if (kw == "ev") {
      if (!in_chunk) return fail("ev 出现在 chunk 之外");
      std::string msg_hex;
      unsigned stage = 0;
      unsigned status = 0;
      uint64_t delta = 0;
      if (!(ss >> msg_hex >> stage >> status >> delta)) {
        return fail("ev 需要 <msg_id_hex> <stage> <status> <ts_delta_ns>");
      }
      TraceEventWire ev;
      ev.msg_id = std::strtoull(msg_hex.c_str(), nullptr, 16);
      ev.stage = static_cast<uint8_t>(stage);
      ev.status = static_cast<uint8_t>(status);
      ev.ts_delta_ns = delta;
      cur_chunk.events.push_back(ev);
    } else if (kw == "junk") {
      if (!in_batch) return fail("junk 出现在 batch 之外");
      if (in_chunk) FlushChunk(&cur_batch, &cur_chunk);
      in_chunk = false;
      std::string hex;
      std::vector<uint8_t> bytes;
      if (!(ss >> hex) || !HexToBytes(hex, &bytes)) return fail("junk 需要偶数长度 hex");
      cur_batch.records.push_back(std::move(bytes));
    } else {
      return fail("未知关键字：" + kw);
    }
  }
  if (in_batch) {
    if (in_chunk) FlushChunk(&cur_batch, &cur_chunk);
    out->batches.push_back(std::move(cur_batch));
  }
  if (out->batches.empty()) {
    *err = path + ": 样本无批";
    return false;
  }
  return true;
}

// 汇聚报告（golden 的唯一内容源）：行序、字段序、进制全部确定性——
// 旅程按 msg_id 升序（JourneyIds 即 map 序），stage 升序（StagesOf 契约）。
std::string RunAndReport(const Sample& sample) {
  TraceStitcher st;
  std::string report;
  char buf[256];
  for (size_t i = 0; i < sample.batches.size(); ++i) {
    const auto bytes = BuildBatchBytes(sample.batches[i]);
    const size_t ingested = st.IngestBatch(bytes.data(), bytes.size());
    std::snprintf(buf, sizeof(buf), "batch=%zu records=%zu ingested=%zu\n", i + 1,
                  sample.batches[i].records.size(), ingested);
    report += buf;
  }
  std::snprintf(buf, sizeof(buf), "journeys=%zu trace_events=%llu\n", st.JourneyCount(),
                static_cast<unsigned long long>(st.trace_events()));
  report += buf;
  for (const uint64_t id : st.JourneyIds(/*limit=*/1024)) {
    const auto stages = st.StagesOf(id);
    std::snprintf(buf, sizeof(buf), "msg=0x%llx stages=", static_cast<unsigned long long>(id));
    report += buf;
    for (size_t i = 0; i < stages.size(); ++i) {
      std::snprintf(buf, sizeof(buf), "%s%u", i == 0 ? "" : ",", stages[i]);
      report += buf;
    }
    if (st.Complete(id, sample.required)) {
      report += " complete=yes";
    } else {
      report += " complete=no missing=";
      bool first = true;
      for (const uint8_t need : sample.required) {
        bool seen = false;
        for (const uint8_t s : stages) seen = seen || s == need;
        if (!seen) {
          std::snprintf(buf, sizeof(buf), "%s%u", first ? "" : ",", need);
          report += buf;
          first = false;
        }
      }
    }
    report += "\n";
  }
  return report;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "用法：%s <sample.txt> <golden|--emit>\n", argv[0]);
    return 2;
  }
  Sample sample;
  std::string err;
  if (!ParseSample(argv[1], &sample, &err)) {
    std::fprintf(stderr, "样本解析失败：%s\n", err.c_str());
    return 2;
  }
  const std::string report = RunAndReport(sample);

  if (std::strcmp(argv[2], "--emit") == 0) {
    std::fputs(report.c_str(), stdout);
    return 0;
  }

  std::ifstream gf(argv[2], std::ios::binary);
  if (!gf) {
    std::fprintf(stderr, "golden 打开失败：%s\n", argv[2]);
    return 2;
  }
  std::string golden((std::istreambuf_iterator<char>(gf)), std::istreambuf_iterator<char>());

  if (report != golden) {
    size_t diff = 0;
    while (diff < report.size() && diff < golden.size() && report[diff] == golden[diff]) ++diff;
    std::fprintf(stderr,
                 "回放对照失败：首个差异字节 offset=%zu（golden %zu 字节，实际 %zu 字节）\n"
                 "—— golden ——\n%s—— 实际 ——\n%s",
                 diff, golden.size(), report.size(), golden.c_str(), report.c_str());
    return 1;
  }
  std::printf("回放对照通过：batches=%zu，报告与 golden 逐字节一致（%zu 字节）\n",
              sample.batches.size(), golden.size());
  return 0;
}
