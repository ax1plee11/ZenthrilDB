# ZenthrilDB 0.1.0 — Stage 6: Crash-Safe WAL Reduction Installation

## 1. Цель Stage 6

Безопасно выбрать опубликованный reduced-комплект при открытии по исходному пути базы, сохранив исходные файлы и существующее recovery. Статус: **COMPLETE WITH LIMITATIONS**, кандидат Stage 6. Все четыре финальные конфигурации прошли по 7/7 групп; ограничения аппаратной модели и первые неудачные попытки указаны ниже.

## 2. Исходное состояние Stage 5

Репозиторий: `C:\Users\User\Desktop\дс аналонг\zenthrildb-push`, ветка main, HEAD `045fa2762451a125eb3fc5f19bcfc16e345cd699`. Исходное дерево уже содержало 31 modified и 11 untracked путей. Состояние сохранено в `build-21/stage6/status-before.txt`, `before.patch`, `head-before.txt` до правок.

Stage 5 создаёт отдельный v3-комплект: рабочая `.zdb`, `.wal`, `.zdb.baseline`, `.zdb.snapshot`; lock — отдельный долгоживущий `.zdb.lock`. Префикс WAL 60 байт, первый SafeCheckpoint — 88 байт, всего pristine WAL 148 байт. В baseline материализовано состояние на B, сохранён верхний Transaction ID T. Публикация каталога не заменяет источник и сама по себе не переключает его путь.

## 3. Архитектура до изменений и аудит

`FileManager::openDatabase` блокирует путь, читает заголовок/identity, создаёт LogManager и вызывает RecoveryCoordinator до доступа к страницам. DatabaseRuntime затем собирает менеджеры, загружает free-list и metadata, валидирует RecordManager и StorageConsistencyChecker. Имеются низкоуровневый open FileManager и поддерживаемый прикладной open DatabaseRuntime; оба должны выбирать одинаковое поколение.

RecoveryCoordinator сначала проверяет identity, WAL, baseline, план физических образов, затем разрешает ремонт. MetadataManager и FreePageManager пользуются существующим StorageMutationCoordinator. SafeCheckpoint проверяет отсутствие активных транзакций, состояние страниц и WAL, выполняет WAL sync, flush страниц, durable SafeCheckpoint. Он не переключает файловый комплект.

Stage 5 использует приватный staging и атомарную публикацию соседнего каталога. POSIX: fsync файлов и каталогов, renameat2 NOREPLACE. Windows: FlushFileBuffers, MoveFileExW WRITE_THROUGH, отдельного fsync каталога нет. В fault injector предусмотрены process-crash и exception hooks, включённые в Debug. Существующие тесты Stage 2–5 сохранены.

GitNexus impact для FileManager сообщил неоднозначность: класс — 12 затронутых символов, MEDIUM, constructor — UNKNOWN. DatabaseRuntime не найден в доступном индексе, UNKNOWN. Это не считалось all-clear; связи проверены по текущим исходникам. Ruflo/ToolSearch не предоставлены среди доступных инструментов. После read-only аудита план описан владельцу до реализации.

## 4. Обнаруженные риски

- Поочерёдное переименование четырёх файлов может смешать поколения; обычный rename одного файла этого не решает.
- Простое переименование каталога кандидата не переключает прежний путь `.zdb`.
- Давно созданный кандидат может не содержать последующие подтверждённые изменения источника.
- Подмена marker по факту существования, выбор по mtime или fallback на старый источник после установки могут потерять новые записи.
- Runtime, низкоуровневый FileManager, snapshot и verification должны согласованно разрешать выбранное поколение и удерживать блокировки.
- Старые бинарники Stage 2–5 не знают о селекторе и не должны использоваться для записи в архивный исходник после установки.

## 5. Реализованные изменения

Добавлены `WalInstallation` — компактный версионированный селектор, `DatabaseRuntime::installReduction(source,candidate)` и CLI `install-wal SOURCE.zdb CANDIDATE.zdb`. Это не автоматический запуск reduction при open и не новый формат `.zdb`/WAL.

