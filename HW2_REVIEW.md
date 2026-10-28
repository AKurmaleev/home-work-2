# HW2 source, standards, and specification review

## Scope and snapshot

- Repository and exclusive persistent write scope: `W:/1c/rust/home-work-2`.
- Initial clean HEAD: `cc67fd16875c1b4f7e8c83a4d4a04f753d5ee8dd`. This is a whole-repository review, not a review of changes since that commit. Standards and specification were assessed separately.
- All 32 initially tracked files were reviewed. The four new review/test-support files were also reviewed. No shared rules, sibling repository, central report, historical measurements, or existing root-level build artifacts were changed.
- No Git state mutations, dependency installation, Docker tests, new performance experiment, or Zen 5 tuning campaign were performed. Build products and inspection helpers remain in task-owned, ignored `.audit-hw2.o/` (matched by the existing `*.o` ignore rule). No `make clean` was run.

## Authority and applicability

Read `../../.rules`, `../.rules`, and `../.clinerules/README.md`; relevant policy sources were `01-general.md`, `02-safety-and-unsafety.md`, `04-types-layout-memory.md` Rule 17, `08-testing-verification.md`, `10-optimization.md`, `11-ai-assistant-guidelines.md`, `13-data-representation-io.md`, and `16-process-control.md`.

This repository is Linux x86-64 assembly plus C, not Rust. Applicable general obligations concern correctness, System V AMD64 ABI, optional-ISA guards, checked sizes, ownership/cleanup, input framing, output errors, numeric semantics, deterministic tests, and performance evidence. Cargo, Rust compiler/lint/MSRV rules, Rust-specific type prescriptions, and the ordinary Rust application unsafe/assembly ban do not apply. The assignment explicitly requires educational assembly and permits non-PIE builds. No manual Zen 5 optimization policy was activated.

Specification sources:

- `../asm-homework.txt`, SHA-256 `e542251ba3b8405c8b5f2084a39ef58ecf695ae3b06d85a0b331398b8c8d95cf`.
- `../asm_homework_.pdf`, SHA-256 `325fd27ffd3024d0734c3e9ba0f60d21febd2012b430c76668ea113507b87b21`; all five pages were read through installed `pdftotext -layout`.
- Repository `README.md` and `BOMB_REPORT.md`; `ZEN5_ARRAY_ADD_REPORT.md` defines the additional benchmark's existing claims, not a requirement to perform more optimization.

## Complete reviewed inventory

The following were the entire initial tracked inventory, including every recorded result:

```text
.gitignore
BOMB_REPORT.md
Makefile
README.md
ZEN5_ARRAY_ADD_REPORT.md
array_add.h
array_add_bench.c
array_add_dispatch.c
array_add_internal.h
array_add_rss_analyze.c
array_add_rss_analyze_test.sh
array_add_scalar.s
array_add_stats.c
array_add_stats.h
array_add_stats_test.c
array_add_test.c
array_add_xgetbv.s
array_add_zen5_avx512f.s
benchmark-results/array-add-aligned-rule63-wsl2.jsonl
benchmark-results/array-add-aligned-wsl2.jsonl
benchmark-results/array-add-offset4-rule63-wsl2.jsonl
benchmark-results/array-add-offset4-wsl2.jsonl
benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl
benchmark-results/array-add-scalar-peak-rss-kib.txt
benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl
benchmark-results/array-add-zen5-peak-rss-kib.txt
benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl
benchmark-results/info-rule63-wsl2.jsonl
benchmark-results/info-wsl2.jsonl
fib.s
hello.s
sum.s
```

New files: `.gitattributes`, `array_add_bench_test_clock.c`, `output_contract_test.sh`, and this report. The attributes file preserves LF for the two executable shell scripts on Windows checkouts. A first test launch exposed CRLF shell incompatibility; the scripts were normalized before collecting behavioral red evidence.

## Standards findings

Locations below identify the final files; baseline locations are noted where useful. P2 means a correctness/evidence issue requiring attention; P3 is a limited numeric edge case. Historical evidence gaps are not new runtime failures.

