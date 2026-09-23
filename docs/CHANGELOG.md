# ZenthrilDB Changelog

All notable project changes are recorded here. The project follows an
incremental internal versioning model during MVP development.

## Unreleased - Architecture Hardening

### Added

- ADR for WAL binary hardening and concurrency safeguards.
- `BinaryIO` helper for explicit little-endian binary read/write operations.
- `BufferPageGuard` RAII guard for safer buffer pin/unpin lifecycle.
- Tests for explicit WAL little-endian frame layout.
- Tests for oversized WAL payload declarations.
- Tests for invalid typed WAL enum values.
- Tests for BinaryIO boundary behavior.
- Tests for automatic buffer unpin through RAII guard.

### Changed

- WAL record frames now use explicit little-endian serialization instead of native byte layout.
- Storage headers, metadata, page directory, free page list, WAL records, and typed WAL payloads now share explicit little-endian helpers.
- WAL readers reject payload sizes larger than `64 MiB` before allocation.
- `BufferManager` operations are now protected by a mutex.
- `TransactionManager::begin()` now rolls back in-memory registration if WAL begin append fails.
- Typed WAL payload readers now reject unknown `PageType` and `MetadataPayloadKind` values.

## `v0.9` - Lock Manager Foundation

### Added

- `LockMode` with shared and exclusive modes.
- `LockResourceId` for database, metadata, table, and page resources.
- `LockManager` with try-acquire, timed acquire, release, release-all, and lock inspection.
- Move-only `LockGuard` for RAII lock release.
- Unit tests for shared compatibility, exclusive conflicts, upgrades, timeouts, RAII release, and concurrent shared acquisition.

## `v0.8` - Typed WAL Payload

### Added

- `LogPayloadCodec` for versioned WAL payload encoding and decoding.
- `PageAllocationPayload`.
- `PageFreePayload`.
- `PageWrittenPayload`.
- `MetadataUpdatedPayload`.
- `TransactionPayload`.
- Payload validation for unsupported versions, truncation, trailing bytes, invalid page identifiers, and invalid transaction identifiers.
- Unit tests for payload round-trip serialization, corrupted payloads, WAL integration, and concurrent decode scenarios.

### Changed

- `TransactionLogAdapter` now writes transaction lifecycle records using the typed transaction payload format.
- Transaction tests now decode payloads through `LogPayloadCodec` instead of assuming raw 8-byte transaction identifiers.
- Architecture documentation now describes typed WAL payload contracts.

## `v0.7` - Undo/Redo Recovery Foundation

### Added

- `RedoApplier` interface.
- `UndoApplier` interface.
- `UndoRedoRecoveryExecutor`.
- `RecoveryExecutionResult`.
- `RecoveryActionType::UndoCandidate`.
- Idempotency protection by LSN.
- Mutex protection for concurrent recovery execution.
- Unit tests for redo, undo, skipped actions, repeated execution, and concurrent recovery execution.

## `v0.6` - Transaction Foundation

### Added

- `TransactionId`.
- `Transaction`.
- `TransactionContext`.
- `TransactionLogAdapter`.
- `TransactionManager`.
- Transaction lifecycle WAL records:
  - `TransactionStarted`
  - `TransactionCommitted`
  - `TransactionRolledBack`
- Thread-safe transaction registry.
- Unit tests for begin, commit, rollback, repeated commit rejection, WAL lifecycle records, concurrent transaction creation, and concurrent transaction finalization.

## `v0.5` - Recovery Foundation

### Added

- `RecoveryContext`.
- `RecoveryEngine`.
- `RecoveryPlan`.
- `CheckpointService`.
- Recovery action classification.
- Checkpoint boundary analysis.
- Unit tests for recovery analysis and checkpoint handling.

## `v0.4` - WAL Foundation

### Added

- Independent `.wal` file format.
- `LogSequenceNumber`.
- `LogRecord`.
- `WriteAheadLog`.
- `LogManager`.
- WAL record CRC validation.
- WAL append, replay, flush, truncate, and checkpoint support.
- Unit tests for serialization, CRC validation, corrupted WAL detection, large WAL, concurrent writes, and checkpoint creation.

## `v0.3` - Page Lifecycle

### Added

- `PageManager`.
- `FreePageManager`.
- `PageDirectory`.
- Reserved-page enforcement.
- Dirty page tracking.
- Free page reuse.
- Unit tests for page allocation, page reuse, invalid pages, thread safety, and storage boundaries.

## `v0.2` - Metadata Foundation

### Added

- `MetadataManager`.
- `DatabaseMetadata`.
- `TableMetadata`.
- Versioned metadata serialization.
- Durable table registry.
- Table create, delete, rename, load, save, and update operations.
- Unit tests for metadata serialization, recovery, duplicate detection, overflow protection, and corrupted metadata detection.

## `v0.1` - Storage Foundation

### Added

- Custom `.zdb` file format.
- `DatabaseHeader`.
- `PageHeader`.
- `Page`.
- `FileManager`.
- `BufferManager`.
- `CRCManager`.
- Fixed 8 KB page architecture.
- LRU page cache.
- Future encryption interfaces.
- Initial unit testing infrastructure.
