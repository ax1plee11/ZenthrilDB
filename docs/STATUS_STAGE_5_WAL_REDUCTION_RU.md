# Stage 5. Safe WAL Reduction Candidate

## 1. Executive Summary

**Итог: Safe WAL Reduction Candidate реализован и проверен на Windows и Ubuntu, Debug/Release — по 6/6 групп.** Новая команда создаёт отдельный комплект v3, исходная база и её история сохраняются. В воспроизводимом workload WAL кандидата уменьшен с 3 581 068 до 148 байт с восстановлением всех страниц и сохранением данных. Это завершение этапа кандидата, не объявление production-релиза всей СУБД.

Задание выполняется на рабочем дереве ZenthrilDB, ветка main, исходный HEAD 045fa27. До изменений записаны Git status, HEAD и binary diff в build-21/wal-reduction/. Дерево уже содержит пользовательские изменения hardening и результаты предыдущих этапов. Commit не создаётся.

## 2. Исходное состояние и аудит

LogManager назначает LSN через nextLsn и восстанавливает следующий номер из последней проверенной записи WAL. WriteAheadLog проверяет структуру кадров, CRC, монотонность, UUID, поколение и snapshot LSN. Неполный EOF-tail сохраняется до проверки плана восстановления и затем может быть исправлен. Полный кадр с ошибкой CRC отклоняется.

Commit, BEGIN и rollback проходят через TransactionLogAdapter и синхронизированный appendWithLsn. Ошибки commit/rollback классифицируются как неоднозначный исход. TransactionManager до изменения ищет верхний Transaction ID среди lifecycle-записей. Без сохранения этой отметки сокращение может привести к повторному использованию ID.

RecoveryCoordinator проверяет идентичность, baseline и полный план до ремонта. RecoveryEngine связывает полные page/header images с BEGIN и исходом COMMIT/ROLLBACK. REDO использует after-image, допустимый UNDO before-image. При наличии снимка физический план содержит только записи после его границы, страницы берутся из проверенного baseline. Исторический префикс всё ещё нужен для проверки границы и счётчиков.

CheckpointService проверяет отсутствие активных транзакций, согласованность страниц и плана, синхронизирует WAL, dirty pages, затем SafeCheckpoint. Это сохраняет полную историю. Сам по себе checkpoint не разрешает удаление образов страниц.

DatabaseRuntime сериализует прикладные операции recursive mutex, FileManager удерживает межпроцессную блокировку. BufferManager может содержать dirty pages и не является отдельной гарантией commit. Поддерживаемая точка входа использует runtime и StorageMutationCoordinator. Отдельный LockManager пока не обеспечивает пользовательскую транзакционную изоляцию.

Snapshot v1 и v2 создают полный baseline, манифест и новый комплект в закрытом соседнем каталоге. v2 требует sidecar-файлы при открытии. Backup сохраняет существующий комплект побайтно. Публикация через rename без замены, Windows FlushFileBuffers/MoveFileExW и Linux fsync/renameat2 уже реализована. Эти пути будут переиспользованы.

Графовый impact-анализ WalIdentity вернул UNKNOWN из-за недоступной LadybugDB. Реальные зависимости проверяются по исходникам и регрессионным тестам. Старый индекс из Documents/база данных не используется как доказательство текущего кода.

## 3. Safe WAL Boundary и дизайн до реализации

### Дополнение аудита WAL

`LogManager::nextLsn` назначает LSN под mutex, хранит следующий номер в памяти, после reopen восстанавливает его по последнему полному проверенному кадру. Нулевой/максимальный LSN и немонотонные кадры отклоняются. Сохранённые кадры не перенумеровываются. Необязательные пропуски между последующими кадрами сами по себе не означают повреждение: неуспешный append мог израсходовать номер. В v3 первый LSN B+1, напротив, обязан присутствовать точно. Полностью утраченный неподтверждённый/неполный хвост не является подтверждённой историей; отсутствие переиспользования его номеров не заявляется.

LogRecord имеет 36-байтовый заголовок: magic, version, type, LSN, timestamp, payload size, CRC. CRC проверяется до принятия кадра, длины ограничены до выделения payload. Типы 1–8 — существующие системные/legacy события, metadata и старый checkpoint; 9–11 — BEGIN/COMMIT/ROLLBACK; 12 — полный before/after image страницы; 13 — полный before/after image заголовка; 14 — SafeCheckpoint. Транзакционные образы связаны с transaction ID, заголовки также проверяются на UUID. Для REDO нужны подтверждённые after-images, для UNDO незавершённых изменений — проверенные before-images и lifecycle. Одного informational metadata event для ремонта физических страниц недостаточно.

