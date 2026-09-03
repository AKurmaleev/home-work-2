# Отчёт о Zen 5 array-add benchmark

## Что проверяется

Дополнительное упражнение не меняет `hello.s`, `sum.s`, `fib.s` или binary bomb. Оно сравнивает две ABI-совместимые реализации операции

```text
dst[i] = lhs[i] + rhs[i]  modulo 2^32
```

- baseline: [`array_add_scalar.s`](array_add_scalar.s), один `uint32_t` за итерацию;
- candidate: [`array_add_zen5_avx512f.s`](array_add_zen5_avx512f.s), 16 `uint32_t` за векторную итерацию плюс scalar tail не более 15 элементов.

Гипотеза основана на AMD *Software Optimization Guide for the AMD Zen5 Microarchitecture*, Publication 58455, Revision 1.00: native 512-bit data paths Zen 5 могут ускорить простой data-parallel loop. Указанные в workbook latency/throughput для `vpaddd` относятся к изолированным условиям и служат только основанием гипотезы, а не результатом приложения.

## Окружение Rule63 и ограничения

Дата измерения: **2026-09-03**.

| Параметр | Значение |
|---|---|
| CPU | AMD Ryzen 9 9950X3D 16-Core Processor |
| Vendor | `AuthenticAMD` |
| Family / model | `26 (0x1A)` / `68 (0x44)` |
| Среда | WSL2, Ubuntu 24.04 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Видимая топология | 16 logical CPU, 8 physical cores, 2-way SMT |
| Закреплённый CPU | logical CPU 6 |
| SMT sibling CPU 6 | logical CPU 7 |
| Компилятор | Clang 18.1.3, target `x86_64-pc-linux-gnu` |
| C/build flags | C17, `-O3 -march=x86-64 -mtune=generic` |
| Optional ISA | AVX-512 только в отдельном assembly symbol |
| Защита executable | PIE, RELRO, NOW, NX stack |

CPU 6 выбран вместо CPU 2, потому что перед измерением на CPU 2 кратковременно наблюдался `k9s` примерно с `0.6%` CPU. Перед свежей RSS-серией environment snapshot показывал около `91.6%` idle, а historical load average — около `4.55–4.79`; load average отражает накопленную активность WSL, сборки и benchmark и не равен мгновенной конкурирующей нагрузке. Этот snapshot относится к свежей RSS-серии, а не к ранее записанным runtime raw.

Ограничения среды:

- WSL2 не позволяет benchmark управлять host power state;
- не зафиксированы CCD и размещение относительно V-Cache;
- SMT sibling CPU 7 не был принудительно переведён в idle или изолирован;
- измерения характеризуют этот WSL2 запуск, а не bare-metal предел процессора;
- для более сильного microarchitecture claim нужен повтор на native OS с контролем power mode, CCD/V-Cache placement, SMT sibling и фоновой нагрузки.

Aligned и offset layout запускались последовательно. Они не образуют paired/randomized layout experiment, поэтому различия между их абсолютными временами или speedup нельзя причинно приписывать alignment.

## Portability и real-host dispatch

Весь C-код компилируется с portable `-march=x86-64`. Глобальные `-mavx512f` и `-march=native` не используются; AVX-512 instructions находятся только в `array_add_u32_zen5_avx512f_asm`.

Real-host resolver выбирает candidate только после:

1. CPUID vendor `AuthenticAMD`;
2. effective family/model allowlist `0x1A/0x44`;
3. CPUID `XSAVE`, `OSXSAVE`, `AVX`;
4. только после этих prerequisites — `XGETBV(0)` и наличие OS state bits по mask `0xE6`;
5. CPUID leaf 7 `AVX-512F`.

[`benchmark-results/info-rule63-wsl2.jsonl`](benchmark-results/info-rule63-wsl2.jsonl) зафиксировал:

| Detector field | Rule63 value |
|---|---:|
| genuine AMD | `true` |
| family | `26 (0x1A)` |
| model | `68 (0x44)` |
| XCR0 | `0xE7` |
| XSAVE / OSXSAVE / AVX | `true / true / true` |
| AVX-512F / OS state | `true / true` |
| Zen 5 | `true` |
| `AUTO` backend | `zen5-avx512f` |

При любом несовпадении `AUTO` возвращает scalar fallback. Forced Zen 5 mode возвращает `UNSUPPORTED`, а не unsafe pointer. Synthetic selector принимает тестовые capabilities, но возвращает только enum backend; исполняемый function pointer выдаёт только resolver после real-host detection.