| ID | Severity/status | Location and evidence | Applicable rule / disposition |
|---|---|---|---|
| S1 | P2, fixed | `hello.s:13–29`, `sum.s:22–38`, `fib.s:36–52`, `array_add_bench.c:1076–1086`. Before the fix, all three assembly programs and all three benchmark commands returned success when stdout was `/dev/full`. | Rules 81.11 and 17.10: success must include checked output finalization. Assembly checks the libc output result and `fflush(NULL)`; the C CLI checks final flush/error state after command execution. Existing successful output is unchanged. |
| S2 | P2, fixed | `array_add_rss_analyze.c:139–180` (baseline `139–155`). A valid final record followed by `NUL` and hidden bytes, without LF, was accepted because `strlen` hid the suffix and `feof` allowed the final line. | Rules 79.4, 81.8, 85.1–85.2: framing must distinguish actual bytes from string termination. Bounded buffered `fgetc` scanning rejects NUL and oversized lines while retaining LF, CRLF, and missing-final-newline support. |
| S3 | P3, fixed | `array_add_stats.c:49–62` (baseline `48–55`). For two `DBL_TRUE_MIN` inputs, halving both before addition produced median zero. The public statistics boundary accepts these positive finite doubles. | Rules 94.7 and 94.12: subnormal behavior and magnitude limits matter. Sum before halving when the sorted nonnegative operands cannot overflow; retain half-before-sum for large operands. Regression also covers unequal subnormals and `DBL_MAX`. |
| S4 | P2, historical inference gap; unchanged | `array_add_stats.c:273–288` and `481–510`, corresponding retained-bootstrap tests, and `benchmark-results/*rule63*.jsonl`. Bootstrap resampling uses only the already-retained 71 pairs or 23 observations and never repeats sample selection/trimming. The synthetic retained-bootstrap test deliberately gives a point interval despite variation in discarded observations. | Rule 63.8 requires inference accounting for trimming. These are conditional retained-set intervals, not demonstrated sampling intervals for the full trimmed estimator. Descriptive ranks, counts, means, medians, MADs, and effects are consistent; population coverage/adequacy claims are not certified here. Resolving this requires a separately agreed inferential method and validation, not silently changing historical data or launching a new benchmark. Repository owner must decide that follow-up. |
| S5 | P2, historical provenance gap; unchanged | `ZEN5_ARRAY_ADD_REPORT.md:45–65,69–89,114–119` and the recorded configs. Workload, seed, affinity, warm-up, raw samples, and retained-set analysis are present, but the original exact compiler/linker versions, build/source identity, full acquisition commands, page/huge-page policy, memory-pressure/fault observations, and complete load controls are not retained. | Rules 63.4, 63.11, 63.20: the current Makefile and today's machine cannot reconstruct missing original-run facts. README now points to these limits without changing the historical numbers. Repository owner must supply original evidence or authorize a future reproducible experiment. |

No kernel/ABI correctness defect was found: argument widths, caller-saved registers, stack alignment before libc calls, `%eax` before variadic calls, zero-length behavior, modulo-2^32 arithmetic, exact destination aliasing, scalar tails, and guarded CPUID/XGETBV dispatch match the documented contract. Unsupported partial destination overlap remains explicitly unsupported; no new overlap behavior was introduced.

The RSS parser remains a bounded extractor for this program's emitted record layout, not a general-purpose JSON schema validator. This review does not certify arbitrary JSON dialects or authenticate supplied measurement files. The analyzer retains all observations, so total storage remains O(N), with O(N log N + B R + B log B) comparison work for bootstrap count B and retained count R. The framing change remains O(input bytes), with the same fixed 8192-byte line buffer; no measured speed or memory improvement is claimed. Array kernels remain Θ(n) time and Θ(1) auxiliary memory.

## Specification assessment

