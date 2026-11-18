# MadObs (local SDK migration, 0.1.0 experimental)

Standalone observability SDK workbench. No IMSDK, no dependency on another Mad SDK.
Existing native capabilities are preserved from the pinned IMObs baseline; this is
not a blank rewrite or a claim of production readiness. Owner-authorized source
checkpoints are public; no product release or GA certification is implied.

## Build

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
# Consumer that only needs the observation contract (no runtime, zlib, disk or network):
cmake -S . -B build-api -DMADOBS_BUILD_RUNTIME=OFF
cmake --build build-api
ctest --test-dir build-api --output-on-failure
```

`MadObs::api` is a C/C++ header-only observation contract. A null observer or
`madobs_noop_observer()` has no side effects. Callbacks borrow input synchronously;
stop/join producers before releasing the callback context. V1 deliberately rejects
arbitrary string attributes; compile-time numeric schema IDs and bounded enum values
are the only attribute shape. This reduces accidental capture, but does not certify
caller-provided integers as privacy-safe. No automatic logging/prompt/token capture.

`MadObs::runtime` is the retained native implementation. Its APIs include new `madobs_` symbols and the retained
`imobs_` compatibility surface; no broad renaming of wire, schema, file magic, or cryptographic labels
has been performed. Host provides the transport. The generic bridge and Mad-prefixed C symbols are now implemented and tested.
The runtime ABI remains experimental; platform façades and stability guarantees are not finished.
Remote control is disabled without an explicitly provisioned key; the historical
well-known development key is rejected. Healing only runs for explicitly registered
scenes; a separately packaged healing module is future work.

## Boundaries / release gates

- Source LICENSE is absent. Choose and audit a public license before publication.
- BoringSSL is optional and explicitly supplied, never discovered in a sibling SDK.
  Without it encrypted logs fail closed; no claim of encryption is made for plain mode.
- Native tests run on Linux x86_64 here. Android/iOS/OHOS device validation is pending.
- Legacy raw-log API is preserved and is **not** an automatic text-redaction engine.
- Callback context lifetime is host-owned; concurrent destroy/emission is unsupported.
- Full compatibility, SBOM, secret scanning and external security review remain gates.

See MIGRATION_SOURCE.toml, PUBLIC_API.md and TASK_RESULT.md.


## Generic runtime integration

Include `madobs/runtime.h`, create/start a runtime with a nonblocking Host exporter,
then bind a Host-owned `madobs_observer_binding_v1` using
`madobs_observer_for_runtime`. The resulting observer can be injected into multiple
independent SDKs. Generic events use the existing WAL and exporter, not a second runtime.
See `samples/runtime_c`, `PUBLIC_API.md` and `docs/GENERIC_WIRE_V1.md` for contracts.
Stop/join producers before stopping/destroying runtime. Default Noop still requires
no runtime link. `stop` is terminal; create a new instance to restart.

## Explicit privacy policy and mobile native builds

`madobs/privacy.h` provides a runtime-free, deny-by-default allowlist helper.
Use `madobs_observer_emit_private` or sanitize before direct submission. Unknown
attributes/out-of-domain enum values and precise timestamps are removed. This is
**opt-in at the callsite**, not automatic protection for raw emit/legacy logging.
See `docs/PRIVACY_SCHEMA.md` for limitations and host schema responsibilities.

`scripts/build-platform.sh` supplies native static build entrypoints for Android,
iOS and OHOS. Target SDKs are absent here, so no mobile binary/package/device result
is claimed. See `platforms/README.md`. Latest local verification:
`docs/PHASE3_RESULT.md`; prior source/security work: `docs/PHASE2_RESULT.md`.

## Optional owning private runtime

`madobs/private_runtime.hpp` adds a C++11 ownership binding with deep-copied schema
and synchronized Emit/Close. It permits WAL recovery only with an exact versioned policy identity and validates
every exported batch against that policy; raw/unknown/changed-policy storage fails closed.
Raw ABI is unchanged. See `docs/PRIVATE_RUNTIME.md` and latest `docs/PHASE5_RESULT.md`.
This native substrate is not a validated JNI/Swift/ArkTS managed package.

## Owning foreign-interface ABI and blocked-WAL operations

`madobs/private.h` exposes the tested owning C binding for future platform adapters,
with generation handles and numeric privacy-gate diagnostics. It is not a compiled
JNI/Swift/ArkTS package. See `samples/private_c`, `docs/BLOCKED_WAL_RUNBOOK.md` and
latest `docs/PHASE6_RESULT.md`. Recovery of future collection never disables the
privacy gate or silently deletes/acknowledges blocked historical data.

Latest diagnostic/package increment: `docs/PHASE7_RESULT.md`. Use additive
`madobs_private_open_ex` for coarse safe failure reasons. `scripts/verify-installed.sh`
checks fresh default null-provider native/API packages and relocation offline. The
BoringSSL-enabled installation still embeds an external archive path and is NOT
certified relocatable. Requested-version matching is exact; unversioned discovery
remains compatible and does not imply a stable ABI guarantee.

Optional real-provider packaging is now implemented and locally verified; see
`docs/BORINGSSL_PACKAGE.md` and `docs/PHASE8_RESULT.md`. This supersedes the phase-7
absolute-path packaging blocker for the explicitly verified bundled configuration.
Static cross-SDK provider composition is still unverified and not symbol-isolated.