## Correctness и статистические тесты

Выполнены команды:

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

Результаты:

- основной suite: `PASS cases=42697 dispatch=53 guard=38`;
- обязательный фактический vector run: PASS;
- тесты trimming, summary и bootstrap statistics: PASS;
- ASan/UBSan запуски kernel harness и stats: PASS;
- сборка Clang с `-Wall -Wextra -Wpedantic -Werror`: чистая.

Проверены:

- все lengths `0..257`, а также `511/512/513` и `4095/4096/4097`;
- vector tails `15/16/17`, `31/32/33`, `63/64/65`;
- offsets `0..15` элементов от 64-byte aligned base;
- wrapping на `UINT32_MAX` и deterministic randomized values;
- `dst == lhs`, `dst == rhs`, `lhs == rhs`, все три pointer равны;
- частичное перекрытие двух read-only inputs;
- canaries и guard pages против записи или чтения за `len`;
- `len == 0` с null pointers;
- synthetic negative matrix для vendor, family, model, XSAVE, OSXSAVE, AVX, каждого XCR0 bit и AVX-512F;
- identity выбранного real scalar/vector assembly symbol;
- обязательный фактический запуск vector kernel.

ASan/UBSan не инструментирует handwritten assembly, поэтому границы отдельно защищены canaries и `PROT_NONE` guard pages.

## Проверенные артефакты и provenance

Runtime, свежий RSS measurement и RSS analysis получены разными исполняемыми артефактами. Один hash не распространяется на все наборы данных:

| Назначение | Executable | SHA-256 | `text` | `data` | `bss` | `dec` | `hex` |
|---|---|---|---:|---:|---:|---:|---:|
| Ранее записанный runtime | `array-add-bench` | `c5a46db046f40c6f01c88c66f650565501ffbdd372cbaa284c6be3d6d59cc6b7` | 25737 | 840 | 8 | 26585 | `67d9` |
| Свежий RSS measurement | `array-add-bench` | `a45e059eff1842ab39674a900d9f3c2bb679b28a3c1b11767a955fcba4229edd` | 28727 | 840 | 8 | 29575 | `7387` |
| RSS inference | `array-add-rss-analyze` | `6fe6bdaf489e9d9840f2c143a623e8f7f0924fee816d3c8deb57fd5801842831` | 14906 | 792 | 8 | 15706 | `3d5a` |

Точная привязка результатов:

- prior runtime artifact создал [`info-rule63-wsl2.jsonl`](benchmark-results/info-rule63-wsl2.jsonl), [`array-add-aligned-rule63-wsl2.jsonl`](benchmark-results/array-add-aligned-rule63-wsl2.jsonl) и [`array-add-offset4-rule63-wsl2.jsonl`](benchmark-results/array-add-offset4-rule63-wsl2.jsonl);
- fresh RSS measurement artifact создал [`array-add-scalar-rss-rule63-wsl2.jsonl`](benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl) и [`array-add-zen5-rss-rule63-wsl2.jsonl`](benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl);
- RSS analyzer создал [`array-add-rss-analysis-rule63-wsl2.jsonl`](benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl).

Текущий `array-add-bench` не выдаётся за источник ранее записанных runtime raw. Read-only проверки `file`, `readelf`, `size`, `nm` и `objdump` зафиксированы для runtime artifact и повторены для двух текущих артефактов. Оба текущих executable — ELF64 System V x86-64 PIE, имеют non-executable `GNU_STACK` (`RW`, без `E`), `GNU_RELRO`, `BIND_NOW`/NOW и не stripped.

Assembly sources kernel между prior runtime и fresh RSS measurement не менялись. В текущем RSS measurement artifact повторно проверены точные linked symbols; адреса являются link-time virtual offsets PIE:

| Symbol | Address | Size | Binding / visibility |
|---|---:|---:|---|
| `array_add_u32_scalar_asm` | `0x3e74` | 29 bytes | `GLOBAL HIDDEN` |
| `array_add_u32_zen5_avx512f_asm` | `0x3e94` | 75 bytes | `GLOBAL HIDDEN` |

Воспроизводимая проверка exact linked symbol в AT&T-синтаксисе:

```sh
objdump -d --disassemble=array_add_u32_zen5_avx512f_asm array-add-bench
```

Candidate hot loop содержит:

