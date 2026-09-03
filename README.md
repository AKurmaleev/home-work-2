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
- [`array_add_stats.c`](array_add_stats.c) и [`array_add_stats_test.c`](array_add_stats_test.c) — deterministic trimming, сводные метрики и тесты статистики;
- [`array_add_bench.c`](array_add_bench.c) — paired AB/BA timing, raw JSONL, bootstrap CI и отдельный peak RSS mode;
- [`array_add_rss_analyze.c`](array_add_rss_analyze.c) и [`array_add_rss_analyze_test.sh`](array_add_rss_analyze_test.sh) — независимый RSS trim, bootstrap CI, equivalence verdict и тесты analyzer;
- [`ZEN5_ARRAY_ADD_REPORT.md`](ZEN5_ARRAY_ADD_REPORT.md) — полная методика Rule63, окружение, raw data и результаты.

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
make clean
make all array-add
make test-array-add
make test-array-add-stats
make test-array-add-zen5
make array-add-test-sanitize array-add-stats-test-sanitize
./array-add-test-sanitize --require-vector
./array-add-stats-test-sanitize
```

Проверка 2026-09-03 прошла: `PASS cases=42697 dispatch=53 guard=38`, обязательный vector run и stats tests — PASS, ASan/UBSan ошибок не нашли, сборка Clang с `-Werror` чистая.

`all` по-прежнему собирает только исходные учебные программы. Array-add targets не наследуют учебный `-no-pie`: они собираются как PIE с RELRO, NOW и non-executable stack.

Для отдельного RSS analyzer в текущем `Makefile` предусмотрены точные build/test/sanitizer targets:

```sh
make array-add-rss-analyze
make test-array-add-rss-analyze
make array-add-rss-analyze-sanitize
make test-array-add-rss-analyze-sanitize
```

### Rule63 runtime benchmark

`compare` по умолчанию использует и требует минимум `101` измеренную полную пару. Для Rule63 использованы `10` полностью исключённых warm-up пар, затем `N=101`; отбрасываются `t=floor(N×15/100)=15` целых пар с каждого края упорядоченного `ln(scalar/candidate)`, остаются `71` пара. Tie-break детерминирован по pair ID и acquisition index; один retained set применяется ко всем метрикам обеих реализаций. Порядок внутри пары чередуется AB/BA, timer — `CLOCK_MONOTONIC_RAW`, каждый timed interval должен быть не короче `5 ms`.

Воспроизведение raw JSONL на CPU 6:

```sh
taskset -c 6 ./array-add-bench info \
  > benchmark-results/info-rule63-wsl2.jsonl

taskset -c 6 ./array-add-bench compare \
  --elements 16777216 \
  --samples 101 \
  --warmups 10 \
  --inner 1 \
  --seed 1592594996 \
  --alignment 64 \
  --offset-u32 0 \
  > benchmark-results/array-add-aligned-rule63-wsl2.jsonl

taskset -c 6 ./array-add-bench compare \
  --elements 16777216 \
  --samples 101 \
  --warmups 10 \
  --inner 1 \
  --seed 1592594996 \
  --alignment 64 \
  --offset-u32 1 \
  > benchmark-results/array-add-offset4-rule63-wsl2.jsonl
```

Если timed interval короче `5 ms`, увеличьте `--inner`. `--offset-u32 1` даёт effective address `mod 64 = 4`.

Rule63 result: paired geomean speedup равен `1.444719×`, 95% CI `[1.432318, 1.457129]` для aligned case и `1.309650×`, CI `[1.297651, 1.321694]` для offset `+4`. Уменьшение времени, вычисленное именно из retained paired geomean как `1−1/geomean`, равно `30.7824%` и `23.6437%` соответственно; это не reduction из ratio of trimmed means. Оба CI доказывают практический эффект выше порога `1.05`; относительная полная ширина CI `0.017174` и `0.018358`, ниже лимита адекватности `0.10`. Эти layout запускались последовательно и не были paired друг с другом, поэтому причинное сравнение aligned и offset недопустимо.

### Peak RSS отдельными процессами

RSS измеряется отдельно от runtime через Linux `getrusage(...).ru_maxrss`. Один warm-up process на реализацию полностью исключается. Затем для каждого варианта последовательно запускается `31` свежий process с run ID `1..31`; runtime samples из RSS mode не выводятся. Каждый process обрабатывает `16,777,216` элементов и держит три массива по `64 MiB`, то есть `192 MiB` logical payload.

```sh
for impl in scalar zen5; do
  taskset -c 6 ./array-add-bench rss \
    --impl "$impl" --run-id 0 --elements 16777216 \
    --seed 1592594996 --alignment 64 --offset-u32 0 \
    > /dev/null

  output="benchmark-results/array-add-${impl}-rss-rule63-wsl2.jsonl"
  : > "$output"
  for run_id in $(seq 1 31); do
    taskset -c 6 ./array-add-bench rss \
      --impl "$impl" --run-id "$run_id" --elements 16777216 \
      --seed 1592594996 --alignment 64 --offset-u32 0 \
      >> "$output"
  done