Append проверяет stream write; durable append дополнительно выполняет stream flush и системную синхронизацию. Ошибка sync не подтверждает commit и запрещает дальнейшую запись текущим LogManager до reopen. Неполный EOF-tail откладывается до проверки плана, полный CRC-bad кадр отклоняется. Закрытие stream само по себе не считается durable commit.

SafeCheckpoint синхронизирует WAL, dirty pages, затем свою запись; удерживает границу предыдущего WAL и T, но не освобождает старые образы. Несколько SafeCheckpoint живут в одном поколении, а не выбирают отдельные файлы. Recovery продолжает анализировать историю или suffix после проверенного baseline; последний checkpoint не служит разрешением удалить прошлое. Snapshot v1/v2 содержит физический baseline, но сохраняет историю; backup только копирует комплект и не вычисляет новую границу. Поэтому сокращение допустимо лишь после нового материализованного baseline, а не по номеру последнего checkpoint.

Под эксклюзивной блокировкой источника копия проходит штатное recovery и полную проверку runtime. B определяется последним полным проверенным LSN после восстановления этой копии. Все незавершённые изменения разрешены в её физическое состояние, а верхний Transaction ID T включает их BEGIN. Отсутствующий полный образ или неоднозначность запрещает создание кандидата.

Планируется идентичность v3, 60 байт, с обязательным baseline и признаком сокращённой истории. Новый каталог получит новое поколение, тот же UUID и snapshot LSN B. Единственная начальная запись WAL использует существующий формат SafeCheckpoint. Её LSN B+1, durableWalLsn B, transactionHighWater T и идентичность нового поколения. Такая запись не объявляет старые транзакции подтверждёнными.

Reader v3 обязан найти целую запись границы первой, проверить точный LSN B+1, UUID и поколение. Её отметка T сохраняется в манифесте и используется для будущих Transaction ID. Новые LSN начинаются после B+1. Неполная или повреждённая запись границы отклоняется, её нельзя заменить пустым WAL. Page LSN baseline не превышает B.

Снимок или повторный reduction из v3 должен сначала материализовать актуальное состояние и создавать новую границу. Утраченная история не объявляется полной. Новый режим создаётся отдельной командой reduce-wal SOURCE NEW_DIRECTORY. Подмена исходных файлов и автоматическое удаление исходной истории в candidate workflow отсутствуют.

## 4. Publication Protocol и Failure Model

Переиспользуется существующий snapshot staging, его fault hooks и атомарная публикация каталога. Дополнительно нужны точки после вычисления B и T, неполной записи нового кадра и завершения записи кадра. До публикации destination отсутствует, после публикации содержит проверенные четыре файла. Ошибка завершающей синхронизации возвращается вызывающему даже при уже опубликованном комплекте.

Сбой до публикации оставляет исходный проверенный комплект. Незавершённый staging автоматически не выбирается. Повтор выполняется после диагностики в новом или том же свободном назначении. Существующее назначение защищено от замены. Неудача удаления собственных временных файлов может оставить диагностируемый staging, но не разрешает его открытие как опубликованного результата.

Модель включает exception, управляемые write/sync/ENOSPC/rename failures и аварийный выход дочернего процесса. OS crash, power loss и неисправность контроллера требуют отдельного стенда. CRC обнаруживает повреждения в модели, а криптографическая подлинность и защита от отката комплекта остаются отдельными задачами.

## 5. План приёмки

Проверить пустой и минимальный журнал, завершённые и незавершённые транзакции, повтор reduction, snapshot/backup/checkpoint после reduction, чтение прежних форматов, монотонность LSN и Transaction ID, CRC/UUID/generation/неполную границу, повторный ремонт всех страниц. Расширить существующую subprocess-матрицу публикации и crash recovery на v3. Выполнить Windows/Ubuntu Debug/Release, CLI, размеры и время на воспроизводимом workload.

Разделы 1–5 записаны до реализации. Их исходная версия сохранена отдельно в `build-21/wal-reduction/design-original.md`. Ниже приведён итог реализации; будущие формулировки в дизайне описывают первоначальный план.

## 6. Архитектура Reduction и публикация

