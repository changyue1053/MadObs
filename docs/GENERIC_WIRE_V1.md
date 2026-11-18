# Generic record G1 (opt-in, experimental)

A new record type; existing record tags and batch/WAL framing are unchanged.
Never reinterpret a native C struct as serialized bytes.

`'G' u8 | version=1 u8 | sdk_id u32-varint | sdk_version u32-varint |
event_id u32-varint | severity u32-varint | monotonic_ns fixed64 little-endian |
attribute_count u32-varint | {key u32-varint, enum_value u32-varint} * count`

Maximum eight attributes; severity 0..5; unknown version and trailing bytes reject.
Schema IDs and enum domains are Host-governed. No free-text, raw path, credential,
message content or prompt fields exist. Arbitrary numeric IDs are not automatically
privacy-safe. The optional privacy allowlist helper and owning PrivateRuntime
now provide explicit schema minimization; raw generic emit/legacy logging do not
automatically apply that policy. See `PRIVACY_SCHEMA.md` and `PRIVATE_RUNTIME.md`.

The producer queue normalizes struct size/version and clears inactive attributes.
Encoding and WAL append run only on the existing single consumer thread. The
host-exported batch retains legacy priority, compression, ID and acknowledgement
semantics. Default Noop never starts this path.

The bundled legacy thin server is not advertised as a generic query backend.
Hosts opting into G1 must decode/support the record or explicitly reject it; do not
silently claim compatibility with pre-G1 server deployments.


The raw G1 decoder rejects integers exceeding five bytes/u32 range, but preserves
compatibility by accepting some non-minimal varint encodings. Its successful
result means structurally decoded, not canonical or policy-approved. The private
export gate separately re-encodes and compares the full envelope, rejecting
noncanonical historical wire representations. Do not substitute a raw decode
success for private policy/export validation.
