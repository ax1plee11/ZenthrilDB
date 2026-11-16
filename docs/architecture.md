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
pages through `FreePageManager`, updates the persistent `PageDirectory` through
`MetadataManager` in the same WAL mutation as page/free-list changes, rejects
direct writes to reserved pages, validates pages through `FileManager`, and tracks
dirty page identifiers until `flushDirtyPages()` is called. Production callers
should use `DatabaseRuntime`, which owns and validates the complete manager graph.

Freed pages are rewritten as `Free` pages before their identifiers return to the
free-page list. CRC generation and verification remain centralized in
`FileManager`, keeping page lifecycle logic separate from binary integrity logic.

`BufferManager` supports both the original manual `pin()` / `unpin()` API and a
RAII `BufferPageGuard` returned by `pinGuard()`. New code should prefer the
guard so pages are automatically unpinned when control leaves scope.

## Heap Records (experimental)

`RecordManager` exposes durable `put`, `get`, `erase`, and `scan` through
`DatabaseRuntime`. A table root links checksummed `Overflow` pages; each page
stores one versioned binary key/value record and the next page id. Insertion
publishes the record and root link in one WAL transaction. Deletion updates
the chain and frees the removed page in one WAL transaction. Startup validates
the chain. Each record must fit in one page, and scans are linear. The on-disk layout is
specified in `docs/RECORD_FORMAT_RU.md` and is not yet declared stable.

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

The root stays at page ID `2`. Version 1 stores the directory in the root:

- offset `0`: `uint32 format_version`
- offset `4`: `uint32 entry_count`
- offset `8`: fixed-size `PageDirectoryEntry` records

Version 2 keeps the first two fields, adds `PageId next_page_id` at offset `8`
(`0` ends the chain), and starts entries at offset `12`. Each page contains up
to 92 entries. The chain uses `Metadata` pages with strictly increasing IDs.
Continuation pages are excluded from user directory entries and the free list.
The reader checks format, entry counts, used bytes, link targets, and duplicate
page IDs across the chain.

The first allocation beyond 92 entries upgrades the directory in the same WAL
transaction as the user page and database header. Later mutations journal only
changed directory pages. Older binaries cannot read a directory after upgrade.

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

This phase provides WAL infrastructure. Transaction lifecycle records exist, but
page changes are not yet transactionally coupled to commit/rollback.

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
- `WriteAheadLog`: append/read/flush/truncate file primitive. `flush()` requests
  OS-level durability; `append()` alone does not.
- `LogManager`: thread-safe API for append, replay, checkpoint, and recovery context preparation.
- `RecoveryContext`: container for future recovery planning.
- `RecoveryManager`, `CrashRecovery`, `CheckpointManager`: extension interfaces for future transaction and crash recovery work.

### Recovery Strategy

Recovery reads WAL records in LSN order after validating CRC and record versions.
Checkpoint records are currently markers only; they do not authorize skipping
older records because pages and checkpoint are not coordinated atomically. The
explicit `RecoveryCoordinator` supports page-image recovery only.

## Recovery Engine Foundation

`RecoveryEngine` analyzes WAL records and builds a `RecoveryPlan`. The limited
page-image `RecoveryCoordinator` runs automatically inside
`FileManager::openDatabase()` while the exclusive database lock is held. An open
fails before exposing storage if WAL validation or recovery fails.

Current action classifications:

- page allocation, page free, and page write records become redo candidates
- metadata records become metadata refresh candidates
- checkpoint records are retained as markers; the current coordinator replays WAL history before and after them
- recovery markers are preserved for future diagnostics
- database lifecycle records are ignored by the physical recovery planner

This keeps recovery infrastructure in place without claiming complete crash
recovery. Page-image recovery is a limited execution path; allocation, free-page
and metadata mutations are not comprehensively replayed.

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

Each transaction record uses the versioned `TransactionPayload`. Lifecycle calls
sync each boundary record before returning. On restart, the next ID is reconstructed
from retained lifecycle records. An explicit coordinator classifies page-image
records as redo/undo, but arbitrary page changes are not connected to transaction
lifecycle operations.

## Undo/Redo Recovery Foundation

`UndoRedoRecoveryExecutor` is an execution-facing recovery component. It
dispatches actions through narrow applier interfaces. A page-image applier exists
for the supported page-image record path; this does not cover every page,
metadata, or transaction mutation.

The executor tracks applied LSNs in memory for one executor instance. A new
process does not retain that set; page-image REDO uses persisted Page LSN, while
UNDO reapplies the before-image. Process-level tests exercise a second coordinator
pass.

Current behavior:

- redo candidates and metadata refresh actions are sent to `RedoApplier`
- undo candidates are sent to `UndoApplier`
- checkpoints, markers, transaction boundaries, ignored records, and zero LSNs are skipped
- execution is mutex-protected so concurrent recovery attempts cannot apply the same LSN twice

Typed WAL payloads define page, metadata, page-image, and transaction boundary
contracts. The supported page-image path is tested, while general transactional
recovery and complete crash recovery remain unimplemented.

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