| Requirement | Evidence and status |
|---|---|
| Linux x86-64, AT&T syntax, `hello.s`/`sum.s`/`fib.s`, Clang build | Satisfied by source, Makefile, successful Linux builds, and linked ELF inspection. Specification: text lines 14–27. Educational `-no-pie` is intentional, not a production-hardening violation. |
| Print `Hello, world!` | Exact bytes and exit status verified. Specification: lines 35–44. |
| Add integers using registers and `add`; explain registers | `sum.s` computes 20 + 22 with `addq`, prints 42, and documents the registers. Specification: lines 54–60. |
| Compute Fibonacci with a loop, not a result constant; explain current/next values | `fib.s` iterates from `(0,1)` for index 10; exact output `F(10) = 55` verified. README explains `%rax`, `%rdx`, `%r8`, `%rcx`; linked disassembly contains the loop. Specification: lines 73–95. |
| Bomb answers plus concise disassembly/debugger analysis | `BOMB_REPORT.md` contains the required structure, commands, comparisons, jump table, binary digest, and historical ptrace limitation. Specification: lines 97–180. No new bomb solution was produced. |
| SP1: actual bomb completion | **Unverified, not a confirmed implementation defect.** The original `bomb_hard` is absent from the reviewed repository. `BOMB_REPORT.md:111–115` records historical normal execution, but this review cannot independently verify its addresses, digest, successful phases, or `explode_bomb` behavior. README labels that evidence historical. The owner must provide the original digest-matching binary and authorized runtime access for independent verification. |
| Independent student work / server conduct | The specification's AI disclaimer (lines 7–12), login isolation, and server conduct cannot be certified from source. This maintenance audit is not evidence of independent authorship. No server access or remote execution was attempted. |
| Bonus array addition | Separate from the required three programs and bomb. Correctness is verified; historical performance limitations remain S4–S5. |

No confirmed functional specification mismatch was found in the three programs. Bomb runtime acceptance remains blocked by the unavailable original artifact, not declared passed.

## Red–green evidence

All tests ran locally in WSL Ubuntu 24.04, against source snapshots under `.audit-hw2.o/`. Production fixes followed the respective deterministic red runs.

1. **S1**: built `array-add-bench-contract-test` with linker wraps for `clock_gettime`, `sched_getaffinity`, and `sched_getcpu`; real CLI parsing, serialization, statistics, allocations, kernels, and hardware guards remain under test. The unchanged first nine checks were run with:

   ```sh
   sh output_contract_test.sh .audit-hw2.o/array-add-bench-contract-test .audit-hw2.o/hello .audit-hw2.o/sum .audit-hw2.o/fib
   ```

   Red: `output contract tests failed: 6/9`, all six specifically stdout-failure exit statuses. JSON parsing passed, disproving an initial suspicion of malformed JSON; no serialization correction was made. Green with the same nine checks: `output contract tests passed: 9 checks (synthetic timings only)`. Subsequent extensions add exact assembly bytes, unbuffered libc failures, and stronger expected failure/diagnostic checks: **15/15 pass**, normal and sanitized CLI fixture.

2. **S2**:

   ```sh
   sh array_add_rss_analyze_test.sh .audit-hw2.o/array-add-rss-analyze
   ```

   Red: `embedded NUL at EOF unexpectedly accepted`. Green after the framing fix with the identical regression: pass. The extended suite has **13 scenarios**, normal and sanitized: valid LF, ordering error, non-ok result, short sample set, embedded NUL at EOF, valid CRLF, valid EOF without LF, blank line, oversized line, zero RSS, missing file, input read failure, and stdout failure.

3. **S3**: copied the new regression test into the isolated build before changing `array_add_stats.c`, then used the same command for red and green:

   ```sh
   make -B -C .audit-hw2.o test-array-add-stats
   ```

   Red: `even-median-extremes:576: check failed: summary.retained_median == expected_medians[index]`; `array add stats tests failed: 1 checks`. Green: `array add stats tests passed`. The final suite has **14 test groups**, including three midpoint boundary datasets. Assertions were not relaxed.

## Verification environment and commands

- Windows-hosted WSL2 Ubuntu 24.04; Linux `6.6.87.2-microsoft-standard-WSL2`, x86-64.
- Current reported processor: AMD Ryzen 9 9950X3D; family 26, model 68, 16 exposed logical CPUs. This does not retrospectively verify the historical report's 9950X model label.
- Ubuntu Clang `18.1.3 (1ubuntu1)`, target `x86_64-pc-linux-gnu`; GNU Make 4.3; GNU ld/binutils 2.42; Python 3.12.3; page size 4096.
- Native terminal working directory was `W:/1c/rust/home-work-2`. Linux bodies below were run through `wsl.exe -d Ubuntu-24.04 --exec sh -c '...'`, with bounded native terminal timeouts of 10–120 seconds. WSL inherited the correct working directory. An explicit `wsl --cd /mnt/w/...` probe failed with `Wsl/ERROR_PATH_NOT_FOUND`; inherited-cwd execution worked. A login-shell probe emitted `source: not found`; final commands use non-login `sh -c`.
- Copies occasionally produced sub-two-second filesystem clock-skew warnings. Final release/sanitizer builds were forced with `-B`; test passes were not inferred from possibly stale original artifacts.

