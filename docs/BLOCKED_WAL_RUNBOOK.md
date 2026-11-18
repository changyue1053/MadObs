# Privacy-blocked WAL: inspect safely and recover future collection

The privacy gate is deliberately not an auto-repair/export/discard mechanism. This
runbook requires a host lifecycle thread and application-controlled private storage.
No command should print WAL payloads or copy them to external diagnostic systems.

1. Inspect `PrivateRuntime::GateStats()` or `madobs_private_get_gate_stats` while the
   handle lives (including after stop). Counters are rejection ATTEMPTS, not distinct
   records. Last batch id/priority/reason are one consistent snapshot. No raw content
   is returned. These are sticky history, NOT proof the last batch is still pending.
2. `BAD_WIRE`: framing/compression parse failed. `UNSAFE_RECORD`: a record is not an
   allowed canonical policy event. `NONCANONICAL`: byte representation/checksum differs
   (including codec incompatibility). `BAD_POLICY` indicates configuration error;
   `RESOURCE_FAILURE` means validation could not complete safely. All refuse forwarding.
   Do not respond by relaxing the policy, deleting manifest, disabling the validator,
   using raw emit/replay, force-acking or copying the WAL into a less restrictive runtime.
3. Stop/join host producers; call private stop, inspect final native and gate stats,
   then destroy. Ensure exporter context remains alive until stop/destroy finishes.
   Native shutdown may attempt export again, so capture final stats after stop.
4. Preserve the old directory unchanged for authorized local investigation. The lock
   is released when C++ Close / C private stop finishes native drain/destroy, even if
   the C handle still exists for stats. After that another exact-policy opener can
   acquire it. Inspection/isolation therefore requires host-level lifecycle coordination;
   do not assume keeping a stopped handle alive reserves the directory. Record
   build, policy/codec versions and numeric rejection reason in the host's own safe logs;
   avoid event payloads and arbitrary path strings in external telemetry.
5. To resume FUTURE observations, explicitly select a NEW application-private directory
   and open the same approved policy there. Do not move old WAL bytes into it. This is
   isolation, not recovery/delivery of the historical blocked records. The automated
   test proves new valid events export and reopening the old directory still refuses
   the contaminated batch. No automatic directory deletion or renaming is provided.
6. Historical repair, policy migration or retention deletion requires a reviewed offline
   procedure and separate user/host authorization. A retry under identical policy and
   codec can recover transient resource errors, but must keep the same fail-closed gate.

A blocked head can prevent its priority queue progressing. P1/P2 underlying retention
can overwrite old data; the rejection counter is not a durable inventory or preservation
guarantee. The SDK does not promise indefinite quarantined storage, remote ack or lossless
recovery. Implement retention/alerts appropriate to the host without bypassing privacy.

## Open failure diagnostics

Use `madobs_private_open_ex` or the C++ Open diagnostic output to avoid guessing from
zero/null. No paths or payloads are returned. INVALID_ARGUMENT/POLICY require caller
configuration repair. WAL_BUSY means another cooperative owner currently holds the
lock; do not bypass it. WAL_UNBOUND means an existing directory lacks an identity,
not proof it is empty/safe. POLICY_OR_MANIFEST_MISMATCH intentionally does not guess
whether storage is corrupt or a different policy/version/order was selected.
WAL_UNSAFE rejects ownership/mode/type/link violations. WAL_UNAVAILABLE or IO_ERROR
requires checking host storage access, not relaxing privacy. Native create/start,
registry capacity and INTERNAL failures remain separate coarse categories.

A concurrent first initializer may transiently leave an existing directory before its
manifest is created, producing UNBOUND. A bounded retry with the SAME policy is safe;
no diagnostic authorizes repairing/deleting metadata or falling back to raw replay.
Legacy Open retains its zero-on-failure ABI. Diagnostic pointers must reference valid
writable storage; they do not make invalid C pointers safe.
