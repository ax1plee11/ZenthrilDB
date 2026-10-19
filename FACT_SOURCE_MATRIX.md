# Fact-source matrix

| Assertion | Source | File / section | Status |
|---|---|---|---|
| Page size is 8192 bytes | source code | `core/Types.hpp` | CODE |
| System page layout begins at page 0 | source code | `core/Types.hpp`, `core/FileManager.cpp` | CODE |
| CRC validates pages and WAL frames | source code and tests | `FileManager.cpp`, `LogRecord.cpp`, `tests/test_*` | CODE + TEST |
| WAL frame uses LSN and typed payload | source code and tests | `wal/*`, `test_wal.cpp`, `test_log_payload.cpp` | CODE + TEST |
| Transaction lifecycle has three states | source code | `transaction/Transaction.hpp` | CODE |
| Physical crash recovery is absent | source code and documentation | `recovery/*`, `docs/RECOVERY_SPEC.md` | CODE + DOCUMENTATION |
| Current unit tests passed | executed verification | `build/zenthrildb_tests.exe`, 25.09.2026 | EXPERIMENT |
| Zenthril integration exists | no source supplied | n/a | MISSING |