Final complete build and regular verification bodies:

```sh
cp -- *.c *.h *.s Makefile array_add_rss_analyze_test.sh output_contract_test.sh .audit-hw2.o/
TMPDIR=/mnt/w/1c/rust/home-work-2/.audit-hw2.o/tmp make -B -C .audit-hw2.o all array-add array-add-test-sanitize array-add-stats-test-sanitize array-add-rss-analyze-sanitize array-add-bench-contract-test
TMPDIR=/mnt/w/1c/rust/home-work-2/.audit-hw2.o/tmp make -C .audit-hw2.o run test-array-add test-array-add-zen5 test-array-add-stats test-array-add-rss-analyze test-array-add-rss-analyze-sanitize test-output-contract
.audit-hw2.o/array-add-test-sanitize --require-vector
.audit-hw2.o/array-add-stats-test-sanitize
```

Results: exact three assembly outputs; **42,697 array cases, 53 dispatch checks, 38 guard-page cases, two implementations, vector executed** in each default, vector-required, and sanitizer run. Statistics: 14 groups pass. RSS analyzer: 13 scenarios pass normally and with ASan/UBSan. Output contract: 15 checks pass. Sanitizers instrument C, not handwritten assembly; assembly access checks are additionally supported by canaries and guard pages, not a claim of full assembly sanitizer instrumentation.

The CLI fixture was separately rebuilt and rerun after the final numeric fix:

```sh
TMPDIR=/mnt/w/1c/rust/home-work-2/.audit-hw2.o/tmp make -B -C .audit-hw2.o array-add-bench-contract-test CC="clang -fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer" ARRAY_ADD_CFLAGS="-std=c17 -march=x86-64 -mtune=generic -fPIE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror"
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TMPDIR=/mnt/w/1c/rust/home-work-2/.audit-hw2.o/tmp make -C .audit-hw2.o test-output-contract
```

Result: 15 checks pass. Its artificial 10 ms clock ticks and 17-element workload are serialization/error-path fixtures, not measured performance. No real timed `compare` campaign or new large RSS acquisition was run.

Recorded-result and final-artifact checks:

```sh
python3 .audit-hw2.o/check_records.py
.audit-hw2.o/array-add-rss-analyze benchmark-results/array-add-scalar-rss-rule63-wsl2.jsonl benchmark-results/array-add-zen5-rss-rule63-wsl2.jsonl > .audit-hw2.o/rss-recomputed.jsonl
cmp benchmark-results/array-add-rss-analysis-rule63-wsl2.jsonl .audit-hw2.o/rss-recomputed.jsonl
python3 .audit-hw2.o/inspect_elf.py
git --no-pager diff --check
```

The ignored inspection helpers only read/inspect data and artifacts. All **11 recorded-result files / 560 lines** were consumed: 540 JSON records and 20 positive integer RSS observations. Runtime ranking, paired retention, means, medians, MADs, effects, checksums, and RSS sample/rank/summary correspondence passed. Older untrimmed series remain identifiable historical archives. Fresh RSS analysis is **byte-identical** to the stored analysis. Conditional interval fields and widths were checked for consistency, not certified for population coverage.

`file`, `readelf -hW/-lW/-dW`, and AT&T `objdump -d --disassemble=...` inspected linked outputs. Seven normal ELF programs have non-executable stacks, no RWX LOAD segment, and RELRO. Four C tools/tests additionally have PIE and immediate binding. The three assignment programs deliberately remain non-PIE. Fibonacci iteration/output calls and all three raw scalar/vector/XGETBV leaf symbols were inspected. No ABI formal proof or binary-bomb runtime verification is claimed.

## Final disposition

- Three confirmed correctness findings fixed with deterministic red–green evidence: S1–S3.
- Two historical standards/evidence findings remain explicitly open: S4–S5. No historical measurements or statistical policy were silently rewritten.
- Specification: three assembly tasks pass; original bomb execution and independent authorship remain unverified.
- Changes are local and uncommitted. No Git index/ref/config mutations were made. The final user handoff records the complete diffstat, including new files; build/inspection artifacts are ignored and retained rather than broadly cleaned.