Добавлен `DatabaseRuntime::reduceWalTo(source, newDirectory)` и CLI `reduce-wal`. Общий внутренний `createSnapshot` используется обоими режимами: второго WAL writer или механизма публикации нет. Прямой `truncate` связанного WAL остаётся запрещённым. Формат `.zdb` и кадров LogRecord не изменён; новая семантика требует отдельной версии **WalIdentity v3**. Читатели v1/v2 отклоняют её, а новый читатель сохраняет поддержку legacy/v1/v2.

Инварианты кандидата:

1. Под исходным межпроцессным lock staged-копия проходит обычный `DatabaseRuntime`, startup recovery и проверку metadata, page directory, free-list и записей. Конкурирующий runtime источника запрещает операцию.
2. B — последний полный проверенный LSN восстановленной копии. Неполный EOF-tail разрешается на копии, источник не ремонтируется. B=0 допустим только для проверенного пустого журнала. B не берётся из timestamp, размера WAL или случайного checkpoint.
3. T включает все BEGIN, в том числе без COMMIT, и сохранённые high-water поля SafeCheckpoint. Все изменения победителей и проигравших уже разрешены в физический baseline. В кандидате нет транзакции, пересекающей B.
4. UUID сохраняется, generation меняется. Обязательная идентичность v3 записывается в страницу 0, WAL и manifest. Флаги v3 строго равны 3; v2 — 1; v1 — 0.
5. WAL содержит преамбулу 60 байт и стандартный SafeCheckpoint: заголовок 36 + payload 52 байта. Итого **148 байт**. Первый LSN строго B+1, durableWalLsn=B, transactionHighWater=T. Это граница baseline, а не фиктивный COMMIT.
6. Manifest прежнего размера 116 байт связывает идентичность, T, размеры и CRC baseline и 148-байтового префикса. Baseline проверяется независимо, включая Page LSN≤B, каталоги и free-list. Отсутствие обоих sidecar не снимает обязательность baseline.
7. Файлы закрываются с проверкой ошибок. WAL, data, baseline и manifest синхронизируются, полный комплект проверяется через `verifySnapshot`, затем синхронизируется staging и публикуется соседний каталог без замены существующего назначения.
8. POSIX использует fsync файлов/каталогов и no-replace rename; Windows — FlushFileBuffers и MoveFileExW WRITE_THROUGH. Затем выполняется завершающий барьер родителя на поддерживаемой платформе. На Windows отдельный directory-fsync отсутствует.
9. Исходные WAL, база и baseline не заменяются и не удаляются. Старый комплект остаётся доступен для отката. Удаляются только собственные неопубликованные временные файлы; ошибки cleanup не превращают основной отказ в успех.

Следующий LogManager назначает LSN после B+1; TransactionManager начинает после T. Checkpoint сохраняет T даже до первой новой транзакции. В recovery из baseline сначала восстанавливается состояние на B, затем обрабатывается suffix; первая граничная запись не создаёт физических действий. Обычный snapshot из v3 материализует новую границу и сохраняет v3, поскольку старую историю восстановить невозможно. Backup копирует полный комплект побайтно.

Повтор с тем же существующим назначением отклоняется. Повтор в свободное назначение после отказа до rename разрешён и проверен. После ошибки завершающей синхронизации опубликованный каталог уже может существовать: требуется `verify-snapshot`, затем `check`; повтор не перезаписывает его.

## 7. Failure Model

| Отказ | Реакция и граница доказательства |
|---|---|
| Обычное завершение | Публикуется проверенный комплект; источник сохранён |
| Process crash до rename | Публичного назначения нет; возможен отдельный `.snapshot-*`, который автоматически не выбирается |
| Process crash после rename | Новый комплект существует целиком; проверяется и дважды открывается |
| Exception, write/close/sync, ENOSPC | Отказ возвращается; источник побайтно сохранён; Debug проверяет управляемые инъекции |
| Частичный первый кадр v3 | Отказ до ремонта; нельзя превратить в пустой WAL |
| Неполный suffix после целой границы | Штатный preflight recovery, затем допустимое исправление EOF-tail; граница сохраняется |
| Полный кадр с неверным CRC/LSN/UUID/generation/checkpoint | Отказ, а не скрытое усечение |
| Отсутствующий/повреждённый baseline или manifest | Открытие, verify и reduction отклоняются, файлы сохраняются |
| Rename отказ / существующее назначение | Замены назначения нет; источник остаётся исходным |
| Ошибка финального sync после rename | Возвращается ошибка при уже опубликованном проверяемом комплекте |
| Отказ удаления staging | Может остаться мусор; исходный recovery baseline не удаляется. Реальный отказ unlink отдельным системным стендом не проверен |
| Нет доступа | Открытие/создание/синхронизация возвращают ошибку; реальные ACL/permission-denied сценарии не исчерпаны |
| Параллельное открытие | Межпроцессный lock; конкурентный reducer отклоняется |
| OS crash / power loss / отказ накопителя | В текущем стенде не воспроизводились; успешный write не объявляется долговечностью |

