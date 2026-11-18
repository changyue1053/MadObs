# Owning private runtime binding (experimental)

`madobs/private_runtime.hpp` is a C++11 native ownership substrate for future
JNI/Swift/OHOS bindings, not those managed-language packages. It links MadObs::runtime
and needs POSIX mkdir (the native platform targets use POSIX). No sibling SDK is used.

`PrivateRuntime::Open` validates and deep-copies the policy. Policy/event input arrays
must be valid during the call. Caller can release policy storage after Open. The class
has no raw handle/observer accessors and its only emission path applies privacy rules.
An empty policy permits no events. `Emit` returns native admission status, not remote
acknowledgement. Stats are live atomic snapshots or final post-drain snapshots.

## Privacy and persistence restrictions

Open creates a new WAL directory with mode 0700, or reopens a directory carrying
an exact matching `.madobs-policy-v1` identity. The manifest contains fixed little-
endian fields for ABI, event ids/versions/severity and active attribute enum rules,
not hashes of padded C structs. Rule/enum ordering is significant; both tighter and
looser policies reject existing storage. Migration is never guessed.

Manifest is mode 0600, single-link regular file owned by current uid, opened relative
to a no-follow directory descriptor. Directory must also be uid-owned with no group/
other permission. Missing, corrupt, truncated, symlink or wrong-policy manifest fails
closed. The manifest and directory are synced before runtime creation, including matched
reopens after a prior failed sync.
The manifest descriptor holds an exclusive advisory lock until native destroy. Existing
owners fail immediately on lock contention. First initializer can wait for a concurrent
reader of its incomplete manifest to release the lock; it does not race its own write.

Parent/path must remain private and trusted; raw callers do not honor the advisory lock.
There is no protection against same-uid hostile path replacement or copied/forged metadata.
No automatic deletion occurs: failed Open may leave partial or valid state, including
when native slots are full. A partial identity requires host inspection/cleanup, not an
automatic repair. A valid matching identity may be safely retried. OS crash release of
locks and process `_exit` replay are tested, not actual power-loss/filesystem guarantees.

Every exported batch is independently validated against the current immutable policy.
Every record must be canonical G1, unchanged by privacy minimization. Entire wire batch
is reconstructed and compared byte-for-byte (including compressed representation and
batch id), rejecting trailing compressed payloads and noncanonical encodings. Unknown
or raw legacy records, extra attributes and precise event time reject the entire batch.
Rejected data is NOT acknowledged/deleted: RejectedBatches reports attempted rejections,
which may count repeated attempts, not unique records. A rejected head batch can stall
that priority's backlog; host intervention is required, no unsafe fallback is offered.

Canonical recompression uses the installed zlib implementation. A future compression
change may refuse old compressed data despite equal policy; an explicit codec/migration
compatibility decision is required. Batch clock anchors and network timing remain
visible; only event timestamps are cleared. No encryption/authentication/consent or
hard process isolation is added. The raw ABI remains unrestricted elsewhere.

## Ownership and shutdown

Each concurrent caller retains its own shared_ptr copy. Do not race operations on the
same shared_ptr variable. Calls through an object whose last owner was freed remain
invalid C++ usage. The exporter context remains host-owned and lives through Close.
Do not call Close or release the last owner from exporter callbacks (worker self-join
is unsupported). Exporter must be nonblocking and call done exactly once.

Emit admission and Close serialize under a mutex. Close detaches the handle before
drain, so no later admission can become an orphan. Concurrent Close callers wait for
the same drain/destroy and then observe final stats. No admission lock is held during
native drain/export callbacks; callback-side Emit returns BAD_STATE. Large policies
are still revalidated for each event. Mutex/condition-variable use is intentional in
this binding; the raw producer queue remains unchanged, not a claim of lock-free
binding performance. Allocation/mutex exceptions follow normal C++ behavior.

Close is terminal. It flushes/attempts export but is not a remote delivery guarantee;
remaining local data may be reopened only under the exact same policy and validated
export gate, never through a less restrictive raw fallback.
`samples/private_cpp` is a fresh installed-package Release C++11 consumer.

### C lifetime completion and error scope

Legacy C Open returns 0 on failure; the additive OpenEx reports coarse reason
codes for configuration, WAL identity/access/lock and native startup failures. Do not treat 0 as a reason
to modify/delete WAL metadata. C destroy removes the token before waiting for drain:
a concurrent second destroy may return INVALID_HANDLE while the first is still
working. INVALID_HANDLE is NOT a drain-completion signal. Use one host lifecycle
owner to stop, read final stats, destroy and then release callback/context storage;
other threads must not free that context on an invalid-token result.

The WAL lock is released by C++ Close or C private stop after native drain/destroy,
not by the later lifetime of a stopped C handle. A stopped handle supports final
stats but does not reserve its path; host-level coordination must prevent another
opener while inspecting or isolating that directory.
