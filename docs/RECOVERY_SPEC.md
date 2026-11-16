# ZenthrilDB Recovery Specification

## Current scope

`RecoveryEngine` analyses the WAL into a `RecoveryPlan`. For bound databases, `RecoveryCoordinator` derives and validates the final file size and page images before applying changes. For legacy databases, `PageImageRecoveryApplier` uses the existing ordered REDO and UNDO path. A committed transaction contributes after-images. An incomplete transaction contributes eligible before-images. [Physical recovery and its limits](PHYSICAL_RECOVERY_RU.md)

`FileManager::openDatabase()` holds an exclusive process lock, validates the database header, opens the canonical sibling WAL (`database.zdb` -> `database.wal`), and runs `RecoveryCoordinator` before returning. Missing WAL or any validation/recovery error closes the database and fails the open. A lock sidecar (`database.zdb.lock`) is kept for the lifetime of the open `FileManager`; the lock file is deliberately not unlinked.

An empty WAL is accepted when persisted V2 page LSNs do not claim a record beyond LSN 0. A missing WAL is rejected. This preserves databases whose current page writes are not coupled to WAL, but does not detect loss of an empty WAL for pages written with LSN 0. Every caller using the same database must use the documented canonical WAL path; standalone WAL users may still pass arbitrary paths to `LogManager`.

Tests include separate processes forcibly terminated after durable page writes (both before COMMIT and after COMMIT), followed by recovery in a new process context and a second recovery pass. This verifies those selected process-crash barriers, not interruption inside OS write/sync calls or power-loss behavior.

## Pipeline

```text
WAL records -> RecoveryContext -> RecoveryEngine -> RecoveryPlan
             -> Bound recovery preflight -> FileManager
             -> Legacy UndoRedoRecoveryExecutor -> PageImageRecoveryApplier -> FileManager
```

## Guarantees and limitations

- WAL records and typed page image payloads are versioned and CRC protected.
- Page-image replay is idempotent against the page LSN for the tested cases.
- Durable lifecycle records use append plus `fsync`/`FlushFileBuffers`; page flush also requests OS sync. There is no general transactional page writer enforcing WAL-before-data for all mutations.
- Runtime allocation, free-list and metadata changes are covered by physical images written by StorageMutationCoordinator. Legacy logical allocation and page records are not sufficient to reconstruct a page.
- Transaction lifecycle order is validated; invalid start/finalize/page-image order fails closed. Boundaries do not provide complete ACID atomicity, isolation, MVCC, or rollback of arbitrary page mutations.
- Startup invokes the coordinator automatically. Recovery supports page images and header images.
- `StorageMutationCoordinator` uses the existing `LogManager` and `TransactionManager`. MetadataManager create/delete/save, and FreePageManager/PageManager allocation/write/free paths use page images, header images, WAL-before-data, durable commit, and fail the FileManager handle on mutation errors.
- The coordinator must be shared by managers for the same database. Direct `FileManager::writePage()` remains an unlogged low-level API and is not covered by transactional guarantees.
- `createDatabase()` refuses an existing database or pre-existing canonical WAL. It initializes the canonical WAL without truncating an existing database.
- Complete CRC failures fail closed; a short final WAL frame is truncated to the last valid frame. This is not general corruption repair.
- No claim of recovery after power loss, controller failure or media corruption is made.

## Next recovery work

Stage 4 adds a durable checkpoint boundary while retaining the full WAL. Recovery validates that boundary and continues to use older images. The next design step is verification of a baseline that could safely replace historical images. [Safe checkpoint](SAFE_CHECKPOINT_RU.md), [recovery roadmap](roadmap/STORAGE_RECOVERY_NEXT_RU.md)
