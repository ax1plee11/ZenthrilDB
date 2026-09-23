# ZenthrilDB Transaction Specification

## Scope

This document defines the current transaction foundation in ZenthrilDB. The
implemented layer manages transaction lifecycle state and writes durable
transaction boundary records to WAL.

The current transaction layer does not implement:

- MVCC
- locks
- isolation levels
- SQL transactions
- savepoints
- distributed transactions
- undo/redo application

## Transaction Identity

Transactions are identified by `TransactionId`.

Properties:

- `uint64` local identifier
- `0` is invalid
- identifiers are currently monotonic within a `TransactionManager` instance

Future versions may persist the next transaction identifier or derive it from WAL
recovery.

## Transaction State

Current states:

| State | Meaning |
|---|---|
| `Active` | transaction is open |
| `Committed` | transaction completed successfully |
| `RolledBack` | transaction was rolled back |

Valid transitions:

```text
Active -> Committed
Active -> RolledBack
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

The transaction boundary is appended to WAL during lifecycle operations.

The current implementation does not yet define a full force/no-force or
steal/no-steal buffer policy. Therefore, transaction records are durable events,
but they do not yet provide complete ACID transaction semantics.

Future transaction durability must define:

- when WAL is flushed relative to page writes
- how commit records become durable
- whether commit waits for WAL flush
- how rollback interacts with undo payloads
- how recovery reconstructs active transaction state after crash

## Race-Condition Requirements

The transaction layer must prevent:

- double commit
- rollback after commit
- commit after rollback
- concurrent terminal transitions
- duplicate transaction identifiers

Existing tests cover concurrent transaction creation and concurrent finalization.

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

- Transaction identifiers are not persisted across process restart.
- No transaction table recovery exists yet.
- No isolation enforcement exists yet.
- No conflict detection exists yet.
- No deadlock detection exists yet. The current lock foundation uses timeout-based waiting.
- No SQL-level transaction API exists yet.