FileManager разрешает committed-селектор до recovery и удерживает два lock: логический source и выбранный физический target. Идентичность target сравнивается с селектором до REDO/UNDO и ремонта хвоста. Runtime использует физический путь для WAL и backup. Snapshot/reduction/verify по логическому пути также используют выбранное поколение. Существующий независимый verify-baseline без селектора сохраняет прежнее поведение.

## 6. Installation protocol

Требования: bound source, pristine reduced v3 candidate, одинаковое имя файла в разных каталогах, доверенные локальные родительские каталоги. Legacy source следует сначала мигрировать в bound snapshot. Source, который требует ремонта/исправления хвоста, отклоняется: сначала явно выполнить обычное recovery, затем проверить актуальность кандидата.

1. Получить эксклюзивные source и candidate lock. Чтение выполняется read-only.
2. Проверить источник штатным WAL/recovery preflight и полной проверкой страниц, metadata, free-list и record chains, без ремонта исходника.
3. Проверить candidate штатным preflight/baseline/manifest. Требуются ровно один кадр WAL, reduced v3, тот же UUID, другое поколение, B равный последнему полному LSN источника и T равный верхней отметке источника.
4. Побайтно сравнить сериализованный файловый заголовок и все страницы, кроме страницы 0 с намеренно новым поколением. Несовпадение запрещает установку. Источник после подготовки не изменяется.
5. Синхронизировать четыре candidate-файла и каталог. Новые копии больших файлов не создаются.
6. Создать эксклюзивный `.zdb.install.pending-RANDOM` в родительском каталоге источника. Записать committed-селектор, проверить close/flush, системный sync, перечитать и проверить CRC/структуру/точное соответствие записанному состоянию.
7. Атомарно опубликовать `.zdb.install` без замены существующего файла. Затем синхронизировать родительский каталог на POSIX. Windows применяет WRITE_THROUGH, но отдельного directory-fsync нет.
8. Освободить подготовительные lock и открыть обычный DatabaseRuntime по исходному пути. Он выбирает target, запускает обычное recovery и consistency scan. Ошибка после публикации не удаляет селектор и не включает старый source.

Формат селектора v1, little-endian: magic ZDIS (4), version=1 (4), state=COMMITTED=1 (4), source WalIdentity (60), target WalIdentity (60), source data bytes (8), source WAL bytes (8), sourceHasBaseline (4), UTF-8 target path length (4), абсолютный нормализованный target path (1–4096), CRC32 (4). Всего 160 + длина пути. Неизвестные версии/состояния, flags, неверный CRC, длины, нулевые/конфликтующие identity, неподходящий путь отклоняются.

Полный путь выбран явно; цепочки селекторов запрещены. Селектор immutable, только одна установка на логический source. Повтор с тем же target проходит штатную проверку уже установленного поколения и сохраняет последующие commits. Другой target отклоняется. Автоматической смены уже выбранного поколения нет.

## 7. State machine

| Состояние | Файлы и операции | После crash |
|---|---|---|
| IDLE | Source выбран своим путём, marker отсутствует; reduced может быть опубликован Stage 5 | Обычный source open, candidate автоматически не выбирается |
| PREPARING | Source/target lock, read-only validation, сравнение данных и B/T | Source прежний; повтор preflight |
| VALIDATED | Candidate проверен и синхронизирован; source не изменяется | Source прежний; повтор validation/sync |
| PENDING | Частичный/полный временный селектор; committed marker отсутствует | Source прежний; pending никогда не выбирается и не исполняется |
| SWITCHED | Полный committed marker опубликован одним rename | Выбирается только указанный target; проверяется identity, запускается штатное recovery |
| VERIFIED / COMPLETE | Обычное открытие и проверка target завершены | Повтор open детерминирован; selector не переписывается |
| FAILED_SAFE | До rename marker отсутствует либо существующий marker/target не прошёл валидацию | До установки — source; после установки — отказ без fallback |

