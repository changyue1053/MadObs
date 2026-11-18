package org.madobs;

import java.nio.charset.StandardCharsets;
import java.util.Objects;

/** Explicitly owned, policy-filtered runtime. Host-JVM binding, not an Android package. */
public final class PrivateRuntime implements AutoCloseable {
    static { System.loadLibrary("madobs_jni"); }
    @FunctionalInterface public interface Exporter {
        /** Return true only for acknowledged delivery. Must not block or close runtimes. */
        boolean send(int priority, long batchId, byte[] batch);
    }
    private volatile long token;
    private volatile long finalFailures;
    /** Rule rows: sdk, version, event, maxSeverity, attributeCount, then
     * (key, valueCount, values...) per attribute. All numbers nonnegative int.
     * The immutable native copy is policy-bound to this private WAL directory.
     * Host owns the parent directory; no attacker may replace it during use.
     */
    public PrivateRuntime(String walDirectory, int[][] rules, Exporter exporter) {
        Objects.requireNonNull(walDirectory); Objects.requireNonNull(rules);
        Objects.requireNonNull(exporter);
        if (walDirectory.indexOf('\0') >= 0 || walDirectory.isEmpty())
            throw new IllegalArgumentException("invalid WAL directory");
        token = nOpen(walDirectory.getBytes(StandardCharsets.UTF_8), rules, exporter);
    }
    /** Status 0 means admitted, not persisted or remotely delivered. */
    public int emit(int sdk, int version, int event, int severity, int[] keyValuePairs) {
        return nEmit(token, sdk, version, event, severity, Objects.requireNonNull(keyValuePairs));
    }
    /** Callback exceptions/allocation or attachment failures, not delivery failures. */
    public long callbackFailures() {
        long current = token;
        return current == 0 ? finalFailures : nFailures(current);
    }
    /** Idempotent, concurrent callers wait for the same drain. Not callable from
     * any exporter callback. Must not hold application locks needed by exporter.
     * No finalizer/cleaner: host MUST close. Closing does not promise remote ACK.
     */
    @Override public void close() {
        if (nInCallback()) throw new IllegalStateException("close from exporter callback");
        synchronized (this) {
            long current = token;
            if (current == 0) return;
            // Keep token until drain completes so all close callers can wait.
            finalFailures = nClose(current);
            token = 0;
        }
    }
    private static native long nOpen(byte[] path, int[][] rules, Exporter exporter);
    private static native int nEmit(long token, int sdk, int version, int event, int severity, int[] pairs);
    private static native long nFailures(long token);
    private static native long nClose(long token);
    private static native boolean nInCallback();
}
