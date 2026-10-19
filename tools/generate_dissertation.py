from pathlib import Path
from datetime import date
from docx import Document
from docx.shared import Cm, Pt, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK, WD_LINE_SPACING
from docx.enum.section import WD_SECTION
from docx.enum.table import WD_TABLE_ALIGNMENT, WD_CELL_VERTICAL_ALIGNMENT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'Zenthril_Dissertation.docx'
ASSET = ROOT / 'build' / 'dissertation_assets'
ASSET.mkdir(parents=True, exist_ok=True)

def font(size=22, bold=False):
    return ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf' if bold else 'C:/Windows/Fonts/arial.ttf', size)

def diagram(name, title, rows):
    p = ASSET / name
    w, h = 1500, max(500, 150 + len(rows) * 125)
    im = Image.new('RGB', (w, h), 'white'); d = ImageDraw.Draw(im)
    d.text((50, 35), title, fill='#152238', font=font(30, True))
    y = 110
    for line in rows:
        parts = line.split(' → ')
        x = 60
        for i, part in enumerate(parts):
            tw = max(230, min(420, 14 * len(part) + 50))
            d.rounded_rectangle((x, y, x+tw, y+62), 12, fill='#EAF2F8', outline='#2874A6', width=3)
            d.text((x+15, y+18), part, fill='#17202A', font=font(19))
            if i < len(parts)-1:
                d.line((x+tw, y+31, x+tw+55, y+31), fill='#2874A6', width=4)
                d.polygon([(x+tw+55,y+31),(x+tw+42,y+23),(x+tw+42,y+39)], fill='#2874A6')
            x += tw + 65
        y += 105
    im.save(p)
    return p

figures = [
    diagram('architecture.png', 'Архитектура ZenthrilDB', [
        'Database API (предполагаемый) → MetadataManager / PageManager',
        'TransactionManager / LockManager → RecoveryEngine',
        'BufferManager → FileManager → .zdb',
        'LogManager → WriteAheadLog → .wal']),
    diagram('layout.png', 'Логическая структура файла .zdb', [
        'DatabaseHeader → PAGE 0: header mirror',
        'PAGE 1: DatabaseMetadata → PAGE 2: PageDirectory',
        'PAGE 3: FreePageList → PAGE 4+: user pages']),
    diagram('buffer.png', 'Алгоритм BufferManager', [
        'pin(pageId) → cache hit → pinCount++',
        'cache miss → evict LRU unpinned page → FileManager.readPage',
        'BufferPageGuard → markDirty / release → unpin(pageId)']),
    diagram('wal.png', 'Текущий поток WAL', [
        'LogManager.append → assigned LSN → LogRecord serialization',
        'CRC32 frame → WriteAheadLog.append → .wal',
        'replay → RecoveryContext → RecoveryPlan']),
    diagram('transaction.png', 'Жизненный цикл транзакции', [
        'begin → Active + TransactionStarted WAL',
        'Active → commit → TransactionCommitted WAL',
        'Active → rollback → TransactionRolledBack WAL']),
    diagram('recovery.png', 'Каркас recovery', [
        'validated WAL records → RecoveryContext → RecoveryEngine',
        'RecoveryPlan → UndoRedoRecoveryExecutor → abstract RedoApplier / UndoApplier']),
    diagram('locks.png', 'Локальный LockManager', [
        'transaction + resource + mode → compatibility check',
        'cannot grant → FIFO wait queue → timeout / wake-up',
        'WaitForGraph → cycle search (требует расширенной проверки)'])]

doc = Document()
sec = doc.sections[0]
sec.page_width, sec.page_height = Cm(21), Cm(29.7)
sec.left_margin, sec.right_margin = Cm(3), Cm(1)
sec.top_margin, sec.bottom_margin = Cm(2), Cm(2)

