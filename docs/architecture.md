# ZenthrilDB Storage Engine Architecture

## MVP Scope

The first version focuses on local storage only.

## On-Disk Layout

Every `.zdb` file begins with a `DatabaseHeader` prefix followed by fixed-size pages.
This preserves the original ZenthrilDB file format while reserving page slots for
future metadata evolution.

### Reserved Pages

- `PAGE 0`: header mirror / future compatibility slot
- `PAGE 1`: `DatabaseMetadata`
- `PAGE 2`: `PageDirectory`
- `PAGE 3`: `FreePageList`
- `PAGE 4+`: user data

### Database Header Fields

- Magic number
- Database version
- Page size
- Total pages
- Creation timestamp
- Database UUID
- Checksum

### Page Layout

Each page is `8192` bytes and begins with a `PageHeader`.

Supported page types:

- Metadata page
- Table page
- Index page
- Free page
- Overflow page

## Design Notes

- All checksum verification uses CRC32.
- The file manager is thread-safe.
- Encryption is not implemented yet, but interfaces exist for future work.
- Metadata and page-directory data are versioned and persisted independently from the database header.
- Free pages are reused before the file grows.

## Metadata Lifecycle

`MetadataManager` owns the in-memory table registry and persists it to `PAGE 1`.
It supports table creation, deletion, renaming, metadata updates, metadata recovery
after reopening a database, and synchronized updates to `PageDirectory`.

## Page Lifecycle

`PageManager` controls user-page lifecycle beginning at `PAGE 4`. It allocates
pages through `FreePageManager`, records local ownership metadata, rejects direct
writes to reserved pages, validates pages through `FileManager`, and tracks dirty
page identifiers until `flushDirtyPages()` is called.

Freed pages are rewritten as `Free` pages before their identifiers return to the
free-page list. CRC generation and verification remain centralized in
`FileManager`, keeping page lifecycle logic separate from binary integrity logic.

`BufferManager` supports both the original manual `pin()` / `unpin()` API and a
RAII `BufferPageGuard` returned by `pinGuard()`. New code should prefer the
guard so pages are automatically unpinned when control leaves scope.

## Future Compatibility

Metadata records carry explicit versions. New fields should be appended to the
serialized structures with version-aware readers so existing `.zdb` files remain
readable.

## Binary Format Versioning

All internal metadata payloads start with a 32-bit little-endian format version.
Current format versions are defined in `storage/BinaryFormat.hpp`.

Primitive binary fields are read and written through `storage/BinaryIO.hpp`.
This keeps on-disk integer encoding explicit and avoids native-endian layout
dependencies in stable database and WAL formats.

### DatabaseMetadata Payload

- offset `0`: `uint32 format_version`
- offset `4`: `uint64 next_table_id`
- offset `12`: `uint32 table_count`
- offset `16`: fixed-size `TableMetadata` records

The reader rejects unsupported format versions, duplicate table names, duplicate
table identifiers, and table counts that exceed the metadata page capacity.

### TableMetadata Record

- offset `0`: `uint32 format_version`
- offset `4`: `uint64 table_id`
- offset `12`: `uint32 root_page_id`
- offset `16`: `uint64 creation_timestamp_ns`
- offset `24`: `uint64 record_count`
- offset `32`: `uint64 page_count`
- offset `40`: `uint32 table_version`
- offset `44`: `uint32 name_size`
- offset `48`: table name bytes, maximum 64 bytes

Each record is currently 128 bytes. Future fields must be appended without
changing existing offsets.

### PageDirectory Payload

- offset `0`: `uint32 format_version`
- offset `4`: `uint32 entry_count`
- offset `8`: fixed-size `PageDirectoryEntry` records

The reader rejects unsupported versions, duplicate page identifiers, and entry
counts that exceed page capacity.

### FreePageList Payload

- offset `0`: `uint32 format_version`
- offset `4`: `uint32 free_page_count`
- offset `8`: `uint32 page_id[]`

The reader rejects reserved page identifiers, duplicate free pages, unsupported
versions, and counts that exceed page capacity.

## Write-Ahead Log Foundation

ZenthrilDB stores WAL data in a dedicated `.wal` file next to the `.zdb` database
file. The WAL is intentionally independent from the storage file so future
transaction recovery, replication, cloud shipping, and encryption can evolve
without changing the database page format.

This phase provides infrastructure only. It does not implement transactional
commit/rollback semantics yet.

### WAL Record Layout

Each WAL record is self-contained and CRC protected:

- offset `0`: `uint32 magic`, currently `ZWAL`
- offset `4`: `uint32 format_version`
- offset `8`: `uint32 record_type`
- offset `12`: `uint64 log_sequence_number`
- offset `20`: `uint64 timestamp_ns`
- offset `28`: `uint32 payload_size`
- offset `32`: `uint32 crc32`
- offset `36`: payload bytes

The CRC is calculated over the serialized record with the CRC field set to zero.
Readers reject invalid magic values, unsupported versions, partial records, wrong
payload sizes, and CRC mismatches.

### WAL Components

