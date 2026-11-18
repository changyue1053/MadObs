#include "jni_helpers.h"
#include <cstdlib>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      std::abort();                                                            \
  } while (0)
namespace {
bool pending = false, class_failure = false, method_failure = false;
unsigned classes = 0, methods = 0, deletions = 0;
jboolean JNICALL ExceptionCheck(JNIEnv *) {
  return pending ? JNI_TRUE : JNI_FALSE;
}
jclass JNICALL GetObjectClass(JNIEnv *, jobject) {
  ++classes;
  if (class_failure) {
    pending = true;
    return nullptr;
  }
  return reinterpret_cast<jclass>(1);
}
jmethodID JNICALL GetMethodID(JNIEnv *, jclass, const char *, const char *) {
  ++methods;
  CHECK(!pending);
  if (method_failure) {
    pending = true;
    return nullptr;
  }
  return reinterpret_cast<jmethodID>(2);
}
void JNICALL DeleteLocalRef(JNIEnv *, jobject) { ++deletions; }
} // namespace
int main() {
  JNINativeInterface_ table{};
  table.ExceptionCheck = ExceptionCheck;
  table.GetObjectClass = GetObjectClass;
  table.GetMethodID = GetMethodID;
  table.DeleteLocalRef = DeleteLocalRef;
  JNIEnv env{&table};
  const auto object = reinterpret_cast<jobject>(3);
  class_failure = true;
  CHECK(!madobs_jni::ResolveExporterMethod(&env, object));
  CHECK(pending && classes == 1 && methods == 0 && deletions == 0);
  // Pre-existing pending exception must not call either lookup.
  CHECK(!madobs_jni::ResolveExporterMethod(&env, object));
  CHECK(classes == 1 && methods == 0);
  class_failure = false;
  pending = false;
  method_failure = true;
  CHECK(!madobs_jni::ResolveExporterMethod(&env, object));
  CHECK(pending && classes == 2 && methods == 1 && deletions == 1);
  pending = false;
  method_failure = false;
  CHECK(madobs_jni::ResolveExporterMethod(&env, object));
  CHECK(!pending && classes == 3 && methods == 2 && deletions == 2);
}
