# ZenthrilDB — Этап 2.1: статус реализации

Дата: 2026-11-13. Этап остаётся частично закрытым; IPC и перенос пользовательских данных не начинались.

## Рабочая копия

- Репозиторий: `C:\Users\User\Desktop\дс аналонг\zenthrildb-push`.
- В рабочем дереве сохранены незакоммиченные изменения предыдущих этапов; сбросов и checkout не выполнялось.
- WAL базы использует документированную схему sibling: `database.zdb` → `database.wal`.

## Реализовано

- `DatabaseFileLock`: Windows `LockFileEx`, Linux `flock`, эксклюзивно на срок жизни открытого `FileManager`.
- Безопасное создание базы без усечения существующих `.zdb` и `.wal`; WAL создаётся при инициализации.
- Автоматический `RecoveryCoordinator` внутри `FileManager::openDatabase()` до возврата вызывающему коду; отсутствующий WAL или ошибка проверки закрывает базу.
- WAL type 13 `DatabaseHeaderImageWritten`: полный before/after заголовок с контрольной суммой; recovery делает REDO/UNDO и усекает файл при откате увеличения числа страниц.
- `StorageMutationCoordinator` использует переданные существующие `LogManager` и `TransactionManager`; он подключён к metadata, free-list и page-manager операциям. Страницы и заголовок попадают в общий WAL перед записью данных; COMMIT синхронизируется до успешного возврата. При ошибке файловый handle блокируется до закрытия и нового открытия.
- `StorageConsistencyChecker`: CRC, физические ID, Page LSN, состояние транзакций/WAL, directory, free-list и table roots.
- `PageManager` принимает `MetadataManager` и записывает PageDirectory в тот же WAL mutation, что пользовательскую страницу и free-list; free entries сохраняются как `PageStatus::Free`.
- Добавлен `DatabaseRuntime`, который владеет FileManager/WAL/TM/coordinator/менеджерами, загружает/инициализирует metadata и проводит consistency check до выдачи API. Второй `StorageMutationCoordinator` для того же `FileManager` отклоняется.
- Публичный DatabaseRuntime предоставляет только page/metadata API, не выдавая приложению низкоуровневые `FileManager::writePage()` и `reservePageId()`.
- Debug-only `IoFaultInjector`: частичная WAL-запись, write/sync ошибки, синтетический ENOSPC. В Release fault hooks выключены.
- Process crash-тесты проверяют откат создания таблицы после ошибок на каждом из четырёх page-write участков; повторное открытие проверяет metadata и consistency.
- Runtime-тест проверяет ошибку посреди PageManager allocation: после закрытия/повторного открытия undo восстанавливает directory/free-list и consistency checker проходит.
- Документы этапа обновлены.

## Проверки

- Ubuntu 24.04 / GCC 13.3 в `zenthrildb-ci-ubuntu:local`: Debug build и CTest прошли; текущая версия тестов включает crash/fault сценарии.
- Ubuntu 24.04 / GCC 13.3: текущие Debug build и CTest прошли повторно; текущие Release build и CTest прошли повторно.
- Windows x64 / Visual Studio 18 2026: Debug и Release build прошли; CTest прошёл в обеих конфигурациях (1/1).
- Crash-тесты включают принудительное завершение после page writes и внутри recovery через синхронизационный барьер; повторное открытие/восстановление проверяется тестами.
- Не проверены реальное исчерпание места, отказ носителя/контроллера и физическая долговечность при отключении питания.

## Оставшиеся блокеры

- Низкоуровневые manager constructors и FileManager API остаются публичными для внутренних и тестовых сценариев; production code должен использовать `DatabaseRuntime`, который не выдаёт прямой доступ к файловым write/reserve API.
- Fault injection проверяет пропагацию управляемых ошибок, но не воспроизводит исчерпание диска ОС или отказ оборудования.
- Реальные ENOSPC и аппаратные power-loss проверки недоступны в текущем тестовом окружении; ограничения модели отказов должны оставаться явно задокументированы.

Reliability foundation продвинут, но полный release blocker остаётся: отсутствуют Record Manager/KV API и прикладной набор сценариев; hardware-level durability не проверена. К IPC/production-интеграции пока не переходить.
