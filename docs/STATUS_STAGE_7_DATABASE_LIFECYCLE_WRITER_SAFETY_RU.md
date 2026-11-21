# ZenthrilDB 0.1.0 — Stage 7: Database Lifecycle & Writer Safety

## 1. Stage 7 goal

Одна физическая база — один контролируемый владелец. Защита supported runtime от конфликтов writer/recovery/checkpoint/reduction/installation без внедрения multi-writer и без изменения форматов `.zdb`, WAL и `.zdb.install`.

**Статус: COMPLETE WITH LIMITATIONS — кандидат Stage 7 в описанной модели cooperating single-writer и поддерживаемого Runtime.** Все четыре финальные конфигурации — 8/8 групп. Ограничения custom graphs, object lifetimes, старых writers и hardware failure перечислены ниже.

## 2. Architecture before Stage 7

Репозиторий `C:\Users\User\Desktop\дс аналонг\zenthrildb-push`, main, HEAD `045fa2762451a125eb3fc5f19bcfc16e345cd699`. Stage 6 уже добавил selector и два database lock при alias open. Перед правками read-only аудит, status/branch/история; начальные status и binary patch сохранены в `build-21/stage7/status-before.txt`, `before.patch`, `head.txt`.

Stage 5 создаёт отдельный reduced-комплект, Stage 6 проверяет свежесть и публикует immutable selector. Startup recovery вызывается FileManager до построения Runtime-графа. Runtime сериализует record/table/checkpoint/backup operations своим recursive mutex. BufferManager не является центральным production page cache: PageManager использует FileManager/StorageMutationCoordinator.

## 3. Audit results

Существующие OS locks работали и до Stage 7. Обнаруженные пробелы:

1. LogManager/WriteAheadLog открывались без database ownership. Другой объект мог писать WAL уже занятой базы.
2. FileManager.closeDatabase освобождал database lock, пока отдельно созданный WAL-объект ещё сохранял writable stream.
3. Два BufferManager одного FileManager могли держать независимые кэши страниц.
4. Ошибки release/close часто поглощались destructor без диагностики.
5. Одних объектных mutex недостаточно для целого logical mutation/recovery и предотвращения закрытия с живыми зависимостями.
6. Windows DOS 8.3 filename aliases способны создать другое имя sidecar для того же физического файла. Введён явный отказ для таких **имён файла**. Алиас родительского каталога оставляет тот же sidecar inode.

GitNexus: FileManager — MEDIUM, 12 затронутых узлов; для части новых символов индекс вернул UNKNOWN, для старых WAL/Buffer классов — неоднозначные результаты. UNKNOWN не считался all-clear; реальные call sites прочитаны и проверены регрессией. Ruflo/ToolSearch отсутствует среди доступных инструментов. План сообщён владельцу после аудита до реализации.

## 4. Current lifecycle

| Состояние | Реализация и переход |
|---|---|
| CLOSED | Нет открытого stream и ownership. Только create/open |
| OPENING | FileManager mutex, OS ownership получен до доступа к изменяемым файлам |
| RECOVERY | При startup или явном owned recover; ownership operation guard, identity/WAL/baseline preflight до ремонта |
| OPEN_READ_WRITE | Startup прошёл, Runtime-граф загружен и проверен; один владелец |
| VALIDATING_READ_ONLY | Внутренний FileManager.openReadOnly для immutable baseline/проверки; не новый публичный concurrent-reader API |
| MUTATING / CHECKPOINTING | Runtime mutex + operation guard владельца; системные записи через прежние WAL-before-data пути |
| WAL_REDUCTION / WAL_INSTALLATION | Только отдельное exclusive acquisition source/target, не переход живого writer в online compaction |
| CLOSING | Зависимости WAL/cache должны быть закрыты. Close с живыми зависимостями отклоняется без release |
| FAILED / RECOVERY_REQUIRED | Прежние failed-storage/poisoned-writer flags блокируют продолжение. Закрыть граф и выполнить новый open/recovery |

Это логические состояния, не искусственный persistent enum и не новый формат. `isOpen` синхронизирован, FileManager failed flag атомарен. Открытие/создание/закрытие защищены его mutex. Ошибка close возвращается после попытки закрыть stream и release OS resources. Неожиданное уничтожение FileManager с живым WAL диагностируется: capability отзывается, новый append запрещён, OS lock остаётся pinned до закрытия зависимого WAL.

## 5. Existing locks

