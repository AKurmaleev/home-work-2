#!/bin/sh
set -eu

analyzer=${1:-./array-add-rss-analyze}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

write_samples()
{
    path=$1
    value=$2
    count=$3
    run_id=1
    : > "$path"
    while [ "$run_id" -le "$count" ]; do
        printf '{"type":"rss_result","result":"ok","run_id":%s,"linux_ru_maxrss_kib":%s}\n' \
            "$run_id" "$value" >> "$path"
        run_id=$((run_id + 1))
    done
}

write_samples "$tmp_dir/baseline.jsonl" 2 31
write_samples "$tmp_dir/candidate.jsonl" 1 31

"$analyzer" "$tmp_dir/baseline.jsonl" "$tmp_dir/candidate.jsonl" \
    > "$tmp_dir/analysis.jsonl"

[ "$(wc -l < "$tmp_dir/analysis.jsonl")" -eq 64 ]
[ "$(grep -c '"implementation":"baseline"' "$tmp_dir/analysis.jsonl")" -eq 31 ]
[ "$(grep -c '"implementation":"candidate"' "$tmp_dir/analysis.jsonl")" -eq 31 ]
[ "$(grep -c '"implementation":"baseline".*"trim_class":"low"' "$tmp_dir/analysis.jsonl")" -eq 4 ]
[ "$(grep -c '"implementation":"baseline".*"trim_class":"retained"' "$tmp_dir/analysis.jsonl")" -eq 23 ]
[ "$(grep -c '"implementation":"baseline".*"trim_class":"high"' "$tmp_dir/analysis.jsonl")" -eq 4 ]

sed -n '1p' "$tmp_dir/analysis.jsonl" |
    grep -q '"type":"rss_analysis_config"'
tail -n 1 "$tmp_dir/analysis.jsonl" |
    grep -q '"relative_reduction":0.5'
tail -n 1 "$tmp_dir/analysis.jsonl" |
    grep -q '"retained_only_independent_bootstrap_95pct_ci":\[0.5,0.5\]'
tail -n 1 "$tmp_dir/analysis.jsonl" |
    grep -q '"adequacy_criterion_met":true,"verdict":"improved"'

cp "$tmp_dir/baseline.jsonl" "$tmp_dir/out-of-order.jsonl"
sed -i '2s/"run_id":2/"run_id":3/' "$tmp_dir/out-of-order.jsonl"
if "$analyzer" "$tmp_dir/out-of-order.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/out-of-order.err"; then
    echo "out-of-order run_id unexpectedly accepted" >&2
    exit 1
fi
grep -q 'expected rss_result/result ok, run_id 2' \
    "$tmp_dir/out-of-order.err"

cp "$tmp_dir/baseline.jsonl" "$tmp_dir/bad-result.jsonl"
sed -i '1s/"result":"ok"/"result":"error"/' "$tmp_dir/bad-result.jsonl"
if "$analyzer" "$tmp_dir/bad-result.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/bad-result.err"; then
    echo "non-ok result unexpectedly accepted" >&2
    exit 1
fi
grep -q 'expected rss_result/result ok, run_id 1' "$tmp_dir/bad-result.err"

write_samples "$tmp_dir/short.jsonl" 2 30
if "$analyzer" "$tmp_dir/short.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/short.err"; then
    echo "short input unexpectedly accepted" >&2
    exit 1
fi
grep -q 'requires at least 31 samples; found 30' "$tmp_dir/short.err"

write_samples "$tmp_dir/binary.jsonl" 2 30
printf '{"type":"rss_result","result":"ok","run_id":31,"linux_ru_maxrss_kib":2}\000ignored' \
    >> "$tmp_dir/binary.jsonl"
if "$analyzer" "$tmp_dir/binary.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/binary.err"; then
    echo "embedded NUL at EOF unexpectedly accepted" >&2
    exit 1
fi
grep -q 'line is empty, binary, or too long' "$tmp_dir/binary.err"

sed 's/$/\r/' "$tmp_dir/baseline.jsonl" > "$tmp_dir/crlf.jsonl"
"$analyzer" "$tmp_dir/crlf.jsonl" "$tmp_dir/candidate.jsonl" \
    > "$tmp_dir/crlf-analysis.jsonl"
cmp "$tmp_dir/analysis.jsonl" "$tmp_dir/crlf-analysis.jsonl"

write_samples "$tmp_dir/no-newline.jsonl" 2 30
printf '{"type":"rss_result","result":"ok","run_id":31,"linux_ru_maxrss_kib":2}' \
    >> "$tmp_dir/no-newline.jsonl"
"$analyzer" "$tmp_dir/no-newline.jsonl" "$tmp_dir/candidate.jsonl" \
    > "$tmp_dir/no-newline-analysis.jsonl"
cmp "$tmp_dir/analysis.jsonl" "$tmp_dir/no-newline-analysis.jsonl"

printf '\n' > "$tmp_dir/empty-line.jsonl"
printf '%8192s\n' '' > "$tmp_dir/long-line.jsonl"
for input in empty-line long-line; do
    if "$analyzer" "$tmp_dir/$input.jsonl" "$tmp_dir/candidate.jsonl" \
        > /dev/null 2> "$tmp_dir/$input.err"; then
        echo "$input unexpectedly accepted" >&2
        exit 1
    fi
    grep -q 'line is empty, binary, or too long' "$tmp_dir/$input.err"
done

write_samples "$tmp_dir/zero-rss.jsonl" 0 31
if "$analyzer" "$tmp_dir/zero-rss.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/zero-rss.err"; then
    echo "zero RSS unexpectedly accepted" >&2
    exit 1
fi
grep -q 'expected rss_result/result ok, run_id 1' "$tmp_dir/zero-rss.err"

if "$analyzer" "$tmp_dir/missing.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/missing.err"; then
    echo "missing input unexpectedly accepted" >&2
    exit 1
fi
grep -q 'cannot open input' "$tmp_dir/missing.err"

if "$analyzer" "$tmp_dir" "$tmp_dir/candidate.jsonl" \
    > /dev/null 2> "$tmp_dir/read-error.err"; then
    echo "input read error unexpectedly accepted" >&2
    exit 1
fi
grep -q 'input read failed' "$tmp_dir/read-error.err"

if "$analyzer" "$tmp_dir/baseline.jsonl" "$tmp_dir/candidate.jsonl" \
    > /dev/full 2> "$tmp_dir/output-error.err"; then
    echo "output write error unexpectedly accepted" >&2
    exit 1
fi
grep -q 'output write failed' "$tmp_dir/output-error.err"

printf 'array add RSS analyzer tests passed: 13 scenarios\n'
