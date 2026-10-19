# ZENTHRIL DISSERTATION STATE

Дата аудита: 24.09.2026. Область аудита: доступный рабочий каталог ZenthrilDB,
приложенный отчёт этапа разработки и исходные файлы проекта. Отдельный репозиторий
Zenthril в предоставленных материалах отсутствует.

## A–C. Состояние реализации

### Реализовано и подтверждено исходным кодом

| Компонент | Подтверждённый объём | Основной исходный код |
|---|---|---|
| Бинарный формат | `.zdb`, `DatabaseHeader`, fixed-size page 8192 bytes, little-endian serialization | `core/Types.hpp`, `storage/DatabaseHeader.*`, `storage/Page.*`, `storage/BinaryIO.hpp` |
| Integrity | CRC32 для DatabaseHeader, страницы и WAL-frame | `core/CRCManager.*`, `core/FileManager.cpp`, `wal/LogRecord.cpp` |
| File storage | создание/открытие файла, чтение/запись страниц, flush, reservation PageId | `core/FileManager.*` |
| Buffer cache | LRU cache, pin/unpin, dirty tracking, eviction, `BufferPageGuard` RAII | `core/BufferManager.*` |
| Metadata | table create/delete/rename/update, persistent `DatabaseMetadata` and `PageDirectory` | `core/MetadataManager.*`, `storage/DatabaseMetadata.*`, `storage/PageDirectory.*` |
| Page lifecycle | allocation/free/read/write, reuse free identifiers, reserved page check | `core/PageManager.*`, `core/FreePageManager.*`, `storage/FreePageList.*` |
| WAL foundation | LSN, versioned record frame, CRC, append/replay/flush/checkpoint/truncate, typed payload codecs | `wal/*` |
| Transaction foundation | local IDs and in-memory lifecycle `Active -> Committed/RolledBack`, WAL boundary events | `transaction/*` |
| Recovery foundation | WAL analysis, checkpoint boundary, recovery plan, abstract redo/undo execution and LSN idempotency | `recovery/*` |
| Lock foundation | shared/exclusive locks, timeout, lock guard; source also contains upgrades, FIFO queue and wait-for graph | `lock/*` |

### Реализовано частично

| Компонент | Фактическое ограничение |
|---|---|
| WAL | Содержит событие и typed metadata, но не before/after images и page deltas для фактического redo/undo страниц. |
| Transactions | Нет MVCC, isolation levels, SQL transactions, durable transaction-id recovery, conflict detection и rollback данных. |
| Recovery | `RecoveryEngine` строит план; `UndoRedoRecoveryExecutor` вызывает интерфейсы appliers, но не восстанавливает файл `.zdb`. |
| Locking | Новая ветвь кода deadlock detection не проходит компиляцию; её поведение актуальными тестами не подтверждено. |
| Buffer concurrency | Метаданные cache защищены mutex, однако `Page&` возвращается после снятия mutex и не имеет page latch. |
| Encryption | Только интерфейсы провайдера/ключей, без реализации криптографии. |

### Не реализовано либо не подтверждено

- SQL, query engine, record manager, B+Tree и полноценный catalog.
- Полный crash recovery, MVCC, replication, remote/cloud storage.
- Реализованное data-at-rest encryption.
- Производственные нагрузочные измерения и benchmark.
- Zenthril, E2EE, backend/frontend, WebSocket, PostgreSQL, Redis, Docker и интеграция ZenthrilDB с Zenthril.

## D–F. Тестирование и актуальная сборка

### Подготовленные тесты

В `tests/` присутствуют тесты CRC, FileManager, Page, BufferManager,
metadata, free-page/page managers, boundary validation, WAL, recovery plan,
transaction lifecycle, typed WAL payloads, binary I/O и базовых locks.

### Фактически выполненные проверки

| Проверка | Результат | Доказательство |
|---|---|---|
| Запуск ранее созданного бинарника тестов | Прошёл: `All ZenthrilDB tests passed` | `build/zenthrildb_tests.exe`; исполняемый файл датирован 01.08.2026 |
| Сборка текущих исходников | Неуспешна | `cmd /c build\\compile_harden.cmd`, 24.09.2026 |

