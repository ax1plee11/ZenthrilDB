# ADR 0001: WAL Binary Hardening and Concurrency Safeguards

## Status

Accepted

## Context

The WAL foundation originally validated record CRCs and payload sizes, but the
record frame used native in-memory byte layout. That creates portability risk for
future replication, cross-platform WAL shipping, and long-term file
compatibility. The WAL reader also trusted the payload size field enough to
allocate a vector before applying an explicit upper bound.

The audit also identified that `BufferManager` managed shared cache structures
without its own mutex, and that transaction begin could append a WAL start record
before the transaction was successfully registered in memory.

## Decision

- Encode WAL record frame fields explicitly in little-endian order.
- Enforce a maximum WAL payload size of `64 MiB`.
- Reject oversized WAL payload declarations before allocation.
- Reject unknown typed WAL payload enum values.
- Add mutex protection to `BufferManager` public operations.
- Make `TransactionManager::begin()` roll back in-memory registration if WAL append fails.

## Consequences

Positive:

- WAL is safer for future cross-platform replay and replication.
- Corrupted WAL files cannot trigger uncontrolled payload allocation.
- Typed payload decoding fails closed on unknown enum values.
- Buffer cache metadata is protected from concurrent mutation.
- Transaction begin has stronger exception safety.

Tradeoffs:

- WAL frame encoding is now stricter. Pre-hardening WAL files that depended on
  native byte order are not considered portable.
- BufferManager returns page references after unlocking. Callers must still avoid
  sharing the same mutable page reference across threads without higher-level
  coordination. A future RAII page guard should address this more completely.

## Follow-Up

- Introduce a reusable binary little-endian utility for all storage formats.
- Add RAII page guards for BufferManager pin/unpin lifecycle.
- Define WAL compatibility migration policy before external releases.
