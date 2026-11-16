# ZenthrilDB

ZenthrilDB — собственный C++23 storage engine для локального хранения. Сейчас это **исследовательская основа**, не готовая СУБД и не production-хранилище.

ZenthrilDB интегрируется с отдельным проектом Zenthril через отдельный процесс и IPC. На этапе испытаний PostgreSQL остаётся основной БД и источником истины, Redis сохраняет сессии, кэш и ограничения запросов. Первый кандидат для испытаний — некритичные журналы событий и модерации. Аккаунты, сообщения и криптографические ключи не переносятся; E2EE не меняется.

## Реализовано

- Формат `.zdb` с фиксированными страницами 8192 байта, little-endian поля и CRC32.
- FileManager, PageManager, FreePageManager, MetadataManager и LRU BufferManager с RAII guard.
- WAL в отдельном `.wal` файле: LSN, типизированные payload, CRC32, checkpoint.
- Page-image WAL payload и `PageImageRecoveryApplier` для ограниченного физического REDO/UNDO страниц.
- Базовый снимок нового поколения с манифестом, независимой проверкой и восстановлением с применением WAL после границы. [Спецификация](docs/SNAPSHOT_FOUNDATION_RU.md).
- Record Manager с атомарной вставкой и удалением, проверкой цепочек записей при открытии и хранением каждой записи в одной странице. Формат описан в [документации](docs/RECORD_FORMAT_RU.md).
- Устанавливаемая утилита `zenthrildb` для работы с таблицами и записями, проверки базы и резервного копирования. [Команды и установка](docs/CLI_RU.md). [Восстановление](docs/OPERATIONS_RU.md).
- Анализ транзакционных границ и устойчивое выделение Transaction ID из WAL; полная ACID-изоляция ещё отсутствует.
- Локальные shared/exclusive блокировки и очередь ожидания с таймаутами; wait-for graph.

## Готовность

В предварительной версии доступны локальный KV CLI, автоматическое восстановление при открытии и многостраничный каталог страниц. На Windows x64 и Ubuntu 24.04 проходят Debug и Release сборки и тесты. Для v1.0 остаются многостраничные значения, B+Tree, SQL, транзакционная изоляция и испытания аппаратной долговечности. Критерии выпуска перечислены в [`docs/RELEASE_ACCEPTANCE_RU.md`](docs/RELEASE_ACCEPTANCE_RU.md).

## Сборка

```powershell
cmake -S . -B build -DZENTHRILDB_BUILD_TESTS=ON
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Дорожная карта: [`docs/ROADMAP.md`](docs/ROADMAP.md). IPC-протокол и критерии прототипа: [`docs/IPC_INTEGRATION.md`](docs/IPC_INTEGRATION.md).

[Единая архитектура ZenthrilDB v1.0 и Fortress‑20](docs/architecture/DATABASE_ARCHITECTURE.md) описывает границы ядра и безопасности, совместимость формата и порядок приёмки.