- `DatabaseFileLock`: Windows CreateFileW + LockFileEx EXCLUSIVE/FAIL_IMMEDIATELY; Linux open O_NOFOLLOW/O_NONBLOCK/O_CLOEXEC + flock EX/NB.
- Runtime `operationsMutex_`: graph-level сериализация record/table/check/backup/checkpoint.
- FileManager recursive mutex: поток и поля конкретного instance.
- LogManager mutex: LSN allocation/append; WriteAheadLog recursive mutex: stream/replay/flush.
- Coordinator, metadata, free-list, PageManager, BufferManager: локальные mutex, не межпроцессные locks.
- LockManager: resource/transaction foundation. Это не новая интегрированная пользовательская изоляция или MVCC.

## 6. Ownership model

Существующий DatabaseFileLock расширен, новый параллельный ownership service не создан. Lock нельзя копировать или перемещать. Shared capability не копирует OS lock: зависимый объект удерживает время жизни уже полученного владения. Capability связана с нормализованным абсолютным database path; WAL нельзя привязать к чужому owner.

DatabaseBusyError означает именно OS contention. Permission/I/O/прочие acquire/release errors возвращаются отдельно с system error code. PID не является lock-механизмом: на POSIX он только запрещает использование унаследованной capability после fork; дочерний destructor не выполняет LOCK_UN родительского open-file-description.

## 7. Writer model

**Single active writer per physical database path.** Второй FileManager/Runtime в том же или другом процессе не получает ownership. Alias удерживает logical source и physical target. Backup/snapshot-копии в других каталогах — отдельные физические комплекты, даже если UUID тот же; глобального mutex по UUID нет.

WAL, относящийся к существующему `.zdb`, требует database ownership: явную capability FileManager либо собственное exclusive acquisition, если базы больше никто не держит. Прямой WAL-доступ к архивному source с selector запрещён. Standalone WAL не объявляется полноценной базой и имеет собственное владение.

## 8. Locking architecture

Добавлен `.wal.lock` — OS lock-domain одного WAL stream. Он препятствует двум независимым LSN allocators даже при одной явно переданной database capability. Имя применяется также к standalone WAL; сам WAL/data/selector формат не изменён. Пределы размеров по-прежнему проверяет соответствующий database/WAL parser; lock-domain standalone WAL не навязывает другой лимит вместо его WalLimits.

WAL stream закрывается до release `.wal.lock` и database capability. FileManager.closeDatabase отказывает, пока shared зависимости существуют. После `LogManager::close()` capability освобождена, дальнейшее использование закрытого LogManager запрещено. Database lock и WAL lock sidecars сохраняются; наличие файла не означает активный PID и не мешает повторному open.

## 9. Lock ordering

Поддерживаемый graph:

1. Exclusive logical database lock, затем выбранный physical lock; acquisition не ждёт, конфликт возвращается.
2. Runtime operation mutex для прикладной операции. Не берётся глобальный mutex на все базы.
3. Локальные manager mutex согласно существующим путям подготовки; перед дисковой mutation/recovery/checkpoint — per-owner recursive operation guard.
4. MutationCoordinator → TransactionManager → LogManager → WriteAheadLog → FileManager I/O. Payload factory не должна запускать произвольные blocking callbacks или менять lifecycle.

Operation guard использует try-lock: конфликт lifecycle отклоняется, а не создаёт бесконечное ожидание при инверсии низкоуровневых manager locks. Внутренние вызовы того же потока рекурсивны. WAL close также fail-fast, если другой поток удерживает lifecycle operation; cleanup не ждёт owner mutex, одновременно удерживая WAL mutex.

Custom low-level graph обязан сериализовать всю подготовку metadata/cache сам либо явно удерживать `files.operation()`. Нельзя приравнивать отдельный Page/FileManager mutex к пользовательской транзакционной изоляции. Runtime — поддерживаемый прикладной entry point.

## 10. WAL Reduction restrictions

Stage 5 algorithm сохранён. Source, а для установленного alias также selected target, удерживаются OS ownership до завершения копирования, recovery и публикации candidate. Active writer блокирует отдельный reducer до чтения изменяемого состояния. Reduction не запускается автоматически при open.

Новый `.wal.lock` учитывается в cleanup только собственного приватного staging после закрытия его WAL. Публичные lock-файлы не удаляются автоматически при release. Source-комплект сохраняется.

## 11. WAL Installation restrictions

Stage 6 selector format не менялся. Source и candidate блокируются до validation/sync/publication. Конкурирующая установка либо получает DatabaseBusyError, либо обнаруживает уже выбранное другое поколение и отказывает. Selector не переписывается. Ошибка окончательного open после публикации не означает откат: committed selector остаётся authority.

После обычного alias open нельзя параллельно открыть source либо selected target. Новые raw WAL writers архивного source отклоняются даже при закрытом alias. Два разных reduced-кандидата не выбираются по mtime: selector фиксирует один путь и generation.

