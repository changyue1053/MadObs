// imobs C ABI（实施计划 P6.1，架构 §18）：唯一稳定对外边界。
//
// 设计约定（与 imlink/imdb 家族一致）：
//   - 全部导出 imobs_ 前缀；共享库 -fvisibility=hidden，符号审计入 CI；
//   - 异常防火墙：任何 C++ 异常不得越境——捕获、记账、按错误码/no-op 返回；
//     内部异常累计超阈值后 SDK 自我熔断为 no-op（观测绝不拖垮宿主）；
//   - 句柄 = 槽索引 + 世代：悬空句柄解析失败而非 UAF；
//   - 热路径函数返回 void（fire-and-forget），失败在自观测账上；
//   - 回调契约：宿主的 send/execute/verify 必须恰好调用一次 done（任意线程）。
//
// 生命周期：create → 配置期（set_x / register_x）→ start → 热路径 → stop → destroy。
// 线程契约：生命周期与配置期接口不得并发调用；热路径任意线程。
// stop/destroy 前宿主必须先停止并 join 所有生产者；打点不得与 stop/destroy 并发。
// stop 为终态；运行中 start 幂等，stop 后 start 返回 BAD_STATE，重启需新建实例。
#ifndef IMOBS_C_H_
#define IMOBS_C_H_

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define IMOBS_API __attribute__((visibility("default")))
#else
#define IMOBS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t imobs_handle_t; /* 0 = 无效句柄 */

typedef enum {
  IMOBS_OK = 0,
  IMOBS_E_INVALID_ARG = 1,
  IMOBS_E_INVALID_HANDLE = 2,
  IMOBS_E_OPEN_FAILED = 3,
  IMOBS_E_INTERNAL = 4,          /* 异常防火墙捕获 */
  IMOBS_E_DISABLED = 5,          /* 自我熔断后的 no-op 态 */
  IMOBS_E_TOO_MANY_INSTANCES = 6,
  IMOBS_E_SCHEMA_REJECTED = 7,   /* 白名单/基数红线拒绝 */
  IMOBS_E_BAD_STATE = 8,         /* 启动后调配置期接口等 */
} imobs_status_t;

/* —— 上报通道注入（宿主实现传输）—— */
typedef void (*imobs_send_done_fn)(void* done_ctx, int ok);
typedef void (*imobs_channel_send_fn)(void* channel_ctx, uint8_t priority /*0=P0 1=P1 2=P2*/,
                                      uint64_t batch_id, const uint8_t* payload, size_t len,
                                      imobs_send_done_fn done, void* done_ctx);

/* —— 自愈域执行器（C 桥；由拥有状态的域实现）—— */
typedef void (*imobs_heal_done_fn)(void* done_ctx, int ok);
typedef void (*imobs_heal_execute_fn)(void* exec_ctx, uint64_t scene_id, const char* action,
                                      imobs_heal_done_fn done, void* done_ctx);
typedef void (*imobs_heal_verify_fn)(void* exec_ctx, uint64_t scene_id, imobs_heal_done_fn done,
                                     void* done_ctx);

typedef struct {
  const char* key;
  const char* const* values; /* NULL 结尾：取值域必须有限枚举（基数红线） */
} imobs_label_def_t;

typedef struct {
  const char* name;
  uint8_t damage_level; /* 0=L0 1=L1 2=L2 */
  uint64_t cooldown_ms;
  uint32_t max_per_day;
  int needs_precondition;
  uint64_t timeout_ms;
} imobs_action_spec_t;

typedef struct {
  uint64_t scene_id;
  const uint64_t* signals_all_of;
  size_t n_signals;
  uint64_t debounce_ms;
  uint64_t debounce_jitter_ms;
  uint64_t diagnose_ms;
  const imobs_action_spec_t* ladder;
  size_t n_actions;
  uint64_t verify_timeout_ms;
  uint32_t breaker_threshold;
  uint64_t muted_ttl_ms;
} imobs_scene_spec_t;

typedef struct {
  uint64_t snapshot_interval_ms;   /* 0 = 保持默认（下同） */
  uint64_t trace_chunk_max_age_ms;
  uint64_t poll_interval_ms;
  uint64_t p1_flush_bytes;
  uint64_t p2_flush_interval_ms;
} imobs_tuning_t;

typedef struct {
  uint64_t drained;
  uint64_t ring_dropped;
  uint64_t unknown_key_dropped;
  uint64_t trace_sampled_out;
  uint64_t internal_errors; /* 防火墙捕获总数（进程级） */
} imobs_self_stats_t;

/* —— 生命周期 —— */
IMOBS_API imobs_handle_t imobs_create(const char* wal_dir, imobs_channel_send_fn send,
                                      void* channel_ctx);