styles = doc.styles
normal = styles['Normal']; normal.font.name = 'Times New Roman'; normal._element.rPr.rFonts.set(qn('w:eastAsia'), 'Times New Roman'); normal.font.size = Pt(14)
normal.paragraph_format.line_spacing = 1.5; normal.paragraph_format.first_line_indent = Cm(1.25); normal.paragraph_format.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY
for s in ['Heading 1', 'Heading 2', 'Heading 3']:
    st = styles[s]; st.font.name = 'Times New Roman'; st._element.rPr.rFonts.set(qn('w:eastAsia'), 'Times New Roman'); st.font.bold = True
styles['Heading 1'].font.size = Pt(16); styles['Heading 2'].font.size = Pt(14); styles['Heading 3'].font.size = Pt(14)

def set_cell(cell, text, bold=False):
    cell.text = ''
    p=cell.paragraphs[0]; p.alignment=WD_ALIGN_PARAGRAPH.CENTER
    r=p.add_run(str(text)); r.bold=bold; r.font.name='Times New Roman'; r.font.size=Pt(11)
    cell.vertical_alignment=WD_CELL_VERTICAL_ALIGNMENT.CENTER

def shade(cell, value='D9EAF7'):
    tcPr=cell._tc.get_or_add_tcPr(); shd=OxmlElement('w:shd'); shd.set(qn('w:fill'),value); tcPr.append(shd)

table_no = 0
def table(headers, rows, widths=None):
    global table_no
    table_no += 1
    caption(f'Таблица {table_no} — Сводные данные раздела')
    t=doc.add_table(rows=1, cols=len(headers)); t.style='Table Grid'; t.alignment=WD_TABLE_ALIGNMENT.CENTER
    for i,h in enumerate(headers): set_cell(t.rows[0].cells[i],h,True); shade(t.rows[0].cells[i])
    for row in rows:
        cells=t.add_row().cells
        for i,val in enumerate(row): set_cell(cells[i],val)
    doc.add_paragraph('')
    return t

def p(text='', bold=False, center=False):
    q=doc.add_paragraph(); q.alignment=WD_ALIGN_PARAGRAPH.CENTER if center else WD_ALIGN_PARAGRAPH.JUSTIFY
    r=q.add_run(text); r.bold=bold; r.font.name='Times New Roman'; r.font.size=Pt(14)
    q.paragraph_format.line_spacing=1.5
    if not center: q.paragraph_format.first_line_indent=Cm(1.25)
    return q

def h(text, level=1):
    if level == 1 and len(doc.paragraphs)>1: doc.add_page_break()
    return doc.add_heading(text, level=level)

def caption(text):
    q=doc.add_paragraph(); q.alignment=WD_ALIGN_PARAGRAPH.CENTER; q.paragraph_format.first_line_indent=Cm(0)
    r=q.add_run(text); r.italic=True; r.font.name='Times New Roman'; r.font.size=Pt(12)

def add_figure(path, cap):
    doc.add_picture(str(path), width=Cm(16)); doc.paragraphs[-1].alignment=WD_ALIGN_PARAGRAPH.CENTER; caption(cap)

def toc():
    q=doc.add_paragraph(); q.alignment=WD_ALIGN_PARAGRAPH.CENTER; q.add_run('СОДЕРЖАНИЕ').bold=True
    fld=OxmlElement('w:fldSimple'); fld.set(qn('w:instr'),'TOC \\o "1-3" \\h \\z \\u'); q._p.append(fld)
    p('Для обновления содержания в Microsoft Word следует выбрать поле и нажать F9.', False)

# title
for _ in range(5): p('', center=True)
p('[Наименование министерства / университета — заполнить]', center=True)
p('[Факультет / кафедра — заполнить]', center=True)
for _ in range(6): p('', center=True)
p('МАГИСТЕРСКАЯ ДИССЕРТАЦИЯ', True, True)
p('на тему:', True, True)
p('«Разработка архитектуры локального storage engine с журналированием и контролем целостности данных на примере ZenthrilDB»', True, True)
for _ in range(7): p('', center=True)
p('Обучающийся: [Ф. И. О. — заполнить]\nНаучный руководитель: [Ф. И. О., степень — заполнить]', center=True)
for _ in range(4): p('', center=True)
p('[Город] — 2026', center=True)
doc.add_page_break()

