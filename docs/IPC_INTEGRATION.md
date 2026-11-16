# Спецификация IPC Zenthril ↔ ZenthrilDB (прототип v1)

**Статус:** утверждённое направление; протокол — спецификация для изолированного прототипа, реализации пока нет. Это не production API.

## Граница и назначение

ZenthrilDB запускается отдельным процессом. На Linux/macOS транспорт — Unix domain stream socket; на Windows — named pipe `\\.\pipe\zenthrildb`. Канал локальный, не TCP. Стороны работают от отдельной учётной записи сервиса с минимальными правами. Реальные DB-path и secrets задаёт только конфигурация процесса, клиент не может указать путь.

Первый запуск использует отдельный временный каталог и тестовый файл `.zdb`. Для проверки механизма разрешён прототипный namespace key/value над собственными страницами, но это не Record Manager, SQL или готовая функциональность СУБД. Zenthril пока не передаёт реальные события/модерацию, пока этап прототипа и shadow acceptance не пройдены.

## Транспортный фрейм

Каждый кадр: 4 байта длины unsigned big-endian, затем UTF-8 JSON тело. Максимум JSON-тела 1 MiB в прототипе; отклонить ноль/превышение до аллокации. Каждое соединение имеет deadline и закрывается после некорректного кадра. На Windows ACL named pipe ограничивает соединение SID сервисного пользователя; на Unix socket mode `0600`, каталог `0700`.

## Запрос / ответ

Запрос: `{ "protocol":1, "id":"<uuid>", "op":"PING|PUT|GET|DELETE|EXISTS|STATS", "key":"...", "value":"<base64>" }`.

Ответ сохраняет `protocol` и `id`, содержит `ok` и `op`; GET возвращает base64 `value`, EXISTS — boolean `exists`, ошибка — `{ "error": { "code":"ERR_...", "message":"..." } }`. Не включать значения, секреты, абсолютный путь или пользовательский текст в диагностические сообщения.

Ключ UTF-8 1–256 байт, непустой. Значение до 256 KiB в прототипе. `PUT` атомарен для одной записи после того, как тестовый record layer это явно гарантирует; до этого команда PUT остаётся недоступной и не маскируется `writePage`. TTL отсутствует. `DELETE` идемпотентен. `GET` отсутствующего ключа → `ERR_KEY_NOT_FOUND`. Команды v1: PING, STATS, PUT, GET, EXISTS, DELETE.

## Ошибки и тайм-ауты

Коды: `ERR_INVALID_REQUEST`, `ERR_PROTOCOL_VERSION`, `ERR_UNKNOWN_OP`, `ERR_INVALID_KEY`, `ERR_VALUE_TOO_LARGE`, `ERR_MESSAGE_TOO_LARGE`, `ERR_KEY_NOT_FOUND`, `ERR_RESOURCE_BUSY`, `ERR_TIMEOUT`, `ERR_STORAGE_ERROR`, `ERR_CORRUPTION`, `ERR_SHUTTING_DOWN`, `ERR_INTERNAL`.

Connect timeout 5 с; операция 10 с; shutdown grace 5 с. На timeout/disconnect возвращаем transient error. `PING`, `GET`, `EXISTS`, `STATS` можно повторять. `DELETE` идемпотентен. `PUT` нельзя автоматически повторять после потерянного ответа без request idempotency ledger: результат первой записи неизвестен. Повторная попытка соединения ограничена экспоненциальной задержкой 100 мс–2 с, максимум 5 попыток; затем circuit открывается до health probe.

## Процесс и восстановление соединения

Go supervisor запускает дочерний бинарь только из фиксированного установленного пути, без shell и пользовательских аргументов. Используются stdin/stdout для журнала процесса, IPC отдельный; shutdown: закрыть приём новых запросов, дождаться in-flight до 5 с, отправить shutdown control request, завершить процесс и ждать exit. Не убивать процесс с тестовым файлом в общем рабочем DB-каталоге.

После процесса crash supervisor ограниченно перезапускает тестовый процесс. Перед повтором неизвестной мутации сначала проверяет request idempotency/status; если механизм ещё не реализован — возвращает ошибку оператору, не повторяет PUT. Startup выполняет CRC/open/recovery только внутри изолированного тестового каталога и сообщает readiness лишь после проверки reopen.

## Испытания и критерии приёмки

1. Протестировать framing: фрагментированный header/body, несколько кадров, EOF, нулевой/слишком большой размер, неверный UTF-8/JSON/версия/op.
2. Проверить access-control IPC от разрешённого и постороннего локального пользователя.
3. PING, PUT, GET, EXISTS, DELETE, ошибки размера, CRC и неизвестного ключа.
4. Убить процесс до/после ответа, проверить timeout, supervisor restart, корректное закрытие и отсутствие бесконтрольного retry mutation.
5. Сохранить PUT, штатно закрыть процесс, снова запустить его с тем же тестовым файлом, прочитать байты и сравнить SHA-256.
6. Повторить reopen после kill процесса; этот результат описывать только как проверенный процессный crash-сценарий, не как power-loss durability.
7. Повредить копию тестового WAL/page и подтвердить fail-closed без изменения исходного файла.
8. Убедиться, что Go/клиент Zenthril не обращаются к production БД и E2EE payload/key отсутствуют во всех IPC fixtures/logs.
9. CI: Windows + Linux build/test, bounded timeout, тестовые файлы удаляются только из уникального temp directory после закрытия handles.

Успех прототипа не включает migration или shadow write. Shadow adapter — отдельный следующий gated шаг: сначала PostgreSQL commit, затем best-effort ZenthrilDB append с correlation id, сверкой и метрикой расхождений; сбой ZenthrilDB не откатывает и не блокирует PostgreSQL.
