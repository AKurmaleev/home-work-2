# Additional benchmark: scalar and AVX-512

This report covers the bonus exercise. The main `hello`, `sum`, `fib`, and Binary Bomb exercises do not depend on it.

## Goal

Compare two x86-64 implementations of the operation:

```text
dst[i] = lhs[i] + rhs[i] modulo 2^32
```

Runtime, algorithmic complexity, and peak process memory are evaluated on large datasets.

## Implementations

- [`array_add_scalar.s`](array_add_scalar.s) processes one `uint32_t` per iteration.
- [`array_add_zen5_avx512f.s`](array_add_zen5_avx512f.s) processes 16 elements using `vpaddd`, then handles the tail with scalar code.
- [`array_add_dispatch.c`](array_add_dispatch.c) checks CPU capabilities and OS extended-register state through CPUID/XGETBV. If the required capabilities are unavailable, the scalar fallback is selected.

Both functions have the same contract and work correctly with aligned and offset addresses.

## Correctness verification

Tests cover:

- empty, small, and large arrays;
- lengths around the 16-element boundary;
- different offsets and unaligned addresses;
- permitted overlap of `dst` with an input array;
- canary values and guard pages;
- mandatory execution of the vector function on a supported CPU;
- selection of the scalar fallback when CPU capabilities are unsuitable.

A total of **42 697** cases were checked. The Clang build with `-Werror`, regular tests, and ASan/UBSan checks completed successfully.

Main commands:

```sh
make array-add
make test-array-add
make test-array-add-zen5
```

## Methodology

Measurements were taken under WSL2 on a machine with an AMD Ryzen 9 9950X.

One run processes `16,777,216` elements. `lhs`, `rhs`, and `dst` occupy 64 MiB each, for a total data size of **192 MiB**.

Two layouts were tested:

- addresses aligned to 64 bytes;
- addresses offset by 4 bytes from a 64-byte boundary.

For each layout:

1. Input data is created before the measured section.
2. Scalar and AVX-512 results are checked against each other.
3. 10 warm-up pairs are executed and excluded from the statistics.
4. 101 pairs of runs are measured, alternating the scalar/AVX-512 order.
5. 15 pairs are excluded from each tail of the comparative-result distribution, leaving 71 pairs.
6. The mean and median for both implementations are calculated from the same retained sample.

Peak RSS is measured separately from runtime. Each implementation has one warm-up and 31 runs in fresh processes; after excluding 15% of values from each tail, 23 observations remain.

## Results

### Runtime

All times are in milliseconds.

| Layout | Scalar mean / median | AVX-512 mean / median | Speedup |
|---|---:|---:|---:|
| 64-byte aligned | 8.078 / 8.085 | 5.594 / 5.594 | 1.44× |
| offset +4 bytes | 7.901 / 7.756 | 6.033 / 5.946 | 1.31× |

The AVX-512 implementation was faster than scalar in both series. The series for the two layouts were run separately, so the difference between `1.44×` and `1.31×` cannot be attributed solely to alignment.

### Memory

Peak RSS is reported in KiB.

| Implementation | Mean | Median |
|---|---:|---:|
| Scalar | 198,249.7 | 198,272 |
| AVX-512 | 198,272.0 | 198,272 |

There is no practical memory reduction. The difference in mean peak RSS is about `0.01%`, which is at the noise level relative to the total process size.

### Complexity and data representation

| Characteristic | Scalar | AVX-512 |
|---|---|---|
| Time complexity | `Θ(n)` | `Θ(n)` |
| Function auxiliary memory | `Θ(1)` | `Θ(1)` |
| Allocations inside the function | 0 | 0 |
| Caller-owned arrays | 192 MiB | 192 MiB |

Vectorization changes the number of elements processed per iteration, but not the algorithmic class or data representation.

## Limitations

WSL2 does not allow full control over CPU power state, thread placement across CCD/V-Cache, or the state of the neighboring SMT thread. In addition, comparing scalar and AVX-512 isolates differences between implementations, but not the effect of the microarchitecture itself.

Therefore, the result means only the following: **in this WSL2 benchmark, the AVX-512 implementation is faster than the scalar implementation**. It does not prove that the speedup was caused specifically by Zen 5 or that the same factor will hold on another system.

Confirming a Zen 5-specific optimization requires a repeat on native Linux or Windows with control over CPU topology and power mode, and with hardware performance counters.

## Conclusion

On a 192 MiB workload, AVX-512 reduced runtime by a factor of approximately `1.44×` for aligned addresses and `1.31×` with an offset of 4 bytes. Peak RSS and asymptotic memory costs did not change. A Zen 5-specific effect under WSL2 has not been confirmed.

## Raw data

- [aligned runtime](benchmark-results/array-add-aligned-rule63-wsl2.jsonl)
- [offset +4 runtime](benchmark-results/array-add-offset4-rule63-wsl2.jsonl)
- [scalar RSS](benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl)
- [AVX-512 RSS](benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl)
