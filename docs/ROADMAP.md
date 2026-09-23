# ZenthrilDB Roadmap

## Purpose

This roadmap defines the planned evolution of ZenthrilDB from a local storage
engine foundation into a complete database management system. It is intentionally
incremental: each phase must compile, pass tests, and preserve compatibility with
existing `.zdb` and `.wal` formats unless a migration strategy is explicitly
documented.

## Development Principles

- Keep the architecture original and independent from existing database engines.
- Prefer small, verifiable subsystems over large rewrites.
- Preserve binary compatibility by using explicit versioned formats.
- Treat durability, corruption detection, and concurrency safety as default requirements.
- Do not introduce SQL, indexes, networking, or distributed systems before the lower layers are stable.

## Version Plan

| Version | Area | Status | Goal |
|---|---|---:|---|
| `v0.1` | Storage Foundation | Complete | `.zdb` file format, page format, CRC, FileManager, BufferManager |
| `v0.2` | Metadata Foundation | Complete | durable table metadata and reserved metadata pages |
| `v0.3` | Page Lifecycle | Complete | PageManager, FreePageManager, PageDirectory |
| `v0.4` | WAL Foundation | Complete | independent `.wal` file, LSN, WAL record framing, CRC |
| `v0.5` | Recovery Foundation | Complete | RecoveryContext, RecoveryEngine, RecoveryPlan, checkpoint analysis |
| `v0.6` | Transaction Foundation | Complete | transaction lifecycle, transaction WAL boundaries |
| `v0.7` | Undo/Redo Foundation | Complete | recovery execution scaffold, redo/undo interfaces, idempotency by LSN |
| `v0.8` | Typed WAL Payload | Complete | versioned payload contracts for page, metadata, and transaction records |
| `v0.9` | Lock Manager | Complete | local lock manager for table/page level coordination |
| `v0.10` | MVCC Foundation | Planned | row/version visibility model and transaction snapshots |
| `v0.11` | B+Tree Foundation | Planned | primary index structure and page split/merge mechanics |
| `v0.12` | Catalog | Planned | system catalog for schemas, tables, indexes, and internal objects |
| `v0.13` | Record Manager | Planned | row layout, record insertion, update, delete, and overflow support |
| `v0.14` | Query Engine MVP | Planned | logical execution primitives without SQL optimization |
| `v0.15` | SQL Parser MVP | Planned | limited DDL/DML syntax and AST generation |
| `v0.16` | Optimizer Foundation | Planned | rule-based planning and cost model scaffolding |
| `v1.0` | Local DB MVP | Planned | stable local database with storage, transactions, recovery, and basic SQL |

## Near-Term Priorities

1. Lock Manager

   Introduce lock modes, lock table, wait policy, and deadlock-prevention
   scaffolding. Keep scope local only.

2. MVCC Foundation

   Define transaction visibility rules and version metadata. Do not implement
   complex garbage collection until the record layer exists.

3. B+Tree Foundation

   Implement a page-based tree compatible with the existing PageManager and WAL
   payload conventions.

4. Catalog

   Move table definitions and future index definitions into an internal catalog
   model built on top of metadata pages.

## Release Rules

- Every version must include unit tests.
- Every binary format change must update the relevant specification.
- Every architectural decision that affects compatibility should be captured in `docs/ADR/`.
- No planned subsystem should silently bypass WAL, CRC, or recovery interfaces.
- Test failures block promotion to the next version.

## Out of Scope Until After `v1.0`

- remote mode
- cloud mode
- distributed replication
- enterprise authentication and authorization
- SQL optimizer sophistication beyond basic rule-based planning
- encryption implementation

Interfaces for these areas may exist, but full behavior should wait until the
local engine is stable.
