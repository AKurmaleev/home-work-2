# Assembly + Binary Bomb

An x86-64 assembly homework assignment for Linux. The assembly sources use AT&T syntax and the System V AMD64 ABI, and are built with `clang`.

## Assignment contents

| Task | File | Result |
|---|---|---|
| Hello, world! | [`hello.s`](hello.s) | Prints `Hello, world!` using `puts` |
| Addition | [`sum.s`](sum.s) | Adds `20` and `22` using `addq`, prints `42` |
| Fibonacci | [`fib.s`](fib.s) | Computes `F(10)` in a loop, prints `55` |
| Binary Bomb | [`BOMB_REPORT.md`](BOMB_REPORT.md) | Contains answers and a brief analysis of all phases |

`Makefile` builds and runs the three assembly programs. Compiled ELF files and object files are not included in the repository.

## Requirements

- Linux x86-64;
- `clang`;
- GNU Make;
- libc.

## Build and run

```sh
make clean all
make run
make clean
```

Expected output:

```text
Hello, world!
42
F(10) = 55
```

Equivalent manual build:

```sh
clang -no-pie hello.s -o hello
clang -no-pie sum.s -o sum
clang -no-pie fib.s -o fib
```

`-no-pie` is used only as an educational simplification to provide stable addresses in `objdump` and GDB. It is not a production recommendation.

## Implementation

### General ABI details

- Each program's entry point is `main`, so libc handles startup and termination.
- The first and second integer function arguments are passed through `%rdi` and `%rsi`.
- Before a variadic `printf` call, `%eax` is zeroed as required by the System V AMD64 ABI.
- `pushq %rbp` provides the required stack alignment before calling libc.
- Strings reside in `.rodata` and use `%rip`-relative addressing.
- `.note.GNU-stack` keeps the stack non-executable.

### Addition

- `%rax` holds the first addend, then the result.
- `%rcx` holds the second addend.
- `addq %rcx, %rax` performs the addition.
- `%rdi` and `%rsi` pass the format and result to `printf`.

### Fibonacci

- `%rax` holds the current value `F(i)`.
- `%rdx` holds the next value `F(i + 1)`.
- `%r8` temporarily holds the sum.
- `%rcx` holds the number of remaining iterations.

Each iteration transforms the pair `(F(i), F(i + 1))` into `(F(i + 1), F(i + 2))`. The result is computed in a loop, not loaded as a precomputed constant.

## Binary Bomb

The bomb was analyzed in AT&T syntax using `file`, `strings`, `nm`, `readelf`, `objdump`, and GDB. The functions `phase_1`–`phase_4`, the recursive `fib`, `secret_phase`, and the path to `explode_bomb` were examined.

The answers, identified comparisons, conditional branches, jump table, and commands used are documented in [`BOMB_REPORT.md`](BOMB_REPORT.md).

## Verification

The assignment was verified on Ubuntu 24.04 x86-64 with Clang 18:

- `make clean all run` successfully builds and runs all programs;
- `file` confirms ELF64 x86-64;
- `readelf -lW` shows `GNU_STACK` without execute permission and the presence of `GNU_RELRO`;
- `objdump -d --disassemble=main fib` confirms a loop with `test`, `je`, `dec`, and `jne`;
- the calculated input fully defuses the four main phases and the hidden phase of `bomb_hard`.

## Additional benchmark: scalar and AVX-512

This is a bonus exercise. It does not change the main `hello`, `sum`, `fib`, and Binary Bomb exercises.

Two implementations of `uint32_t` array addition modulo `2^32` are compared:

- [`array_add_scalar.s`](array_add_scalar.s) — scalar x86-64;
- [`array_add_zen5_avx512f.s`](array_add_zen5_avx512f.s) — AVX-512, 16 elements per iteration;
- [`array_add_dispatch.c`](array_add_dispatch.c) — implementation selection with a safe scalar fallback;
- [`array_add_test.c`](array_add_test.c) and [`array_add_bench.c`](array_add_bench.c) — correctness checks and measurements;
- [`ZEN5_ARRAY_ADD_REPORT.md`](ZEN5_ARRAY_ADD_REPORT.md) — a brief report with methodology and results.

The AVX-512 function is called only after checking CPU capabilities and OS state through CPUID/XGETBV. If the requirements are not met, the scalar implementation is used.

Build and verify:

```sh
make array-add
make test-array-add
make test-array-add-zen5
```

The following results were obtained on arrays with a combined size of 192 MiB under WSL2:

| Layout | Scalar mean | AVX-512 mean | Speedup |
|---|---:|---:|---:|
| 64-byte aligned | 8.078 ms | 5.594 ms | 1.44× |
| offset +4 bytes | 7.901 ms | 6.033 ms | 1.31× |

Peak RSS is practically identical: approximately `198,250 KiB` for both implementations. Vectorization reduced runtime, but not the size of the input and output arrays.

**Limitation:** the benchmark ran under WSL2, where power state, CCD/V-Cache placement, and the neighboring SMT thread cannot be fully controlled. The result shows an advantage for the AVX-512 implementation over scalar in these runs, but **does not confirm** that the gain was caused specifically by the Zen 5 microarchitecture.
