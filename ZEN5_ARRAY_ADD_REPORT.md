# AMD Zen 5 array-add benchmark

## Что проверяется

Дополнительное упражнение не меняет `hello.s`, `sum.s`, `fib.s` или binary bomb. Оно сравнивает две ABI-совместимые реализации:

```text
dst[i] = lhs[i] + rhs[i]  modulo 2^32
```

- baseline: [`array_add_scalar.s`](array_add_scalar.s), один `uint32_t` за итерацию;
- candidate: [`array_add_zen5_avx512f.s`](array_add_zen5_avx512f.s), 16 `uint32_t` через 512-bit `vpaddd` плюс scalar tail.

Гипотеза из AMD *Software Optimization Guide for the AMD Zen5 Microarchitecture*, Publication 58455, Revision 1.00: native 512-bit data paths Zen 5 могут ускорить простой data-parallel loop. Workbook указывает для `vpaddd` estimated latency 1 и throughput 4 в изолированных L1/no-contention условиях; эти числа используются только как гипотеза, не как результат приложения.

## Target и ограничения

Дата измерения: 2026-09-02.

```text
CPU reported by OS: AMD Ryzen 9 9950X3D 16-Core Processor
Vendor/family/model/stepping: AuthenticAMD / 26 (0x1A) / 68 (0x44) / 0
Environment: Ubuntu 24.04 under WSL2
Kernel: 6.6.87.2-microsoft-standard-WSL2
Visible topology: 16 logical CPUs, 8 cores, 2 threads/core
Pinned logical CPU: 2
Reported SMT sibling: 3
Clang: 18.1.3, target x86_64-pc-linux-gnu
GNU binutils: 2.42
Build: C17, -O3, -march=x86-64, -mtune=generic, PIE
Optional ISA flags on whole binary: none
Hardware counters: unavailable (`perf` absent)
```

Аппаратная база оказалась **Ryzen 9 9950X3D**, а не заявленным ранее 9950X. Dispatch allowlist ограничен реально измеренными AMD family/model `0x1A/0x44`.

WSL2 не позволяет надёжно фиксировать CCD/V-Cache placement, host power state или простой SMT sibling. Worker закреплён на CPU 2, affinity mask проверяется внутри benchmark и migration проверяется до/после каждого timed interval. CPU 3 не был принудительно изолирован. Background load не был нулевым: load average около `5.62`. Результаты относятся к WSL2; для строгого bare-metal вывода нужен повтор на native Linux или Windows-native harness.

## Portability и dispatch

Весь C-код компилируется с portable `-march=x86-64`. AVX-512 instructions находятся только в отдельном assembly symbol.

Real-host resolver выбирает candidate только после:

1. CPUID vendor `AuthenticAMD`;
2. effective family/model allowlist `0x1A/0x44`;
3. CPUID `XSAVE`, `OSXSAVE`, `AVX`;
4. только после этих prerequisites — `XGETBV(0)` и mask `0xE6`;
5. CPUID leaf 7 `AVX-512F`.

`AUTO` возвращает scalar fallback при любом несовпадении. Forced Zen 5 mode возвращает `UNSUPPORTED`, а не unsafe pointer. Synthetic selector принимает тестовые CPU capabilities, но возвращает только enum backend; исполняемый pointer выдаёт только resolver после real-host detection.

## Correctness

Команды:

```sh
make clean all array-add
make run
make test-array-add-zen5
make array-add-test-sanitize
./array-add-test-sanitize --require-vector
```

Результат обычного и ASan/UBSan запусков:

```text
PASS array_add_test cases=42697 dispatch=53 guard=38 \
  implementations=2 vector_required=true vector_executed=true \
  auto=zen5-avx512f
```

Проверены:

- все lengths `0..257`, а также `511/512/513` и `4095/4096/4097`;
- vector tails `15/16/17`, `31/32/33`, `63/64/65`;
- offsets `0..15` элементов от 64-byte aligned base;
- wrapping на `UINT32_MAX` и deterministic randomized values;
- `dst == lhs`, `dst == rhs`, `lhs == rhs`, все три pointer равны;
- частичное перекрытие двух read-only inputs;
- canaries и guard pages против записи/чтения за `len`;
- `len == 0` с null pointers;
- synthetic negative matrix для vendor, family, model, XSAVE, OSXSAVE, AVX, каждого XCR0 bit и AVX-512F;
- identity выбранного real scalar/vector assembly symbol;
- обязательный фактический запуск vector kernel.

ASan/UBSan не инструментирует handwritten assembly, поэтому границы дополнительно проверяются canaries и `PROT_NONE` guard pages.

## Final artifact

`readelf` подтвердил:

- ELF64 System V x86-64;
- PIE (`Type: DYN`);
- non-executable `GNU_STACK` (`RW`, без `E`);
- `GNU_RELRO`;
- `BIND_NOW`.

Размер `array-add-bench`:

```text
text=20454 data=840 bss=8 total=21302 bytes
```

Symbols:

```text
array_add_u32_scalar_asm:          29 bytes, GLOBAL HIDDEN
array_add_u32_zen5_avx512f_asm:    75 bytes, GLOBAL HIDDEN
```

AT&T disassembly candidate содержит ожидаемый hot loop:

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

