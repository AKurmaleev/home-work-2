# Assembly + Binary Bomb

Домашняя работа по x86-64 assembly для Linux. Ассемблерные исходники написаны в AT&T-синтаксисе, используют System V AMD64 ABI и собираются через `clang`.

## Состав работы

| Задача | Файл | Результат |
|---|---|---|
| Hello, world! | [`hello.s`](hello.s) | Выводит `Hello, world!` через `puts` |
| Сложение | [`sum.s`](sum.s) | Складывает `20` и `22` инструкцией `addq`, выводит `42` |
| Fibonacci | [`fib.s`](fib.s) | Циклом вычисляет `F(10)`, выводит `55` |
| Binary Bomb | [`BOMB_REPORT.md`](BOMB_REPORT.md) | Содержит ответы и краткий разбор всех фаз |

`Makefile` собирает и запускает три ассемблерные программы. Скомпилированные ELF-файлы и объектные файлы не входят в репозиторий.

## Требования

- Linux x86-64;
- `clang`;
- GNU Make;
- libc.

## Сборка и запуск

```sh
make clean all
make run
make clean
```

Ожидаемый вывод:

```text
Hello, world!
42
F(10) = 55
```

Эквивалентная ручная сборка:

```sh
clang -no-pie hello.s -o hello
clang -no-pie sum.s -o sum
clang -no-pie fib.s -o fib
```

`-no-pie` используется только как учебное упрощение для стабильных адресов в `objdump` и GDB. Это не production-рекомендация.

## Реализация

### Общие детали ABI

- Точка входа каждой программы — `main`, поэтому запуск и завершение обслуживает libc.
- Первый и второй целочисленные аргументы функций передаются через `%rdi` и `%rsi`.
- Перед variadic-вызовом `printf` регистр `%eax` обнуляется согласно System V AMD64 ABI.
- `pushq %rbp` обеспечивает требуемое выравнивание стека перед вызовом libc.
- Строки находятся в `.rodata` и адресуются относительно `%rip`.
- `.note.GNU-stack` оставляет стек неисполняемым.

### Сложение

- `%rax` хранит первое слагаемое, затем результат.
- `%rcx` хранит второе слагаемое.
- `addq %rcx, %rax` выполняет сложение.
- `%rdi` и `%rsi` передают формат и результат в `printf`.

### Fibonacci

- `%rax` хранит текущее значение `F(i)`.
- `%rdx` хранит следующее значение `F(i + 1)`.
- `%r8` временно хранит сумму.
- `%rcx` хранит число оставшихся итераций.

Каждая итерация переводит пару `(F(i), F(i + 1))` в `(F(i + 1), F(i + 2))`. Результат вычисляется циклом, а не загружается готовой константой.

## Binary Bomb

Бомба исследована в AT&T-синтаксисе с помощью `file`, `strings`, `nm`, `readelf`, `objdump` и GDB. Проверялись функции `phase_1`–`phase_4`, рекурсивная `fib`, `secret_phase` и путь к `explode_bomb`.

Ответы, найденные сравнения, условные переходы, таблица переходов и использованные команды приведены в [`BOMB_REPORT.md`](BOMB_REPORT.md).

## Проверка

Работа проверена на Ubuntu 24.04 x86-64 с Clang 18:

- `make clean all run` успешно собирает и запускает все программы;
- `file` подтверждает ELF64 x86-64;
- `readelf -lW` показывает `GNU_STACK` без права исполнения и наличие `GNU_RELRO`;
- `objdump -d --disassemble=main fib` подтверждает наличие цикла с `test`, `je`, `dec` и `jne`;
- рассчитанный ввод полностью обезвреживает четыре основные и скрытую фазы `bomb_hard`.

## Дополнительный opt-in benchmark для AMD Zen 5

Отдельное упражнение сравнивает сложение массивов `uint32_t` по модулю `2^32`:

- [`array_add_scalar.s`](array_add_scalar.s) — portable scalar x86-64 baseline;
- [`array_add_zen5_avx512f.s`](array_add_zen5_avx512f.s) — 16 элементов за итерацию через `vmovdqu32` и `vpaddd`;
- [`array_add_dispatch.c`](array_add_dispatch.c) — real-host CPUID/XGETBV dispatch;
- [`array_add_test.c`](array_add_test.c) — tails, alignment offsets, aliasing, canaries, guard pages и synthetic selector matrix;
- [`array_add_bench.c`](array_add_bench.c) — paired AB/BA timing, raw JSONL, MAD, bootstrap CI и отдельный peak RSS mode;
- [`ZEN5_ARRAY_ADD_REPORT.md`](ZEN5_ARRAY_ADD_REPORT.md) — методика, окружение, raw data и измеренный результат.

Это условная оптимизация, применённая вручную для известного hardware target. Обычные `hello`, `sum` и `fib` не требуют Zen 5 и не менялись.

### Безопасный dispatch

AVX-512 код находится только в отдельном assembly symbol. Остальной executable собирается с portable `-march=x86-64`, без глобального `-mavx512f` или `-march=native`.

Векторный backend выбирается только когда real-host проверка подтверждает:

- `AuthenticAMD`, family `0x1A`, измеренный model allowlist `0x44`;
- CPUID `XSAVE`, `OSXSAVE`, `AVX`;
- после этих проверок — `XGETBV(0)` mask `0xE6`;
- CPUID leaf 7 `AVX-512F`.

Иначе `AUTO` возвращает scalar fallback. Synthetic тестовые данные выбирают только enum backend и не могут вернуть исполняемый function pointer.

### Сборка и correctness

```sh
make clean all array-add
make test-array-add
make test-array-add-zen5       # требует target CPU и реальный vector run
make array-add-test-sanitize
./array-add-test-sanitize --require-vector
```

`all` по-прежнему собирает только исходные учебные программы. Array-add targets не наследуют учебный `-no-pie`: они собираются как PIE с RELRO, NOW и non-executable stack.

### Benchmark

Runtime mode требует affinity ровно к одному logical CPU, не менее 21 samples и не менее 5 ms на timed interval:

```sh
taskset -c 2 ./array-add-bench info

taskset -c 2 ./array-add-bench compare \
  --elements 16777216 \
  --samples 101 \
  --warmups 10 \
  --inner 1 \
  --seed 1592594996 \
  --alignment 64 \
  --offset-u32 0
```

Если timed interval короче 5 ms, увеличьте `--inner`. Для проверки cache-line crossing можно повторить с `--offset-u32 1`.

Peak RSS измеряется отдельными процессами и не используется как runtime sample:

```sh
/usr/bin/time -f "%M" taskset -c 2 ./array-add-bench rss \
  --impl scalar --elements 16777216 --seed 1592594996 \
  --alignment 64 --offset-u32 0

/usr/bin/time -f "%M" taskset -c 2 ./array-add-bench rss \
  --impl zen5 --elements 16777216 --seed 1592594996 \
  --alignment 64 --offset-u32 0
```

Scalar и AVX-512 используют одинаковые три массива. Ускорение не означает уменьшение памяти; peak RSS публикуется отдельной метрикой.
