# ZenthrilDB WAL Specification

Stage 4 defines an additional bound-WAL record type 14. The header, CRC, and type 7 remain unchanged. [SafeCheckpoint payload and validation](SAFE_CHECKPOINT_RU.md).

## Scope

This document defines the Write-Ahead Log format used by ZenthrilDB. The WAL is
an independent binary file stored separately from the `.zdb` database file. Its
purpose is to provide a checksummed event stream for future crash recovery,
transaction recovery, replication, cloud shipping, and audit-oriented inspection.

This specification describes infrastructure only. It does not claim that full
transactional crash recovery is complete.

## File Naming

For a database file:

```text
database.zdb
```

The WAL file is expected to be:

```text
database.wal
```

The current implementation accepts an explicit WAL path through `LogManager`.

## Byte Order

The WAL record frame and all typed WAL payloads are encoded in little-endian
order. Readers must not rely on native machine byte order.

## WAL Record Frame

Each WAL record contains a fixed header followed by a payload.

| Offset | Field | Type | Description |
|---:|---|---|---|
| `0` | `magic` | `uint32` | WAL record magic, currently `ZWAL` |
| `4` | `format_version` | `uint32` | WAL frame version, currently `1` |
| `8` | `record_type` | `uint32` | logical WAL record type |
| `12` | `log_sequence_number` | `uint64` | monotonic LSN |
| `20` | `timestamp_ns` | `uint64` | wall-clock timestamp in nanoseconds |
| `28` | `payload_size` | `uint32` | payload byte length |
| `32` | `crc32` | `uint32` | CRC32 of the serialized record with this field set to zero |
| `36` | `payload` | `byte[]` | typed payload bytes |

Header size is `36` bytes.

The maximum accepted payload size is currently `64 MiB`. Readers must reject
larger payload sizes before allocating payload buffers.

## Record Types

| Value | Name | Current Recovery Meaning |
|---:|---|---|
| `0` | `Unknown` | ignored |
| `1` | `DatabaseCreated` | database lifecycle event |
| `2` | `DatabaseOpened` | database lifecycle event |
| `3` | `PageAllocated` | redo candidate |
| `4` | `PageFreed` | redo candidate |
| `5` | `PageWritten` | redo candidate |
| `6` | `MetadataUpdated` | metadata refresh candidate |
| `7` | `Checkpoint` | recovery boundary |
| `8` | `RecoveryMarker` | diagnostic marker |
| `9` | `TransactionStarted` | transaction boundary |
| `10` | `TransactionCommitted` | transaction boundary |
| `11` | `TransactionRolledBack` | transaction boundary |
| `12` | `PageImageWritten` | full before/after page image for physical recovery |
| `13` | `DatabaseHeaderImageWritten` | transactional before/after database header image |

## Typed Payload Version

Every typed payload begins with:

| Offset | Field | Type |
|---:|---|---|
| `0` | `payload_format_version` | `uint32` |

Current payload version is `1`.

Readers must reject:

- unsupported payload versions
- truncated payloads
- trailing bytes
- invalid page identifiers
- invalid transaction identifiers
- unknown enum values

## PageAllocated Payload

Used by `PageAllocated`.

| Field | Type | Description |
|---|---|---|
| `payload_format_version` | `uint32` | payload version |
| `page_id` | `uint32` | allocated page identifier |
| `page_type` | `uint32` | page type |
| `owner_table_id` | `uint64` | owning table identifier, `0` if system or unknown |

## PageFreed Payload

Used by `PageFreed`.

| Field | Type | Description |
|---|---|---|
| `payload_format_version` | `uint32` | payload version |
| `page_id` | `uint32` | freed page identifier |
| `owner_table_id` | `uint64` | former owning table identifier, `0` if unknown |

## PageWritten Payload

Used by `PageWritten`.

| Field | Type | Description |
|---|---|---|
| `payload_format_version` | `uint32` | payload version |
| `page_id` | `uint32` | written page identifier |
| `page_type` | `uint32` | written page type |
| `page_checksum` | `uint32` | page checksum after write |
| `used_bytes` | `uint32` | used bytes reported by the page header |
| `page_version` | `uint32` | page header version |
| `owner_table_id` | `uint64` | owning table identifier, `0` if system or unknown |

## MetadataUpdated Payload

Used by `MetadataUpdated`.

| Field | Type | Description |
|---|---|---|
| `payload_format_version` | `uint32` | payload version |
| `metadata_kind` | `uint32` | metadata payload kind |
| `page_id` | `uint32` | page where metadata is persisted |
| `metadata_version` | `uint32` | metadata structure version |
| `payload_checksum` | `uint32` | checksum of the metadata payload |

