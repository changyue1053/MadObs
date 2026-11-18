# Eleventh increment: synthetic JNI failure-boundary coverage

Status: test-only expansion; production JNI implementation is unchanged from
Phase10. The complete JNI-enabled Release suite passes 45/45. Evidence:
verification/phase11-jni-faults.log. Phase10 public commit
9df07725e4bc167f12b7f8084872e3a6b381343e passed all four CI jobs, including host-JVM:
https://github.com/changyue1053/MadObs/actions/runs/44287721102

## What the new test proves

The isolated native executable compiles the actual bridge implementation against
synthetic JNIEnv/JavaVM function tables. It does not start a real JVM. It checks:

- GetEnv unsupported version and AttachCurrentThread failure NACK exactly once;
  an unsuccessful attachment is never detached.
- NewByteArray failure, SetByteArrayRegion failure and Java callback exception
  clear the pending callback exception, increment the numeric diagnostic and NACK.
- Plain callback false is a delivery NACK, not a callback-exception counter.
- Success ACKs once; only threads attached by the bridge are detached. Already
  attached caller threads are not detached by the bridge.
- Oversized array input is rejected before attempting Java array allocation.
- Callback guard is active during Java invocation and restored afterwards.
- NewGlobalRef failure in the actual nOpen path returns no token and preserves
  the original pending allocation exception, before any native WAL is opened.
- Existing separate lookup tests cover GetObjectClass/GetMethodID short-circuiting.

## What it does not prove

These are deterministic branch/cleanup assertions, not whole-JVM memory pressure,
OS thread-exhaustion or allocator-failure tests. They do not qualify Android ART,
NDK/AAR, mobile devices, classloader hot-unload, DetachCurrentThread failure,
physical disk/power behavior or native/JVM sanitizer interoperability. Explicit
close and nonblocking exporter requirements are unchanged. No new SDK license was
accepted and no Android NDK was downloaded for this increment.
