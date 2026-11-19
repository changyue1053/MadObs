#import <XCTest/XCTest.h>
#include <atomic>
#include "madobs/runtime.h"
extern "C" int madobs_api_only_contract(void);

namespace {
// No payload retention, decoding, logging, endpoint, or network operation.
// Success here means this local test sink consumed the callback, never remote ACK.
struct Sink {
    std::atomic<unsigned> batches{0};
    std::atomic<unsigned> malformed{0};
};
void consume(void *context, uint8_t priority, uint64_t batch_id,
             const uint8_t *bytes, size_t length, madobs_send_done_fn done, void *done_context) {
    (void)batch_id;
    Sink *sink = static_cast<Sink *>(context);
    if (priority > 2 || bytes == nullptr || length == 0) ++sink->malformed;
    ++sink->batches;
    done(done_context, 1); // Exactly once; no lifecycle calls from this callback.
}
struct Owner {
    madobs_handle_t handle;
    ~Owner() { if (handle) { (void)madobs_stop(handle); (void)madobs_destroy(handle); } }
};
}
@interface MadObsNativeTests : XCTestCase
@end
@implementation MadObsNativeTests
- (void)testHeaderOnlyContract {
    XCTAssertEqual(madobs_api_only_contract(), 0);
}
- (void)testInstalledArchiveLocalSinkAndLifecycle {
    NSURL *directory = [NSURL fileURLWithPath:[NSTemporaryDirectory()
        stringByAppendingPathComponent:[@"MadObs-synthetic-" stringByAppendingString:NSUUID.UUID.UUIDString]]];
    NSError *error = nil;
    XCTAssertTrue([NSFileManager.defaultManager createDirectoryAtURL:directory
        withIntermediateDirectories:NO attributes:nil error:&error]);
    if (error) return;
    @try {
        Sink sink; // Must outlive owner and every callback.
        Owner owner{madobs_create(directory.fileSystemRepresentation, consume, &sink)};
        XCTAssertNotEqual(owner.handle, (madobs_handle_t)0);
        if (!owner.handle) return;
        XCTAssertEqual(madobs_start(owner.handle), MADOBS_OK);
        madobs_observer_binding_v1 binding{};
        madobs_observer_v1 observer{};
        XCTAssertEqual(madobs_observer_for_runtime(owner.handle, &binding, &observer), MADOBS_OK);
        madobs_event_v1 event{};
        event.struct_size = sizeof(event); event.abi_version = MADOBS_ABI_VERSION;
        event.sdk_id = 42; event.sdk_version = 1; event.event_id = 7; event.severity = 4;
        event.attribute_count = 1; event.attributes[0] = {8, 1};
        XCTAssertEqual(madobs_observer_emit(&observer, &event), MADOBS_EMITTED);
        XCTAssertEqual(madobs_emit_generic_event(owner.handle, &event), MADOBS_OK);
        // This test is the only producer. It has now stopped; context remains live.
        XCTAssertEqual(madobs_stop(owner.handle), MADOBS_OK);
        XCTAssertEqual(madobs_stop(owner.handle), MADOBS_OK);
        madobs_generic_stats_v1 stats{};
        XCTAssertEqual(madobs_get_generic_stats(owner.handle, &stats), MADOBS_OK);
        XCTAssertEqual(stats.enqueued, (uint64_t)2);
        XCTAssertEqual(stats.drained, (uint64_t)2);
        XCTAssertEqual(stats.queue_dropped, (uint64_t)0);
        XCTAssertEqual(stats.wal_dropped, (uint64_t)0);
        XCTAssertGreaterThan(sink.batches.load(), 0u);
        XCTAssertEqual(sink.malformed.load(), 0u);
        XCTAssertEqual(madobs_start(owner.handle), MADOBS_E_BAD_STATE);
        const madobs_handle_t old = owner.handle;
        XCTAssertEqual(madobs_destroy(old), MADOBS_OK);
        owner.handle = 0;
        XCTAssertEqual(madobs_destroy(old), MADOBS_E_INVALID_HANDLE);
    } @finally {
        // Only this test's UUID directory, after native owner has drained/destroyed.
        [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
    }
}
@end