Known metadata kinds:

| Value | Name |
|---:|---|
| `1` | `DatabaseMetadata` |
| `2` | `PageDirectory` |
| `3` | `FreePageList` |
| `4` | `TableRegistry` |

## PageImageWritten Payload

Used by `PageImageWritten` for transactional physical recovery. It contains the
transaction ID, page ID, previous transaction LSN, page-format version, and
two complete 8192-byte, CRC-protected page images: the state before and after
the change. During recovery, committed transactions redo the after-image and
incomplete or explicitly rolled-back transactions undo the before-image in
reverse LSN order. The applier verifies the image Page LSN against the WAL record LSN.

## DatabaseHeaderImageWritten Payload

This version 1 payload stores transaction ID, previous transaction LSN, and
complete serialized before/after images of the database file header prefix.
Both images must pass the database header checksum and format checks. Recovery
uses the transaction outcome to restore the selected image; when UNDO reduces
`totalPages`, it truncates the physical file to the restored page count. This
record type is used by `StorageMutationCoordinator` when a page-set mutation
changes `totalPages`.

## Transaction Payload

Used by:

- `TransactionStarted`
- `TransactionCommitted`
- `TransactionRolledBack`

| Field | Type | Description |
|---|---|---|
| `payload_format_version` | `uint32` | payload version |
| `transaction_id` | `uint64` | local transaction identifier |

## Integrity Rules

- WAL frames are protected with CRC32.
- Payloads are protected indirectly through the enclosing WAL frame CRC.
- A corrupted frame must stop replay and surface an error.
- Recovery must not silently ignore CRC failures.
- WAL readers reject oversized payload declarations before allocating memory.

## Compatibility Rules

- New payload fields must be appended.
- Existing field offsets must not change within the same payload version.
- Unsupported payload versions must fail closed.
- Future encrypted WAL support should wrap payload bytes or full records without changing logical record definitions.

## Durability and Current Limitations

- `WriteAheadLog::append()` appends a framed record and checks stream errors, but does not force it to storage.
- `LogManager::appendWithLsn()` performs append followed by `WriteAheadLog::flush()`. That flushes the stream and calls `FlushFileBuffers` on Windows or `fsync` on POSIX; errors propagate. A sync failure poisons that `LogManager` instance so later appends fail until reopen/recovery.
- Transaction `begin`, `commit`, and `rollback`, and checkpoint creation use the durable append path. A successful COMMIT confirms the commit record was synced by the OS interface before returning. `StorageMutationCoordinator` provides WAL-before-data page/header image transactions for managers wired to the same LogManager and TransactionManager. Direct low-level FileManager writes remain outside that guarantee.
- Page `FileManager::flush()` now flushes the stream and requests the same OS file sync. A single page write can still tear; CRC detects damage but cannot restore data without a valid page-image WAL record.
- On startup, only an incomplete final frame (short header or short payload at EOF) is truncated to the last complete frame and synced. A complete frame with invalid magic/version/CRC or an oversized payload fails closed. Mid-file corruption is not repaired.
- WAL compaction writes and syncs a sibling temporary file, atomically replaces the WAL, then syncs the parent directory on POSIX (Windows uses `MOVEFILE_WRITE_THROUGH`). Transaction lifecycle records are retained to preserve the Transaction ID high-water mark. Full checkpoint-based WAL reclamation is not implemented.
- `FileManager::openDatabase()` now holds an exclusive process lock and automatically runs `RecoveryCoordinator` before returning. The coordinator validates page LSNs against the canonical sibling WAL, replays page-image actions, then syncs recovered pages and WAL. Missing WAL fails closed. An empty WAL is accepted only when no persisted page LSN claims a positive LSN; this does not prove that pages written without WAL are recoverable.
- При открытии проверяются сигнатура, версия, размер страницы и границы числа страниц заголовка базы. UUID обоих исторических образов заголовка сверяются с UUID базы. У связанных WAL префикс проверяет UUID, поколение и LSN снимка до REDO/UNDO и обрезки хвоста, независимо от типов кадров. Обычные кадры сохраняют описание из этого документа. [Формат префикса и совместимость](WAL_IDENTITY_RU.md)
- Allocation/free/metadata transactions have no full physical before/after recovery path. There is no general transaction/page/lock integration or full ACID guarantee. A successful COMMIT proves durability of the commit record, not arbitrary data-page updates.
- Process-kill tests cover durable COMMIT, page-image REDO/UNDO, and a second recovery pass. They do not kill the process inside kernel writes/sync calls, simulate power loss, or prove behavior of every filesystem/controller. Hardware power-loss durability is not claimed.