done
```

Analyzer принимает baseline первым, candidate вторым:

```text
usage: ./array-add-rss-analyze <baseline.jsonl> <candidate.jsonl>
```

```sh
./array-add-rss-analyze \
  benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl \
  benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl \
  > benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl
```

RSS distributions не paired. Каждая реализация независимо сортируется по RSS и trim-ится: `N=31`, `t=4` на каждом краю, retained `23`; ties разрешаются по sample ID, затем acquisition index. По двум retained sets выполняется независимый percentile bootstrap без повторного trimming: `10,000` повторов, seed `7640891576956012809`, 95% CI для `1−candidate_trimmed_mean/baseline_trimmed_mean`. Предъявлены practical-reduction threshold `5%`, equivalence bounds `±1%` и adequacy limit абсолютной полной ширины CI `≤0.005` (`≤0.5` percentage points).

| RSS metric | Scalar baseline | Zen 5 candidate |
|---|---:|---:|
| Trimmed mean | `198249.73913043478 KiB` | `198272 KiB` |
| Retained median | `198272 KiB` | `198272 KiB` |
| Retained MAD | `0 KiB` | `0 KiB` |

Candidate/baseline mean ratio равен `1.0001122870055863`. Relative reduction равен `-0.00011228700558629789` (`-0.0112287006%`), 95% CI `[-0.00022459923074769073, -0.000028069387525864187]`, или `[-0.0224599231%, -0.00280693875%]`. Абсолютная полная ширина CI `0.00019652984322182654`, то есть `0.0196529843` percentage points, проходит adequacy limit. Весь CI находится внутри `±1%`, поэтому verdict — **`practically_equivalent`**. Candidate описательно чуть выше, но practical peak RSS equivalence установлена в заранее заданных границах; уменьшение памяти не заявляется.

Raw RSS находятся в [`array-add-scalar-rss-rule63-wsl2.jsonl`](benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl) и [`array-add-zen5-rss-rule63-wsl2.jsonl`](benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl). Классификация всех `62` observations и summary находятся в 64-строчном [`array-add-rss-analysis-rule63-wsl2.jsonl`](benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl): config, 62 classified sample lines и summary.

Происхождение данных разделено по исполняемым артефактам:

| Данные | Artifact SHA-256 | GNU `size`: text/data/bss/dec/hex |
|---|---|---|
| `info` и оба runtime raw | `array-add-bench` `c5a46db046f40c6f01c88c66f650565501ffbdd372cbaa284c6be3d6d59cc6b7` | `25737/840/8/26585/67d9` |
| Fresh scalar/Zen 5 RSS raw | `array-add-bench` `a45e059eff1842ab39674a900d9f3c2bb679b28a3c1b11767a955fcba4229edd` | `28727/840/8/29575/7387` |
| RSS analysis JSONL | `array-add-rss-analyze` `6fe6bdaf489e9d9840f2c143a623e8f7f0924fee816d3c8deb57fd5801842831` | `14906/792/8/15706/3d5a` |

Перед свежей RSS-серией environment snapshot показывал примерно `91.6%` idle; historical load average был около `4.55–4.79` и не равен мгновенной конкурирующей нагрузке. WSL2 не даёт контролировать host power, CCD/V-Cache placement или idle-состояние SMT sibling, поэтому bare-metal claim не делается.

Оба kernel имеют время `Θ(n)`, auxiliary space `Θ(1)` и не выделяют память. RSS — process-wide high-water mark: он включает payload и allocator/runtime overhead и не является kernel-only allocation profile. Полные таблицы и ограничения приведены в [`ZEN5_ARRAY_ADD_REPORT.md`](ZEN5_ARRAY_ADD_REPORT.md).