h('АННОТАЦИЯ',1)
p('Работа посвящена анализу и разработке архитектуры ZenthrilDB — локальной основы storage engine на C++23. Рассматриваются страничный формат .zdb, контроль целостности CRC32, управление метаданными и свободными страницами, LRU-буферизация, журнал write-ahead log, а также foundations транзакций, восстановления и блокировок. Технические выводы ограничены доступным исходным кодом и повторно выполненной сборкой с unit tests. Полноценный crash recovery, SQL, MVCC, шифрование и интеграция с Zenthril не заявляются как реализованные.')
h('ABSTRACT',1)
p('The dissertation examines ZenthrilDB, a C++23 local storage-engine foundation. The reviewed implementation includes a page-oriented .zdb format, CRC32 integrity checks, metadata and free-page management, an LRU buffer manager, write-ahead log framing, and transaction, recovery, and locking foundations. Claims are limited to supplied source code and a rebuilt unit-test executable. Full crash recovery, SQL, MVCC, encryption, and an integration with Zenthril are not presented as implemented features.')
toc()
h('ПЕРЕЧЕНЬ СОКРАЩЕНИЙ И ОБОЗНАЧЕНИЙ',1)
table(['Сокращение','Значение'],[
    ['CRC32','циклический избыточный код длиной 32 бита'],['LSN','log sequence number'],['WAL','write-ahead log'],['LRU','least recently used'],['MVCC','многоверсионный контроль конкурентного доступа'],['RAII','освобождение ресурса в деструкторе объекта'],['DBMS','система управления базами данных']])

h('ВВЕДЕНИЕ',1)
p('Локальные системы хранения данных требуют явно определённого формата файла, управляемого жизненного цикла страниц и механизмов, позволяющих обнаруживать повреждение данных. В доступной кодовой базе ZenthrilDB эти задачи решаются как фундамент storage engine. В отличие от готовой СУБД, проект не содержит языка запросов, индексов, MVCC и полного алгоритма восстановления после аварии. Такое разграничение является определяющим для корректной интерпретации результатов.')
p('Объект исследования — подсистемы локального страничного хранения данных. Предмет исследования — методы организации бинарного формата, контроля целостности, буферизации, журналирования и foundations конкурентного доступа. Цель работы состоит в разработке и исследовании архитектуры ZenthrilDB, обеспечивающей управляемое страничное хранение, контроль целостности, журналирование и расширяемую основу recovery.')
p('Научная задача имеет инженерно-исследовательский характер: необходимо подтвердить состав компонентов, их взаимодействие и ограничения. Научная новизна не заявляется как открытие нового алгоритма; возможным результатом является документированная архитектурная композиция и экспериментально проверяемое описание её границ.')

h('1. АНАЛИЗ ПРЕДМЕТНОЙ ОБЛАСТИ',1)
h('1.1. Страничное хранение и embedded storage engines',2)
p('Страничная организация делит файл на блоки одинакового размера, упрощая адресацию, кэширование и обновление. SQLite документирует единый размер страниц внутри файла, заголовок, свободные страницы и отдельные журнальные файлы [1]. ZenthrilDB использует другой, собственный формат: файл начинается с DatabaseHeader, а последующие страницы имеют фиксированный размер 8192 байта [исходный код ZenthrilDB: core/Types.hpp; storage/Page.hpp].')
h('1.2. Буферизация, целостность и журналирование',2)
p('Буферный менеджер уменьшает количество обращений к носителю, поддерживая кэш страниц и политику вытеснения. CRC не предотвращает преднамеренное изменение и не заменяет криптографическую аутентификацию, но позволяет обнаруживать многие случайные повреждения. В ZenthrilDB CRC32 вычисляется для заголовка базы, каждой страницы и полного WAL frame [исходный код ZenthrilDB: core/FileManager.cpp; wal/LogRecord.cpp].')
p('WAL фиксирует последовательность событий до их использования механизмом восстановления. В ARIES журналирование связано с recovery и изоляцией [5]. RocksDB также использует WAL для восстановления состояния memtable [4]. Эти источники являются архитектурными аналогами, но не служат основанием считать ZenthrilDB эквивалентным им по надёжности.')
h('1.3. Сравнение подходов',2)
table(['Критерий','ZenthrilDB','SQLite','RocksDB','PostgreSQL'],[
['Тип','local storage foundation','embedded SQL DBMS','embedded LSM key-value engine','client-server DBMS'],
['Страницы / формат','8192-byte pages, own .zdb','page-based database file [1]','SST/block format','physical storage relations [3]'],
['WAL','frame foundation; no physical redo','WAL or rollback journal [1,2]','production WAL [4]','production WAL [3]'],
['SQL / indexes','не реализованы','реализованы','key-value API','реализованы'],
['Recovery','planning scaffold','реализовано','реализовано','реализовано'],
['Численное сравнение','не проводилось','не проводилось','не проводилось','не проводилось']])