## 12. Recovery restrictions

RecoveryCoordinator требует действующее FileManager ownership и per-owner operation guard. Для bound базы WAL path должен совпадать с owned physical database path. Старый legacy recovery fixture с отдельным standalone WAL сохранён в режиме совместимости; он не является новым production external-WAL API.

Startup recovery происходит до построения прикладного графа и доступа к данным. Другой процесс блокируется до его начала. Одновременные lifecycle mutation/recovery одного owner отклоняются operation guard. Не рекламируется безопасное ручное смешивание RecoveryCoordinator с произвольными low-level BEGIN/COMMIT и незалогированными изменениями pages: это transaction foundation, не multi-user API.

## 13. Multi-process behavior

На Windows и Ubuntu в Debug/Release проверены реальные процессы, а не только threads: второй writer/reducer/installer/recovery/checkpointer/raw-WAL получает безопасный отказ при удерживаемом ownership. После normal close и принудительного завершения владельца следующий процесс получает владение и проверяет данные. Гонка двух acquisition даёт одного удерживаемого владельца; гонка двух установок — один committed selector. Source и WAL сохраняются при отказах.

## 14. FileManager safety

Состояние stream защищено object mutex, failed flag атомарен. Read/write/flush/reserve/restore/layout получают owner operation guard; внутренние read-only baseline checks отдельно ограничены их caller ownership/immutable baseline.

Close живого owner при наличии WAL/cache возвращает ошибку и не освобождает lock. Для внутренних low-level инструментов последовательность: уничтожить/закрыть dependent managers и buffers, `wal.close()`, затем `files.closeDatabase()`. В Runtime это обеспечивается обратным порядком destruction его manager graph.

Два независимых BufferManager одного owner теперь запрещены. BufferPageGuard остаётся заимствованием: он должен уничтожаться до BufferManager; Page& и header/identity references нельзя использовать после уничтожения/смены соответствующего graph. Concurrent object destruction и использование dangling C++ references не поддерживаются.

## 15. Crash behavior

Process termination освобождает OS handles/fds, а не PID-file. Оставшиеся `.zdb.lock`/`.wal.lock` допустимы и используются повторно. Child handshake подтверждает полученное ownership и завершённый commit до kill; после kill другой процесс открывает базу и проверяет подтверждённое значение.

Как и прежде, recovery проверяет LSN/identity и не меняет корректные повторно открытые данные. Process-crash не является имитацией физического отказа диска или потери питания.

## 16. I/O failure tests

Debug добавляет 5 сценариев: acquire до получения lock; отказ сразу после OS acquisition с cleanup; ошибка close; ошибка release после закрытия OS handle; PageSync failure при close. После каждого проверяются сохранность или корректная закрытость, отсутствие неразрешимого stale ownership и новый open/recovery.

Существующий IoFaultInjector расширен, второй injector не создан. Диагностика destructor/close больше не скрывает ошибки. Часть старых тестов изменена только для передачи ownership и явного закрытия WAL перед FileManager; assertions не отключены.

## 17. Multi-process tests

В новой `ZenthrilDBLifecycle` выполняются 22 дочерних процесса на конфигурацию:

- два owner-процесса (normal close и принудительный crash после commit), по шесть конкурирующих probe: open, reduce, install, raw WAL, recovery, checkpoint; после release/crash — успешный новый process open;
- два процесса за общим GO barrier соревнуются за ownership: ровно один удерживает lock, loser обязан вернуть именно busy code, а не произвольную ошибку;
- два процесса одновременно устанавливают разные корректные кандидаты: проверяется единственный committed selector и целостность данных;
- два probe после alias open подтверждают блокировку logical source и physical target.

Синхронизация — ready/go/release файловые барьеры и std::latch для threads; ожидание signal/exit имеет deadline. Sleep используется только как polling interval, не как доказательство ownership. Аварийный exit/kill проверяется конкретным status, данные сравниваются.

Дополнительно: два FileManager в одном процессе, raw WAL без ownership, два WAL с одной capability, два cache, close с живым WAL/cache, отзыв capability при неожиданном destruction FileManager, close/recovery/read при operation другого потока, повтор recovery после освобождения guard. Windows 8.3 alias проверяется условно, при отсутствии distinct short filename печатается SKIP.

## 18. Windows results

Windows x64, MSVC 19.51 / Visual Studio 18 2026, SDK 10.0.26100.0. Отдельные configure/build Debug и Release; полные CTest выполнены последовательно с тем же timeout 300. Configure/build/CTest/install завершились exit 0.

