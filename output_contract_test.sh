#!/bin/sh
set -eu

# Python only inspects child output; the production compare clock is synthetic.
python3 - "$@" <<'PY'
import json
import subprocess
import sys

if len(sys.argv) != 5:
    raise SystemExit("usage: output_contract_test.sh <bench-fixture> <hello> <sum> <fib>")

bench, hello, addition, fib = sys.argv[1:]
compare = [bench, "compare", "--elements", "17", "--samples", "101",
           "--warmups", "0", "--inner", "1"]
rss = [bench, "rss", "--impl", "scalar", "--elements", "17", "--run-id", "1"]
failures = []
checks = 0


def check(condition, description):
    global checks
    checks += 1
    if not condition:
        failures.append(description)
        print("FAIL " + description, file=sys.stderr)


for command, expected_rows in [(compare, 103), ([bench, "info"], 1), (rss, 1)]:
    result = subprocess.run(command, capture_output=True, timeout=10, check=False)
    try:
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        valid = result.returncode == 0 and len(rows) == expected_rows
        if command == compare:
            summary = rows[-1]
            valid = valid and summary["raw_pair_count"] == 101
            valid = valid and summary["retained_pair_count"] == 71
            validation = summary["validation"]
            valid = valid and validation["elementwise"]
            valid = valid and validation["scalar_checksum"] == validation["candidate_checksum"]
        check(valid, "valid complete JSONL: " + command[1])
    except (ValueError, KeyError, IndexError) as error:
        check(False, "valid complete JSONL: " + command[1] + ": " + str(error))

for program, expected in [(hello, b"Hello, world!\n"), (addition, b"42\n"),
                          (fib, b"F(10) = 55\n")]:
    result = subprocess.run([program], capture_output=True, timeout=10, check=False)
    check(result.returncode == 0 and result.stdout == expected,
          "exact assembly output: " + program)

with open("/dev/full", "wb", buffering=0) as full:
    commands = [[hello], [addition], [fib], [bench, "info"], rss, compare]
    commands += [["stdbuf", "-o0", program] for program in [hello, addition, fib]]
    for command in commands:
        result = subprocess.run(command, stdout=full, stderr=subprocess.PIPE,
                                timeout=10, check=False)
        expected_failure = result.returncode == 1
        if command[0] == bench:
            expected_failure = expected_failure and b"output write failed" in result.stderr
        check(expected_failure, "stdout failure returns nonzero: " + " ".join(command))

if failures:
    raise SystemExit(f"output contract tests failed: {len(failures)}/{checks}")
print(f"output contract tests passed: {checks} checks (synthetic timings only)")
PY
