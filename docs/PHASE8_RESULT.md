# Eighth increment: optional real-provider distribution and opening OOM tests

Status: native optional provider and package verified locally; cross-platform/GA gates
remain. Existing public checkpoint and subsequent CI are recorded separately.

## Provider and package

- Official google/boringssl commit 533902fb2e8fd0586db7d58879d697721726605a;
  source tar SHA256 47383a2562506ffeb6b76d6f2a9ee906c57425ee7fb07e54292b27c52aa9d6b6.
  Offline verifier compares all 8,657 files and rejects extra files/links or wrong hash.
- Independently built Release PIC crypto archive; local artifact SHA256
  8a715430ae5ca6f471206132ebbc69f54d42d7f495ec05beb6e028ca875a78c0.
  Build commands: cmake source -> separate build with BUILD_TESTING=OFF,
  CMAKE_BUILD_TYPE=Release, CMAKE_POSITION_INDEPENDENT_CODE=ON; build target crypto.
- Explicit verified-source packaging copies a prefix-relative provider archive,
  upstream BoringSSL LICENSE and Fiat LICENSE/AUTHORS, source lock and binary identity
  evidence. Imported target checks archive hash and conflicts; no machine-local archive
  path leaks through installed link interfaces. Default remains null-provider.
- Explicit incomplete provider fails configure. PACKAGE_BORINGSSL=ON without provider
  or with runtime OFF fails configure. Provider installation without verified packaging
  fails; no silent installation of an absolute-path dependency remains.
- Negative tests reject wrong source hash, archive tampering, and ignored packaging
  intent. Local bwrap consumer has original source/build/workspace paths absent and
  successfully links/runs encrypted logs from relocated installed files.

Source verification does NOT prove an externally supplied binary was compiled from it.
Binary SHA is integrity, not compiler provenance. Actual build/test logs are observation,
not a signed/reproducible-build attestation, full SBOM, vulnerability audit or FIPS proof.
This pinned revision is not represented as the latest/security-approved upstream release.

## Privacy/runtime and tests

- Added first-six-new-allocation failure injection around private Open; each failure
  remains inside the C ABI with INTERNAL diagnostic, no returned handle, and successful
  same-policy retry. Not every allocation/native startup point is covered.
- Real provider exercises Ed25519 tamper/downgrade and AES-GCM authenticated encrypted
  log paths, with an explicit Available-required test so null fallback cannot fake pass.
- Default/null-provider matrix and API-only compatibility remain independently tested.
  ASan leak checking is still disabled for the pre-existing ptrace limitation.

Final verified matrix: default native 42/42, default Release 42/42, ASan+UBSan
41/41 (leaks disabled), TSan 41/41, API-only 6/6, real-provider Release 43/43 PASS.
Fresh default offline package verification and isolated real-provider encrypted C
consumer both pass. Installed BoringSSL/Fiat license copies match upstream bytes.
Exact logs appear in docs/verification/phase8-*.log. CI now includes a separately
pinned-source real-provider job; a configured workflow is not proof its remote run passed.

## Composition and remaining gates

Shared export audit hides provider symbols. Static BoringSSL retains generic upstream
symbols; linking another OpenSSL/BSSL version via a different SDK can conflict or resolve
incorrectly. No arbitrary cross-SDK static composition or namespace isolation is claimed.
The same-name imported target guard is not a complete link-time composition analysis.

Public first-party licensing, mobile SDK/package/device tests, physical disk/power failure,
LSan, exhaustive OOM, full SBOM/security/vulnerability review and final ABI compatibility
remain release gates. BoringSSL is external at source publication; no compiled archive
or third-party source tree is committed. Binary install distributions carry license files.
