# ADR 0002: BinaryIO and Buffer Page Guards

## Status

Accepted

## Context

Several storage serializers wrote integral fields with direct memory copies.
Although `memcpy` avoids alignment undefined behavior, it still preserves native
byte order. That is not suitable for a database file format intended to remain
stable across platforms and future replication modes.

The buffer manager also exposed a manual `pin()` / `unpin()` lifecycle. Manual
pin management is easy to misuse when exceptions or early returns are added.

## Decision

- Introduce `storage/BinaryIO.hpp` as a shared helper for explicit little-endian
  integer reads and writes.
- Move storage header, metadata, free-list, WAL frame, and typed WAL payload
  serialization toward this shared helper.
- Add `BufferPageGuard`, a move-only RAII object that unpins its page in the
  destructor.
- Preserve the existing manual buffer API for compatibility, while recommending
  `pinGuard()` for new code.

## Consequences

Positive:

- Binary format behavior is explicit rather than dependent on native byte order.
- Serialization code is more consistent and easier to audit.
- Buffer pin lifecycle is safer in exception and early-return paths.
- Existing callers are not broken.

Tradeoffs:

- `BufferPageGuard` does not yet hold an exclusive page latch while the caller
  mutates the page. Higher-level concurrency control is still required.
- Some string and bulk byte copies still use raw byte views because they are not
  integer encoding concerns.

## Follow-Up

- Prefer `pinGuard()` in future storage code.
- Add page latch modes when Lock Manager and MVCC require coordinated page access.
- Continue moving future binary structures onto `BinaryIO` from the start.