| Build | Passed | Failed | Время |
|---|---:|---:|---:|
| debug | 8/8 | 0 | 330.23 с |
| release | 8/8 | 0 | 121.16 с |

Логи `build-21/stage7/windows-final-{debug,release}-{configure,build,ctest,install}.log`; полный stdout тестов — `windows-final-*-details.log`. Установленные CLI и test benchmark прошли, source SHA-256 сохранён.

SKIP в финальных Windows-логах отсутствуют; 8.3 alias-сценарий выполнялся на этой файловой системе.

## 19. Ubuntu results

Ubuntu 24.04.5 x86_64, GCC 13.3, CMake 3.28.3, Docker Desktop image `zenthrildb-ci-ubuntu:local`. Build и temporary databases — собственный `/tmp` контейнера, source mount read-only. Обе финальные сборки сделаны с нуля; configure/build/CTest/install — exit 0.

| Build | Passed | Failed | Время |
|---|---:|---:|---:|
| debug | 8/8 | 0 | 165.46 с |
| release | 8/8 | 0 | 43.78 с |

Первый ранний прогон выявил 3 failed группы: порядок low-level WAL/database ownership, чтение checkpoint WAL при живом runtime и несовпадение места проверки custom standalone WalLimits. Исправлены реальные wiring/lock-domain boundaries без удаления assertions; последующий check прошёл 8/8, затем выполнены финальные чистые прогоны. Сохранены early/check/before-alias логи, они не выдаются за финальные.

GCC `-Wall -Wextra -Wpedantic -fsyntax-only` на FileManager, WriteAheadLog, LogManager, BufferManager, RecoveryCoordinator, StorageMutationCoordinator: exit 0, без диагностик. Это не race detector, sanitizer или whole-program proof.

## 20. Debug results

На каждой Debug-платформе прошли 8/8 групп, включая 22 процесса Stage 7 и 5 новых I/O lifecycle сценариев. Проверены in-process conflicts, owner crash, stale sidecars, reopen, recovery и capability revocation. Crash/I/O матрицы Stage 2–6 также прошли в соответствующих старых группах.

## 21. Release results

Windows и Ubuntu Release прошли 8/8 групп. Assertions включены через UNDEBUG. Все 22 process tests Stage 7, включая forced owner termination, выполняются и в Release. Управляемые IoFaultInjector сценарии включены только в Debug, и Release не выдаётся за их подтверждение.

## 22. Performance

| OS / Build | CLI Stage 6, медиана мс | CLI Stage 7, медиана мс | Open /3, мс | Close /3, мс | Ownership /3, мс |
|---|---:|---:|---:|---:|---:|
| windows/debug | 221.63 | 252.35 | 55.36 | 4.30 | 14.661 |
| windows/release | 150.67 | 176.76 | 39.41 | 4.14 | 9.963 |
| linux/debug | 317.08 | 273.39 | 23.53 | 0.73 | 0.157 |
| linux/release | 129.81 | 123.82 | 12.14 | 0.73 | 0.114 |

На этом workload Windows Release CLI вырос с 150,67 до 176,76 мс: около **+26,10 мс / +17,3%**. Это измеренная цена дополнительных проверок/владения на данном стенде, а не нулевой overhead. Меньшее время Linux в трёх samples не объявляется доказанным ускорением. Для оценки устойчивого throughput нужны более длительные повторяемые серии без посторонней нагрузки.

Вторая таблица: 10 операций для TX/WAL, по одному checkpoint/reduction/install, три alias reopen/close.

| OS / Build | 10 TX, мс | 10 WAL, мс | Checkpoint, мс | Reduction, мс | Install, мс | Alias reopen /3, мс |
|---|---:|---:|---:|---:|---:|---:|
| windows/debug | 2232.45 | 155.16 | 142.98 | 393.36 | 384.58 | 125.80 |
| windows/release | 1071.16 | 120.99 | 91.44 | 382.31 | 317.53 | 106.88 |
| linux/debug | 335.95 | 34.19 | 153.05 | 336.13 | 207.36 | 35.81 |
| linux/release | 319.02 | 34.08 | 78.68 | 163.20 | 111.97 | 18.97 |

Сырые samples и все команды/stdout/stderr/exit code — `build-21/stage7/{windows,linux}-{debug,release}-measurements.json` и `*-commands.jsonl`. Все четыре workload завершились exit 0, source hashes совпали. Reference Stage 6 уже имел OS database lock: сравнивается добавочная работа Stage 7, а не режим с отключённой безопасностью. По трём измерениям на desktop нельзя доказать улучшение throughput или отсутствие всех timing regressions. Предварительные замеры сохранены отдельно, итоговые используют финальные проверенные бинарники.