Системные sync вызываются, но достоверность аппаратного cache flush, батарейного кэша и контроллера не доказана. Linux проверен в контейнере с собственным `/tmp` на Docker Desktop, не на отдельном физическом Ubuntu-хосте. Каталоги должны принадлежать доверенному владельцу; path-based I/O не обеспечивает полной защиты от TOCTOU. CRC не аутентифицирует WAL и не предотвращает откат всего комплекта.

## 8. Реализованные изменения

| Файлы относительно корня репозитория | Изменение текущего этапа |
|---|---|
| `storage/WalIdentity.hpp`, `.cpp` | Идентичность v3, строгие флаги и проверка пространства LSN |
| `wal/WriteAheadLog.cpp` | Обязательная первая граница v3, запрет ремонта неполной границы, проверка LSN/счётчика |
| `storage/SnapshotManifest.cpp`, `core/SnapshotBaseline.cpp` | Валидация компактного префикса и сохранённого T, обязательный baseline |
| `transaction/TransactionManager.cpp`, `recovery/CheckpointService.cpp` | Сохранение верхней отметки Transaction ID через checkpoint и reopen |
| `core/DatabaseRuntime.hpp`, `.cpp` | Общий builder snapshot/reduction, материализация, новое поколение, прежняя публикация |
| `storage/IoFaultInjector.hpp`, `.cpp` | Три дополнительных управляемых точки reduction |
| `tools/zenthrildb_cli.cpp` | Команда reduce-wal и help/аргументы |
| `tests/test_wal_reduction.cpp` | Новый функциональный/негативный/integrity набор |
| `tests/test_snapshot_publication.cpp` | Прежняя матрица расширена на v3, v1/v2 сохранены |
| `tests/test_physical_recovery_crash.cpp` | Baseline, REDO, UNDO и повторный ремонт v3 |
| `tests/test_checkpoint_crash.cpp`, `tests/test_main.cpp` | Checkpoint-crash для v3 и отдельная группа Reduction |
| `CMakeLists.txt`, `.github/workflows/ci.yml` | Шестая CTest-группа, проверки установленного CLI в CI |
| `tools/measure_wal_reduction.py` | Воспроизводимый workload и физическое восстановление тестовых копий |

Не изменялись алгоритмы MVCC/SQL/B+Tree, серверный протокол, API приложения Zenthril или реальные рабочие базы. Используется существующее физическое восстановление. Версия продукта остаётся 0.1.0; формат identity v3 не означает релиз продукта v3.

## 9. Тесты

`ZenthrilDBReduction` выполняет пустой legacy WAL, минимальную базу, несколько транзакций, удаление записей, checkpoint, BEGIN без COMMIT, v1/v2 snapshot→reduction, повторный reduction, snapshot из v3, backup, новые записи и монотонность счётчиков. Занятый источник и существующее назначение отклоняются.

Сравнение данных не ограничено существованием файлов: проверяются все байты заголовка и страниц за исключением страницы 0 с намеренно новым поколением. После удаления всех страниц кроме identity страница metadata, page directory, free-list и записи восстанавливаются, затем весь файл сравнивается с исходным ожидаемым образом и дважды переоткрывается.

17 негативных вариантов: CRC кадра; усечение; преамбула без границы; duplicate LSN; UUID/generation/T/durableWalLsn checkpoint; пропущенный первый LSN; неправильный тип первой записи; CRC baseline; CRC manifest; потеря baseline; потеря обоих sidecar; подмена UUID/generation преамбулы с корректным CRC; отсутствующий WAL. Для каждого проверяются отказ runtime/verify/reduction, отсутствие опубликованного назначения и побайтная сохранность всех существовавших файлов.

Все прежние пять CTest-групп сохранены. Assertions включены и в Release. Инъекции аварий и ошибок ввода-вывода собираются только в Debug; Release не объявляется подтверждением этих инъекций.

## 10. Crash Injection

Для v3 в каждой Debug-платформе выполняются **32 реальных аварийных выхода дочернего процесса**:

- 18 публикационных: до вызова, после возврата, 13 прежних staging/синхронизация/manifest/rename точек и 3 новых — boundary, половина граничного кадра, запись кадра;
- 6 физических: baseline, REDO и UNDO, по два обрыва на двух повреждённых страницах, затем два нормальных открытия;
- 8 checkpoint: до начала, preparing, WAL durable, pages durable, частичный кадр, buffered frame, durable frame, после возврата.

Проверяется конкретный ожидаемый ненулевой exit code; исключение не заменяет process crash. После каждого соответствующего сценария проверяются данные, WAL, baseline/manifest и повторный recovery. Помимо этого для публикации v3 — **26 I/O-инъекций**: 13 прежних фаз, 3 reduction-фазы и 10 системных/файловых отказов. Исходник остаётся побайтно прежним. Старые v1/v2 publication и checkpoint/recovery матрицы продолжают запускаться отдельно.

Точки до/после удаления старого WAL не добавлялись: такого удаления в реализованном протоколе нет. Не проверялось принудительное завершение внутри исполняющегося системного fsync/FlushFileBuffers или физическое заполнение носителя.

## 11. Windows Validation

Windows x64, Visual Studio 18 2026, MSVC 19.51, Windows SDK 10.0.26100.0. Отдельные деревья `build-21-win/wal-reduction-Debug` и `build-21-win/wal-reduction-Release`. Configure, build, CTest, install и CLI запускаются в текущей сессии.

| Финальная конфигурация | CTest | Время |
|---|---|---:|
| Windows Debug | 6/6, 0 failures | 313,79 с |
| Windows Release | 6/6, 0 failures | 115,30 с |

Оба configure/build/CTest/install завершились с кодом 0. CLI-smoke из актуального CI прошёл в обеих конфигурациях с кодом 0. В Debug прошли инъекции v3; Release проверил обычный функциональный, негативный и регрессионный набор. Логи `build-21/wal-reduction/windows-final-{debug,release}-*.log`, `windows-{debug,release}-cli-smoke.log`.

Изначальный параллельный прогон подхватил старый `test_main.obj` после изменения сигнатуры тестового helper и завершился LNK2019. Причина: исходник изменился, пока компилировался прежний вариант объекта; timestamp объекта оказался новее. Обновлена метка исходника без изменения содержимого и выполнены повторные build/полный CTest. Неудачные логи сохранены в `windows-debug-build.log` и `windows-release-build.log`; это не скрыто и не относится к Storage Engine.

## 12. Linux Validation

Ubuntu 24.04.5 x86_64 в `zenthrildb-ci-ubuntu:local`, GCC 13.3, CMake 3.28.3. Исходники монтировались read-only, build и временные базы находились в `/tmp` контейнера. Debug и Release собирались отдельно с нуля.

| Финальная конфигурация | CTest | Время |
|---|---|---:|
| Ubuntu Debug | 6/6, 0 failures | 197,41 с |
| Ubuntu Release | 6/6, 0 failures | 50,58 с |

Configure, build, CTest и install завершились с кодом 0 в обеих конфигурациях. Установленный CLI обеих конфигураций прошёл `create/table/put/get/check/checkpoint/snapshot v1/v2/reduce-wal/verify/backup/reduction again`, повторное открытие и проверку SHA-256 источника. Логи: `build-21/wal-reduction/linux-final-{debug,release}-*.log`, `linux-cli-smoke.log`.

Более ранние текущие прогоны тоже прошли (186,10/52,34 с), но не включали последние восемь v3 checkpoint-crash сценариев. Итог опирается на финальные прогоны выше, а не на цифры предыдущих этапов.

## 13. Performance Measurements

Workload: 40 ключей по 128 байт, 10 обновлений, 5 удалений, checkpoint; 45 страниц. Скрипт создаёт только новый каталог. Для измерения recovery три независимые копии до и после reduction усекаются до заголовка и identity page; обычный CLI восстанавливает остальные страницы. Восстановленный файл сравнивается побайтно и проверяются все записи.

| Показатель | Windows Release | Ubuntu Release |
|---|---:|---:|
| WAL до | 3 581 068 байт, 373 кадра | 3 581 068 байт, 373 кадра |
| WAL кандидата | 148 байт, 1 кадр | 148 байт, 1 кадр |
| Исторических кадров заменено в кандидате | 373; чистое уменьшение 372 | 373; чистое уменьшение 372 |
| Reduction | 1 338,33 мс | 888,88 мс |
| Checkpoint | 1 389,02 мс | 1 174,16 мс |
| Recovery до, медиана 3 запусков | 1 445,25 мс | 1 162,41 мс |
| Recovery после, медиана 3 запусков | 528,85 мс | 307,39 мс |