h('2. ПОСТАНОВКА ЗАДАЧИ',1)
h('2.1. Требования и границы',2)
p('К подтверждённым требованиям относятся: явное little-endian кодирование целочисленных полей, версионирование форматов, контроль CRC32, защита системных страниц, повторное использование свободных идентификаторов и сериализация метаданных. Система не должна описываться как production DBMS: отсутствуют SQL, B+Tree, record layer, MVCC, replication, remote storage и реализованное encryption.')
h('2.2. Задачи исследования',2)
table(['№','Задача','Статус'],[
['1','Исследование page-oriented storage и WAL','выполнено аналитически'],['2','Проектирование .zdb и его системных страниц','реализовано'],['3','Реализация CRC, file/page/buffer managers','реализовано'],['4','WAL и transaction/recovery foundations','реализовано частично'],['5','Проверка сборки и unit tests','выполнено 25.09.2026'],['6','Нагрузочные benchmark и crash harness','не выполнено; требуется далее']])

h('3. АРХИТЕКТУРА ZENTHRILDB',1)
add_figure(figures[0], 'Рисунок 3.1 — Подтверждённая архитектура ZenthrilDB')
h('3.1. Формат .zdb',2)
p('DatabaseHeader содержит magic, версию, размер страницы, число страниц, timestamp создания, UUID и checksum. Файловый менеджер создаёт системные страницы. PAGE 0 зарезервирована под mirror заголовка; PAGE 1 хранит DatabaseMetadata; PAGE 2 — PageDirectory; PAGE 3 — FreePageList; PAGE 4 и далее предназначены для пользовательских данных [исходный код ZenthrilDB: core/Types.hpp; core/FileManager.cpp].')
add_figure(figures[1], 'Рисунок 3.2 — Размещение системных страниц в .zdb')
table(['Структура','Подтверждённые поля / назначение','Ограничение'],[
['DatabaseHeader','magic, version, pageSize, totalPages, timestamp, UUID, checksum','не является криптографически подписанным'],['PageHeader','pageId, PageType, checksum, usedBytes, version','фиксированный заголовок не содержит latch/transaction ownership'],['Page','8192 bytes; payload после PageHeader','record format не реализован'],['BinaryIO','явное little-endian чтение/запись','формат не совместим с SQLite']])
h('3.2. FileManager и управление страницами',2)
p('FileManager создаёт и открывает файл, читает и записывает страницы, поддерживает flush и резервирует PageId. Перед возвратом страницы readPage проверяет CRC. PageManager выделяет и освобождает пользовательские страницы, а FreePageManager сохраняет и загружает перечень свободных PageId. PageManager запрещает прямую запись в системные страницы. Эти операции подтверждены unit tests для file, page, free-page и metadata managers.')
h('3.3. BufferManager',2)
add_figure(figures[2], 'Рисунок 3.3 — Логика LRU-буферизации')
p('BufferManager хранит кэш в unordered map и список LRU. При pin cache hit увеличивает pinCount, при miss читает страницу через FileManager; при переполнении вытесняется наименее недавно используемая незакреплённая страница. BufferPageGuard освобождает pin в деструкторе. Mutex защищает операции над структурой кэша, однако возвращаемая ссылка Page не имеет отдельной page latch; поэтому синхронизация изменения одной страницы остаётся задачей более высокого слоя.')
h('3.4. WAL и typed payload',2)
add_figure(figures[3], 'Рисунок 3.4 — Поток операций текущего WAL')
p('LogRecord имеет magic, format version, record type, LSN, timestamp, payload size, CRC32 и payload. Код ограничивает payload 64 MiB до выделения буфера. LogManager сериализует append, выдаёт возрастающие LSN, предоставляет replay, checkpoint и truncate. Typed payload описывают allocation/free/write страницы, metadata update и transaction boundary. Flush использует std::fstream::flush; вызова fsync/FlushFileBuffers в коде нет, поэтому семантика аппаратной durability не доказана.')
h('3.5. Transactions, recovery и locks',2)
add_figure(figures[4], 'Рисунок 3.5 — Фактическая модель состояний транзакции')
p('TransactionManager поддерживает только Active, Committed и RolledBack. begin/commit/rollback записывают границы в WAL через TransactionLogAdapter. Нет состояний Committing/Aborting, уровней изоляции, MVCC, сохранения next transaction ID после рестарта или отката изменения страниц.')
add_figure(figures[5], 'Рисунок 3.6 — Recovery foundation')
p('RecoveryEngine находит последний checkpoint, классифицирует записи и создаёт RecoveryPlan. UndoRedoRecoveryExecutor вызывает внедряемые RedoApplier и UndoApplier, избегая повторного применения LSN в памяти. В проекте отсутствует реализация applier, применяющая before/after image к .zdb. Следовательно, компонент является foundation, а не production-ready crash recovery.')
add_figure(figures[6], 'Рисунок 3.7 — Локальная модель блокировок')
p('LockManager содержит shared/exclusive locks, timeout и LockGuard. В текущей верифицированной ревизии исправлены несоответствие типов condition variable, linkage WaitForGraph и время жизни waiter-записи. Базовые lock tests проходят. Отдельных тестов upgrade/downgrade, FIFO fairness и цикла wait-for graph в предоставленном наборе нет; эти свойства нельзя считать экспериментально подтверждёнными.')