Benchmark не отключает ownership. Stage 6 reference CLI используется только на той же закрытой v1 fixture **до появления selector**, последовательно с текущим CLI, с проверкой SHA-256. Сравнение CLI включает startup/open/recovery/check/close. In-process driver отдельно измеряет acquisition/release, FileManager open и close (по 3), 10 транзакций, 10 durable WAL appends, checkpoint, reduction, installation и повтор alias open/close.

Три samples — ограниченный desktop workload, не latency SLO или throughput-гарантия. Время зависит от host load. Нельзя приписывать всё различие ownership: новый runtime делает прежние validation/recovery и системные sync. Глобальной сериализации независимых баз нет.

## 23. Compatibility

Новые writers используют прежний database sidecar и новый WAL sidecar; старые FileManager-based writers, использующие тот же database lock, блокируются во время активного owner. Старые raw WAL writers не знают новый lock-domain и могут его игнорировать. Stage 6 и более старые бинарники не обязаны понимать selector.

Безопасная политика: остановить все старые writers, сохранить полный комплект и backup, обновить все entry points, проверить closed copy и только затем install. После установки не использовать старый writer для архивного source. Не удалять source и marker для обхода отказа. Защита от произвольного процесса с прямой записью в файлы не обещается; Linux flock advisory.

API низкоуровневой сборки графа требует явной capability: `LogManager(databaseWalPath(path), files.ownership())` либо paired constructor с пятым параметром. WAL должен быть закрыт раньше FileManager. Старые тестовые helpers адаптированы к этому порядку с сохранением проверок. `.zdb`/WAL/selector migration не нужна.

## 24. Known limitations

- Single writer, не multi-writer и не MVCC. Runtime сериализует свои операции, но это не multi-command пользовательская изоляция.
- Произвольные custom low-level manager graphs, сохраняющие устаревшие metadata/page references после close/reopen, не становятся безопасными автоматически. Использовать Runtime или явно удерживать operation и соблюдать lifetimes; после ошибки custom graph пересоздать.
- BufferPageGuard должен жить короче BufferManager, external threads завершены до destruction Runtime. Это C++ lifetime contract, не глобальный collector ссылок.
- Доверенные локальные каталоги обязательны. Удаление/замена lock-файла сторонним процессом, TOCTOU, hostile parent rename, filesystem without working locks, network FS и advisory-lock bypass вне гарантии.
- Windows 8.3 filename aliases отвергаются. Symlinks/hard links/reparse points остаются запрещёнными. При невозможности получить short alias conditional test явно SKIP.
- Post-fork reuse capability запрещено; child должен exec/reopen. Полный fork-safe произвольный многопоточный application runtime не заявляется.
- Power-loss/OS-crash/реальный ENOSPC/отказ контроллера, mid-syscall kill, реальный отказ kernel unlock требуют отдельного стенда. I/O injection не подменяет эти испытания.
- Криптографическая аутентификация WAL, anti-rollback всего комплекта и безопасная автоматическая очистка source не реализованы этим этапом.

## 25. Changed files

Новые файлы этого этапа: `tests/test_database_lifecycle.cpp`, `tests/benchmark_lifecycle.cpp`, `tools/measure_database_lifecycle.py`, `docs/STATUS_STAGE_7_DATABASE_LIFECYCLE_WRITER_SAFETY_RU.md`.

Production: `core/DatabaseFileLock.hpp`, `core/FileManager.hpp`, `core/FileManager.cpp`, `core/BufferManager.hpp`, `core/BufferManager.cpp`, `core/DatabaseRuntime.cpp`, `core/WalReductionInstallation.cpp`, `wal/LogManager.hpp`, `wal/LogManager.cpp`, `wal/WriteAheadLog.hpp`, `wal/WriteAheadLog.cpp`, `recovery/RecoveryCoordinator.cpp`, `recovery/CheckpointService.cpp`, `transaction/StorageMutationCoordinator.cpp`, `storage/IoFaultInjector.hpp`, `storage/IoFaultInjector.cpp`, `storage/SafeFilePath.hpp`.

Build/docs: `CMakeLists.txt`, `docs/OPERATIONS_RU.md`, `LIMITATIONS.md`.

