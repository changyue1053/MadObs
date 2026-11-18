cmake_minimum_required(VERSION 3.20)
project(imobs LANGUAGES CXX)
include(CTest)
option(MADOBS_BUILD_SHARED "Build shared runtime alongside static archive" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# 消杀矩阵：-DIMOBS_SAN=asan（address+undefined）/ tsan（thread）
set(IMOBS_SAN "" CACHE STRING "sanitizer: asan | tsan | empty")
if(IMOBS_SAN STREQUAL "asan")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
  add_link_options(-fsanitize=address,undefined)
elseif(IMOBS_SAN STREQUAL "tsan")
  add_compile_options(-fsanitize=thread -O1 -g)
  add_link_options(-fsanitize=thread)
endif()

set(IMOBS_SOURCES
  src/core/signal/schema.cc
  src/core/signal/metrics.cc
  src/core/signal/tdigest.cc
  src/core/signal/records.cc
  src/core/pipeline/wal_ring.cc
  src/core/pipeline/pipeline.cc
  src/core/health/healer.cc
  src/core/policy/policy.cc
  src/core/kernel/obs_kernel.cc
  src/core/signal/log_store.cc
  src/infra/wire.cc
  src/infra/codec.cc
  src/infra/batch_view.cc
  src/infra/sha256.cc
  src/infra/tcp_channel.cc
  src/platform/crash_sentry.cc
  src/server/stitcher.cc
  src/server/collector.cc
  src/server/thin_server.cc
  src/api/ffi.cc
  ../src/madobs_compat.cc
  ../src/private_binding.cc
  ../src/generic_events.cc
)

# 强加密提供方（架构 §12）：BoringSSL——与 IMLink/IMDB 三内核共一套底座。
# 注入优先级（imsdk 集成期 OB-2 修订，issue I-005）：
#   1) 显式双变量 IMOBS_BORINGSSL_INCLUDE + IMOBS_BORINGSSL_CRYPTO_LIB（交叉编译：
#      boringssl 的 include/ 与逐平台 build-<plat>/libcrypto.a 分离）；
#   2) IMOBS_BORINGSSL_ROOT（宿主便利：含 include/ 与 build/libcrypto.a）；
#   3) 家族布局探测 ../../IMLink/imlink/third_party/boringssl；
#   4) 皆无 → 空提供方（crypto::Available()=false），独立构建永远成立。
set(IMOBS_BORINGSSL_ROOT "" CACHE PATH "BoringSSL 根目录（含 include/ 与 build/libcrypto.a）")
set(IMOBS_BORINGSSL_INCLUDE "" CACHE PATH "BoringSSL include 目录（显式注入，优先）")
set(IMOBS_BORINGSSL_CRYPTO_LIB "" CACHE FILEPATH "libcrypto.a 完整路径（显式注入，优先）")
if(IMOBS_BORINGSSL_INCLUDE STREQUAL "" OR IMOBS_BORINGSSL_CRYPTO_LIB STREQUAL "")
  if(NOT IMOBS_BORINGSSL_ROOT STREQUAL "" AND EXISTS "${IMOBS_BORINGSSL_ROOT}/build/libcrypto.a")
    set(IMOBS_BORINGSSL_INCLUDE "${IMOBS_BORINGSSL_ROOT}/include")
    set(IMOBS_BORINGSSL_CRYPTO_LIB "${IMOBS_BORINGSSL_ROOT}/build/libcrypto.a")
  endif()
endif()
if(NOT IMOBS_BORINGSSL_CRYPTO_LIB STREQUAL "" AND EXISTS "${IMOBS_BORINGSSL_CRYPTO_LIB}")
  set(IMOBS_HAVE_BORINGSSL ON)
  list(APPEND IMOBS_SOURCES src/infra/crypto_boringssl.cc)
  message(STATUS "imobs: 强加密提供方 = BoringSSL (${IMOBS_BORINGSSL_CRYPTO_LIB})")
else()
  set(IMOBS_HAVE_BORINGSSL OFF)
  list(APPEND IMOBS_SOURCES src/infra/crypto_null.cc)
  message(STATUS "imobs: 无 BoringSSL 产物，编入空加密提供方（HMAC/明文路径可用）")
endif()

find_package(ZLIB REQUIRED)  # 打包压缩（架构 §9）：系统 zlib，双端零引入成本

add_library(imobs_core STATIC ${IMOBS_SOURCES})
target_include_directories(imobs_core PUBLIC
 $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
 $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
 $<INSTALL_INTERFACE:include>)
target_link_libraries(imobs_core PRIVATE ZLIB::ZLIB)
if(IMOBS_HAVE_BORINGSSL)
  target_include_directories(imobs_core SYSTEM PRIVATE "${IMOBS_BORINGSSL_INCLUDE}")
  target_link_libraries(imobs_core PUBLIC "${IMOBS_BORINGSSL_CRYPTO_LIB}")
endif()

# 共享库产物（宿主集成形态）：-fvisibility=hidden，仅导出 imobs_ C ABI；
# 符号审计入 ctest（消杀构建下跳过——san 运行时会注入自身符号）。
if(IMOBS_SAN STREQUAL "" AND MADOBS_BUILD_SHARED)
  add_library(imobs_shared SHARED ${IMOBS_SOURCES})
  target_include_directories(imobs_shared PUBLIC include ../include)
  target_link_libraries(imobs_shared PRIVATE ZLIB::ZLIB)
  target_compile_options(imobs_shared PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
  if(IMOBS_HAVE_BORINGSSL)
    target_include_directories(imobs_shared SYSTEM PRIVATE "${IMOBS_BORINGSSL_INCLUDE}")
    # -hidden-l：静态归档符号以 hidden 可见性并入——共享库仍只导出 imobs_ C ABI
    get_filename_component(_imobs_bssl_libdir "${IMOBS_BORINGSSL_CRYPTO_LIB}" DIRECTORY)
    if(APPLE)
      target_link_options(imobs_shared PRIVATE
        "-L${_imobs_bssl_libdir}" "-Wl,-hidden-lcrypto")
    else()
      target_link_libraries(imobs_shared PRIVATE "${IMOBS_BORINGSSL_CRYPTO_LIB}")
    endif()
  endif()
  if(UNIX AND NOT APPLE)
    target_link_options(imobs_shared PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
  endif()
  set_target_properties(imobs_shared PROPERTIES OUTPUT_NAME madobs_runtime)
endif()

if(BUILD_TESTING)
enable_testing()

set(IMOBS_UNIT_TESTS test_clock test_event_ring test_schema test_metrics test_wal_ring
                     test_tdigest test_wire test_pipeline test_records test_kernel test_healer
                     test_kernel_heal test_sha256 test_policy test_kernel_policy test_log_store
                     test_ffi test_stitcher test_codec test_crypto)
foreach(t IN LISTS IMOBS_UNIT_TESTS)
  add_executable(${t} test/unit/${t}.cc)
  target_link_libraries(${t} PRIVATE imobs_core)
  target_include_directories(${t} PRIVATE test)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# 崩溃剧本：fork + _exit 模拟 kill -9，验证 WAL 恢复
add_executable(test_wal_recovery test/crash/test_wal_recovery.cc)
target_link_libraries(test_wal_recovery PRIVATE imobs_core)
target_include_directories(test_wal_recovery PRIVATE test)
add_test(NAME test_wal_recovery COMMAND test_wal_recovery)

# 风暴仿真（chaos）：1000 实例全局故障 → 抖动摊开恢复（P4 决策门）
add_executable(test_healer_storm test/chaos/test_healer_storm.cc)
target_link_libraries(test_healer_storm PRIVATE imobs_core)
target_include_directories(test_healer_storm PRIVATE test)
add_test(NAME test_healer_storm COMMAND test_healer_storm)

# 崩溃哨兵剧本：fork 子进程致命信号自杀 → 标记补报 + crash-loop 判定
add_executable(test_crash_sentry test/crash/test_crash_sentry.cc)
target_link_libraries(test_crash_sentry PRIVATE imobs_core)
target_include_directories(test_crash_sentry PRIVATE test)
target_compile_options(test_crash_sentry PRIVATE -fno-omit-frame-pointer -fno-optimize-sibling-calls)
add_test(NAME test_crash_sentry COMMAND test_crash_sentry)

# 回环 e2e：三内核 × 真实 TCP × 收集器拼接 + 重放幂等（M3 前奏）
add_executable(test_e2e_loopback test/e2e/test_e2e_loopback.cc)
target_link_libraries(test_e2e_loopback PRIVATE imobs_core)
target_include_directories(test_e2e_loopback PRIVATE test)
add_test(NAME test_e2e_loopback COMMAND test_e2e_loopback)

# 薄服务端 e2e：内核 × TCP ingest × 聚合 × HTTP 查询面（P7 单机形态）
add_executable(test_thin_server test/e2e/test_thin_server.cc)
target_link_libraries(test_thin_server PRIVATE imobs_core)
target_include_directories(test_thin_server PRIVATE test)
add_test(NAME test_thin_server COMMAND test_thin_server)

# 回放骨架（架构 §18 test/replay/，W6 残项 5）：脱敏轨迹样本（乱序/晚到/断链）
# → 拼接器 → 报告与 golden 逐字节对照；样本语法见 test/replay/trace_sample_v1.txt
add_executable(test_replay_stitcher test/replay/test_replay_stitcher.cc)
target_link_libraries(test_replay_stitcher PRIVATE imobs_core)
target_include_directories(test_replay_stitcher PRIVATE test)
add_test(NAME test_replay_stitcher
         COMMAND test_replay_stitcher
                 ${CMAKE_CURRENT_SOURCE_DIR}/test/replay/trace_sample_v1.txt
                 ${CMAKE_CURRENT_SOURCE_DIR}/test/replay/trace_sample_v1.golden)

# IMDB(Rust) × imobs 宿主桥 e2e：imdb-ffi 静态库真实链入（三内核集成的另一半）。
# 仅非消杀构建：Rust 侧未插桩，TSan 看不见其线程同步会误报，ASan 亦不覆盖——
# 内存/竞态正确性由两侧各自的消杀矩阵负责，这里验的是跨语言契约。
# IMDB bridge source retained for provenance only; no other SDK build dependency.

# imobs-lite 守护进程（可运行的薄服务端）
add_executable(imobs_lited tools/imobs_lited.cc)
target_link_libraries(imobs_lited PRIVATE imobs_core)

# echo 演示台（P8）：一条命令演完整版故事；退出码 0 = 剧本验收通过 → 兼作冒烟测试
add_executable(imobs_echo_demo tools/imobs_echo_demo.cc)
target_link_libraries(imobs_echo_demo PRIVATE imobs_core)
add_test(NAME demo_echo_smoke COMMAND imobs_echo_demo)

# 性能门禁：不进 ctest（秒级运行时），由 bootstrap 显式跑；IMOBS_BENCH_GATE=1 时 P99>=1us 退出非零
add_executable(bench_event_ring test/perf/bench_event_ring.cc)
target_link_libraries(bench_event_ring PRIVATE imobs_core)

# 防腐审计：热路径文件禁锁/禁堆/禁系统调用（P0.3；W6 残项 3 扩至 api/ 与 core/signal/ 全目录）
add_test(NAME audit_hotpath
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_hotpath.py ${CMAKE_CURRENT_SOURCE_DIR})

# signal-safe 审计：崩溃 handler 调用链函数白名单，清单外即红（架构 §10/§18，W6 残项 4）
add_test(NAME audit_signal_safe
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_signal_safe.py ${CMAKE_CURRENT_SOURCE_DIR})

# 符号审计：共享库只准导出 imobs_ 前缀（三内核共存的链接卫生）
if(TARGET imobs_shared)
  add_test(NAME audit_symbols
           COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_symbols.py
                   $<TARGET_FILE:imobs_shared>)
endif()

endif() # BUILD_TESTING