Причина текущей ошибки: `WaiterEntry::cv` имеет тип `std::condition_variable*`,
в то время как в `LockManager::acquire()` и `upgrade()` ему присваивается адрес
`std::condition_variable_any`. Ошибки MSVC находятся в `lock/LockManager.cpp:100`
и `lock/LockManager.cpp:251`.

Дополнительно `CMakeLists.txt` не включает `lock/WaitForGraph.cpp`, хотя этот
translation unit требуется `LockManager`. Следовательно, до исправления сборки
результат старого бинарника не является свидетельством состояния текущего кода.

## G–J. Степень готовности данных для диссертации

### В разработке

- Исправление и верификация расширенного LockManager/WaitForGraph.
- Полноценное присоединение recovery actions к физическим изменениям страниц.

### Только планы

Согласно `docs/ROADMAP.md`: MVCC, B+Tree, catalog, record manager, query engine,
SQL parser, optimizer и local DB MVP v1.0. Remote/cloud mode, replication и
encryption отложены за v1.0.

### Допустимые результаты

- Описание реализованных модулей с точными ссылками на код.
- Описание unit-test design и факт успешного запуска исторического бинарника с
обязательной маркировкой его даты.
- Факт неуспешной сборки текущей ревизии и точные причины.
- Анализ ограничений архитектуры и план экспериментов.

### Недопустимые результаты

- Числовые результаты benchmark, load testing, recovery time, CPU/memory usage.
- Утверждение о готовом crash recovery, ACID, SQL, MVCC, encryption или production readiness.
- Утверждение о Zenthril/E2EE/PostgreSQL/Redis/Docker либо об интеграции с ZenthrilDB.

## Traceability map

| Claim / component | Source and code | Test | Status / evidence | Proposed dissertation section |
|---|---|---|---|---|
| 8192-byte page model | `core/Types.hpp`, `storage/Page.*` | `test_page_format.cpp` | Implemented; historical binary passed | 2.2, 3.1 |
| Header/page CRC32 | `core/CRCManager.*`, `core/FileManager.cpp` | `test_crc_manager.cpp`, `test_storage_boundaries.cpp` | Implemented; historical binary passed | 2.3, 4.2 |
| LRU buffer and RAII guard | `core/BufferManager.*` | `test_buffer_manager.cpp` | Implemented; historical binary passed | 3.2, 4.3 |
| Metadata and free-page reuse | `core/MetadataManager.*`, `core/FreePageManager.*` | `test_metadata_manager.cpp`, `test_free_page_manager.cpp` | Implemented; historical binary passed | 3.3 |
| WAL frame, LSN and typed payload | `wal/*` | `test_wal.cpp`, `test_log_payload.cpp` | Implemented; historical binary passed | 3.4, 4.4 |
| Transaction boundaries | `transaction/*` | `test_transaction.cpp` | Foundation only; historical binary passed | 3.5, 4.5 |
| Recovery planning/idempotency scaffold | `recovery/*` | `test_recovery.cpp` | Foundation only; no physical recovery | 3.6, 4.6 |
| Shared/exclusive locking | `lock/LockManager.*` | `test_lock_manager.cpp` | Basic API test exists; current revision does not compile | 3.7, 4.7 |
| Deadlock detection | `lock/WaitForGraph.*`, `lock/LockManager.*` | no dedicated test identified | Not verified; current revision does not compile | 4.7, limitations |
| Zenthril integration | no supplied source | none | Not confirmed | 6, future work |

## Recommended working topic

«Разработка архитектуры локального storage engine с журналированием и контролем
целостности данных на примере ZenthrilDB».

The topic accurately reflects a storage-engine foundation and does not claim a
complete DBMS, production recovery, or an implemented Zenthril integration.

## Documentary discrepancies

1. The attached v0.2 development report uses an older reserved-page layout;
the current code establishes PAGE 0 mirror, PAGE 1 DatabaseMetadata, PAGE 2
PageDirectory, PAGE 3 FreePageList and PAGE 4+ user pages.
2. `docs/LOCK_SPEC.md` says deadlock detection is absent, while current sources
include it. This code cannot currently be compiled and is therefore neither a
confirmed operational capability nor a valid dissertation result.
3. `README.md` describes the first MVP subsystem, while the code includes later
WAL, transaction, recovery and lock foundations. The dissertation should use
the code and the detailed architecture documents as the source of truth.