Это wall-clock CLI: создание процесса + open + физический repair + consistency scan. Это не чистый I/O benchmark и не сравнение производительности двух ОС. Измерения выполнены при параллельных сборках, поэтому время зависит от нагрузки. Результат применим к данному workload, не доказывает ускорения всех операций. WAL исходника не удалён; общий расход диска с сохранённым источником и новым baseline растёт. Уменьшается WAL нового комплекта, не суммарное хранение всех резервных версий.

Сырые замеры: `windows-benchmark/measurements.json`, `linux-measurements.json`; каждая CLI-команда, exit code, stdout/stderr и время записаны в `windows-benchmark/commands.jsonl` и `linux-benchmark-commands.jsonl` внутри `build-21/wal-reduction/`.

## 14. Ограничения и статический анализ

- Это отдельный candidate workflow, не online compaction и не автоматическая сборка мусора. Для запуска нужен эксклюзивно доступный источник и свободное место для копии, baseline и временного WAL.
- Если источник превышает существующие лимиты WAL/страниц или не имеет однозначного восстановления, reduction отклоняется. Нет обхода лимитов за счёт удаления неизвестной истории.
- Header/identity page остаются корнем доверия; поломка всех доступных копий или носителя автоматически не исправляется. Обязательный baseline должен храниться вместе с текущими файлами.
- Нет шифрования, MAC, подписанного manifest или внешнего anti-rollback состояния. Нельзя заявлять криптографическую подлинность WAL.
- Нет новой многокомандной транзакционной изоляции или MVCC. Имеющаяся сериализация runtime и блокировки сохранены.
- Power-loss/OS-crash/storage-failure, реальные ENOSPC/ACL/unlink failures и защита от враждебного конкурентного изменения путей требуют дополнительного стенда.

Запущен GCC `-Wall -Wextra -Wpedantic -fanalyzer -c` на семи изменённых production translation units. Компиляция завершилась, но analyzer выдал предупреждения в шаблонах libstdc++ deque/allocator/unordered_map; чистый analyzer не заявляется. Минимальный пример только со стандартными deque/unordered_map, без ZenthrilDB, также воспроизводит предупреждения use-of-uninitialized-value и leak. Диагностика и reproducer сохранены в `gcc-analyzer-objects.log`, `gcc-analyzer-stl-repro.log`, `analyzer-stl-repro.cpp`. Это ограничение анализа GCC/STL, не доказательство отсутствия всех ошибок проекта. Предупреждения не отключались. Первоначальная отдельная проверка CLI без build-time `ZENTHRILDB_VERSION` завершилась ошибкой макроса; CLI успешно собирается штатным CMake с определением версии.

GitNexus impact и Ruflo не были доступны как рабочий графовый анализ текущего репозитория; отсутствие результата не считалось успешной проверкой. Проверены реальные исходники и зависимости. `git diff --check` выполняется отдельно.

## 15. Изменённые файлы и документация

Корень всех путей: `C:\Users\User\Desktop\дс аналонг\zenthrildb-push`.

Код текущего этапа перечислен в разделе 8. Обновлены `docs/WAL_IDENTITY_RU.md`, `docs/CLI_RU.md`, `docs/OPERATIONS_RU.md`, `docs/SNAPSHOT_FOUNDATION_RU.md`, `docs/roadmap/STORAGE_RECOVERY_NEXT_RU.md`, `LIMITATIONS.md`. Созданы этот отчёт, `tests/test_wal_reduction.cpp` и `tools/measure_wal_reduction.py`. Исходные документы критериев релиза сохранены. В полном Git status ниже также присутствуют изменения предыдущих этапов: они не объявляются выполненными заново.

## 16. Git Status

Ветка **main**, HEAD **045fa2762451a125eb3fc5f19bcfc16e345cd699**. Commit/push не создавались. Reset/clean не выполнялись. Начальные Git status и patch сохранены до реализации в `build-21/wal-reduction/git-status-before.txt`, `head-before.txt`, `tracked-before.patch`.

Сравнение начального и конечного status: ни один исходный путь не исчез. Дополнительно изменены ранее чистые `core/SnapshotBaseline.cpp`, `recovery/CheckpointService.cpp`, `storage/IoFaultInjector.cpp`, `storage/IoFaultInjector.hpp`, `tests/test_checkpoint_crash.cpp`, `transaction/TransactionManager.cpp`. Добавлены три файла текущего этапа. Остальные изменённые пути уже были dirty и до этого задания.