Существующие tests/helpers адаптированы к явной зависимости ownership и безопасному close: `tests/TestDatabaseFiles.hpp`, `tests/test_main.cpp`, `tests/test_buffer_manager.cpp`, `tests/test_free_page_manager.cpp`, `tests/test_metadata_manager.cpp`, `tests/test_page_manager.cpp`, `tests/test_storage_boundaries.cpp`, `tests/test_database_wal_identity.cpp`, `tests/test_header_recovery.cpp`, `tests/test_safe_checkpoint.cpp`, `tests/test_physical_page_recovery.cpp`, `tests/test_physical_recovery_crash.cpp`, `tests/test_snapshot_foundation.cpp`, `tests/test_wal_reduction.cpp`, `tests/test_wal_installation.cpp`, `tests/test_snapshot_generation.cpp`, `tests/test_crash_recovery.cpp`. Состав assertions сохранён; чтение checkpoint WAL перенесено за границу жизни writer, старые buffer-проверки выполняются последовательно вместо одновременных независимых cache.

Все пути относительно `C:\Users\User\Desktop\дс аналонг\zenthrildb-push`. Core/WalReductionInstallation и часть тестов были untracked до Stage 7; они сохранены и дополнены, не объявляются вновь созданными этим этапом.

## 26. Git status

main, HEAD `045fa2762451a125eb3fc5f19bcfc16e345cd699`; commit NOT CREATED; push NOT PERFORMED; user changes PRESERVED. Ни один исходный dirty путь не исчез. Binary patch до начала сохранён; reset/clean/destructive checkout не выполнялись.

Полный текущий status, включая Stage 2–6:

```text
 M .github/workflows/ci.yml
 M CMakeLists.txt
 M LIMITATIONS.md
 M core/BufferManager.cpp
 M core/BufferManager.hpp
 M core/DatabaseFileLock.hpp
 M core/DatabaseRuntime.cpp
 M core/DatabaseRuntime.hpp
 M core/FileManager.cpp
 M core/FileManager.hpp
 M core/SnapshotBaseline.cpp
 M docs/CLI_RU.md
 M docs/OPERATIONS_RU.md
 M docs/PHYSICAL_RECOVERY_RU.md
 M docs/SNAPSHOT_FOUNDATION_RU.md
 M docs/WAL_IDENTITY_RU.md
 M docs/roadmap/STORAGE_RECOVERY_NEXT_RU.md
 M recovery/CheckpointService.cpp
 M recovery/RecoveryCoordinator.cpp
 M storage/DurableFileSync.hpp
 M storage/IoFaultInjector.cpp
 M storage/IoFaultInjector.hpp
 M storage/SnapshotManifest.cpp
 M storage/WalIdentity.cpp
 M storage/WalIdentity.hpp
 M tests/TestDatabaseFiles.hpp
 M tests/test_buffer_manager.cpp
 M tests/test_checkpoint_crash.cpp
 M tests/test_crash_recovery.cpp
 M tests/test_database_wal_identity.cpp
 M tests/test_free_page_manager.cpp
 M tests/test_header_recovery.cpp
 M tests/test_main.cpp
 M tests/test_metadata_manager.cpp
 M tests/test_page_manager.cpp
 M tests/test_physical_page_recovery.cpp
 M tests/test_physical_recovery_crash.cpp
 M tests/test_safe_checkpoint.cpp
 M tests/test_snapshot_foundation.cpp
 M tests/test_snapshot_generation.cpp
 M tests/test_snapshot_publication.cpp
 M tests/test_storage_boundaries.cpp
 M tools/zenthrildb_cli.cpp
 M transaction/StorageMutationCoordinator.cpp
 M transaction/TransactionManager.cpp
 M wal/LogManager.cpp
 M wal/LogManager.hpp
 M wal/WriteAheadLog.cpp
 M wal/WriteAheadLog.hpp
?? AUDIT_REPORT_RU.md
?? PHASE_2_1_STATUS_RU.md
?? RELIABILITY_REPORT_RU.md
?? core/WalReductionInstallation.cpp
?? docs/DATABASE_VERIFICATION_2026-11-20_RU.md
?? docs/STATUS_2026-11-20_RU.md
?? docs/STATUS_STAGE_5_WAL_REDUCTION_RU.md
?? docs/STATUS_STAGE_6_CRASH_SAFE_WAL_INSTALLATION_RU.md
?? docs/STATUS_STAGE_7_DATABASE_LIFECYCLE_WRITER_SAFETY_RU.md
?? storage/SafeFilePath.hpp
?? storage/StorageLimits.hpp
?? storage/WalInstallation.cpp
?? storage/WalInstallation.hpp
?? tests/benchmark_lifecycle.cpp
?? tests/test_database_lifecycle.cpp
?? tests/test_storage_hardening.cpp
?? tests/test_wal_installation.cpp
?? tests/test_wal_reduction.cpp
?? tools/measure_database_lifecycle.py
?? tools/measure_wal_installation.py
?? tools/measure_wal_reduction.py
```

