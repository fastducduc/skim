#!/usr/bin/env bash
set -euo pipefail

driver="${1:?Usage: $0 DRIVER}"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

printf '%s\n' alpha alphabet beta gamma > "$work_dir/input.txt"
printf '%s\n' a al zzz > "$work_dir/queries.txt"

"$driver" --input "$work_dir/input.txt" --queries "$work_dir/queries.txt" \
  --workers 2 --limit 3 > "$work_dir/success.jsonl"

grep -q '"event":"ready".*"items":4.*"rounds":3' "$work_dir/success.jsonl"
[[ "$(grep -c '"event":"round"' "$work_dir/success.jsonl")" == 3 ]]
grep -q '"event":"round".*"matched":0.*"verified":true' "$work_dir/success.jsonl"
grep -q '"event":"complete".*"verified":true' "$work_dir/success.jsonl"

: > "$work_dir/empty.txt"
if "$driver" --input "$work_dir/empty.txt" --query a --query al \
    > "$work_dir/failure.jsonl"; then
  echo "empty input unexpectedly succeeded" >&2
  exit 1
fi
grep -q '"event":"error".*"input-load-failed-or-empty"' "$work_dir/failure.jsonl"
if grep -q '"event":"complete"' "$work_dir/failure.jsonl"; then
  echo "failed run emitted a completion record" >&2
  exit 1
fi

echo "fzf-native session driver smoke tests passed"
