# Консольная утилита ZenthrilDB

Собрать и установить утилиту из корня репозитория можно командами

```sh
cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Release -DZENTHRILDB_BUILD_TESTS=OFF
cmake --build build-cli --config Release
cmake --install build-cli --config Release --prefix stage
```

Утилита находится в `stage/bin/zenthrildb` на Linux и в `stage/bin/zenthrildb.exe` на Windows. Архивы ZIP и TGZ с файлами SHA-256 можно собрать командой `cpack --config build-cli/CPackConfig.cmake -C Release`. Сборка MSVC по умолчанию включает среду выполнения в бинарник. Для динамической среды выполнения укажите `-DZENTHRILDB_STATIC_MSVC_RUNTIME=OFF` при конфигурации.

## Пример

```sh
zenthrildb create demo.zdb
zenthrildb create-table demo.zdb notes
zenthrildb put demo.zdb notes first 68656c6c6f
zenthrildb get demo.zdb notes first
zenthrildb scan demo.zdb notes
zenthrildb check demo.zdb
zenthrildb checkpoint demo.zdb
zenthrildb backup demo.zdb backup-demo
zenthrildb get backup-demo/demo.zdb notes first
zenthrildb snapshot demo.zdb snapshot-demo
zenthrildb verify-snapshot snapshot-demo/demo.zdb
zenthrildb verify-baseline snapshot-demo/demo.zdb
```

`get` напечатает `68656c6c6f`. Это байты строки `hello` в шестнадцатеричном виде. Путь к базе должен оканчиваться на `.zdb`. Файл `.wal` лежит рядом и имеет такое же базовое имя. Обычное открытие базы, включая команду `check`, выполняет штатное восстановление и может изменить файлы. Команда `snapshot` получает эксклюзивную блокировку исходной базы, а восстановление выполняет на копии.

| Команда | Аргументы после имени базы | Результат |
| --- | --- | --- |
| `create` | | Создаёт новую базу |
| `create-table` | `TABLE` | Создаёт таблицу |
| `rename-table` | `TABLE NEW_NAME` | Переименовывает таблицу |
| `drop-table` | `TABLE` | Удаляет таблицу и её записи |
| `list-tables` | | Печатает имя каждой таблицы в шестнадцатеричном виде на отдельной строке |
| `put` | `TABLE KEY HEX_VALUE` | Записывает или заменяет значение. Передайте `-` для значения длиной 0 |
| `get` | `TABLE KEY` | Печатает значение в шестнадцатеричном виде |
| `erase` | `TABLE KEY` | Удаляет запись |
| `scan` | `TABLE` | Печатает по одному JSON-объекту с полями `key_hex` и `value_hex` на строку |
| `check` | | Печатает JSON с числом проверенных страниц, записей WAL и транзакций |
| `checkpoint` | | Синхронизирует WAL и страницы, записывает контрольную точку и печатает JSON. История WAL сохраняется |
| `backup` | `NEW_DIRECTORY` | Побайтно копирует согласованную пару, а для снимка также базовый образ и манифест |
| `snapshot` | `NEW_DIRECTORY` | Публикует новое поколение с базовым образом, манифестом и полной историей WAL |
| `verify-snapshot` | | Перечитывает полный комплект и проверяет текущую согласованность рабочей базы |
| `verify-baseline` | | Перечитывает только базовый образ и манифест, даже если рабочей пары рядом нет |

Обе команды `verify-*` печатают JSON с полями `snapshot_lsn`, `transaction_high_water` и `pages_checked` по состоянию на момент снимка. Они открывают данные для чтения. `verify-snapshot` может создать штатный `.lock` и возвращает ошибку, если комплекту нужен ремонт. [Формат и гарантии](SNAPSHOT_FOUNDATION_RU.md).

Для `backup` и `snapshot` укажите новый каталог внутри существующего каталога с доверенными правами доступа. Используйте команды и проверяйте результаты по [руководству эксплуатации](OPERATIONS_RU.md).

Команда `checkpoint` работает с базой, которая поддерживает привязку WAL к поколению. Для перехода со старого формата используйте `snapshot`. В ответе `checkpoint_lsn` обозначает LSN записи контрольной точки, `durable_wal_lsn` — последнюю подтверждённую позицию WAL до записи контрольной точки, а `transaction_high_water` — наибольший идентификатор транзакции в проверенном журнале. Контрольная точка сохраняет историю для восстановления, поэтому команда не уменьшает размер WAL.

Команды `--help` и `--version` работают без файла базы. Имена таблиц и ключи в аргументах передаются в UTF-8. Код завершения `0` означает успех, `1` — ошибка выполнения, `2` — ошибка аргументов, `3` — запись или таблица, указанная для переименования или удаления, отсутствует. В `get` пустое значение даёт пустую строку с кодом `0`, а отсутствующий ключ даёт код `3`.
