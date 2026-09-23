# ZenthrilDB

ZenthrilDB is an original C++23 storage engine foundation designed for local-first database persistence.

This repository currently contains the first MVP subsystem:

- database file format
- page format and checksum validation
- binary file manager
- CRC32 integrity checking
- basic LRU buffer management
- future security interfaces

The architecture is intentionally small and explicit so later work can extend it toward remote, cloud, and enterprise deployments.

## Project Documentation

- `docs/architecture.md`: current architecture overview
- `docs/ROADMAP.md`: phased development plan
- `docs/CHANGELOG.md`: internal version history
- `docs/WAL_SPEC.md`: write-ahead log binary specification
- `docs/RECOVERY_SPEC.md`: recovery architecture and execution contract
- `docs/TRANSACTION_SPEC.md`: transaction lifecycle specification
- `docs/LOCK_SPEC.md`: local lock manager specification
- `docs/ADR/`: architectural decision records
