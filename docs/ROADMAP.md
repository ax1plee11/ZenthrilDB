# Дорожная карта ZenthrilDB

Развитие исходного C++23 storage engine малыми проверяемыми этапами. Совместимость `.zdb`/`.wal` сохраняется либо описывается отдельной миграцией. Наличие интерфейса или класса не означает production-гарантию.

[Архитектура ZenthrilDB v1.0 и Fortress‑20](architecture/DATABASE_ARCHITECTURE.md) сохраняет порядок ядра и задаёт параллельные меры безопасного хранения. [Карта этапов и приёмки](roadmap/FORTRESS_ROADMAP.md).

| Версия | Статус | Цель |
|---|---|---|
| v0.1 Storage | Complete | Файловый/страничный формат, CRC, FileManager, BufferManager |
| v0.2 Metadata | Complete | Метаданные и резервированные страницы |
| v0.3 Page lifecycle | Complete | Page/FreePage managers |
| v0.4 WAL foundation | Complete | Framing, LSN, CRC |
| v0.5 Recovery analysis | Complete | RecoveryContext/Engine/Plan, checkpoint boundary |
| v0.6 Transaction lifecycle | Foundation | Begin/commit/rollback в WAL; не ACID |
| v0.7 Recovery execution | Foundation | Executor, LSN idempotency, page-image applier |
| v0.8 Typed WAL | Complete | Версионированные payload |
| v0.9 Lock Manager | Foundation | Локальные блокировки и очередь ожидания |
| v0.10 Reliability validation | Partial; release gate remains open | Windows/Ubuntu Debug and Release pass; WAL/page sync, automatic locked startup recovery, persistent PageDirectory coordination through PageManager, DatabaseRuntime ownership, fault injection and selected crash/recovery tests exist. Real ENOSPC/media failure, hardware power loss and broader crash-point coverage remain |
| v0.11 IPC storage prototype | Authorized, gated | Начинать после закрытия reliability gaps; отдельный процесс и IPC v1 с изолированными тестовыми записями |
| v0.12 MVCC foundation | Planned | Видимость версий и snapshot model |
| v0.13 B+Tree foundation | Planned | Страничный индекс |
| v0.14 Catalog and Record Manager | Partial | Версионированные записи, атомарный CRUD и многостраничный каталог страниц. Открыты масштабирование каталога таблиц и free-list, многостраничные значения |
| v0.15 Query Engine MVP | Planned | Логические операции над записями |
| v0.16 SQL Parser MVP | Planned | Ограниченный DDL/DML |
| v0.17 Optimizer foundation | Planned | Простая rule-based оптимизация |
| v1.0 Local DB MVP | Planned | Проверенная сохранность, транзакции, записи и базовый SQL |
| Zenthril shadow trial | Planned, gated | Только некритичные журналы; PostgreSQL authoritative; сверка и rollback |

## Утверждённая граница интеграции

Zenthril и ZenthrilDB — самостоятельные проекты. Канал — отдельный процесс через IPC. PostgreSQL остаётся источником истины, Redis не заменяется. Начальная область — некритичные журналы событий/модерации. Аккаунты, пользовательские сообщения и криптографические ключи не переносятся; E2EE не меняется. Ошибка ZenthrilDB не должна срывать PostgreSQL-запись или основной запрос.

## Порядок

1. Актуализировать спецификации и матрицу гарантий.
2. Проверить durability, crash/reopen и corruption границы.
3. Создать изолированный IPC процесс для синтетических тестовых записей.
4. Подтвердить чтение после остановки и повторного запуска.
5. Только затем добавить Go shadow adapter для некритичных журналов; PostgreSQL остаётся authoritative.
6. Сверить записи и отказоустойчивость, задокументировать результат.

Каждый переход требует зелёной сборки и тестов предыдущего этапа. IPC не реализует SQL и не заменяет PostgreSQL/Redis.