```asm
vmovdqu32 (%rsi), %zmm0
vpaddd    (%rdx), %zmm0, %zmm0
vmovdqu32 %zmm0, (%rdi)
addq      $64, %rsi
addq      $64, %rdx
addq      $64, %rdi
subq      $16, %rcx
cmpq      $16, %rcx
jae       vector_loop
```

Каждая векторная итерация обрабатывает 16 `uint32_t`. Остаток `0..15` выполняется scalar loop без masked over-read. Prefetch, non-temporal stores, unrolling и speculative padding не добавлялись.

## Rule63 runtime protocol

### Набор данных и измерение

| Параметр | Значение |
|---|---:|
| Elements | 16,777,216 `uint32_t` |
| Размер `lhs` | 64 MiB |
| Размер `rhs` | 64 MiB |
| Размер `dst` | 64 MiB |
| Logical payload representation | 192 MiB |
| Effective bandwidth model | 12 algorithmic bytes/element |
| Генератор данных | fixed-seed SplitMix64 |
| Seed | `1592594996` |
| Affinity | ровно один logical CPU, CPU 6 |
| Excluded warm-up | 10 полных пар |
| Measured complete pairs | `N=101` |
| Порядок | чередование scalar→candidate (`AB`) и candidate→scalar (`BA`) |
| Timer | `CLOCK_MONOTONIC_RAW` |
| Inner repetitions | 1 |
| Minimum timed interval | 5 ms |

Текущее значение `--samples` по умолчанию и минимально допустимое значение — `101`. Одна полная A/B-пара — экспериментальная единица. Обе реализации внутри пары обрабатывают одни и те же resident arrays. Affinity проверяется benchmark; timed interval короче 5 ms отклоняется с требованием увеличить `--inner`.

`12` algorithmic bytes/element — две загрузки по 4 bytes и одна запись 4 bytes. Это нормализованная effective bandwidth, а не измеренный физический DRAM/cache traffic; write allocation, cache effects и hardware prefetch в это число не декомпозируются.

### Rule63 trimming и inference

1. Для каждой полной пары вычисляется `ln(scalar_time/candidate_time)`.
2. `N=101` пар сортируются по этой метрике по возрастанию.
3. Tie-break детерминирован: сначала pair/sample ID, затем acquisition index.
4. `t=floor(N×15/100)=15` целых пар удаляются с каждого края.
5. Остаётся `101−15−15=71` pair; один и тот же retained set используется для статистик scalar, candidate и paired ratio.
6. Ad-hoc outlier removal сверх этого правила не применяется.

Публикуются:

- trimmed arithmetic mean времени на общем retained set;
- retained median и retained MAD для каждого backend;
- ratio trimmed means `scalar_mean/candidate_mean`;
- primary paired estimate: geometric mean `scalar/candidate` по retained pairs;
- уменьшение времени candidate, рассчитанное из retained paired geomean speedup как `1−1/geomean`;
- retained-only paired percentile bootstrap: 10,000 повторов, 95% CI, без повторного trimming внутри bootstrap replicate;
- deterministic bootstrap seed `7640891576956012809`;
- практический порог 5%: нижняя граница speedup CI должна быть не меньше `1.05`;
- критерий достаточности: `(CI_high−CI_low)/point_estimate ≤ 0.10`.

Каждая raw sample сохраняет исходный pair ID/order, времена, ratio, `paired_log_ratio_rank`, `trim_class` и `retained`, поэтому состав trimmed tails и retained set проверяем.

### Команды воспроизведения runtime raw data

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

Оба runtime raw-файла содержат 103 строки: config, 101 raw sample и summary.

## Runtime results

### 64-byte aligned: effective addresses `mod 64 = 0`

Raw: [`benchmark-results/array-add-aligned-rule63-wsl2.jsonl`](benchmark-results/array-add-aligned-rule63-wsl2.jsonl).

| Метрика | Scalar | Zen 5 AVX-512F |
|---|---:|---:|
| Trimmed arithmetic mean | 8.078376 ms | 5.593868 ms |
| Retained median | 8.085487 ms | 5.593793 ms |
| Retained MAD | 0.173918 ms | 0.131730 ms |
| Effective BW, trimmed-mean denominator | 24.9217 GB/s | 35.9906 GB/s |
| Effective BW, retained-median denominator | 24.8997 GB/s | 35.9911 GB/s |

| Сводная paired метрика | Значение |
|---|---:|
| Ratio of trimmed means | `1.444148×` |
| Retained paired geomean speedup | **`1.444719×`** |
| Candidate time reduction из retained paired geomean (`1−1/geomean`) | `30.7824%` |
| Retained-only paired bootstrap 95% CI | `[1.432318, 1.457129]` |
| Relative total CI width | `0.017174` |
| Adequacy limit / result | `≤0.10` / PASS |
| Practical threshold / result | CI low `≥1.05` / PASS |
| Verdict | `improved`; practical gain proven |