PREPARING/VALIDATED/VERIFIED — логические, а не лишние постоянные маркеры. Единственное постоянное состояние — committed record; его наличие без успешного разбора не является разрешением на запись. Source и его WAL/baseline/manifest не удаляются ни в одном переходе. Отдельный rollback marker не нужен: до rename ещё выбран source, после rename нельзя откатываться поверх новых commits.

## 8. Crash recovery behavior

При обычном open сначала проверяется точный `.install`. Если его нет, нет сканирования поколений, выбора по времени или reduction. Если он есть, проверяются CRC, source identity/размеры/наличие исходных sidecar, однозначность target. После target lock проверяется его header/identity против committed record; дальше работают неизменённые WAL identity validation и RecoveryCoordinator.

Незавершённый временный селектор не является намерением, которое нужно автоматически завершать: операция до committed rename ещё не установлена. Повтор install заново проверяет source/candidate, создаёт новый временный файл и публикует authority. Оставшиеся pending-файлы не мешают выбору и не удаляются автоматически при open.

После rename повторный open выполняет ограниченный page-image REDO/UNDO с baseline и Page LSN, как прежде. Выбранные UUID/generation/B не меняются при повторе. Повтор install не требует pristine target, если тот уже выбран этим селектором: последующие записи сохраняются. При повреждении target/selector нет fallback на устаревший исходник.

## 9. Source preservation

Исходные четыре файла в тестах сравниваются до/после подготовки, установки, отказа, process crash, двух открытий и дальнейшей записи через alias. Селектор и lock являются отдельными вспомогательными файлами. Источник остаётся архивным комплектом, а новые операции через исходный логический путь меняют только target.

Для проверки наличия архивного источника при open читаются его header/identity и размеры, проверяется наличие sidecar, если они были при установке. Это не полная повторная проверка CRC всех архивных страниц/WAL на каждом open. Полная проверка сделана до установки. Подлинность архива против злоумышленника не заявляется. Исходник не удаляется автоматически даже после COMPLETE.

## 10. Validation

Штатные проверки сохраняются: CRC headers/pages/WAL/manifest, UUID, generation, B, T, монотонность LSN, Page LSN, целостность baseline, metadata, page directory, free-list и RecordManager. До marker publication дополнительно проверяется логическое/физическое совпадение с source и отсутствие stale candidate. Новая authority-запись защищена собственной CRC и ограниченным parser.

При каждом обычном open добавлен только поиск `.install`; обычный recovery остаётся прежним. При marker — ограниченное чтение селектора и source header, затем recovery выбранного комплекта. Полная source validation, сравнение всех страниц и sync candidate выполняются только install, не при каждом open.

## 11. Failure injection

Использован существующий IoFaultInjector. Новые hook: prepared, validated, staging created, write, partial write, flush, sync, durable marker, rename, switched, directory sync, parent durable, ordinary open, verified, cleanup.

Debug-матрица включает 14 process-crash переходов и 14 соответствующих I/O exception сценариев. Дополнительно — ошибка sync именно временного marker (пятая InstallSync occurrence после четырёх файлов), ENOSPC и ошибка cleanup вместе с partial write: всего 17 I/O сценариев. Два отдельных process crash прерывают repair выбранного target через source alias, затем повторный recovery доводит все страницы до побайтного эталона. Итого 16 process-crash сценариев Stage 6 на Debug-платформу.

Crash — реальный аварийный выход дочернего процесса с обязательным кодом 91, не exception. При частичной записи создаётся реальный короткий файл. System-call mid-flight, физическое ENOSPC и отключение питания так не моделируются. Исходные Stage 5 crash/I/O сценарии создания reduced-комплекта продолжают запускаться в Regression.

## 12. Тесты

Новая группа `ZenthrilDBInstallation`, файл `tests/test_wal_installation.cpp`:

- Unit: roundtrip selector, все усечения и одиночный бит в каждом байте, проверка CRC/структуры.
- Integration: две таблицы, бинарные и пустые значения, updates/deletes/checkpoint, durable BEGIN без COMMIT, source v2 baseline, reduction→installation→обычный open.
- Данные: точные значения и scan, два открытия, запись после установки, checkpoint/backup, snapshot/reduction по alias, сохранение post-install commit при повторном install.
- Source preservation: полный побайтный образ source-комплекта, включая baseline/manifest; целевой комплект и marker также сравниваются при отказах.
- Freshness: stale candidate после новой подтверждённой записи отклоняется без marker. Конкурирующий runtime запрещает install; direct target runtime запрещён, пока alias удерживает target lock. Конфликтующий второй корректный кандидат не выбирается.
- 18 negative сценариев: повреждённый/усечённый/удлинённый selector; CRC WAL/manifest; отсутствие target/source/data/WAL/baseline/manifest; generation/B mismatch с пересчитанным CRC selector; цепочка selector; неправильный target filename; изменение размера исходного WAL; неполная обязательная граница target.
- Recovery: реальный обрыв ремонта и повторный recovery выбранного target, без повторного изменения корректного файла или отката на source.

Assertions в Release сохранены через UNDEBUG. Инъекции crash/I/O включены только Debug; Release не считается доказательством этих инъекций. Все шесть групп Stage 2–5 сохранены, добавлена седьмая.

## 13. Windows results

Windows x64, Visual Studio 18 2026, MSVC 19.51, Windows SDK 10.0.26100.0. Сборки в отдельных `build-21-win/stage6-Debug` и `stage6-Release`. Финальная полная матрица запущена последовательно, без изменения assertions и таймаутов.

| Build | Passed | Failed | Время |
|---|---:|---:|---:|
| Debug | 7/7 | 0 | 181.17 с |
| Release | 7/7 | 0 | 78.05 с |

Configure/build/CTest/install финальных прогонов — exit 0. Установленный CLI обеих конфигураций прошёл installation workload, post-install commit, retry, verify-snapshot, SHA-256 source. UTF-8 путь marker также проверен: Windows fixtures находятся внутри рабочего каталога с кириллицей.

Первые параллельные Windows-прогоны завершились 6/7: общая ZenthrilDBTests получила Timeout, прочие группы прошли. В логе последней строкой был test_database_runtime. Индивидуальный повтор Release с тем же таймаутом прошёл за 30,06 с; затем оба полных последовательных прогона прошли. Точная причина начального замедления не установлена; оно не скрыто и не объявлено доказанно только ресурсной конкуренцией. Логи неуспешных прогонов `windows-{debug,release}-ctest.log`, повторов — `windows-final-{debug,release}-ctest.log`, индивидуальный — `windows-release-core-retry.log`.

Ранний Windows benchmark попытался запустить ещё линкуемый EXE и получил WinError 32; последующее ожидание установленного EXE истекло после failed CTest, потому что install закономерно не выполнялся. Финальный workload запускается только после успешных CTest/install; оба прошли. Начальные неудачные артефакты сохранены, реальные пользовательские базы не использовались.

## 14. Ubuntu results

Ubuntu 24.04.5 x86_64, GCC 13.3, CMake 3.28.3, образ `zenthrildb-ci-ubuntu:local`. Две отдельные чистые сборки в `/tmp/build`, исходники read-only; временные базы внутри контейнера. В текущей сессии configure/build/CTest/install завершились с кодом 0.

| Build | Passed | Failed | Время |
|---|---:|---:|---:|
| Debug | 7/7 | 0 | 307,57 с |
| Release | 7/7 | 0 | 67,61 с |