h('4. РЕАЛИЗАЦИЯ ОСНОВНЫХ ПОДСИСТЕМ',1)
h('4.1. Алгоритмы',2)
table(['Алгоритм','Вход','Основные шаги','Результат / ошибки'],[
['Создание базы','path','создать header; записать pages 0–3; flush','новый .zdb либо исключение I/O'],['Чтение страницы','PageId','seek; read 8192 bytes; deserialize; CRC32 check','Page либо exception'],['Allocation','PageType, owner','получить free PageId или reserve; register directory','новая page в памяти'],['LRU eviction','full cache','найти LRU с pinCount=0; при dirty write; erase','освобождён cache slot либо exception'],['WAL append','type, payload','назначить LSN; serialize with CRC; append','LSN либо exception'],['Recovery planning','RecoveryContext','найти last checkpoint; classify later records','RecoveryPlan'],['Lock acquisition','tx, resource, mode','compatibility; grant или enqueue/wait','true/false либо exception']])
h('4.2. Безопасность и обработка повреждений',2)
p('Код проверяет magic и format version WAL, допустимый payload size, CRC WAL и страниц, границы бинарного буфера, версию payload и ряд некорректных идентификаторов. Это снижает риск некорректной интерпретации повреждённых файлов. Механизмы не предоставляют конфиденциальность: EncryptionInterfaces содержат лишь абстрактные методы encrypt/decrypt и key management без concrete provider. Допущения о правах доступа к файлам, защите ключей и adversarial filesystem не реализованы.')