Scalar tail выполняется только для остатка `0..15`, без masked over-read. `cmp`/`jcc` стоят рядом, сохраняя branch-fusion opportunity. Prefetch, non-temporal stores, unrolling и speculative padding не добавлялись.

## Runtime protocol

Основной набор:

```text
Elements: 16,777,216
Payload per array: 64 MiB
Arrays: lhs, rhs, dst
Total payload representation: 192 MiB
Values: fixed-seed SplitMix64, seed 1592594996
Samples: 101 paired observations
Warm-ups: 10 pairs
Order: alternating scalar→AVX-512 / AVX-512→scalar
Timer: CLOCK_MONOTONIC_RAW
Affinity: exactly one logical CPU, CPU 2
Minimum timed interval: 5 ms
Outlier removal: none
Experimental unit: one paired processing of the same resident arrays
Point estimate: geometric mean of paired scalar/candidate ratios
Dispersion: MAD per implementation
Inference: deterministic paired percentile bootstrap, 10,000 replicates
Confidence level: 95%
Practical threshold: CI lower bound >= 1.05
Effective bandwidth: 12 algorithmic bytes/element, not physical traffic
```

Команда aligned case:

```sh
taskset -c 2 ./array-add-bench compare \
  --elements 16777216 \
  --samples 101 \
  --warmups 10 \
  --inner 1 \
  --seed 1592594996 \
  --alignment 64 \
  --offset-u32 0
```

Второй прогон отличался только `--offset-u32 1`, то есть effective addresses имели `mod 64 = 4`.

## Runtime results

| Layout | Scalar median ± MAD | AVX-512 median ± MAD | Paired speedup | 95% CI | Effective bandwidth scalar → AVX-512 | Verdict |
|---|---:|---:|---:|---:|---:|---|
| 64-byte aligned | 9.255 ± 0.573 ms | 7.722 ± 0.573 ms | **1.230×** | [1.204, 1.256] | 21.75 → 26.07 GB/s | improved |
| +4-byte offset | 7.502 ± 0.448 ms | 7.022 ± 0.489 ms | **1.081×** | [1.051, 1.112] | 26.83 → 28.67 GB/s | improved |

В aligned run paired speedup 1.230× соответствует примерно **18.7% меньшему времени** на пару. В +4-byte run — примерно **7.5%**. Обе нижние границы CI выше практического порога 1.05.

Нельзя заключить, что alignment сам по себе сделал scalar или AVX-512 быстрее: два layout запускались последовательно, а не как один paired layout experiment; абсолютные медианы показывают сильный environment drift. Допустимый вывод уже: AVX-512 candidate превзошёл scalar внутри каждого layout, но его relative advantage оказался layout-sensitive в этом WSL запуске.

## Память и allocations

Peak RSS измерялся отдельно от runtime, по 10 fresh processes на backend:

```sh
/usr/bin/time -f "%M" taskset -c 2 ./array-add-bench rss \
  --impl scalar --elements 16777216 --seed 1592594996 \
  --alignment 64 --offset-u32 0

/usr/bin/time -f "%M" taskset -c 2 ./array-add-bench rss \
  --impl zen5 --elements 16777216 --seed 1592594996 \
  --alignment 64 --offset-u32 0
```

| Backend | Peak RSS median | Range | Result |
|---|---:|---:|---|
| scalar | 198400 KiB (193.75 MiB) | 198400–198400 KiB | baseline |
| Zen 5 AVX-512F | 198400 KiB (193.75 MiB) | 198400–198400 KiB | unchanged |

Обе реализации используют ровно три buffer allocations и одинаковый payload `3 × 64 MiB = 192 MiB`. Allocation count на уровне harness и representation size одинаковы. Allocator-internal metadata/traffic отдельно не профилировались.

Итог по памяти: **peak RSS не уменьшился**. Векторизация меняет инструкции и runtime, но не representation. Утверждать memory saving здесь было бы неверно.

## Raw data

- [`benchmark-results/info-wsl2.jsonl`](benchmark-results/info-wsl2.jsonl) — real CPUID/XCR0 и dispatch;
- [`benchmark-results/array-add-aligned-wsl2.jsonl`](benchmark-results/array-add-aligned-wsl2.jsonl) — config, 101 raw pairs, summary;
- [`benchmark-results/array-add-offset4-wsl2.jsonl`](benchmark-results/array-add-offset4-wsl2.jsonl) — offset config, 101 raw pairs, summary;
- [`benchmark-results/array-add-scalar-peak-rss-kib.txt`](benchmark-results/array-add-scalar-peak-rss-kib.txt) — 10 scalar peak RSS values;
- [`benchmark-results/array-add-zen5-peak-rss-kib.txt`](benchmark-results/array-add-zen5-peak-rss-kib.txt) — 10 vector peak RSS values.

## Вывод

- Correctness и real dispatch подтверждены exhaustive tests, guard pages и mandatory vector run.
- На 192 MiB representation AVX-512F дал statistically and practically significant **1.230×** aligned speedup в этом WSL2 прогоне.
- При +4-byte offset speedup снизился до **1.081×**, оставаясь выше заданного практического порога.
- Peak RSS и representation size не изменились.
- Portable scalar fallback сохранён; unsupported host не может получить executable AVX-512 pointer.
- Для production/microarchitecture claims benchmark нужно повторить bare-metal с контролем power mode, CCD placement, SMT sibling и background load.
