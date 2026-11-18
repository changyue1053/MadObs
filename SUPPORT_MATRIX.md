# Support matrix

| Target | Status |
|---|---|
| Linux x86_64 GCC native | Local test target; see docs/PHASE3_RESULT.md for exact run |
| C / strict C++11 privacy headers, C++20 runtime | Local contract tests, with and without exceptions |
| BoringSSL encryption | Not tested; external dependency absent |
| Android / iOS / OHOS | Native static build entrypoints added; SDKs absent, no binary/device/package verification |
| Cross-SDK adapters | Not implemented in this work unit |
| No-runtime API-only | Supported local configuration |

No platform is GA. No model or other SDK is included.