Installation: Debug 43,00 с, Release 4,38 с. Установленный CLI обеих конфигураций прошёл workload `create/create-table/put/erase/checkpoint/reduce-wal/install-wal/check/scan/get/verify-snapshot`, повтор install после post-install commit и проверку SHA-256 source. GCC syntax/warnings на `WalInstallation.cpp`, `WalReductionInstallation.cpp`, `FileManager.cpp`, `DatabaseRuntime.cpp` с `-Wall -Wextra -Wpedantic -fsyntax-only`: exit 0, диагностик нет. Это не whole-program static proof.

Логи: `build-21/stage6/linux-{debug,release}-{configure,build,ctest,install}.log`, `linux-{debug,release}-commands.jsonl`, `gcc-syntax.log`.

## 15. Debug results

Windows и Ubuntu Debug прошли полные 7/7 групп. Новая Installation выполняет 16 реальных process-crash, 17 управляемых I/O сценариев, 18 corruption cases, parser truncation/bit tests, точные сравнения данных и повторное recovery. Stage 5 и более ранние crash/I/O наборы также прошли в Regression. Инъекции собираются только в Debug.

## 16. Release results и производительность

Windows и Ubuntu Release прошли полные 7/7 групп с включёнными assertions. Проверены функциональные, integrity, negative и регрессионные сценарии, обычный install/open/retry и CLI. Debug-only injection из этих результатов не выводится.

`tools/measure_wal_installation.py` создаёт новый каталог: 20 записей по 128 байт, update/delete, checkpoint, reduction, измеряет install и по три open/check до/после. Проверяет все значения через scan, запись после установки, idempotent retry и SHA-256 source. Scope времени — CLI startup+open+recovery+check, не чистое время системных вызовов.

| OS / Build | Install, мс | Open source, мс | Open reduced напрямую, мс | Open через selector, мс | Selector, байт |
|---|---:|---:|---:|---:|---:|
| windows / debug | 287.87 | 263.30 | 75.10 | 81.88 | 287 |
| windows / release | 258.13 | 238.85 | 77.89 | 88.55 | 289 |
| linux / debug | 682.98 | 1001.30 | 102.34 | 115.75 | 201 |
| linux / release | 381.64 | 434.53 | 69.28 | 81.62 | 203 |

Медианы open — по 3 запуска, сырые samples и stdout/stderr/exit code в `build-21/stage6/*measurements.json`, `windows-*-benchmark-verified/measurements.json` и соответствующих `commands.jsonl`. Источник SHA-256 сохранён во всех четырёх workload. Дополнительное разрешение selector имеет измеримую стоимость относительно прямого открытия reduced-пути; ускорение самого open не заявляется. Эти данные не являются сравнением с бинарником Stage 5 на идентичном чистом хосте. Для обычной базы без marker архитектурно добавляется один metadata probe, копирования и полная проверка source не выполняются. На загруженном desktop стенде нет гарантированного latency SLO.

Во время install нет копирований больших файлов. Новая запись занимает 160 + UTF-8 длина target path байт, pending переименовывается в committed record. При успехе остаётся один marker; при исключении cleanup может оставить маленький pending. Перед switch явно синхронизируются четыре файла candidate и marker — 5 file sync, плюс 2 directory sync на POSIX. Final ordinary open использует существующие recovery/close barriers дополнительно; Windows directory-fsync не реализован. Верхняя оценка нового staging — 4256 байт на попытку, без учёта FS metadata и уже созданного Stage 5 комплекта. Повтор install не создаёт новую копию данных или marker.

## 17. Известные ограничения

