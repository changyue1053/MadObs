#pragma once
// 极简 hermetic 测试框架（实施计划 P0.4）：零外部依赖，先跑起来；
// gtest/gbenchmark 随 third_party 引入后按同名宏平滑迁移。

#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

namespace imobs_test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Cases() {
  static std::vector<Case> v;
  return v;
}
inline int& Failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* n, void (*f)()) { Cases().push_back({n, f}); }
};

// std::cmp_equal 只接受"纯"有/无符号整数（排除 bool 与全部字符类型，标准如此）。
template <class T>
inline constexpr bool kSafeCmpInt =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> &&
    !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>;

// 整数比较走 cmp_equal（免 -Wsign-compare），其余类型走 operator==。
template <class A, class B>
inline bool EqCmp(const A& a, const B& b) {
  if constexpr (kSafeCmpInt<A> && kSafeCmpInt<B>) {
    return std::cmp_equal(a, b);
  } else {
    return a == b;
  }
}

inline int RunAll() {
  int ran = 0;
  for (const auto& c : Cases()) {
    std::printf("[ RUN      ] %s\n", c.name);
    const int before = Failures();
    c.fn();
    ++ran;
    std::printf("%s %s\n", before == Failures() ? "[       OK ]" : "[     FAIL ]", c.name);
  }
  std::printf("== %d case(s), %d failure(s)\n", ran, Failures());
  return Failures() == 0 ? 0 : 1;
}

}  // namespace imobs_test

#define TEST(Suite, Name)                                                      \
  static void Suite##_##Name##_impl();                                         \
  static ::imobs_test::Registrar reg_##Suite##_##Name(#Suite "." #Name,        \
                                                      &Suite##_##Name##_impl); \
  static void Suite##_##Name##_impl()

#define IMOBS_FAIL_(fatal, ...)                         \
  do {                                                  \
    std::printf("  [FAIL] %s:%d ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__);                           \
    std::printf("\n");                                  \
    ++::imobs_test::Failures();                         \
    if (fatal) return;                                  \
  } while (0)

#define EXPECT_TRUE(c)                                     \
  do {                                                     \
    if (!(c)) IMOBS_FAIL_(false, "EXPECT_TRUE(%s)", #c);   \
  } while (0)
#define ASSERT_TRUE(c)                                     \
  do {                                                     \
    if (!(c)) IMOBS_FAIL_(true, "ASSERT_TRUE(%s)", #c);    \
  } while (0)
#define EXPECT_FALSE(c)                                    \
  do {                                                     \
    if ((c)) IMOBS_FAIL_(false, "EXPECT_FALSE(%s)", #c);   \
  } while (0)

#define IMOBS_EQ_(a, b, fatal)                                                       \
  do {                                                                               \
    const auto va_ = (a);                                                            \
    const auto vb_ = (b);                                                            \
    if (!::imobs_test::EqCmp(va_, vb_)) {                                            \
      if constexpr (std::is_arithmetic_v<std::decay_t<decltype(va_)>> &&             \
                    std::is_arithmetic_v<std::decay_t<decltype(vb_)>>) {             \
        IMOBS_FAIL_(fatal, "EXPECT_EQ(%s, %s) 实际 %lld vs %lld", #a, #b,            \
                    static_cast<long long>(va_), static_cast<long long>(vb_));       \
      } else {                                                                       \
        IMOBS_FAIL_(fatal, "EXPECT_EQ(%s, %s)", #a, #b);                             \
      }                                                                              \
    }                                                                                \
  } while (0)

#define EXPECT_EQ(a, b) IMOBS_EQ_(a, b, false)
#define ASSERT_EQ(a, b) IMOBS_EQ_(a, b, true)

#define EXPECT_LT(a, b)                                                              \
  do {                                                                               \
    const auto va_ = (a);                                                            \
    const auto vb_ = (b);                                                            \
    if (!(va_ < vb_)) {                                                              \
      IMOBS_FAIL_(false, "EXPECT_LT(%s, %s) 实际 %lld vs %lld", #a, #b,              \
                  static_cast<long long>(va_), static_cast<long long>(vb_));         \
    }                                                                                \
  } while (0)

#define IMOBS_TEST_MAIN \
  int main() { return ::imobs_test::RunAll(); }
