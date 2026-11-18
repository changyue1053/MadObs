#include "jni_helpers.h"
#include "org_madobs_PrivateRuntime.h"
#include <atomic>
#include <climits>
#include <jni.h>
#include <limits>
#include <madobs/private.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
JavaVM *vm = nullptr;
thread_local bool in_callback = false;
void Throw(JNIEnv *e, const char *type, const char *message) {
  if (e->ExceptionCheck())
    return;
  jclass c = e->FindClass(type);
  if (c) {
    e->ThrowNew(c, message);
    e->DeleteLocalRef(c);
  }
}
struct Env {
  JNIEnv *e = nullptr;
  bool attached = false;
  Env() {
    const auto result =
        vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
      attached = vm->AttachCurrentThread(reinterpret_cast<void **>(&e),
                                         nullptr) == JNI_OK;
      if (!attached)
        e = nullptr;
    } else if (result != JNI_OK)
      e = nullptr;
  }
  ~Env() {
    if (attached)
      vm->DetachCurrentThread();
  }
};
struct Owner {
  jobject exporter = nullptr;
  jmethodID send = nullptr;
  madobs_private_handle_t handle = 0;
  std::atomic<uint64_t> failures{0};
  ~Owner() {
    if (handle)
      madobs_private_destroy(handle);
    if (exporter) {
      Env env;
      if (env.e)
        env.e->DeleteGlobalRef(exporter);
    }
  }
};
struct Registry {
  std::mutex mutex;
  std::unordered_map<jlong, std::shared_ptr<Owner>> owners;
  jlong next_token = 1;
};
// No JVM calls from C++ static destructors after VM shutdown. Explicit close
// is mandatory; an abandoned owner remains process-lifetime (bounded by native
// capacity), rather than invoking Java during process teardown.
Registry &State() {
  static auto *state = new Registry;
  return *state;
}
std::shared_ptr<Owner> Find(jlong token) {
  std::lock_guard<std::mutex> lock(State().mutex);
  const auto i = State().owners.find(token);
  return i == State().owners.end() ? nullptr : i->second;
}
void Send(void *p, uint8_t priority, uint64_t id, const uint8_t *data,
          size_t size, madobs_send_done_fn done, void *dc) {
  auto &owner = *static_cast<Owner *>(p);
  Env env;
  bool ok = false;
  if (!env.e || size > INT_MAX)
    ++owner.failures;
  else {
    auto array = env.e->NewByteArray(static_cast<jsize>(size));
    if (array) {
      env.e->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                                reinterpret_cast<const jbyte *>(data));
      if (!env.e->ExceptionCheck()) {
        const bool previous = in_callback;
        in_callback = true;
        ok = env.e->CallBooleanMethod(
                 owner.exporter, owner.send, static_cast<jint>(priority),
                 static_cast<jlong>(id), array) == JNI_TRUE;
        in_callback = previous;
      }
      env.e->DeleteLocalRef(array);
    }
    if (env.e->ExceptionCheck()) {
      env.e->ExceptionClear();
      ++owner.failures;
      ok = false;
    }
  }
  done(dc, ok ? 1 : 0);
}
bool Rules(JNIEnv *e, jobjectArray rows,
           std::vector<madobs_event_rule_v1> &rules) {
  if (!rows)
    return false;
  auto count = e->GetArrayLength(rows);
  if (count > static_cast<jsize>(MADOBS_SCHEMA_MAX_EVENTS))
    return false;
  rules.resize(count);
  for (jsize i = 0; i < count; ++i) {
    auto row = static_cast<jintArray>(e->GetObjectArrayElement(rows, i));
    if (!row)
      return false;
    const auto n = e->GetArrayLength(row);
    if (n < 5 || n > 149) {
      e->DeleteLocalRef(row);
      return false;
    }
    jint input[149]{};
    e->GetIntArrayRegion(row, 0, n, input);
    e->DeleteLocalRef(row);
    if (e->ExceptionCheck())
      return false;
    for (jsize j = 0; j < n; ++j)
      if (input[j] < 0)
        return false;
    auto &r = rules[i];
    r.sdk_id = input[0];
    r.sdk_version = input[1];
    r.event_id = input[2];
    r.max_severity = input[3];
    r.attribute_count = input[4];
    if (r.attribute_count > MADOBS_MAX_ATTRIBUTES)
      return false;
    jsize cursor = 5;
    for (uint32_t j = 0; j < r.attribute_count; ++j) {
      if (cursor + 2 > n)
        return false;
      auto &a = r.attributes[j];
      a.key = input[cursor++];
      a.value_count = input[cursor++];
      if (!a.value_count || a.value_count > MADOBS_SCHEMA_MAX_VALUES ||
          cursor + static_cast<jsize>(a.value_count) > n)
        return false;
      for (uint32_t k = 0; k < a.value_count; ++k)
        a.values[k] = input[cursor++];
    }
    if (cursor != n)
      return false;
  }
  return true;
}
} // namespace
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *v, void *) {
  vm = v;
  return JNI_VERSION_1_6;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_madobs_PrivateRuntime_nInCallback(JNIEnv *, jclass) {
  return in_callback ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jlong JNICALL Java_org_madobs_PrivateRuntime_nOpen(
    JNIEnv *e, jclass, jbyteArray path, jobjectArray rows, jobject exporter) {
  try {
    if (!path || !exporter) {
      Throw(e, "java/lang/IllegalArgumentException", "null argument");
      return 0;
    }
    auto length = e->GetArrayLength(path);
    if (length <= 0 || length > 1048576) {
      Throw(e, "java/lang/IllegalArgumentException", "invalid path length");
      return 0;
    }
    std::string directory(static_cast<size_t>(length), '\0');
    e->GetByteArrayRegion(path, 0, length,
                          reinterpret_cast<jbyte *>(&directory[0]));
    if (e->ExceptionCheck())
      return 0;
    if (directory.find('\0') != std::string::npos) {
      Throw(e, "java/lang/IllegalArgumentException", "NUL path");
      return 0;
    }
    std::vector<madobs_event_rule_v1> rules;
    if (!Rules(e, rows, rules)) {
      Throw(e, "java/lang/IllegalArgumentException", "invalid policy encoding");
      return 0;
    }
    madobs_privacy_policy_v1 policy{sizeof(policy), MADOBS_ABI_VERSION,
                                    static_cast<uint32_t>(rules.size()),
                                    rules.data()};
    if (!madobs_privacy_policy_valid(&policy)) {
      Throw(e, "java/lang/IllegalArgumentException", "invalid policy");
      return 0;
    }
    auto owner = std::make_shared<Owner>();
    owner->send = madobs_jni::ResolveExporterMethod(e, exporter);
    if (!owner->send) {
      Throw(e, "java/lang/IllegalStateException",
            "exporter method resolution failed");
      return 0;
    }
    owner->exporter = e->NewGlobalRef(exporter);
    if (!owner->exporter) {
      Throw(e, "java/lang/OutOfMemoryError", "exporter global reference");
      return 0;
    }
    madobs_private_open_result reason = MADOBS_PRIVATE_OPEN_INTERNAL;
    owner->handle = madobs_private_open_ex(directory.c_str(), Send, owner.get(),
                                           &policy, &reason);
    if (!owner->handle) {
      const auto message =
          std::string("private open reason ") + std::to_string(reason);
      Throw(e, "java/lang/IllegalStateException", message.c_str());
      return 0;
    }
    std::lock_guard<std::mutex> lock(State().mutex);
    if (State().next_token == std::numeric_limits<jlong>::max()) {
      Throw(e, "java/lang/IllegalStateException", "token exhaustion");
      return 0;
    }
    const auto token = State().next_token++;
    State().owners.emplace(token, std::move(owner));
    return token;
  } catch (const std::bad_alloc &) {
    Throw(e, "java/lang/OutOfMemoryError", "native allocation");
  } catch (...) {
    Throw(e, "java/lang/IllegalStateException", "native open failure");
  }
  return 0;
}
extern "C" JNIEXPORT jint JNICALL Java_org_madobs_PrivateRuntime_nEmit(
    JNIEnv *e, jclass, jlong token, jint sdk, jint version, jint id,
    jint severity, jintArray pairs) {
  try {
    auto owner = Find(token);
    if (!owner)
      return MADOBS_E_BAD_STATE;
    if (!pairs || sdk < 0 || version < 0 || id < 0 || severity < 0)
      return MADOBS_E_SCHEMA_REJECTED;
    const auto n = e->GetArrayLength(pairs);
    if (n > 16 || n % 2)
      return MADOBS_E_SCHEMA_REJECTED;
    jint input[16]{};
    e->GetIntArrayRegion(pairs, 0, n, input);
    if (e->ExceptionCheck())
      return MADOBS_E_SCHEMA_REJECTED;
    madobs_event_v1 event{};
    event.struct_size = sizeof(event);
    event.abi_version = MADOBS_ABI_VERSION;
    event.sdk_id = sdk;
    event.sdk_version = version;
    event.event_id = id;
    event.severity = severity;
    event.attribute_count = n / 2;
    for (jint i = 0; i < n; i += 2) {
      if (input[i] < 0 || input[i + 1] < 0)
        return MADOBS_E_SCHEMA_REJECTED;
      event.attributes[i / 2] = {static_cast<uint32_t>(input[i]),
                                 static_cast<uint32_t>(input[i + 1])};
    }
    return madobs_private_emit(owner->handle, &event);
  } catch (...) {
    Throw(e, "java/lang/IllegalStateException", "native emit failure");
    return MADOBS_E_BAD_STATE;
  }
}
extern "C" JNIEXPORT jlong JNICALL
Java_org_madobs_PrivateRuntime_nFailures(JNIEnv *e, jclass, jlong token) {
  try {
    auto owner = Find(token);
    return owner ? static_cast<jlong>(owner->failures.load()) : 0;
  } catch (...) {
    Throw(e, "java/lang/IllegalStateException", "native stats failure");
    return 0;
  }
}
extern "C" JNIEXPORT jlong JNICALL
Java_org_madobs_PrivateRuntime_nClose(JNIEnv *e, jclass, jlong token) {
  if (in_callback) {
    Throw(e, "java/lang/IllegalStateException", "close from exporter callback");
    return 0;
  }
  try {
    std::shared_ptr<Owner> owner;
    {
      std::lock_guard<std::mutex> lock(State().mutex);
      auto i = State().owners.find(token);
      if (i == State().owners.end())
        return 0;
      owner = i->second;
      State().owners.erase(i);
    }
    // Owner stays alive through native drain, including concurrent Emit
    // lookups.
    madobs_private_stop(owner->handle);
    madobs_private_destroy(owner->handle);
    // Do not mutate handle: concurrent Emit can safely see a revoked C token.
    // Global reference cleanup waits for all outstanding JNI lookups to finish.
    return static_cast<jlong>(owner->failures.load());
  } catch (...) {
    Throw(e, "java/lang/IllegalStateException", "native close failure");
  }
  return 0;
}