- Кандидат Stage 6 покрывает process-crash и управляемые I/O отказы, не hardware/power-loss/OS-crash. Успешные write/close не объявляются долговечностью; системный flush вызывается, аппаратный контроллер отдельно не проверен.
- Linux — Ubuntu 24.04 в Docker Desktop с временными базами на `/tmp` контейнера, не отдельный физический Ubuntu-host.
- Trusted local directories обязательны. Нет полной TOCTOU-защиты при конкурентной подмене путей; Windows права наследуются от ACL родителя, POSIX staging 0600.
- CRC — не MAC/подпись. Нет защиты от удаления/отката целого selector+комплекта доверенным администратором или злоумышленником с правом записи.
- Старые бинарники не распознают selector. После установки нельзя использовать их для записи в архивный source или переносить исходную пару без `.install`, рассчитывая получить новые записи. Backup активного runtime создаёт самостоятельный target-комплект; исходный архив отдельно сохраняется.
- Абсолютный путь target фиксирован. Перемещение каталогов требует отдельной процедуры, автоматического переноса marker нет.
- Смена уже установленного target, chains, automatic cleanup source/pending и автоматическая rollback-политика не добавлены. При ошибке после commit выдаётся ошибка при уже установленном target; проверять `check`/`verify-snapshot`, не удалять marker для повтора.
- Для установки требуется pristine v3 candidate и source, уже допускающий read-only recovery validation. Неполный source tail или необходимые ремонты требуют явного обычного recovery до установки.
- Реальные permission-denied, full-disk, mid-syscall kill и аппаратные ошибки требуют отдельного стенда. Вне Stage 6 не добавлены SQL/MVCC/B+Tree/Server Protocol.

## 18. Оставшиеся проблемы

До production-оценки нужны OS/power-loss и файловые стенды, переносимый handle-based backend, политика архивов/миграции selector, внешняя anti-rollback и криптографическая аутентификация, строгие эксплуатационные процедуры для старых бинарников. Наличие текущего кандидата не означает готовность всей СУБД к production-релизу.

## 19. Изменённые файлы

Новые: `storage/WalInstallation.hpp`, `storage/WalInstallation.cpp`, `core/WalReductionInstallation.cpp`, `tests/test_wal_installation.cpp`, `tools/measure_wal_installation.py`, этот отчёт.

Изменения Stage 6 в существующих: `core/FileManager.hpp`, `.cpp`, `core/DatabaseRuntime.hpp`, `.cpp`, `storage/IoFaultInjector.hpp`, `.cpp`, `tools/zenthrildb_cli.cpp`, `tests/test_main.cpp`, `CMakeLists.txt`, `docs/CLI_RU.md`, `docs/OPERATIONS_RU.md`, `LIMITATIONS.md`. Предыдущие изменения не удалены. Начальный patch и status сохранены отдельно; полный status ниже включает предыдущие этапы.

## 20. Git status

Ветка **main**, HEAD **045fa2762451a125eb3fc5f19bcfc16e345cd699**. Ни один исходный dirty путь не исчез. Из прежде чистых tracked файлов дополнительно изменён `core/FileManager.hpp`; шесть новых файлов Stage 6 перечислены выше. Остальные изменения текущего этапа добавлены к уже существовавшей работе.

Полный текущий status:

```text
 M .github/workflows/ci.yml
 M CMakeLists.txt
 M LIMITATIONS.md
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
 M tests/test_checkpoint_crash.cpp
 M tests/test_main.cpp
 M tests/test_physical_recovery_crash.cpp
 M tests/test_snapshot_foundation.cpp
 M tests/test_snapshot_publication.cpp
 M tools/zenthrildb_cli.cpp
 M transaction/TransactionManager.cpp
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
?? storage/SafeFilePath.hpp
?? storage/StorageLimits.hpp
?? storage/WalInstallation.cpp
?? storage/WalInstallation.hpp
?? tests/test_storage_hardening.cpp
?? tests/test_wal_installation.cpp
?? tests/test_wal_reduction.cpp
?? tools/measure_wal_installation.py
?? tools/measure_wal_reduction.py
```

Полный tracked diffstat относительно HEAD, включая предыдущие этапы (новые untracked файлы в нём не учитываются):

