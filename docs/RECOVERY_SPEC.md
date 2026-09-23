# ZenthrilDB Recovery Specification

## Scope

This document defines the current recovery architecture for ZenthrilDB. The
implemented recovery system is a foundation for future crash recovery. It
analyzes WAL records, creates a recovery plan, and provides an idempotent
execution scaffold.

The current implementation does not yet perform full page redo, page undo, MVCC
visibility restoration, or committed transaction reconstruction.

## Recovery Pipeline

```text
WAL
  -> RecoveryContext
  -> RecoveryEngine
  -> RecoveryPlan
  -> UndoRedoRecoveryExecutor
  -> RedoApplier / UndoApplier
```

## Components

### RecoveryContext

`RecoveryContext` stores validated `LogRecord` instances prepared by a
`RecoveryManager`.

Responsibilities:

- hold replayed WAL records
- provide immutable access to recovery analysis
- keep recovery input separate from WAL file IO

### RecoveryEngine

`RecoveryEngine` analyzes `RecoveryContext`.

Responsibilities:

- locate the latest checkpoint
- ignore records before the checkpoint
- classify records into recovery actions
- produce a `RecoveryPlan`

### RecoveryPlan

`RecoveryPlan` is a deterministic list of `RecoveryAction` entries.

Each action contains:

- `LSN`
- `LogRecordType`
- `RecoveryActionType`

The plan is intentionally separated from execution. This allows future tooling to
inspect, test, simulate, or audit recovery before applying changes.

### UndoRedoRecoveryExecutor

`UndoRedoRecoveryExecutor` consumes a `RecoveryPlan` and delegates actual work to
`RedoApplier` and `UndoApplier`.

Current guarantees:

- mutex-protected execution
- idempotency by LSN
- safe skipping of non-executable recovery actions
- explicit execution result counters

## Recovery Action Types

| Action Type | Meaning |
|---|---|
| `RedoCandidate` | should be passed to redo logic |
| `UndoCandidate` | should be passed to undo logic |
| `MetadataRefresh` | should be passed to redo-style metadata refresh logic |
| `CheckpointBoundary` | recovery boundary, not directly applied |
| `RecoveryMarker` | diagnostic marker, not directly applied |
| `TransactionBoundary` | transaction state boundary, not directly applied yet |
| `Ignore` | ignored by recovery execution |

## Current Classification Rules

| WAL Record Type | Recovery Action |
|---|---|
| `PageAllocated` | `RedoCandidate` |
| `PageFreed` | `RedoCandidate` |
| `PageWritten` | `RedoCandidate` |
| `MetadataUpdated` | `MetadataRefresh` |
| `Checkpoint` | `CheckpointBoundary` |
| `RecoveryMarker` | `RecoveryMarker` |
| `TransactionStarted` | `TransactionBoundary` |
| `TransactionCommitted` | `TransactionBoundary` |
| `TransactionRolledBack` | `TransactionBoundary` |
| `DatabaseCreated` | `Ignore` |
| `DatabaseOpened` | `Ignore` |
| `Unknown` | `Ignore` |

## Idempotency

The executor records applied LSN values in memory. If a plan is executed more
than once through the same executor, already-applied LSNs are skipped.

This protects against:

- retry loops
- accidental duplicate recovery execution
- concurrent execution attempts
- race-condition windows that could otherwise double-apply page changes

The idempotency state can be cleared with `resetIdempotencyState()` for test and
controlled lifecycle scenarios.

## Failure Handling

The current recovery foundation fails closed:

- invalid WAL CRC causes replay failure
- unsupported WAL versions cause replay failure
- unsupported payload versions cause payload decoding failure
- malformed payloads must not be silently interpreted

Future recovery executors should preserve the same behavior.

## Future Crash Recovery Strategy

The future crash recovery algorithm should:

1. Replay and validate WAL frames.
2. Build a transaction table from transaction boundary records.
3. Identify committed and uncommitted transaction effects.
4. Redo committed page and metadata operations.
5. Undo uncommitted operations when undo payloads exist.
6. Write a recovery marker after successful recovery.
7. Create or advance a checkpoint once the storage state is durable.

## Current Limitations

- No transaction table reconstruction yet.
- No page before-image or after-image application yet.
- No MVCC cleanup or version pruning.
- No dirty page table.
- No durable recovery marker policy.
- No crash simulation harness.

These limitations are intentional. The current phase provides safe interfaces and
execution structure before adding complex recovery semantics.
