# ZenthrilDB

ZenthrilDB is an original C++23 storage engine foundation designed for local-first database persistence.

## Status: Research component (not integrated with Go backend)

ZenthrilDB is a standalone research project. It is **not** integrated with the Zenthril Go backend, which uses PostgreSQL as its primary database. No decision has been made to adopt ZenthrilDB as a replacement for PostgreSQL.

## Implemented subsystems

The following C++23 subsystems are implemented:

- Database file format
- Page format and checksum validation
- Binary file manager
- CRC32 integrity checking
- Basic LRU buffer management
- Write-Ahead Log (WAL) — `wal/`
- Crash recovery — `wal/RecoveryContext.*`
- Transaction management — `transaction/`
- Lock manager with wait-for graph — `lock/`
- Free page management — `core/FreePageManager.*`
- Future security interfaces — `core/EncryptionInterfaces.hpp`

## Build

CMake-based build. See `CMakeLists.txt` for targets. Build artifacts (`.obj`) are present in this directory but are not part of the source tree.

## Project documentation

The following design documents are planned but not yet present in this tree:

- Architecture overview
- Phased development roadmap
- Change history
- WAL binary specification
- Recovery architecture and execution contract
- Transaction lifecycle specification
- Local lock manager specification
- Architectural decision records (ADR)

## Relationship to Zenthril

ZenthrilDB is maintained in the same repository for provenance purposes but is developed independently. The Go backend (`backend/`) does not depend on ZenthrilDB and uses PostgreSQL.