### Offset +4 bytes: effective addresses `mod 64 = 4`

Raw: [`benchmark-results/array-add-offset4-rule63-wsl2.jsonl`](benchmark-results/array-add-offset4-rule63-wsl2.jsonl).

| Метрика | Scalar | Zen 5 AVX-512F |
|---|---:|---:|
| Trimmed arithmetic mean | 7.901255 ms | 6.033465 ms |
| Retained median | 7.756275 ms | 5.945920 ms |
| Retained MAD | 0.257257 ms | 0.190120 ms |
| Effective BW, trimmed-mean denominator | 25.4803 GB/s | 33.3683 GB/s |
| Effective BW, retained-median denominator | 25.9566 GB/s | 33.8596 GB/s |

| Сводная paired метрика | Значение |
|---|---:|
| Ratio of trimmed means | `1.309572×` |
| Retained paired geomean speedup | **`1.309650×`** |
| Candidate time reduction из retained paired geomean (`1−1/geomean`) | `23.6437%` |
| Retained-only paired bootstrap 95% CI | `[1.297651, 1.321694]` |
| Relative total CI width | `0.018358` |
| Adequacy limit / result | `≤0.10` / PASS |
| Practical threshold / result | CI low `≥1.05` / PASS |
| Verdict | `improved`; practical gain proven |

Допустимый вывод делается только внутри каждого layout: candidate быстрее scalar, CI узкий и полностью выше `1.05`. Нельзя заключать, что alignment вызвал разницу между двумя таблицами: layout runs последовательны и не paired друг с другом.

## Память, allocations и algorithmic complexity

### RSS protocol и воспроизведение

RSS измерен встроенным Linux `getrusage(...).ru_maxrss` в KiB, отдельно от runtime. Каждый process обрабатывает `16,777,216` элементов: три массива по `64 MiB`, всего `192 MiB` logical payload. Для каждого backend сначала запущен один отдельный warm-up process; его результат полностью исключён. Затем последовательно выполнен 31 запуск на CPU 6, каждый новым process ID и с уникальным `--run-id` `1..31` внутри варианта.

Текущий `Makefile` предоставляет отдельные build/test/sanitizer targets analyzer:

```sh
make array-add-rss-analyze
make test-array-add-rss-analyze
make array-add-rss-analyze-sanitize
make test-array-add-rss-analyze-sanitize
```

Исходник и regression test находятся в [`array_add_rss_analyze.c`](array_add_rss_analyze.c) и [`array_add_rss_analyze_test.sh`](array_add_rss_analyze_test.sh).

Fresh-process raw воспроизводится так:

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

CLI требует baseline первым и candidate вторым:

```text
usage: ./array-add-rss-analyze <baseline.jsonl> <candidate.jsonl>
```

Rule63 analysis создаётся так:

```sh
./array-add-rss-analyze \
  benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl \
  benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl \
  > benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl
```

RSS distributions не paired. Каждая независимо сортируется по RSS и trim-ится: `N=31`, по `t=4` observations с каждого края, retained `23`. Tie-break детерминирован: sample ID, затем acquisition index. По двум retained sets выполняется независимый percentile bootstrap без повторного trimming внутри replicate: `10,000` повторов, seed `7640891576956012809`, 95% CI.

Estimand — relative reduction:

```text
1 - candidate_trimmed_mean / baseline_trimmed_mean
```

До анализа заданы practical-reduction threshold `5%`, equivalence bounds `±1%` и adequacy limit абсолютной полной ширины CI `≤0.005` reduction fraction, то есть `≤0.5` percentage points. Verdict `improved` требует `CI_low ≥ 0.05`; `regressed` — `CI_high ≤ -0.05`; `practically_equivalent` — весь CI внутри `[-0.01, 0.01]`. При неадекватном или invalid CI результат `inconclusive`.

Analysis JSONL содержит ровно 64 строки: config, 62 classified sample lines и summary. Это process-wide peak RSS inference, не runtime sample и не kernel-only memory profile.

### RSS results