Полный status (включая прежнюю работу):

```text
 M .github/workflows/ci.yml
 M CMakeLists.txt
 M LIMITATIONS.md
 M core/DatabaseFileLock.hpp
 M core/DatabaseRuntime.cpp
 M core/DatabaseRuntime.hpp
 M core/FileManager.cpp
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
?? docs/DATABASE_VERIFICATION_2026-11-20_RU.md
?? docs/STATUS_2026-11-20_RU.md
?? docs/STATUS_STAGE_5_WAL_REDUCTION_RU.md
?? storage/SafeFilePath.hpp
?? storage/StorageLimits.hpp
?? tests/test_storage_hardening.cpp
?? tests/test_wal_reduction.cpp
?? tools/measure_wal_reduction.py
```

Полный tracked diffstat относительно HEAD (включает предыдущие этапы; untracked файлы в нём не учитываются):

```text
 .github/workflows/ci.yml                 |  19 +++++
 CMakeLists.txt                           |   7 ++
 LIMITATIONS.md                           |   5 ++
 core/DatabaseFileLock.hpp                |  20 ++++-
 core/DatabaseRuntime.cpp                 |  63 ++++++++++++++--
 core/DatabaseRuntime.hpp                 |  16 +++-
 core/FileManager.cpp                     |  19 +++--
 core/SnapshotBaseline.cpp                |  17 ++++-
 docs/CLI_RU.md                           |  10 ++-
 docs/OPERATIONS_RU.md                    |  33 +++++++++
 docs/PHYSICAL_RECOVERY_RU.md             |  22 +++++-
 docs/SNAPSHOT_FOUNDATION_RU.md           |  39 +++++++++-
 docs/WAL_IDENTITY_RU.md                  |  12 ++-
 docs/roadmap/STORAGE_RECOVERY_NEXT_RU.md |   8 ++
 recovery/CheckpointService.cpp           |   3 +
 recovery/RecoveryCoordinator.cpp         |   7 +-
 storage/DurableFileSync.hpp              |   6 +-
 storage/IoFaultInjector.cpp              |   2 +-
 storage/IoFaultInjector.hpp              |   3 +
 storage/SnapshotManifest.cpp             |  16 +++-
 storage/WalIdentity.cpp                  |  21 ++++--
 storage/WalIdentity.hpp                  |   4 +
 tests/test_checkpoint_crash.cpp          |  19 ++++-
 tests/test_main.cpp                      |  19 ++++-
 tests/test_physical_recovery_crash.cpp   | 122 ++++++++++++++++++++++++++++++
 tests/test_snapshot_foundation.cpp       | 123 +++++++++++++++++++++++++++++++
 tests/test_snapshot_publication.cpp      |  90 ++++++++++++++++------
 tools/zenthrildb_cli.cpp                 |  16 +++-
 transaction/TransactionManager.cpp       |   6 ++
 wal/WriteAheadLog.cpp                    |  58 +++++++++++----
 wal/WriteAheadLog.hpp                    |  10 ++-
 31 files changed, 726 insertions(+), 89 deletions(-)
```

## 17. Фактические команды и воспроизведение

В PowerShell из корня репозитория:

```powershell
$R = 'C:\Users\User\Desktop\дс аналонг\zenthrildb-push'
Set-Location -LiteralPath $R
$CMake = Join-Path $env:APPDATA 'Python\Python313\site-packages\cmake\data\bin\cmake.exe'
$CTest = Join-Path (Split-Path $CMake) 'ctest.exe'
foreach ($C in @('Debug', 'Release')) {
  $B = "build-21-win/wal-reduction-$C"
  & $CMake -S . -B $B -G 'Visual Studio 18 2026' -A x64 -DZENTHRILDB_BUILD_TESTS=ON
  & $CMake --build $B --config $C --parallel 3
  & $CTest --test-dir $B -C $C --output-on-failure --no-tests=error --timeout 300
  & $CMake --install $B --config $C --prefix "build-21/wal-reduction/windows-final-$($C.ToLower())-install"
}
python tools/measure_wal_reduction.py build-21-win/wal-reduction-Release/Release/zenthrildb.exe build-21/wal-reduction/NEW-benchmark
git diff --check
git status --short
git diff --stat
```

