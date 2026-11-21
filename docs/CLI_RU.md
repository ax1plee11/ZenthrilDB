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
zenthrildb snapshot demo.zdb snapshot-required --require-baseline
zenthrildb reduce-wal demo.zdb reduced-demo
zenthrildb verify-snapshot reduced-demo/demo.zdb
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
| `snapshot` | `NEW_DIRECTORY` | Публикует новое поколение с baseline и manifest; полная история сохраняется для v1/v2, сокращённый режим наследуется от v3 |
| `reduce-wal` | `NEW_DIRECTORY` | Публикует отдельный кандидат v3 с материализованным baseline и сокращённым WAL, исходный комплект сохраняется |
| `install-wal` | `CANDIDATE_DATABASE` | Проверяет свежий v3-кандидат и публикует селектор установки; дальнейший open исходного пути использует выбранное поколение |
| `verify-snapshot` | | Перечитывает полный комплект и проверяет текущую согласованность рабочей базы |
| `verify-baseline` | | Перечитывает только базовый образ и манифест, даже если рабочей пары рядом нет |

Обе команды `verify-*` печатают JSON с полями `snapshot_lsn`, `transaction_high_water` и `pages_checked` по состоянию на момент снимка. Они открывают данные для чтения. `verify-snapshot` может создать штатный `.lock` и возвращает ошибку, если комплекту нужен ремонт. [Формат и гарантии](SNAPSHOT_FOUNDATION_RU.md).

Параметр `--require-baseline` создаёт новое поколение с идентичностью v2. При утрате baseline, manifest или обоих файлов открытие отклоняется до ремонта страниц и хвоста WAL. Все четыре файла входят в комплект. Дочерние снимки этого поколения автоматически сохраняют требование. Старые программы с поддержкой только идентичности v1 отклоняют такой комплект. Для перехода создайте снимок в новом каталоге и сохраните исходную базу.

Для `backup` и `snapshot` укажите новый каталог внутри существующего каталога с доверенными правами доступа. Используйте команды и проверяйте результаты по [руководству эксплуатации](OPERATIONS_RU.md).

Для `reduce-wal` также требуется новое назначение и остановленный источник: эксклюзивный lock защищает копирование. Успех создаёт четыре файла нового поколения. Это не сокращение исходного WAL на месте. Для проверки используйте `verify-snapshot`, `check` и чтение нужных записей по новому пути. При ошибке после rename каталог уже может существовать: проверьте опубликованный комплект, не пытайтесь заменить его повторной командой. Неполную или повреждённую обязательную границу v3 нельзя исправить удалением WAL. Храните исходный комплект; автоматического переключения и удаления истории нет.

Установка выполняется явно, после `reduce-wal`, при остановленном приложении:

```sh
zenthrildb reduce-wal demo.zdb candidate
zenthrildb install-wal demo.zdb candidate/demo.zdb
zenthrildb check demo.zdb
zenthrildb put demo.zdb notes installed 0102
```

После установки `demo.zdb.install` выбирает candidate; исходные data/WAL/baseline/manifest сохранены как архив. Новый open, checkpoint, backup, snapshot, reduction и verify по старому логическому пути работают с выбранным комплектом. Кандидат должен иметь то же имя файла, другой каталог, актуальную границу B/T и совпадающие данные. После последующих commits повтор `install-wal` с тем же выбранным путём безопасен; другой target отклоняется. Ошибка после публикации не означает откат: выполните `check`/`verify-snapshot`, не удаляйте `.install`. Сохраняйте selector, исходный архив и выбранный каталог; не используйте старые бинарники для записи в архив. [Протокол и ограничения Stage 6](STATUS_STAGE_6_CRASH_SAFE_WAL_INSTALLATION_RU.md).

Команда `checkpoint` работает с базой, которая поддерживает привязку WAL к поколению. Для перехода со старого формата используйте `snapshot`. В ответе `checkpoint_lsn` обозначает LSN записи контрольной точки, `durable_wal_lsn` — последнюю подтверждённую позицию WAL до записи контрольной точки, а `transaction_high_water` — наибольший идентификатор транзакции в проверенном журнале. Контрольная точка сохраняет историю для восстановления, поэтому команда не уменьшает размер WAL.

Команды `--help` и `--version` работают без файла базы. Имена таблиц и ключи в аргументах передаются в UTF-8. Код завершения `0` означает успех, `1` — ошибка выполнения, `2` — ошибка аргументов, `3` — запись или таблица, указанная для переименования или удаления, отсутствует. В `get` пустое значение даёт пустую строку с кодом `0`, а отсутствующий ключ даёт код `3`.
