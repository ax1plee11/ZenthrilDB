# ZenthrilDB Transaction Specification

## Scope

This document defines the current transaction foundation in ZenthrilDB. The
implemented layer manages in-memory transaction lifecycle state and appends
transaction boundary records to WAL. Appending does not itself request an OS
durable flush.

The current transaction layer does not implement:

- MVCC
- lock acquisition/release integration
- isolation levels
- SQL transactions
- savepoints
- distributed transactions
- atomicity for writes that bypass `StorageMutationCoordinator` or span managers that were constructed with different LogManager/TransactionManager instances

## Transaction Identity

Transactions are identified by `TransactionId`.

Properties:

- `uint64` local identifier
- `0` is invalid
- identifiers are currently monotonic within a `TransactionManager` instance

The next identifier is reconstructed from retained lifecycle WAL records after restart. WAL compaction retains these records. There is no independent transaction catalog.

## Transaction State

Current states:

| State | Meaning |
|---|---|
| `Active` | transaction is open |
| `Committed` | durable commit record was synced before return |
| `RolledBack` | durable rollback record was synced before return |
| `CommitInDoubt` | commit record append/sync failed; resolve after reopen/recovery |
| `RollbackInDoubt` | rollback record append/sync failed; resolve after reopen/recovery |

Valid transitions:

```text
Active -> Committed
Active -> RolledBack
Active -> CommitInDoubt
Active -> RollbackInDoubt
```

Invalid transitions:

- `Committed -> Committed`
- `Committed -> RolledBack`
- `RolledBack -> Committed`
- `RolledBack -> RolledBack`
- any transition for an unknown transaction id

Invalid transitions throw an exception.

## TransactionManager

`TransactionManager` is the public lifecycle coordinator.

Supported operations:

- `begin()`
- `commit(TransactionId)`
- `rollback(TransactionId)`
- `getTransaction(TransactionId)`
- `isActive(TransactionId)`
- `activeCount()`
- `activeTransactions()`

Concurrency model:

- all lifecycle operations are protected by a mutex
- transaction identifiers are assigned while holding the mutex
- state transitions are validated and applied while holding the mutex
- concurrent finalization of the same transaction permits exactly one terminal transition

## WAL Integration

`TransactionLogAdapter` records lifecycle events through `LogManager`.

Transaction lifecycle records:

| Operation | WAL Record Type |
|---|---|
| `begin()` | `TransactionStarted` |
| `commit()` | `TransactionCommitted` |
| `rollback()` | `TransactionRolledBack` |

Transaction payload format is defined in `docs/WAL_SPEC.md`.

## Durability Notes

`begin`, `commit`, and `rollback` append their lifecycle record through
`LogManager::appendWithLsn()`, which syncs the WAL before returning. If commit or
rollback synchronization fails after append, the corresponding in-doubt state is
reported and the `LogManager` rejects further writes until it is reopened.
`TransactionId` is reconstructed from retained lifecycle records; WAL compaction
keeps these records.

This is durability of lifecycle records only. Page modifications are not
transactionally coupled to `TransactionManager`, arbitrary rollback is absent,
and full ACID semantics are not implemented.

Remaining transaction work is to couple every supported page mutation to WAL, define page-level locking and recovery, and ensure lock cleanup on commit/rollback. Current commit durability applies to the lifecycle record only.

## Race-Condition Requirements

The transaction layer must prevent:

- double commit
- rollback after commit
- commit after rollback
- concurrent terminal transitions
- duplicate transaction identifiers

Tests cover concurrent transaction creation/finalization, durable lifecycle records, restart ID progression, and process termination after a successful COMMIT.

## Future MVCC Integration

Future MVCC work should extend transactions with:

- begin timestamp or transaction sequence number
- commit timestamp
- snapshot visibility set
- row version ownership
- active transaction table recovery
- garbage collection rules

These fields should be added through explicit versioned structures rather than
implicit changes to existing binary payloads.

## Future Lock Manager Integration

The Lock Manager depends on `TransactionId` and does not own transaction
lifecycle state.

Expected relationship:

```text
TransactionManager
  -> owns transaction lifecycle

LockManager
  -> coordinates local access for transaction ids

RecoveryEngine
  -> reconstructs transaction boundaries from WAL
```

## Current Limitations

- Transaction identifiers are reconstructed from WAL lifecycle records; WAL compaction retains those records. No independent transaction catalog exists.
- No transaction table recovery exists yet.
- No isolation enforcement exists yet.
- No conflict detection exists yet.
- `LockManager` has local wait-for graph detection and timeout-based waiting, but it is not wired into `TransactionManager`; no automatic lock release on transaction completion is provided.
- No SQL-level transaction API exists yet.