```text
 .github/workflows/ci.yml                 |  19 +++++
 CMakeLists.txt                           |  13 ++++
 LIMITATIONS.md                           |   7 ++
 core/DatabaseFileLock.hpp                |  20 ++++-
 core/DatabaseRuntime.cpp                 |  96 +++++++++++++++++++++---
 core/DatabaseRuntime.hpp                 |  17 ++++-
 core/FileManager.cpp                     |  58 ++++++++++++---
 core/FileManager.hpp                     |   3 +
 core/SnapshotBaseline.cpp                |  17 ++++-
 docs/CLI_RU.md                           |  22 +++++-
 docs/OPERATIONS_RU.md                    |  35 +++++++++
 docs/PHYSICAL_RECOVERY_RU.md             |  22 +++++-
 docs/SNAPSHOT_FOUNDATION_RU.md           |  39 +++++++++-
 docs/WAL_IDENTITY_RU.md                  |  12 ++-
 docs/roadmap/STORAGE_RECOVERY_NEXT_RU.md |   8 ++
 recovery/CheckpointService.cpp           |   3 +
 recovery/RecoveryCoordinator.cpp         |   7 +-
 storage/DurableFileSync.hpp              |   6 +-
 storage/IoFaultInjector.cpp              |   2 +-
 storage/IoFaultInjector.hpp              |  18 +++++
 storage/SnapshotManifest.cpp             |  16 +++-
 storage/WalIdentity.cpp                  |  21 ++++--
 storage/WalIdentity.hpp                  |   4 +
 tests/test_checkpoint_crash.cpp          |  19 ++++-
 tests/test_main.cpp                      |  28 ++++++-
 tests/test_physical_recovery_crash.cpp   | 122 ++++++++++++++++++++++++++++++
 tests/test_snapshot_foundation.cpp       | 123 +++++++++++++++++++++++++++++++
 tests/test_snapshot_publication.cpp      |  90 ++++++++++++++++------
 tools/zenthrildb_cli.cpp                 |  23 +++++-
 transaction/TransactionManager.cpp       |   6 ++
 wal/WriteAheadLog.cpp                    |  58 +++++++++++----
 wal/WriteAheadLog.hpp                    |  10 ++-
 32 files changed, 848 insertions(+), 96 deletions(-)
```

`git diff --check`: exit 0. Исходный patch/status сохранены; автоматический commit/push не выполнялись.

commit: **NOT CREATED**. push: **NOT PERFORMED**. user changes: **PRESERVED**. Reset/clean/checkout с потерей изменений не использовались.

## 21. Команды повторения тестов

PowerShell из корня репозитория, Windows Debug/Release последовательно:

```powershell
$R = 'C:\Users\User\Desktop\дс аналонг\zenthrildb-push'
Set-Location -LiteralPath $R
$CMake = Join-Path $env:APPDATA 'Python\Python313\site-packages\cmake\data\bin\cmake.exe'
$CTest = Join-Path (Split-Path $CMake) 'ctest.exe'
foreach ($C in @('Debug', 'Release')) {
  $B = "build-21-win/stage6-$C"
  & $CMake -S . -B $B -G 'Visual Studio 18 2026' -A x64 -DZENTHRILDB_BUILD_TESTS=ON
  & $CMake --build $B --config $C --parallel 3
  & $CTest --test-dir $B -C $C --output-on-failure --no-tests=error --timeout 300
  & $CMake --install $B --config $C --prefix "build-21/stage6/windows-$($C.ToLower())-install"
}
python tools/measure_wal_installation.py build-21/stage6/windows-release-install/bin/zenthrildb.exe build-21/stage6/NEW-workload
git status --short
git diff --stat
git diff --check
```

Ubuntu configure/build/CTest/install выполнялись в двух отдельных контейнерах; `/src` — read-only bind корня репозитория, `/logs` — bind `build-21/stage6`, собственный `/tmp` каждого контейнера. Образ: `zenthrildb-ci-ubuntu:local` (`sha256:b61ff7136d5dd1a488f7adc0a81ace52389e72af5c0dd41a4512a16254302249`). Внутри:

Полная команда запуска из PowerShell с определённым выше `$R` (оба новых контейнера; без изменения пользовательских баз):