IMOBS_API imobs_status_t imobs_set_control_key(imobs_handle_t h, const uint8_t* key, size_t len);
IMOBS_API imobs_status_t imobs_set_tuning(imobs_handle_t h, const imobs_tuning_t* tuning);
IMOBS_API imobs_status_t imobs_register_counter(imobs_handle_t h, const char* name,
                                                const imobs_label_def_t* labels, size_t n_labels,
                                                uint64_t* out_key);
IMOBS_API imobs_status_t imobs_register_histogram(imobs_handle_t h, const char* name,
                                                  const imobs_label_def_t* labels,
                                                  size_t n_labels, uint64_t* out_key);
IMOBS_API imobs_status_t imobs_resolve_labels(imobs_handle_t h, uint64_t key,
                                              const char* const* keys, const char* const* values,
                                              size_t n, uint16_t* out_idx);
IMOBS_API imobs_status_t imobs_register_heal_scene(imobs_handle_t h,
                                                   const imobs_scene_spec_t* spec,
                                                   imobs_heal_execute_fn execute,
                                                   imobs_heal_verify_fn verify, void* exec_ctx);
/* 日志域（配置期）：path 为环形日志文件；key32 非 NULL = AES-256-GCM 逐条加密
 * （长度必须 32；本构建无加密提供方时 start 失败——fail-closed 绝不落明文）。 */
IMOBS_API imobs_status_t imobs_enable_logs(imobs_handle_t h, const char* path,
                                           uint32_t segment_size, uint32_t segment_count,
                                           const uint8_t* key32);
IMOBS_API imobs_status_t imobs_start(imobs_handle_t h);
IMOBS_API imobs_status_t imobs_stop(imobs_handle_t h);    /* 幂等：停线程 + 终局冲刷 */
IMOBS_API imobs_status_t imobs_destroy(imobs_handle_t h); /* 含 stop；句柄随即失效 */

/* —— 热路径（任意线程，fire-and-forget）—— */
IMOBS_API void imobs_counter_add(imobs_handle_t h, uint64_t key, uint16_t label_idx,
                                 uint64_t delta);
IMOBS_API void imobs_histogram_record(imobs_handle_t h, uint64_t key, uint16_t label_idx,
                                      uint64_t value);
IMOBS_API void imobs_trace_stage(imobs_handle_t h, uint64_t msg_id, uint8_t stage, uint8_t status,
                                 uint32_t attr, uint64_t value);
IMOBS_API void imobs_emit_event(imobs_handle_t h, uint64_t key, uint8_t level, uint32_t a,
                                uint32_t b);
IMOBS_API void imobs_health_signal(imobs_handle_t h, uint64_t signal_id, int active);
IMOBS_API void imobs_global_outage(imobs_handle_t h, int outage);
IMOBS_API void imobs_set_foreground(imobs_handle_t h, int foreground);
IMOBS_API void imobs_submit_control(imobs_handle_t h, const uint8_t* data, size_t len);
/* 结构化日志（任意线程，冷路径）：本地环形全都记，上行只走回捞命令。
 * module_hash 用 imobs_name_hash 生成；text 超长按内核上限截断。 */
IMOBS_API void imobs_log(imobs_handle_t h, uint8_t level, uint64_t module_hash, const char* text,
                         size_t text_len);
/* 回捞授权（隐私铁律）：默认拒绝；用户明示同意后宿主打开，撤回同意即关闭。 */
IMOBS_API void imobs_set_fetch_consent(imobs_handle_t h, int granted);

/* —— 崩溃哨兵（P5.3/P5.4 出海，imsdk 集成期 OB-1）——
 * 进程级一次安装（多实例宿主由首个实例负责）；handler 铁律见 crash_sentry.h。 */
IMOBS_API imobs_status_t imobs_crash_install(const char* marker_path);
/* 次启补报：有待补报标记 → 取出并清除，以 P0 事件入账
 * （key = imobs_name_hash("imobs.crash")，a=signal_no，b=崩溃总序号；
 *   回溯 PC 以结构化日志入日志域，回捞后离线符号化）。
 * 返回 1=已补报，0=无标记，负值=错误。 */
IMOBS_API int32_t imobs_crash_consume_pending(imobs_handle_t h, const char* marker_path);
/* crash-loop 判定（安全模式信号源）：窗口 window_ms 内崩溃 ≥ threshold 返回 1，否则 0。 */
IMOBS_API int32_t imobs_crash_in_loop(const char* marker_path, uint32_t threshold,
                                      int64_t window_ms, int64_t now_wall_ms);

/* —— 工具与自观测 —— */
IMOBS_API uint64_t imobs_name_hash(const char* name); /* Fnv1a：与内核 schema 同源 */
IMOBS_API imobs_status_t imobs_get_self_stats(imobs_handle_t h, imobs_self_stats_t* out);

/* —— 测试专用（发布构建可宏裁剪；用于验证防火墙熔断行为本身）—— */
IMOBS_API void imobs_debug_force_internal_error(void);
IMOBS_API void imobs_debug_reset_firewall(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IMOBS_C_H_ */
