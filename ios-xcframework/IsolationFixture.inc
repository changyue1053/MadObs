#import <XCTest/XCTest.h>
#include <atomic>
#include <new>
#include "madobs/runtime.h"

namespace {
struct Session {
    explicit Session(uint32_t id) {
        rule.sdk_id = 42;
        rule.sdk_version = 1;
        rule.event_id = id;
    }
    madobs_event_rule_v1 rule{};
    const madobs_privacy_policy_v1 policy{sizeof(policy), MADOBS_ABI_VERSION, 1, &rule};
    std::atomic<unsigned> batches{0};
    std::atomic<unsigned> bad{0};
    madobs_handle_t handle{};
};
void discard(void *context, uint8_t priority, uint64_t, const uint8_t *bytes,
             size_t size, madobs_send_done_fn done, void *doneContext) {
    auto &s = *static_cast<Session *>(context);
    const bool valid = s.batches.fetch_add(1) < 4 && priority <= 2 &&
        bytes && size > 0 && size <= 1024 * 1024 &&
        madobs_check_private_batch(bytes, size, &s.policy) == MADOBS_PRIVATE_BATCH_OK;
    if (!valid) ++s.bad;
    done(doneContext, valid ? 1 : 0);
}
bool emit(Session &s) {
    madobs_event_v1 input{}, canonical{};
    input.struct_size = sizeof(input);
    input.abi_version = MADOBS_ABI_VERSION;
    input.sdk_id = s.rule.sdk_id;
    input.sdk_version = s.rule.sdk_version;
    input.event_id = s.rule.event_id;
    return madobs_sanitize_event(&s.policy, &input, &canonical) == MADOBS_EMITTED &&
           madobs_emit_generic_event(s.handle, &canonical) == MADOBS_OK;
}
bool dispose(Session *&s, uint64_t expected, bool check) {
    if (!s) return false;
    if (!s->handle) { delete s; s = nullptr; return !check; }
    // Stop/destroy failure must retain callback state and its WAL until test process exit.
    if (madobs_stop(s->handle) != MADOBS_OK) return false;
    madobs_generic_stats_v1 stats{};
    bool ok = !check ||
        (madobs_get_generic_stats(s->handle, &stats) == MADOBS_OK &&
         stats.enqueued == expected && stats.drained == expected &&
         stats.queue_dropped == 0 && stats.wal_dropped == 0 &&
         s->batches.load() > 0 && s->batches.load() <= 4 && s->bad.load() == 0);
    if (madobs_destroy(s->handle) != MADOBS_OK) return false;
    delete s; s = nullptr;
    return ok;
}
}

static bool madobsRunIsolation(void) {
    NSString *parent = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [@"MadObs-isolation-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSString *aDir = [parent stringByAppendingPathComponent:@"a"];
    NSString *bDir = [parent stringByAppendingPathComponent:@"b"];
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtPath:parent withIntermediateDirectories:NO
        attributes:@{NSFilePosixPermissions: @0700} error:nullptr]) {
        return false;
    }
    if (![fm createDirectoryAtPath:aDir withIntermediateDirectories:NO
        attributes:@{NSFilePosixPermissions: @0700} error:nullptr] ||
        ![fm createDirectoryAtPath:bDir withIntermediateDirectories:NO
        attributes:@{NSFilePosixPermissions: @0700} error:nullptr]) {
        (void)[fm removeItemAtPath:parent error:nullptr];
        return false;
    }
    Session *a = new (std::nothrow) Session(1);
    Session *b = new (std::nothrow) Session(2);
    bool ok = a && b && madobs_privacy_policy_valid(&a->policy) && madobs_privacy_policy_valid(&b->policy);
    if (ok) {
        a->handle = madobs_create(aDir.fileSystemRepresentation, discard, a);
        b->handle = madobs_create(bDir.fileSystemRepresentation, discard, b);
        ok = a->handle && b->handle && a->handle != b->handle &&
            madobs_start(a->handle) == MADOBS_OK && madobs_start(b->handle) == MADOBS_OK &&
            emit(*a) && emit(*b);
    }
    const bool first = dispose(a, 1, ok);
    // No additional work is admitted until the independent A has fully shut down.
    const bool after = ok && first && emit(*b);
    const bool second = dispose(b, 2, after);
    bool cleaned = false;
    if (!a && !b) cleaned = [fm removeItemAtPath:parent error:nullptr];
    return ok && first && after && second && cleaned;
}