h('5. ЭКСПЕРИМЕНТАЛЬНОЕ ИССЛЕДОВАНИЕ',1)
h('5.1. Среда и методика',2)
p('Сборка выполнена 25.09.2026 на Windows в доступной рабочей среде Microsoft Visual Studio 2026 Developer Command Prompt с MSVC 19.50.35728. Использован стандарт /std:c++latest, соответствующий C++23 намерению проекта. Запускался вновь собранный build/zenthrildb_tests.exe; его timestamp: 25.09.2026 17:40:09. Точные сведения о CPU и RAM не фиксировались, поэтому в документ не включаются.')
h('5.2. Результаты',2)
table(['Эксперимент','Метод','Фактический результат','Вывод'],[
['Build verification','compile_harden.cmd с актуальными sources','успешная компиляция и link после точечных исправлений','текущая тестовая ревизия собирается'],['Unit tests','запуск нового zenthrildb_tests.exe','All ZenthrilDB tests passed; exit 0','существующие сценарии прошли'],['WAL corruption','существующие unit tests','в наборе имеются тесты CRC/corruption/truncation','есть проверка формата, без crash benchmark'],['Concurrent WAL/transactions','существующие unit tests','есть многопоточные тесты append и lifecycle','не измерялась производительность'],['Page/metadata','существующие unit tests','проверяются serialization, allocation/reuse, boundaries','покрытие не равно формальной гарантии'],['Benchmark latency/throughput','не запускался','не измерено','требуется отдельный стенд'],['Sanitizer/static analysis','не запускались','не измерено','требуется совместимая toolchain']])
h('5.3. Интерпретация',2)
p('Успешный unit-test run подтверждает лишь покрытые утверждения. Он не подтверждает отсутствие гонок во всех сценариях, atomic durability после потери питания, быстродействие или production readiness. Сценарии с полноценным восстановлением невозможны на текущем API, поскольку WAL не содержит физических данных для повторного применения страниц.')

h('6. АНАЛИЗ РЕЗУЛЬТАТОВ И ОГРАНИЧЕНИЯ',1)
h('6.1. Подтверждённые результаты',2)
p('Подтверждены работа бинарной сериализации, CRC32 в критических структурах, управление страницами и метаданными, WAL frame validation, transaction boundaries, recovery planning и базовые блокировки. Практическая ценность состоит в наличии расширяемой локальной основы для дальнейших работ над storage layer.')
h('6.2. Ограничения разработанной системы',2)
table(['Ограничение','Причина','Корректная формулировка в работе'],[
['SQL и query engine','нет соответствующего кода','не реализованы'],['B+Tree и record layer','roadmap: planned','не реализованы'],['MVCC и isolation','нет модели snapshot/version visibility','не реализованы'],['Crash recovery','нет page images/deltas и concrete applier','recovery foundation'],['Durability','нет fsync policy','не подтверждена при потере питания'],['Encryption','только interfaces','не реализовано'],['Deadlock detection','нет dedicated tests','не подтверждено экспериментально'],['Zenthril integration','репозиторий Zenthril не предоставлен','future work'],['Performance','нет benchmark dataset/results','не измерено']])
h('6.3. Практическая применимость',2)
p('На текущем этапе ZenthrilDB не является полноценной production DBMS. Реализация может использоваться как учебная и исследовательская основа embedded storage, а также как база для последующей интеграции в local-first приложение. Интеграция с Zenthril в кодовой базе не подтверждена и в настоящей работе не заявляется.')

h('7. ПЕРСПЕКТИВЫ РАЗВИТИЯ',1)
p('Первоочередными направлениями являются: закрепление и расширение lock tests; формализация force/no-force и steal/no-steal policy; WAL payload с page before/after images; transaction table и dirty-page table; crash simulation; B+Tree, record manager и catalog; MVCC; устойчивое управление ключами и encryption provider; измеримый benchmark. Только после появления исходного кода Zenthril возможно проектировать и проверять отдельную интеграцию через конкретный Database API или Go/C API.')

h('ЗАКЛЮЧЕНИЕ',1)
p('В работе исследована фактическая архитектура ZenthrilDB. Реализованная часть включает фиксированный страничный формат, CRC32, управление файлами, буферизацию, метаданные, свободные страницы, WAL framing, foundations транзакций и recovery, а также базовую локальную синхронизацию. Актуальная тестовая ревизия была пересобрана и прошла существующий набор unit tests. Одновременно показано, что SQL, MVCC, полный rollback и crash recovery, encryption, performance validation и интеграция Zenthril отсутствуют либо не подтверждены. Это ограничивает выводы, но делает документ проверяемым и пригодным для дальнейшего развития работы.')

