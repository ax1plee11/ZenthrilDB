# ZenthrilDB WAL Specification

## Scope

This document defines the Write-Ahead Log format used by ZenthrilDB. The WAL is
an independent binary file stored separately from the `.zdb` database file. Its
purpose is to provide a durable event stream for future crash recovery,
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

## Current Limitations

- WAL contains physical/logical event metadata, not full before-images or after-images.
- Full crash recovery is not yet implemented.
- WAL truncation exists as an infrastructure primitive and must be used carefully until checkpoint durability rules are finalized.
- Existing `.wal` files produced before explicit little-endian WAL frame encoding are not guaranteed to be portable across architectures.
