import org.madobs.PrivateRuntime;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class JvmSmoke {
    private static final int[][] RULES = {{2, 1, 1, 1, 1, 7, 2, 0, 1}};
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void invalid(Path path, int[][] rules) {
        try (PrivateRuntime ignored = new PrivateRuntime(path.toString(), rules, (p,i,b) -> true)) {
            ignored.callbackFailures(); throw new AssertionError("policy accepted");
        } catch (IllegalArgumentException expected) { /* rejected */ }
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("madobs_jvm_");
        AtomicInteger batches = new AtomicInteger();
        try {
            invalid(root.resolve("bad1"), new int[][]{{2,1,1,1,0,99}});
            invalid(root.resolve("bad2"), new int[][]{{2,1,1,1,1,7,2,1,1}});
            invalid(root.resolve("bad3"), new int[][]{null});
            invalid(root.resolve("bad4"), new int[][]{{-1,1,1,1,0}});
            int[][] copy = {{2,1,1,1,0}};
            try (PrivateRuntime runtime = new PrivateRuntime(root.resolve("copy").toString(), copy,
                    (p,id,b) -> { check(b.length > 0); batches.incrementAndGet(); return true; })) {
                copy[0][0] = 9;
                check(runtime.emit(2,1,1,1,new int[]{7,1}) == 0);
                check(runtime.emit(9,1,1,1,new int[]{}) != 0);
                check(runtime.emit(2,1,1,1,new int[]{7}) != 0);
                check(runtime.emit(2,1,1,1,new int[]{7,-1}) != 0);
            }
            check(batches.get() > 0);
            AtomicReference<PrivateRuntime> holder = new AtomicReference<>();
            AtomicInteger closeRejected = new AtomicInteger();
            PrivateRuntime runtime = new PrivateRuntime(root.resolve("race").toString(), RULES,
                (p,id,b) -> {
                    try { holder.get().close(); throw new AssertionError("callback close accepted"); }
                    catch (IllegalStateException expected) { closeRejected.incrementAndGet(); }
                    return true;
                });
            holder.set(runtime);
            check(runtime.emit(2,1,1,1,new int[]{7,1}) == 0);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            CountDownLatch go = new CountDownLatch(1);
            Thread producer = new Thread(() -> {
                try { go.await(); for (int i=0;i<2000;i++) runtime.emit(2,1,1,1,new int[]{7,0}); }
                catch (Throwable t) { failure.set(t); }
            });
            Thread first = new Thread(() -> { try { go.await(); runtime.close(); } catch(Throwable t) { failure.set(t); } });
            Thread second = new Thread(() -> { try { go.await(); runtime.close(); } catch(Throwable t) { failure.set(t); } });
            producer.start(); first.start(); second.start(); go.countDown();
            producer.join(); first.join(); second.join();
            check(failure.get() == null); check(closeRejected.get() > 0);
            check(runtime.emit(2,1,1,1,new int[]{}) != 0); runtime.close();
            // Deliberately stalled test-only exporter proves both close callers
            // wait for native drain. Production exporters must not block.
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            AtomicInteger closed = new AtomicInteger();
            PrivateRuntime draining = new PrivateRuntime(root.resolve("drain").toString(), RULES,
                (p,id,b) -> {
                    entered.countDown();
                    try { release.await(); } catch (InterruptedException ex) {
                        Thread.currentThread().interrupt(); return false;
                    }
                    return true;
                });
            check(draining.emit(2,1,1,1,new int[]{}) == 0);
            Thread closeA = new Thread(() -> { draining.close(); closed.incrementAndGet(); });
            Thread closeB = new Thread(() -> { draining.close(); closed.incrementAndGet(); });
            closeA.start();
            check(entered.await(5, java.util.concurrent.TimeUnit.SECONDS));
            closeB.start(); Thread.sleep(50);
            check(closed.get() == 0);
            release.countDown(); closeA.join(); closeB.join();
            check(closed.get() == 2);
            // Worker callback exception must be cleared and NACKed; same-policy
            // reopen then exports the pending record successfully.
            Path exceptionWal = root.resolve("exception");
            AtomicInteger throwsSeen = new AtomicInteger();
            PrivateRuntime throwing = new PrivateRuntime(exceptionWal.toString(), RULES,
                (p,id,b) -> { throwsSeen.incrementAndGet(); throw new IllegalStateException("test callback"); });
            try (throwing) {
                check(throwing.emit(2,1,1,1,new int[]{7,1}) == 0);
            }
            check(throwsSeen.get() > 0);
            check(throwing.callbackFailures() > 0);
            AtomicInteger replay = new AtomicInteger();
            try (PrivateRuntime recovered = new PrivateRuntime(exceptionWal.toString(), RULES,
                (p,id,b) -> { replay.incrementAndGet(); return true; })) {
                recovered.callbackFailures();
            }
            check(replay.get() > 0);
            try (PrivateRuntime mismatch = new PrivateRuntime(exceptionWal.toString(), new int[][]{}, (p,id,b) -> true)) {
                mismatch.callbackFailures(); throw new AssertionError("mismatch accepted");
            } catch (IllegalStateException expected) { check(expected.getMessage().contains("reason 6")); }
            // Repeated ownership churn and non-ASCII UTF-8 path.
            for (int i=0;i<40;i++) {
                try (PrivateRuntime r = new PrivateRuntime(root.resolve("路径"+i).toString(), RULES,(p,id,b)->true)) {
                    check(r.emit(2,1,1,1,new int[]{}) == 0);
                }
            }
        } finally {
            try (var paths = Files.walk(root)) {
                paths.sorted(Comparator.reverseOrder()).forEach(p -> {
                    try { Files.delete(p); } catch (java.io.IOException e) { throw new java.io.UncheckedIOException(e); }
                });
            }
        }
        System.out.println("JVM lifecycle, policy, exception, replay and concurrent close/emit passed");
    }
}
