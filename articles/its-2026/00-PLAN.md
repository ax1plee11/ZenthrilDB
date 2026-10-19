# План статей для ITS (БГУИР) по диссертации ZenthrilDB

**Базовый проект:** `C:\Users\User\Documents\база данных` (ZenthrilDB)  
**Связанный проект (не смешивать в одну статью):** `C:\Users\User\Desktop\дс аналонг` (Zenthril messenger)  
**Конференция:** XVI ITS-2026, https://its.bsuir.by/ru/  
**Ориентир по оформлению (по ITS-2025):** тезисы ~2 полные страницы, LaTeX по шаблону ITS, языки RU/BE/EN.

## Почему выбран ZenthrilDB, а не Zenthril

1. Диссертация уже сформулирована вокруг storage engine ZenthrilDB.
2. Для первой статьи нужны аналоги + лит./патентный обзор + цель/задачи — это глава 1–2 диссертации.
3. Zenthril (мессенджер) лучше оставить для отдельной серии (realtime, E2EE, gateway), без смешения с page/WAL/CRC.

## Серия статей (по частям)

| № | Рабочая тема | Что входит | Что НЕ входит |
|---|---|---|---|
| **01** | Аналоги, обзор, цель и задачи (ZenthrilDB) | SQLite / RocksDB / PostgreSQL, источники, цель, задачи | Код, бенчмарки, детали WAL/locks |
| **02** | E2EE от протокола до хранилища (Zenthril) | key bundles, OTPK, X3DH/DH, envelopes, PostgreSQL, durability | полный Signal-audit, бенчмарки |
| 03 | Формат `.zdb` и CRC32 | layout страниц, header, CRC | recovery, SQL |
| 04 | Buffer/Page/FreePage managers | LRU, pin/unpin, free list | deadlock |
| 05 | WAL и transaction foundations | LSN, typed payload, состояния транзакций | полный crash redo |
| 06 | Lock manager foundation | shared/exclusive, timeout, RAII | distributed locks |

**Автор (статьи 02):** Хожиев Нурбек Абдулхат угли  
**Word:** `Desktop\Хожиев_E2EE_Zenthril_ITS.docx` и `articles/its-2026/02-e2ee-protocol-to-storage.docx`

## Статус

- [x] Выбор проекта для статьи 01: **ZenthrilDB**
- [x] Черновик статьи 01: `01-analogues-lit-goals.md`
- [x] Статья 02 (E2EE): Word готов, кафедра не указана (по запросу)
- [ ] Кафедра / научный руководитель — добавить позже
- [ ] Перенос в LaTeX-шаблон ITS после публикации шаблона ITS-2026
- [ ] Список литературы уточнить по требованиям кафедры