Ubuntu: два контейнера с `/src` read-only, `/logs` mapped на `build-21/wal-reduction`, свой `/tmp/build` для каждого запуска. Внутри каждого:

```sh
cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Debug -DZENTHRILDB_BUILD_TESTS=ON
cmake --build /tmp/build -j 3
ctest --test-dir /tmp/build --output-on-failure --no-tests=error --timeout 300
cmake --install /tmp/build --prefix /logs/linux-final-debug-install
# Отдельный новый контейнер: те же команды с Release и linux-final-release-install.
bash /logs/linux-cli-smoke.sh
# Python 3 установлен в отдельном временном tooling-контейнере:
python3 /src/tools/measure_wal_reduction.py /tmp/install/zenthrildb /tmp/NEW-benchmark
g++ -std=c++23 -I/src -Wall -Wextra -Wpedantic -fanalyzer -c /src/storage/WalIdentity.cpp -o /tmp/analysis.o
```

Каждый build/install/CTest имеет отдельный лог; shell останавливается на ненулевом exit code. Windows CLI-smoke извлечён из актуального `run` блока `.github/workflows/ci.yml`; заменён только путь к установленному/собранному CLI и временный RUNNER_TEMP, сам набор проверок сохранён в `build-21/wal-reduction/ci-cli-smoke.ps1`.

## 18. Результаты команд

| Команда/проверка | Результат |
|---|---|
| Исходные git branch/HEAD/status/diff | Выполнены, исходный dirty state сохранён |
| Ubuntu final Debug configure/build/CTest/install | Exit 0, 6/6 |
| Ubuntu final Release configure/build/CTest/install | Exit 0, 6/6 |
| Windows final Debug/Release | Exit 0 для configure/build/CTest/install, 6/6 в обеих конфигурациях |
| Ubuntu установленный CLI Debug/Release | Exit 0, обе конфигурации |
| Windows CLI Debug/Release | Exit 0, обе конфигурации; исходник и backup проверены SHA-256 |
| Windows/Ubuntu workload | Exit 0, 148-байтовый WAL, все страницы/записи восстановлены, SHA-256 исходника сохранён |
| GCC syntax/warnings, семь production TU | Exit 0, без диагностик |
| GCC object analyzer, семь production TU | Exit 0, с описанными STL warnings, не clean |
| Минимальный STL reproducer | Exit 0, warnings воспроизведены без кода базы |
| Ранняя Windows сборка | Exit 1, stale test_main.obj/LNK2019; повторена после актуализации объекта |
| Ранняя отдельная GCC-проверка CLI | Exit 1, отсутствует CMake VERSION define; не используется как успешный результат |
| git diff --check | Exit 0, нарушений whitespace нет |

## 19. Definition of Done

| Критерий | Подтверждение |
|---|---|
| Architecture, safe boundary, recovery dependencies, UUID/generation/LSN | Реализовано; B/T формально определены и проверяются |
| Candidate, прежний WAL/recovery/publication, сохранение baseline | Реализовано; только новое назначение, источник не удаляется |
| Functional/integrity/negative/repeat/recovery | CTest Reduction и прежние пять групп |
| Crash injection | 32 process-crash и 26 I/O сценариев v3 в Debug каждой проверяемой платформы |
| Windows Debug/Release | 6/6 и 6/6, текущая сессия |
| Ubuntu Debug/Release | 6/6 и 6/6, текущая сессия |
| CLI smoke | Windows/Ubuntu, Debug/Release — успешно |
| Performance | До/после, reduction, checkpoint, физический recovery измерены и воспроизводимы |
| Documentation | Архитектура, инварианты, отказы, эксплуатация, команды и ограничения описаны |

**Stage 5 завершён в объёме Safe WAL Reduction Candidate для описанной process-crash/I/O модели.** Исходный WAL не заменяется; критерии кандидата выполнены на Windows и Ubuntu. Это не подтверждение production-релиза всей СУБД, аппаратной долговечности или разрешение автоматически удалить исходную историю. Дополнительные аппаратные и эксплуатационные проверки перечислены явно.

## 20. Следующий этап

Рекомендуется отдельная приёмка эксплуатации candidate: политика хранения исходного комплекта, независимое резервирование, стенд OS-crash/power-loss и дисковых отказов, контроль предельных объёмов и файловый backend на удерживаемых дескрипторах. После этого можно согласовать безопасное переключение и cleanup. До отдельного решения старый WAL не удаляется автоматически. Переход к B+Tree/MVCC/SQL/Server Mode в рамках этой работы не выполнялся.
