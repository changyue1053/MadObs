#ifndef MADOBS_JNI_HELPERS_H
#define MADOBS_JNI_HELPERS_H
#include <jni.h>
namespace madobs_jni {
inline jmethodID ResolveExporterMethod(JNIEnv *env, jobject exporter) {
  if (env->ExceptionCheck())
    return nullptr;
  jclass cls = env->GetObjectClass(exporter);
  if (!cls || env->ExceptionCheck()) {
    if (cls)
      env->DeleteLocalRef(cls);
    return nullptr;
  }
  jmethodID method = env->GetMethodID(cls, "send", "(IJ[B)Z");
  env->DeleteLocalRef(cls); // permitted even with a pending exception
  if (env->ExceptionCheck())
    return nullptr;
  return method;
}
} // namespace madobs_jni
#endif