- `LogSequenceNumber`: monotonic WAL identifier.
- `LogRecord`: versioned, CRC-protected serialized event.
- `WriteAheadLog`: append/read/flush/truncate file primitive.
- `LogManager`: thread-safe API for append, replay, checkpoint, and recovery context preparation.
- `RecoveryContext`: container for future recovery planning.
- `RecoveryManager`, `CrashRecovery`, `CheckpointManager`: extension interfaces for future transaction and crash recovery work.

### Recovery Strategy

Future recovery will replay WAL records in LSN order after validating CRC and
record versions. Checkpoint records provide safe recovery boundaries. Page and
metadata records already carry typed payloads, but payload interpretation remains
reserved for the future transaction layer.

## Recovery Engine Foundation

`RecoveryEngine` currently performs recovery analysis only. It consumes a
`RecoveryContext`, finds the latest checkpoint, and builds a `RecoveryPlan` from
records at or after that checkpoint.

Current action classifications:

- page allocation, page free, and page write records become redo candidates
- metadata records become metadata refresh candidates
- checkpoint records define recovery boundaries
- recovery markers are preserved for future diagnostics
- database lifecycle records are ignored by the physical recovery planner

This keeps crash recovery infrastructure in place without pretending that a full
transaction engine exists. Future phases can attach redo/undo executors to
`RecoveryPlan` once transaction records and page-delta payloads are formally
defined.

### Replication Compatibility

Because WAL records are ordered by LSN and independent from local page storage,
future replication can stream validated records without reading arbitrary database
pages directly.

## Transaction Layer Foundation

The transaction layer currently provides lifecycle infrastructure only. It does
not implement MVCC, locking, isolation levels, page undo, or SQL semantics.

Current components:

- `TransactionId`: monotonic local transaction identifier.
- `Transaction`: immutable identity plus lifecycle timestamps and state.
- `TransactionManager`: thread-safe registry for active and completed local transactions.
- `TransactionLogAdapter`: small adapter that records transaction lifecycle events into WAL.
- `TransactionContext`: lightweight handle prepared for future storage operations.

Transaction WAL record types:

- `TransactionStarted`
- `TransactionCommitted`
- `TransactionRolledBack`

Each transaction record payload uses the versioned `TransactionPayload` format
defined by `LogPayloadCodec`. Recovery analysis classifies these records as
transaction boundaries so a future recovery executor can reconstruct transaction
state before applying redo or undo logic.

This design keeps the storage engine honest: transaction boundaries are durable
and visible to recovery, while full transactional behavior is intentionally left
for the next dedicated phase.

## Undo/Redo Recovery Foundation

`UndoRedoRecoveryExecutor` is the first execution-facing recovery component. It
does not mutate database pages directly. Instead, it depends on narrow
`RedoApplier` and `UndoApplier` interfaces so future page, metadata, and
transaction recovery logic can be attached without changing recovery planning.

The executor is intentionally idempotent per LSN. If the same recovery plan is
submitted more than once, already-applied LSNs are skipped. This protects future
crash-recovery code from duplicate application windows and race-prone retry
loops.

Current behavior:

- redo candidates and metadata refresh actions are sent to `RedoApplier`
- undo candidates are sent to `UndoApplier`
- checkpoints, markers, transaction boundaries, ignored records, and zero LSNs are skipped
- execution is mutex-protected so concurrent recovery attempts cannot apply the same LSN twice

Typed WAL payloads now define the basic page, metadata, and transaction boundary
contracts. Future phases will extend these contracts with page images, page
deltas, and transaction ownership semantics. Until those execution payloads
exist, this layer remains a safe scaffold rather than a full recovery algorithm.

## Typed WAL Payload Format

WAL records now have versioned typed payload helpers in `LogPayloadCodec`. The
outer `LogRecord` remains responsible for record framing, CRC, LSN, timestamp,
and record type. Payload codecs are responsible only for interpreting the binary
payload attached to a record.

All payloads start with:

- offset `0`: `uint32 payload_format_version`

Current payload version is `1`.

### PageAllocated Payload

- `uint32 payload_format_version`
- `uint32 page_id`
- `uint32 page_type`
- `uint64 owner_table_id`

### PageFreed Payload

- `uint32 payload_format_version`
- `uint32 page_id`
- `uint64 owner_table_id`

### PageWritten Payload

- `uint32 payload_format_version`
- `uint32 page_id`
- `uint32 page_type`
- `uint32 page_checksum`
- `uint32 used_bytes`
- `uint32 page_version`
- `uint64 owner_table_id`

### MetadataUpdated Payload

- `uint32 payload_format_version`
- `uint32 metadata_kind`
- `uint32 page_id`
- `uint32 metadata_version`
- `uint32 payload_checksum`

### Transaction Payload

- `uint32 payload_format_version`
- `uint64 transaction_id`

Readers reject unsupported payload versions, truncated payloads, trailing bytes,
invalid page identifiers, and invalid transaction identifiers. This gives future
crash recovery a stable binary contract while keeping WAL extensible for cloud
shipping, replication, and encryption.
