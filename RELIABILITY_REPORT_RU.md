# ZenthrilDB — отчёт по этапу 2: надёжность и восстановление

**Состояние на 2026-11-13:** частично выполнено; к IPC-прототипу и релизу не готово. Работа основана на `main` HEAD `57a56f6` с сохранёнными незакоммиченными изменениями. Рабочие базы Zenthril не затрагивались; испытания выполнялись на временных файлах.

## Исправлено

- WAL больше не открывается с `trunc` после ошибки открытия существующего файла.
- Добавлена системная синхронизация WAL и страниц: Windows `FlushFileBuffers`, Linux `fsync`; проверяются ошибки вызова и они возвращаются вызывающему коду. Создание WAL синхронизирует файл и каталог.
- `begin`, `commit`, `rollback` и checkpoint используют сериализованный append+sync путь. Ошибка COMMIT/rollback после записи переводит транзакцию в in-doubt; LogManager блокирует дальнейшие записи после ошибки I/O/sync.
- WAL восстанавливает только неполный последний frame до последнего целого frame; повреждение CRC/формата отклоняется. WAL compaction переписывается через синхронизированный temp-файл и атомарную замену; транзакционные lifecycle записи сохраняются для high-water Transaction ID.
- `RecoveryCoordinator` автоматически запускается внутри `FileManager::openDatabase()` после межпроцессной блокировки и до выдачи файлового handle вызывающему коду; ошибки recovery закрывают базу.
- Откатанные транзакции теперь направляются в UNDO; некорректный порядок BEGIN/COMMIT/ROLLBACK/page-image отклоняется. Recovery обрабатывает журнал до checkpoint, пока checkpoint не является полным протоколом согласования страниц.
- В тестовом target Release не отключает `assert`.

## Файлы

Изменены WAL (`wal/WriteAheadLog.*`, `wal/LogManager.*`), системная синхронизация (`storage/DurableFileSync.hpp`), страницы (`core/FileManager.*`), транзакции (`transaction/*`), recovery (`recovery/RecoveryEngine.cpp`, новый `recovery/RecoveryCoordinator.*`), CMake и тесты (`tests/test_wal.cpp`, `test_recovery.cpp`, `test_crash_recovery.cpp`, `test_lock_manager.cpp`, `test_main.cpp`). Обновлены `docs/WAL_SPEC.md`, `TRANSACTION_SPEC.md`, `RECOVERY_SPEC.md`, `ROADMAP.md`, `PROJECT_TRUTH.md`, `LIMITATIONS.md`.

## Проверки

Windows x64 / Visual Studio 18 2026:

```powershell
python -m cmake -S . -B build-21-win -G "Visual Studio 18 2026" -A x64 -DZENTHRILDB_BUILD_TESTS=ON
python -m cmake --build build-21-win --config Debug --parallel 4
ctest --test-dir build-21-win -C Debug --output-on-failure
python -m cmake --build build-21-win --config Release --parallel 4
ctest --test-dir build-21-win -C Release --output-on-failure
```

Linux Ubuntu 24.04 container, GCC 13.3 (`zenthrildb-ci-ubuntu:local`):

```sh
cmake --build build-21 --target zenthrildb_tests -j2
ctest --test-dir build-21 --output-on-failure
cmake --build build-21-release --target zenthrildb_tests -j2
ctest --test-dir build-21-release --output-on-failure
```

Текущая ревизия: Windows Debug и Release — сборки успешны, CTest 1/1 прошёл в обеих конфигурациях; Ubuntu 24.04 Debug и Release — сборки успешны, CTest 1/1 прошёл в обеих конфигурациях. Suite включает аварийное завершение отдельного процесса на выбранных WAL/page/metadata шагах, принудительное прерывание startup recovery, повторный recovery, torn WAL tail, CRC, Page LSN, отсутствующий WAL, propagation sync errors, конкурентный WAL и lock deadlock/waiters. Это подтверждает только перечисленные тестовые сценарии.

## Оставшиеся ограничения и готовность

Не моделировались реальное исчерпание места, отказ диска/контроллера и потеря питания. Нет полной изоляции транзакций или интеграции LockManager с завершением транзакций. `PageManager` теперь сохраняет directory вместе со страницей/free-list в одной coordinated mutation; `DatabaseRuntime` владеет графом и проверяет consistency при старте. Низкоуровневый `FileManager::writePage()` остаётся незажурналированным методом, но runtime не предоставляет его приложению. CRC обнаруживает часть повреждений, но не лечит произвольные повреждённые страницы.

**Подтверждаемая граница:** успешный `commit()` синхронизирует commit record через системный API локальной ОС. Координированные page/header/catalog mutations используют WAL-before-data, durable commit и recovery. Это не гарантия сохранности аппаратного кэша при отключении питания и не покрывает вызовы низкоуровневых API в обход runtime. IPC остаётся отложенным до более широких crash/fault и application-level тестов.