Tracked diffstat к HEAD включает предыдущую работу, untracked файлы в него не входят:

```text
 .github/workflows/ci.yml                   |  19 +++++
 CMakeLists.txt                             |  18 ++++
 LIMITATIONS.md                             |   9 ++
 core/BufferManager.cpp                     |   4 +
 core/BufferManager.hpp                     |   2 +
 core/DatabaseFileLock.hpp                  | 130 +++++++++++++++++++++++++----
 core/DatabaseRuntime.cpp                   | 106 +++++++++++++++++++----
 core/DatabaseRuntime.hpp                   |  17 +++-
 core/FileManager.cpp                       | 106 +++++++++++++++++++----
 core/FileManager.hpp                       |  19 ++++-
 core/SnapshotBaseline.cpp                  |  17 +++-
 docs/CLI_RU.md                             |  22 ++++-
 docs/OPERATIONS_RU.md                      |  39 +++++++++
 docs/PHYSICAL_RECOVERY_RU.md               |  22 ++++-
 docs/SNAPSHOT_FOUNDATION_RU.md             |  39 ++++++++-
 docs/WAL_IDENTITY_RU.md                    |  12 ++-
 docs/roadmap/STORAGE_RECOVERY_NEXT_RU.md   |   8 ++
 recovery/CheckpointService.cpp             |   4 +
 recovery/RecoveryCoordinator.cpp           |  13 ++-
 storage/DurableFileSync.hpp                |   6 +-
 storage/IoFaultInjector.cpp                |   2 +-
 storage/IoFaultInjector.hpp                |  22 +++++
 storage/SnapshotManifest.cpp               |  16 +++-
 storage/WalIdentity.cpp                    |  21 +++--
 storage/WalIdentity.hpp                    |   4 +
 tests/TestDatabaseFiles.hpp                |   2 +
 tests/test_buffer_manager.cpp              |   4 +
 tests/test_checkpoint_crash.cpp            |  19 ++++-
 tests/test_crash_recovery.cpp              |  17 ++--
 tests/test_database_wal_identity.cpp       |   2 +-
 tests/test_free_page_manager.cpp           |   3 +-
 tests/test_header_recovery.cpp             |   2 +-
 tests/test_main.cpp                        |  40 ++++++++-
 tests/test_metadata_manager.cpp            |   9 +-
 tests/test_page_manager.cpp                |   3 +-
 tests/test_physical_page_recovery.cpp      |   2 +-
 tests/test_physical_recovery_crash.cpp     | 122 +++++++++++++++++++++++++++
 tests/test_safe_checkpoint.cpp             |   6 +-
 tests/test_snapshot_foundation.cpp         | 125 ++++++++++++++++++++++++++-
 tests/test_snapshot_generation.cpp         |   2 +-
 tests/test_snapshot_publication.cpp        |  90 ++++++++++++++------
 tests/test_storage_boundaries.cpp          |   3 +-
 tools/zenthrildb_cli.cpp                   |  23 ++++-
 transaction/StorageMutationCoordinator.cpp |   1 +
 transaction/TransactionManager.cpp         |   6 ++
 wal/LogManager.cpp                         |  10 ++-
 wal/LogManager.hpp                         |   4 +-
 wal/WriteAheadLog.cpp                      |  98 +++++++++++++++++-----
 wal/WriteAheadLog.hpp                      |  18 +++-
 49 files changed, 1134 insertions(+), 154 deletions(-)
```

`git diff --check`: exit 0.

## 27. Reproduction commands

PowerShell из корня проекта. Windows configure/build могут выполняться отдельно, но полный CTest — последовательно:

```powershell
$R = 'C:\Users\User\Desktop\дс аналонг\zenthrildb-push'
Set-Location -LiteralPath $R
$CMake = Join-Path $env:APPDATA 'Python\Python313\site-packages\cmake\data\bin\cmake.exe'
$CTest = Join-Path (Split-Path $CMake) 'ctest.exe'
foreach ($C in @('Debug', 'Release')) {
  $B = "build-21-win/stage7-$C"
  & $CMake -S . -B $B -G 'Visual Studio 18 2026' -A x64 -DZENTHRILDB_BUILD_TESTS=ON
  & $CMake --build $B --config $C --parallel 4
  & $CTest --test-dir $B -C $C --output-on-failure --no-tests=error --timeout 300
  if ($LASTEXITCODE) { throw 'Windows tests failed' }
  & $CMake --install $B --config $C --prefix "build-21/stage7/windows-$($C.ToLower())-install"
}
& "$R\build-21-win\stage7-Debug\Debug\zenthrildb_tests.exe" --lifecycle-only
$Fixture = Join-Path $env:TEMP ('zdb-stage7-benchmark-' + [guid]::NewGuid().ToString('N'))
python tools/measure_database_lifecycle.py build-21/stage7/windows-release-install/bin/zenthrildb.exe build-21-win/stage7-Release/Release/zenthrildb_tests.exe $Fixture --reference build-21/stage6/windows-release-install/bin/zenthrildb.exe
git status --short
git diff --stat
git diff --check
```

