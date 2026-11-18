# Experimental host-JVM private binding

Explicit opt-in: `-DMADOBS_BUILD_JNI=ON`, with `JAVA_HOME` pointing at a full JDK.
Linux x86_64 / GCC 14 / Temurin 21 is the tested configuration. Default builds do
not find or require Java. This is **not an Android NDK/AAR or device qualification**.
The JAR is compiled for the selected JDK, not an Android-compatible bytecode floor.

Build/install creates `lib/libmadobs_jni.so` and
`share/MadObs/java/madobs-jvm.jar`. Put the JAR on the classpath and the library
folder on `java.library.path`. Use the JAR and native library from the same build;
no cross-version Java/native compatibility is promised. `scripts/verify-jvm.sh` rebuilds and exercises the
installed/relocated pair. The script tests the default null-provider runtime;
optional BoringSSL JNI composition has not been tested. Provider archives must be
PIC if linked into a shared binding. No JDK binary is redistributed here.

## Ownership and contract

`org.madobs.PrivateRuntime` implements AutoCloseable. Use try-with-resources or
explicit close on every path. There is no finalizer/cleaner or automatic shutdown
hook. Abandoned runtimes hold native slots, threads, WAL locks and strong exporter
references until process exit. The native registry deliberately has no destructor
that could call a JVM after shutdown; this is not a leak-free misuse guarantee.

- Construct with a host-private WAL directory, immutable configured rule rows and
  an exporter. Rows are copied into a native policy; do not concurrently mutate
  constructor inputs. Encoding is documented in the Java source. Nonnegative
  Java int IDs/enum values only (not the full uint32 native domain).
- Constructor rejects malformed rules. Native opening failures throw a numeric
  reason via IllegalStateException; original Java allocation exceptions remain
  pending and are not replaced. Caller-owned parent directory must not be
  maliciously replaced; policy-bound native WAL restrictions still apply.
- `emit` returns a native status. Zero is admission only, not durability/delivery.
  No timestamp or string payload API is offered. Unknown events reject; unknown
  dimensions and out-of-domain values are removed by the configured policy.
- Export runs on a native worker attached to the JVM for that call. `byte[]` is a
  Java-owned copy, so it can be retained. Return true only for acknowledged
  delivery. There is no asynchronous acknowledgement interface in this binding.
- Exporter must not block or close **any** runtime, and close caller must not hold
  application locks needed by the exporter. Callback-side close throws before
  waiting for the Java monitor, preventing worker self-join. An uncaught callback
  exception is cleared, counted and NACKed. Failed attachment/allocation is also
  NACKed. No exception or batch payload is logged by this bridge.
- Concurrent emit/close is supported. Concurrent close callers synchronize and
  wait for drain; close is idempotent. After close, emit returns BAD_STATE and
  `callbackFailures` returns the final snapshot. During close, stats can observe
  revocation and return zero; they are diagnostic, not an atomic lifecycle barrier.
- Opening may replay existing pending batches before constructor returns: the
  exporter must not assume the new Java owner has already been assigned.
- Stop/drain is not a remote-delivery guarantee. Callback exceptions leave WAL
  data unacknowledged for same-policy replay; policy changes still fail closed.

Native exports are restricted to JNI entry points on tested Linux. Hidden static
provider symbols do not prove arbitrary cross-library ABI or provider compatibility.
The façade is not a security sandbox against reflective/native code in the same VM.

## Verified and unverified

`-Xcheck:jni` tests include policy rejection/copying, UTF-8 paths, callback-close
rejection, two-close drain waiting, emit/close race, exception NACK/replay, policy
mismatch and repeated ownership churn. Synthetic JNI function-table injection
checks GetObjectClass/GetMethodID failure short-circuiting and preservation of
pending exceptions. It is **not** actual whole-JVM OOM testing: AttachCurrentThread,
NewGlobalRef and NewByteArray failures have not all been injected. JVM plus native
ASan/TSan/LSan interoperability is not yet qualified. No classloader hot-unload,
Android GC/runtime, Swift or OHOS tests are claimed.
