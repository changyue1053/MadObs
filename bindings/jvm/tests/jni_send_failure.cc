// Include the bridge implementation in this isolated native test executable so
// the callback/attachment boundary can use synthetic JNI tables, not a real VM.
// This does not simulate the JVM allocator or prove whole-process OOM safety.
#include "../private_jni.cc"
#include <cstdlib>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      std::abort();                                                            \
  } while (0)
namespace fixture {
JNIEnv *env = nullptr;
jint get_env = JNI_OK;
bool attach_fail = false, array_fail = false, copy_fail = false;
bool method_fail = false, result = true, pending = false;
unsigned attachments = 0, detachments = 0, arrays = 0, methods = 0,
         deletions = 0, clears = 0;
unsigned completions = 0, global_refs = 0;
int acknowledged = -1;
jint JNICALL GetEnv(JavaVM *, void **p, jint) {
  *p = env;
  return get_env;
}
jint JNICALL Attach(JavaVM *, void **p, void *) {
  ++attachments;
  *p = attach_fail ? nullptr : env;
  return attach_fail ? JNI_ERR : JNI_OK;
}
jint JNICALL Detach(JavaVM *) {
  ++detachments;
  return JNI_OK;
}
jbyteArray JNICALL NewArray(JNIEnv *, jsize) {
  ++arrays;
  if (array_fail) {
    pending = true;
    return nullptr;
  }
  return reinterpret_cast<jbyteArray>(1);
}
void JNICALL Copy(JNIEnv *, jbyteArray, jsize, jsize, const jbyte *) {
  if (copy_fail)
    pending = true;
}
jboolean JNICALL Pending(JNIEnv *) { return pending ? JNI_TRUE : JNI_FALSE; }
void JNICALL Clear(JNIEnv *) {
  ++clears;
  pending = false;
}
void JNICALL Delete(JNIEnv *, jobject) { ++deletions; }
jboolean JNICALL Call(JNIEnv *, jobject, jmethodID, va_list) {
  ++methods;
  CHECK(in_callback);
  if (method_fail)
    pending = true;
  return result ? JNI_TRUE : JNI_FALSE;
}
// nOpen reaches NewGlobalRef but must fail before touching native WAL/runtime.
jsize JNICALL Length(JNIEnv *, jarray array) {
  return array == reinterpret_cast<jarray>(4) ? 1 : 0;
}
void JNICALL ReadPath(JNIEnv *, jbyteArray, jsize, jsize count, jbyte *out) {
  CHECK(count == 1);
  out[0] = '.';
}
jclass JNICALL ObjectClass(JNIEnv *, jobject) {
  return reinterpret_cast<jclass>(6);
}
jmethodID JNICALL Method(JNIEnv *, jclass, const char *, const char *) {
  CHECK(!pending);
  return reinterpret_cast<jmethodID>(7);
}
jobject JNICALL GlobalRef(JNIEnv *, jobject) {
  ++global_refs;
  pending = true;
  return nullptr;
}
void Done(void *, int ok) {
  ++completions;
  acknowledged = ok;
}
void Reset() {
  get_env = JNI_OK;
  attach_fail = array_fail = copy_fail = method_fail = pending = false;
  result = true;
  attachments = detachments = arrays = methods = deletions = clears =
      completions = 0;
  acknowledged = -1;
}
void Run(uint64_t failures, int ack, size_t size = 1) {
  Owner owner;
  owner.exporter = reinterpret_cast<jobject>(2);
  owner.send = reinterpret_cast<jmethodID>(3);
  const uint8_t byte = 0;
  Send(&owner, 0, 1, &byte, size, Done, nullptr);
  CHECK(owner.failures == failures && completions == 1 && acknowledged == ack);
  CHECK(!pending && !in_callback);
  owner.exporter = nullptr; // synthetic reference, never pass to a real JVM
}
} // namespace fixture
int main() {
  using namespace fixture;
  JNINativeInterface_ native{};
  native.NewByteArray = NewArray;
  native.SetByteArrayRegion = Copy;
  native.ExceptionCheck = Pending;
  native.ExceptionClear = Clear;
  native.DeleteLocalRef = Delete;
  native.CallBooleanMethodV = Call;
  native.GetArrayLength = Length;
  native.GetByteArrayRegion = ReadPath;
  native.GetObjectClass = ObjectClass;
  native.GetMethodID = Method;
  native.NewGlobalRef = GlobalRef;
  JNIEnv environment{&native};
  env = &environment;
  JNIInvokeInterface_ invocation{};
  invocation.GetEnv = GetEnv;
  invocation.AttachCurrentThread = Attach;
  invocation.DetachCurrentThread = Detach;
  JavaVM machine{&invocation};
  CHECK(JNI_OnLoad(&machine, nullptr) == JNI_VERSION_1_6);
  Reset();
  get_env = JNI_EDETACHED;
  attach_fail = true;
  Run(1, 0);
  CHECK(attachments == 1 && detachments == 0 && arrays == 0);
  Reset();
  get_env = JNI_EVERSION;
  Run(1, 0);
  CHECK(attachments == 0 && arrays == 0);
  Reset();
  array_fail = true;
  Run(1, 0);
  CHECK(clears == 1 && methods == 0 && deletions == 0);
  Reset();
  get_env = JNI_EDETACHED;
  copy_fail = true;
  Run(1, 0);
  CHECK(attachments == 1 && detachments == 1 && methods == 0 &&
        deletions == 1 && clears == 1);
  Reset();
  method_fail = true;
  Run(1, 0);
  CHECK(methods == 1 && clears == 1 && deletions == 1 && detachments == 0);
  Reset();
  result = false;
  Run(0, 0);
  CHECK(methods == 1 && clears == 0);
  Reset();
  get_env = JNI_EDETACHED;
  Run(0, 1);
  CHECK(attachments == 1 && detachments == 1 && methods == 1);
  Reset();
  Run(0, 1);
  CHECK(attachments == 0 && detachments == 0 && methods == 1);
  Reset();
  Run(1, 0, static_cast<size_t>(INT_MAX) + 1);
  CHECK(arrays == 0);
  Reset();
  CHECK(Java_org_madobs_PrivateRuntime_nOpen(
            env, nullptr, reinterpret_cast<jbyteArray>(4),
            reinterpret_cast<jobjectArray>(5),
            reinterpret_cast<jobject>(8)) == 0);
  CHECK(global_refs == 1 && pending && clears == 0 && completions == 0);
  // Native code preserved the original pending allocation exception. No real
  // JVM/allocator was involved, and no directory or native runtime was opened.
  pending = false;
}