```powershell
foreach ($C in @('Debug', 'Release')) {
  docker run --rm --mount "type=bind,source=$R,target=/src,readonly" --mount "type=bind,source=$R\build-21\stage6,target=/logs" zenthrildb-ci-ubuntu:local bash -eu -c "cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=$C -DZENTHRILDB_BUILD_TESTS=ON; cmake --build /tmp/build -j 3; ctest --test-dir /tmp/build --output-on-failure --no-tests=error --timeout 300; cmake --install /tmp/build --prefix /logs/reproduced-$C-install"
  if ($LASTEXITCODE) { throw 'Linux validation failed' }
}
```

Содержимое команд внутри контейнера отдельно:

```sh
cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Debug -DZENTHRILDB_BUILD_TESTS=ON
cmake --build /tmp/build -j 3
ctest --test-dir /tmp/build --output-on-failure --no-tests=error --timeout 300
cmake --install /tmp/build --prefix /logs/linux-debug-install
# Новый отдельный контейнер: те же команды с Release и linux-release-install.
```

Для отдельных стадий используются те же собранные тесты:

```sh
./zenthrildb_tests --installation-only
./zenthrildb_tests --reduction-only
./zenthrildb_tests --snapshot-only
./zenthrildb_tests --checkpoint-only
./zenthrildb_tests --security-only
./zenthrildb_tests --hardening-only
```

Workload установленного Linux CLI выполнялся в отдельном контейнере после установки Python 3.12: бинарник копировался в собственный `/tmp/bin-CONFIG` и получал execute bit, затем:

```sh
python3 /src/tools/measure_wal_installation.py /tmp/bin-release/zenthrildb /tmp/NEW-workload
g++ -std=c++23 -I/src -Wall -Wextra -Wpedantic -fsyntax-only /src/storage/WalInstallation.cpp
```

Команды остановлены при неуспехе; финальные configure/build/CTest/install и workload возвращают exit 0. Ранняя Linux сборка завершилась exit 2 из-за отсутствующего include `DatabaseWalPath.hpp` в новом translation unit; include добавлен, выполнены новые чистые сборки. Ни исходная compile error, ни Windows timeout не переименованы в success. Все выводы и версии команд хранятся в `build-21/stage6/`. После окончательного обновления документации install дополнительно обновляет docs, используя уже проверенные бинарники; это не новый тестовый прогон.

## 22. Обоснованный статус Stage 6

**COMPLETE WITH LIMITATIONS — закрытый кандидат Stage 6 в описанной модели process-crash и контролируемых I/O отказов.**

| Definition of Done | Доказательство |
|---|---|
| Audit / план / compatibility | Изучены реальные entry points и платформенные операции; `.zdb`/WAL не изменены; прежние тесты сохранены |
| Отдельный reduced / source preservation | Stage 5 сохранён; source четыре файла побайтно сравниваются, не удаляются |
| Validation / identity / generation / B / T / integrity | Read-only preflight + baseline + точное сравнение страниц и marker CRC |
| Installation / deterministic recovery | Immutable committed selector; отсутствующий pending не выбирается; damaged committed state fail-closed |
| Idempotency / отсутствие повторного изменения данных | Два open, retry после нового commit, два process crash внутри repair, сравнение образов |
| Crash / I/O | 16 process-crash и 17 I/O сценариев Stage 6 на каждой Debug-платформе |
| Logical data | Бинарные/пустые значения, scan, update/delete, post-install commit, alias backup/reduction/verify |
| Regression / Windows Debug+Release / Ubuntu Debug+Release | По 7/7 групп в финальных полных прогонах |
| Documentation / Git safety | Этот отчёт, CLI/operations/limitations; status сохранён, commit/push отсутствуют |

Production-релиз всей СУБД, аппаратная долговечность, автоматическое удаление source или последующая смена установленного поколения этим заключением не разрешаются. Начальные Windows timeout и ограничения доверенных путей/старых бинарников остаются явно описанными эксплуатационными рисками. Нельзя выдавать process-crash стенд за проверку отключения питания.