Linux — два отдельных clean containers, source read-only, build/test DB в собственном `/tmp`, evidence в `/logs`:

```powershell
foreach ($C in @('Debug', 'Release')) {
  docker run --rm --mount "type=bind,source=$R,target=/src,readonly" --mount "type=bind,source=$R\build-21\stage7,target=/logs" zenthrildb-ci-ubuntu:local bash -eu -c "cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=$C -DZENTHRILDB_BUILD_TESTS=ON; cmake --build /tmp/build -j 3; ctest --test-dir /tmp/build --output-on-failure --no-tests=error --timeout 300; cmake --install /tmp/build --prefix /logs/reproduced-$C-install"
  if ($LASTEXITCODE) { throw 'Linux tests failed' }
}
```

Image: Ubuntu 24.04.5, `zenthrildb-ci-ubuntu:local`, id `sha256:b61ff7136d5dd1a488f7adc0a81ace52389e72af5c0dd41a4512a16254302249`. Внутренние команды те же:

```sh
cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Debug -DZENTHRILDB_BUILD_TESTS=ON
cmake --build /tmp/build -j 3
ctest --test-dir /tmp/build --output-on-failure --no-tests=error --timeout 300
cmake --install /tmp/build --prefix /logs/linux-debug-install
/tmp/build/zenthrildb_tests --lifecycle-only
```

Linux benchmark запускается в отдельном контейнере с Python 3.12. Проверенные CLI и test executable копируются с bind mount в `/tmp/bin-CONFIG` и получают execute bit; Stage 6 reference используется только на closed fixture без marker:

```sh
python3 /src/tools/measure_database_lifecycle.py /tmp/bin-release/current /tmp/bin-release/tests /tmp/NEW-benchmark --reference /tmp/bin-release/reference
g++ -std=c++23 -I/src -Wall -Wextra -Wpedantic -fsyntax-only /src/wal/WriteAheadLog.cpp
```

Один benchmark directory должен быть новым; скрипт отказывается перезаписывать существующий. In-process benchmark — test-only driver, не новый пользовательский CLI command. Windows test-driver fixtures используют обычный temp path, production CLI сохраняет прежний Unicode entry point.

Фактические результаты каждого финального configure/build/CTest/install и workload — exit 0. `git diff --check` — exit 0. Начальный early CTest 3 failed группы сохранён в `early-ctest.log`; исправления и повторный check 8/8 — `check-ctest.log`. Затем выполнены чистые финальные прогоны, включая Windows alias fix. Не использованы результаты Stage 6 как якобы новые Stage 7 тесты. Финальная установка документации обновляет docs у уже проверенных бинарников и не подменяет тестовый прогон.


## 28. Final Stage 7 status

**COMPLETE WITH LIMITATIONS.**

| Definition of Done | Фактическое подтверждение |
|---|---|
| Audit / Git / сохранение изменений | Аудит до правок, status/patch сохранены, commit/push отсутствуют |
| Lifecycle / ownership / single writer | Прежний OS lock расширен capability, зависимостями и operation guard |
| Atomic acquisition / release / stale lock / process crash | Реальные процессы в четырёх конфигурациях; lock sidecars не удаляются |
| FileManager instances / dependency lifetime | Два instance, два WAL, два cache, busy close, revoke/reopen |
| Recovery / reduction / installation / checkpoint conflicts | Busy-owner probes, latch-sensitive operation test, прежние regression группы |
| Lock ordering / deadlock-sensitive paths | Fail-fast ownership operations, bounded process barriers и thread latch |
| I/O / recovery / data integrity | 5 новых Debug failure сценариев и все старые матрицы; побайтные сравнения и подтверждённый commit после crash |
| Windows + Ubuntu Debug/Release | Финальные полные 8/8 в каждой конфигурации |
| Performance / compatibility / documentation | Замеры со Stage 6 reference на безопасной closed fixture, известные ограничения явно описаны |

Это закрытый кандидат безопасности поддерживаемого lifecycle, не production-релиз всей СУБД. Запрещено интерпретировать результаты как multi-writer, неограниченную безопасность custom low-level graphs, dangling references или power-loss proof. Stage 8 автоматически не начинался.
