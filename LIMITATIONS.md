# Limitations

- Zenthril source, E2EE, network components and integration are unavailable.
- No SQL, query engine, record manager, B+Tree, MVCC or isolation levels exist.
- WAL does not contain physical page images/deltas; crash recovery is a foundation only.
- `std::fstream::flush()` is used; an fsync/FlushFileBuffers durability policy is not implemented.
- Encryption interfaces have no provider, cipher, key storage or threat-model implementation.
- No reproducible latency, throughput, CPU, RAM, benchmark or sanitizer result exists.
- Dedicated tests for wait-for graph cycles, starvation, upgrade/downgrade and fairness are absent.
- The project is not a production DBMS.