| Метрика | Scalar baseline | Zen 5 candidate |
|---|---:|---:|
| Raw fresh processes | 31 | 31 |
| Trimmed tails | 4 low + 4 high | 4 low + 4 high |
| Retained observations | 23 | 23 |
| Trimmed arithmetic mean | 198249.73913043478 KiB | 198272 KiB |
| Retained median | 198272 KiB | 198272 KiB |
| Retained MAD | 0 KiB | 0 KiB |

| Inference для mean RSS | Значение |
|---|---:|
| Candidate / baseline ratio | `1.0001122870055863` |
| Relative reduction | `-0.00011228700558629789` (`-0.0112287006%`) |
| Retained-only independent bootstrap 95% CI | `[-0.00022459923074769073, -0.000028069387525864187]` |
| Тот же CI в процентах | `[-0.0224599231%, -0.00280693875%]` |
| Absolute total CI width | `0.00019652984322182654` (`0.0196529843` percentage points) |
| Adequacy limit / result | `≤0.005` / PASS |
| Equivalence bounds / result | `±0.01` / весь CI внутри границ |
| Practical reduction threshold | `0.05`; уменьшение не установлено |
| Verdict | **`practically_equivalent`** |

Candidate descriptively имеет немного больший mean RSS, и весь CI отрицателен в терминах reduction. Однако весь CI также далеко внутри заранее заданных equivalence bounds `±1%`: practical peak RSS equivalence установлена. Уменьшение памяти не заявляется.

### Разделение метрик kernel и harness

| Метрика | Scalar | Zen 5 AVX-512F | Интерпретация |
|---|---:|---:|---|
| Время kernel | `Θ(n)` | `Θ(n)` | Один линейный проход по `n` элементам |
| Auxiliary kernel space | `Θ(1)` | `Θ(1)` | Только регистры/счётчики, независимо от `n` |
| Kernel allocations | 0 | 0 | Буферы передаёт caller |
| Harness payload | `Θ(n)` | `Θ(n)` | `lhs`, `rhs`, `dst`: всего `12n` bytes |
| Payload при `n=16,777,216` | 192 MiB | 192 MiB | Ровно `3 × 64 MiB` |
| Process peak RSS, retained mean | 198249.73913043478 KiB | 198272 KiB | Payload плюс allocator/runtime/process overhead; practically equivalent в заданных `±1%` bounds |
| Allocation profiler | не использовался | не использовался | Нельзя делать выводы об allocator metadata/traffic сверх наблюдаемого RSS |

Векторизация меняет инструкции и runtime, но не data representation, asymptotic memory или число kernel allocations. RSS — process-wide high-water mark; он не разделяет payload, allocator metadata, runtime и page accounting.

## Raw data

- [`benchmark-results/info-rule63-wsl2.jsonl`](benchmark-results/info-rule63-wsl2.jsonl) — real CPUID/XCR0, OS state и выбранный dispatch;
- [`benchmark-results/array-add-aligned-rule63-wsl2.jsonl`](benchmark-results/array-add-aligned-rule63-wsl2.jsonl) — aligned config, 101 ranked/classified raw pairs и summary;
- [`benchmark-results/array-add-offset4-rule63-wsl2.jsonl`](benchmark-results/array-add-offset4-rule63-wsl2.jsonl) — offset +4 config, 101 ranked/classified raw pairs и summary;
- [`benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl`](benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl) — 31 scalar fresh-process RSS observations с run ID;
- [`benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl`](benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl) — 31 Zen 5 fresh-process RSS observations с run ID;
- [`benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl`](benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl) — config, 62 classified RSS observations и inference summary.

## Вывод

- Correctness, статистический код и real dispatch подтверждены обычными, mandatory-vector и sanitizer tests.
- На aligned layout Rule63 paired geomean speedup равен **`1.444719×`**, 95% CI `[1.432318, 1.457129]`; geomean-derived time reduction `1−1/geomean` равен `30.7824%`.
- На offset +4 layout paired geomean speedup равен **`1.309650×`**, 95% CI `[1.297651, 1.321694]`; geomean-derived time reduction равен `23.6437%`.
- Оба runtime CI проходят practical threshold `1.05` и adequacy limit относительной ширины `0.10`.
- Между layout нет paired experiment, поэтому причинный alignment claim не делается.
- Kernel complexity остаётся `Θ(n)` time и `Θ(1)` auxiliary space, kernel allocations отсутствуют.
- Representation равен 192 MiB для обоих backend; свежий RSS CI полностью внутри `±1%`, поэтому process peak RSS признан `practically_equivalent`, а не reduced.
- Portable scalar fallback сохранён; unsupported host не получает executable AVX-512 pointer.
