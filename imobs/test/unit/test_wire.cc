#include "imobs/wire.h"

#include <cstdint>
#include <random>
#include <vector>

#include "imobs_test.h"

using imobs::DecodeTraceEvent;
using imobs::EncodeTraceEvent;
using imobs::Event;
using imobs::TraceEventWire;
using imobs::TraceFromEvent;
using imobs::WireReader;
using imobs::WireWriter;

TEST(Wire, VarintRoundtripAndBounds) {
  const uint64_t cases[] = {0,     1,        127,        128,
                            16383, 16384,    0xFFFFFFFF, 0x123456789ABCDEFull,
                            UINT64_MAX};
  for (uint64_t v : cases) {
    std::vector<uint8_t> buf;
    WireWriter w(&buf);
    w.Varint(v);
    WireReader r(buf.data(), buf.size());
    uint64_t back = 0;
    ASSERT_TRUE(r.Varint(&back));
    EXPECT_EQ(back, v);
    EXPECT_EQ(r.remaining(), size_t{0});
    // 截断一个字节必须失败，绝不越界读
    WireReader rt(buf.data(), buf.size() - 1);
    uint64_t junk = 0;
    EXPECT_FALSE(rt.Varint(&junk));
  }
  // 11 字节全延续位：非法 varint
  std::vector<uint8_t> evil(11, 0xFF);
  WireReader r(evil.data(), evil.size());
  uint64_t junk = 0;
  EXPECT_FALSE(r.Varint(&junk));
}

TEST(Wire, Fixed64Roundtrip) {
  std::vector<uint8_t> buf;
  WireWriter w(&buf);
  w.Fixed64(0xDEADBEEFCAFEBABEull);
  WireReader r(buf.data(), buf.size());
  uint64_t v = 0;
  ASSERT_TRUE(r.Fixed64(&v));
  EXPECT_EQ(v, 0xDEADBEEFCAFEBABEull);
}

TEST(Wire, TraceEventRoundtripRandomized) {
  std::minstd_rand rng(13);
  for (int iter = 0; iter < 1000; ++iter) {
    TraceEventWire ev;
    ev.msg_id = (static_cast<uint64_t>(rng()) << 32) ^ rng();
    ev.stage = static_cast<uint8_t>(rng() % 11);
    ev.status = static_cast<uint8_t>(rng() % 3);
    ev.ts_delta_ns = static_cast<uint64_t>(rng()) * (rng() % 1000);
    ev.n_attrs = static_cast<uint8_t>(rng() % 5);
    for (uint8_t i = 0; i < ev.n_attrs; ++i) {
      ev.attrs[i].tag = 1 + rng() % 4;
      ev.attrs[i].value = (static_cast<uint64_t>(rng()) << 31) | rng();
    }
    std::vector<uint8_t> buf;
    EncodeTraceEvent(ev, &buf);
    WireReader r(buf.data(), buf.size());
    TraceEventWire back;
    ASSERT_TRUE(DecodeTraceEvent(r, &back));
    EXPECT_EQ(back.msg_id, ev.msg_id);
    EXPECT_EQ(back.stage, ev.stage);
    EXPECT_EQ(back.status, ev.status);
    EXPECT_EQ(back.ts_delta_ns, ev.ts_delta_ns);
    ASSERT_EQ(back.n_attrs, ev.n_attrs);
    for (uint8_t i = 0; i < ev.n_attrs; ++i) {
      EXPECT_EQ(back.attrs[i].tag, ev.attrs[i].tag);
      EXPECT_EQ(back.attrs[i].value, ev.attrs[i].value);
    }
    EXPECT_EQ(r.remaining(), size_t{0});
  }
}

TEST(Wire, TypicalTraceEventUnder50Bytes) {
  // 契约断言（实施计划 P1.4 DoD）：典型轨迹事件线格式 ≤ 50B。
  // 典型 = T4 事件：conn 低位 + seq + label，批内 delta 30s。
  Event ev{};
  ev.entity_id = 0x7A3F0000'00009C21ull;  // msg_id
  ev.stage = 4;
  ev.status = 0;
  ev.ts_mono_ns = 30'000'000'000ull;  // base + 30s
  ev.attr_a = 991'700;                // conn 低位
  ev.value = 10'482;                  // seq
  ev.label_idx = 3;
  const TraceEventWire w = TraceFromEvent(ev, 0);
  std::vector<uint8_t> buf;
  EncodeTraceEvent(w, &buf);
  EXPECT_TRUE(buf.size() <= 50);
  EXPECT_TRUE(buf.size() >= 16);  // 下限 sanity：msg_id 都没带就是编码错了
}

TEST(Wire, DecodeRandomBytesNeverCrashes) {
  // 解码器是攻击面：任意字节序列只允许"返回 false"，不允许越界/崩溃（ASan 兜底）
  std::minstd_rand rng(17);
  std::vector<uint8_t> buf;
  for (int iter = 0; iter < 100'000; ++iter) {
    const size_t n = rng() % 64;
    buf.resize(n);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    WireReader r(buf.data(), buf.size());
    TraceEventWire ev;
    (void)DecodeTraceEvent(r, &ev);
  }
  EXPECT_TRUE(true);  // 走到这里 = 没崩
}

IMOBS_TEST_MAIN
