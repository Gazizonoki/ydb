# {{ process_command }}

Преобразовать входную таблицу с помощью {% if oss != true %}UDF на [C++](../udf/cpp.md){% if yt %}, [Python](../udf/python.md) или [JavaScript](../udf/javascript.md){% endif %} или {% endif %}[лямбда функции](expressions.md#lambda), которая применяется последовательно к каждой строке входа и имеет возможность для каждой строки входа создать ноль, одну или несколько строк результата (аналог Map в терминах MapReduce).

{% if feature_mapreduce %}Таблица по имени ищется в базе данных, заданной оператором [USE](use.md).{% endif %}

В параметрах вызова функции после ключевого слова `USING` явно указывается, значения из каких колонок и в каком порядке передавать для каждой строки входа.

Допустимы функции, которые возвращают результат одного из трех составных типов от `OutputType` (возможные варианты `OutputType` описаны ниже):

* `OutputType` — на каждую строку входа всегда должна быть строка выхода, схема которой определяется типом структуры.
* `OutputType?` — функции оставляет за собой право пропускать строки, возвращая пустые значения (`TUnboxedValue()` в C++, `None` в Python или `null` в JavaScript).
* `Stream<OutputType>` или `List<OutputType>` — возможность вернуть несколько строк.

Вне зависимости от того, какой вариант из перечисленных выше трех выбран, результат преобразовывается к плоской таблице с колонками, определяемыми типом `OutputType`.

В качестве `OutputType` может выступать один из типов:

* `Struct<...>` — у `{{ process_command }}` будет ровно один выход с записями заданной структуры, представляющий собой плоскую таблицу с колонками соответствующими полям `Struct<...>`
* `Variant<Struct<...>,...>` — у `{{ process_command }}` число выходов будет равно числу альтернатив в `Variant`. Записи каждого выхода представлены плоской таблицей с колонками по полям из соответствующей альтернативы. Ко множеству выходов `{{ process_command }}` в этом случае можно обратиться как к кортежу (`Tuple`) списков, который можно распаковать в отдельные [именованные выражения](expressions.md#named-nodes) и использовать независимо.

В списке аргументов функции после ключевого слова `USING` можно передать одно из двух специальных именованных выражений:

* `TableRow()` — текущая строка целиком в виде структуры;
* `TableRows()` — ленивый итератор по строкам, с точки зрения типов — `Stream<Struct<...>>`. В этом случае выходным типом функции может быть только `Stream<OutputType>` или `List<OutputType>`.

{% note info "Примечание" %}

После выполнения `{{ process_command }}` в рамках того же запроса по результирующей таблице (или таблицам) можно выполнить {% if select_command != "SELECT STREAM" %}[SELECT](select/index.md), [REDUCE](reduce.md){% else %}[SELECT STREAM](select_stream.md){% endif %}, [INSERT INTO](insert_into.md), ещё один `{{ process_command }}` и так далее в зависимости от необходимого результата.

{% endnote %}

Ключевое слово `USING` и указание функции необязательны: если они не указаны, то возвращается исходная таблица. {% if feature_subquery %}Это может быть удобно для применения [шаблона подзапроса](subquery.md).{% endif %}

В `{{ process_command }}` можно передать несколько входов (под входом здесь подразумевается таблица,{% if select_command != "PROCESS STREAM" %} {% if feature_bulk_tables %}[диапазон таблиц](select/concat.md){% else %}диапазон таблиц{% endif %}{% endif %}, подзапрос, [именованное выражение](expressions.md#named-nodes)), разделенных запятой. В функцию из `USING` в этом случае можно передать только специальные именованные выражения `TableRow()` или  `TableRows()`, которые будут иметь следующий тип:

* `TableRow()` — альтернатива (`Variant`), где каждый элемент имеет тип структуры записи из соответствущего входа. Для каждой входной строки в альтернативе заполнен элемент, соответствущий номеру входа этой строки
* `TableRows()` — ленивый итератор по альтернативам, с точки зрения типов — `Stream<Variant<...>>`. Альтернатива имеет такую же семантику, что и для `TableRow()`

После `USING` в `{{ process_command }}` можно опционально указать `ASSUME ORDER BY` со списком столбцов. Результат такого `{{ process_command }}` будет считаться сортированным, но без выполнения фактической сортировки. Проверка сортированности осуществляется на этапе исполнения запроса. Поддерживается задание порядка сортировки с помощью ключевых слов `ASC` (по возрастанию) и `DESC` (по убыванию). Выражения в `ASSUME ORDER BY` не поддерживается.

## Примеры

{% if process_command != "PROCESS STREAM" %}

```yql
PROCESS my_table
USING MyUdf::MyProcessor(value)
```

```yql
$udfScript = @@
def MyFunc(my_list):
    return [(int(x.key) % 2, x) for x in my_list]
@@;

-- Функция возвращает итератор альтернатив
$udf = Python3::MyFunc(Callable<(Stream<Struct<...>>) -> Stream<Variant<Struct<...>, Struct<...>>>>,
    $udfScript
);

-- На выходе из PROCESS получаем кортеж списков
$i, $j = (PROCESS my_table USING $udf(TableRows()));

SELECT * FROM $i;
SELECT * FROM $j;
```

```yql
$udfScript = @@
def MyFunc(stream):
    for r in stream:
        yield {"alt": r[0], "key": r[1].key}
@@;

-- Функция принимает на вход итератор альтернатив
$udf = Python::MyFunc(Callable<(Stream<Variant<Struct<...>, Struct<...>>>) -> Stream<Struct<...>>>,
    $udfScript
);

PROCESS my_table1, my_table2 USING $udf(TableRows());
```

{% else %}

```yql

-- лямбда функция принимает примитивный тип и возвращает структуру с тремя одинаковыми полями
$f = ($r) -> {
   return AsStruct($r as key, $r as subkey, $r as value);
};

-- фильтруем входящий стрим в секции WHERE
PROCESS STREAM Input USING $f(value) WHERE cast(subkey as int) > 3;

```

```yql
-- лямбда функция принимает тип `Struct<key:String, subkey:String, value:String>`
-- и возвращает аналогичную структуру, добавив некий суффикс к каждому полю

$f = ($r) -> {
    return AsStruct($r.key || "_1" as key, $r.subkey || "_2" as subkey, $r.value || "_3" as value);
};

PROCESS STREAM Input USING $f(TableRow());
```

```yql
-- лямбда функция принимает запись типа `Struct<...>`
-- и возвращает список из двух одинаковых элементов
$f1 = ($x) -> {
    return AsList($x, $x);
};

-- лямбда функция принимает и возвращает тип `Stream<Struct<...>>`
-- применяет к каждому элементу функцию $f1 и возвращает строчки-дубликаты
$f2 = ($x) -> {
    return ListFlatMap($x, $f1);
};

$p = (PROCESS STREAM Input USING $f2(TableRows()));

SELECT STREAM * FROM $p;
```

{% endif %}

