# Optional host composition smoke

This is an opt-in integration fixture, not a MadObs dependency, new facade or
supported universal SDK bundle. It requires separately prepared MadLink headers,
static libraries and a matching BoringSSL SSL archive. No third-party source or
binary is copied here. It never starts a network connection.

```sh
cmake -S samples/madlink_composition -B build-composition \
  -DCMAKE_PREFIX_PATH=/path/to/verified-provider-MadObs-install \
  -DMADLINK_SOURCE=/path/to/frozen/MadLink \
  -DMADLINK_ARCHIVE=/path/to/libimlink.a \
  -DMADLINK_CORE_ARCHIVE=/path/to/libimlink_core.a \
  -DBORINGSSL_SSL_ARCHIVE=/path/to/matching/libssl.a
cmake --build build-composition
ctest --test-dir build-composition --output-on-failure
```

Host selects sdk_id=2 for this isolated test, not a globally allocated ID. The
adapter's `delivered` counter means callback dispatch only: two dispatched events
produce one successful private admission and one policy rejection. A third event
is filtered before dispatch. The explicit private policy removes all dimensions
and exact event timestamps; raw ABI is not silently sanitized. Client lifecycle
finishes before the host callback context/runtime is destroyed.

The single crypto provider is the one exported by the MadObs package. Supply only
an SSL library from its exact upstream revision and a compatible toolchain/build.
This fixture does not verify arbitrary incoming archive provenance, prevent all
ABI mismatches, resolve multiple crypto providers or namespace upstream symbols.
See ../../docs/PHASE9_RESULT.md for the one tested configuration and hashes.
