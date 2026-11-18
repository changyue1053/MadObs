# G0 preliminary review (not release approval)

- Imported source pinned and all 86 original file blobs verified against GitHub SHA.
- No LICENSE/NOTICE in original tree: license selection and ownership audit remain BLOCKED.
- No vendored third-party source or model files copied. Native dependency is system zlib;
  crypto provider BoringSSL is optional, explicitly injected, not downloaded by the build.
- A narrow pattern scan covers private-key blocks and common service-token formats.
  It is not an exhaustive secret or dependency vulnerability scan.
- Historical well-known control key removed as default and rejected at envelope entry.
- Tests deliberately contain fixed public fixture data; these are not production credentials.
- Raw legacy logs are not privacy-certified. New API only allows bounded numeric schema
  fields; hosts must enforce their enum schema and consent requirements.
- Memory-safety test limitations and unpublished status are recorded in TASK_RESULT.md.