h('СПИСОК ИСПОЛЬЗОВАННЫХ ИСТОЧНИКОВ',1)
sources=[
'1. SQLite. Database File Format. URL: https://www.sqlite.org/fileformat2.html (дата обращения: 25.09.2026).',
'2. SQLite. Write-Ahead Logging. URL: https://www.sqlite.org/wal.html (дата обращения: 25.09.2026).',
'3. PostgreSQL Global Development Group. Write-Ahead Logging; Database Physical Storage. URL: https://www.postgresql.org/docs/current/wal-intro.html; https://www.postgresql.org/docs/current/storage.html (дата обращения: 25.09.2026).',
'4. RocksDB. Write Ahead Log. URL: https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-%28WAL%29 (дата обращения: 25.09.2026).',
'5. Mohan C., Haderle D., Lindsay B., Pirahesh H., Schwarz P. ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging // ACM TODS. 1992. Vol. 17, No. 1. P. 94–162.',
'6. Исходный код ZenthrilDB: core/, storage/, wal/, transaction/, recovery/, lock/, tests/. Рабочая копия аудита от 25.09.2026.',
'7. ZenthrilDB. Architecture documentation: docs/architecture.md, WAL_SPEC.md, RECOVERY_SPEC.md, TRANSACTION_SPEC.md, LOCK_SPEC.md, ROADMAP.md.']
for x in sources: p(x)

h('ПРИЛОЖЕНИЕ А. КАРТА СООТВЕТСТВИЯ УТВЕРЖДЕНИЙ',1)
table(['Утверждение','Источник','Статус','Раздел'],[
['Фиксированные pages 8192 bytes','core/Types.hpp; test_page_format.cpp','CODE + TEST','3.1'],['CRC header/page/WAL','FileManager.cpp; LogRecord.cpp; tests','CODE + TEST','3.1, 4.2'],['LRU и RAII guard','BufferManager.cpp; test_buffer_manager.cpp','CODE + TEST','3.3'],['WAL and typed payload','wal/*; test_wal.cpp; test_log_payload.cpp','CODE + TEST','3.4'],['Transaction boundaries','transaction/*; test_transaction.cpp','CODE + TEST','3.5'],['Recovery foundation','recovery/*; test_recovery.cpp','CODE + TEST','3.5'],['Current build/test outcome','compile audit and new executable','EXPERIMENT','5.2'],['Zenthril integration','нет исходного кода','MISSING','6.2']])
h('ПРИЛОЖЕНИЕ Б. РЕАЛЬНЫЕ И ПЛАНИРУЕМЫЕ МЕХАНИЗМЫ',1)
table(['Компонент','Состояние','Комментарий'],[
['Storage format / pages / CRC','IMPLEMENTED','подтверждено кодом и tests'],['WAL','FOUNDATION','frame, LSN, CRC, payload codecs'],['Transactions','FOUNDATION','lifecycle boundaries only'],['Recovery','FOUNDATION','analysis + executor interfaces'],['Lock Manager','PARTIAL','basic tests; advanced graph requires dedicated validation'],['Encryption','MISSING','interfaces only'],['MVCC, B+Tree, SQL','PLANNED','roadmap'],['Zenthril','UNVERIFIED','repository not supplied']])

# page numbers
for section in doc.sections:
    footer=section.footer.paragraphs[0]; footer.alignment=WD_ALIGN_PARAGRAPH.CENTER
    fld=OxmlElement('w:fldSimple'); fld.set(qn('w:instr'),'PAGE'); footer._p.append(fld)

doc.core_properties.title='ZenthrilDB Dissertation'
doc.core_properties.subject='Working dissertation based on verified project state'
doc.core_properties.author='[Заполнить]'
doc.save(OUT)
print(OUT)
