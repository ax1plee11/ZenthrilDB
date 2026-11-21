# Технический отчёт ZenthrilDB — этап актуализации

**Ревизия:** `57a56f6`, ветка `main`. Код движка не менялся. Документация сверена с исходниками и последними коммитами.

## Проверки

Среда Windows 11, MSVC 19.51.36260.0, Windows SDK 10.0.26100, CMake 4.4.4.

```powershell
cmake -S . -B build-audit -G 'Visual Studio 18 2026' -A x64 -DZENTHRILDB_BUILD_TESTS=ON
cmake --build build-audit --config Debug --parallel 2
cmake --build build-audit --config Release --parallel 2
cmake --build build-audit --config Debug --target RUN_TESTS
```

Фактически: configure exit 0; Debug build exit 0; Release build exit 0; CTest `1/1` passed, 0.57 с. Проверка только Windows x64; Linux CI workflow существует, но здесь не выполнялся. Sanitizer/power-failure test не выполнялся.

## Состояние движка

Реализованы page/file/buffer/metadata/free-page модули, WAL с CRC/LSN/typed payload, checkpoint analysis, page-image applier и локальный lock manager. Recovery test покрывает явный REDO committed page image, UNDO incomplete page image, Page LSN и повторное применение.

Не доказаны полная ACID атомарность, автоматический crash-recovery при штатном открытии файла, durability до устройства, восстановление metadata/free-page транзакций, persistence transaction ID или межпроцессная координация lock. Нет record layer, KV store, B+Tree, MVCC и SQL. Это остаётся storage-engine foundation, не production DBMS.

## Открытые проблемы по приоритету

1. P0 для persistent IPC PUT: определить и обеспечить упорядочение WAL-before-data и настоящий sync-to-device, тестировать crash consistency.
2. P0 для атомарности: привязать page image WAL к транзакциям на всём пути write/commit/rollback; покрыть metadata/free-list.
3. P1: startup recovery coordinator (open WAL → analysis → page redo/undo → flush → ready), restart тест с отдельным процессом.
4. P1: устойчивый transaction ID/LSN без переиспользования после restart.
5. P1: дедлок cycle/timeout/cancel/fairness тесты и стресс/race sanitizer.
6. P2: IPC прототип по `docs/IPC_INTEGRATION.md`; до завершения reliability gate запись остаётся только в изолированной тестовой среде.

## Устаревшая копия

`C:\Users\User\Documents\база данных` — отдельная старшая копия на `bcd2b66` без двух ноябрьских исправлений. Чистая сборка той копии на MSVC 19.51 завершилась ошибками C2440 в `lock/LockManager.cpp:100,251`: поле wait condition имеет тип `condition_variable*`, тогда как реализация использует `condition_variable_any*`. Подключённый к Zenthril источник `zenthrildb-push` содержит правильный согласованный тип и успешно собрался. Старую копию не исправлял.
