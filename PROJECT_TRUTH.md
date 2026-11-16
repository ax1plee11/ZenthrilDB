# Фактическое состояние ZenthrilDB

Чистые сборки Stage 5 проверены на Windows x64 с Visual Studio 18 2026 и Ubuntu 24.04 с GCC 13.3, в Debug и Release. Во всех четырёх конфигурациях прошли четыре группы CTest. Проект предоставляет локальный KV CLI и остаётся предварительной версией 0.1.0. [Критерии релиза](docs/RELEASE_ACCEPTANCE_RU.md).

| Область | Состояние | Граница доказательства |
|---|---|---|
| Формат страниц | 8192 байта, little-endian, CRC32 | CRC обнаруживает часть повреждений, но не восстанавливает произвольный файл |
| WAL | CRC/LSN, fail-closed на CRC, trim неполного EOF-tail, durable append API | Системный flush протестирован на двух ОС; аппаратный power-loss не проверен |
| Transaction lifecycle | BEGIN/COMMIT/ROLLBACK sync до успешного возврата; in-doubt при sync failure; IDs из WAL | Гарантия lifecycle record, не атомарность произвольных page mutations |
| Страницы | FileManager flush вызывает ОС sync; PageManager атомарно обновляет страницу, PageDirectory и free-list через общий coordinator | Необёрнутый `FileManager::writePage()` пишет без WAL и не даёт транзакционной гарантии |
| Recovery | Автоматически вызывается из `FileManager::openDatabase()` после межпроцессного lock; PageImage и DatabaseHeaderImage REDO/UNDO | Гарантия применяется к мутациям через общий StorageMutationCoordinator; прямой low-level `FileManager::writePage()` не журналируется |
| Crash-тесты | Отдельные процессы и повторное открытие, частичная запись WAL, прерывание восстановления и публикации снимка, инъекции write/sync/ENOSPC в тестовых конфигурациях | ENOSPC моделируется исключением. Проверка физического отказа остаётся отдельной. |
| Межпроцессная блокировка | Windows `LockFileEx`, Linux `flock`, блокировка на срок жизни FileManager | Проверены сборкой и тестами на Windows и Ubuntu; внешние файловые системы отдельно не проверялись |
| Storage consistency | Checker сверяет страницы/CRC/Page LSN, валидность WAL и транзакций, metadata/directory/free-list/root pages; `DatabaseRuntime` проверяет базу при старте | Низкоуровневые constructors остаются доступны для внутренних тестов; app API — DatabaseRuntime |
| Снимок | Базовый образ, манифест, проверка страниц и публикация нового поколения. Восстановление применяет образы из WAL после границы | Полный WAL требуется для сверки префикса. Сокращение истории запланировано отдельно. |
| Верхний слой | Record Manager и KV CLI. B+Tree, MVCC и SQL в дорожной карте | Каждая запись занимает одну страницу. Пользовательская транзакционная изоляция остаётся открытой. |
| Zenthril IPC | Не реализовано | Спецификация есть, reliability gate остаётся открытым |
