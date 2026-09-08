#!/usr/bin/env bash
set -euo pipefail

driver="${1:?Usage: $0 DRIVER}"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

printf '%s\n' alpha alphabet '' beta gamma > "$work_dir/input.txt"
printf '%s\n' a al zzz > "$work_dir/queries.txt"

"$driver" --input "$work_dir/input.txt" --queries "$work_dir/queries.txt" \
  --workers 2 --limit 3 > "$work_dir/success.jsonl"

grep -q '"event":"ready".*"items":5.*"rounds":3' "$work_dir/success.jsonl"
[[ "$(grep -c '"event":"round"' "$work_dir/success.jsonl")" == 3 ]]
grep -q '"event":"round".*"matched":0.*"verified":true' "$work_dir/success.jsonl"
grep -q '"event":"complete".*"verified":true' "$work_dir/success.jsonl"

# Equal-score candidates use Unicode character length, not byte length, as
# the default rank tiebreak.  The longer candidate comes first in the input so
# successful verification also proves that the result was reordered.
printf '%s\n' 'a界界' 'aé' zzz > "$work_dir/unicode-rank.txt"
"$driver" --input "$work_dir/unicode-rank.txt" --query a --workers 1 \
  --limit 0 > "$work_dir/unicode-rank.jsonl"
grep -q '"event":"round".*"matched":2,"emitted":2.*"filter_only":false.*"verified":true' \
  "$work_dir/unicode-rank.jsonl"

# fzf preserves producer order when a query has no positive term.  Exercise
# that rule in the full scorer and the filter-only path.
printf '%s\n' longword b middle xray > "$work_dir/inverse-only.txt"
"$driver" --input "$work_dir/inverse-only.txt" --query '!x' --workers 1 \
  --limit 0 > "$work_dir/inverse-normal.jsonl"
grep -q '"event":"round".*"matched":3,"emitted":3.*"filter_only":false.*"verified":true' \
  "$work_dir/inverse-normal.jsonl"
"$driver" --input "$work_dir/inverse-only.txt" --query '!x' --workers 1 \
  --limit 0 --filter-only-min-pool 1 > "$work_dir/inverse-filter.jsonl"
grep -q '"event":"round".*"matched":3,"emitted":3.*"filter_only":true.*"verified":true' \
  "$work_dir/inverse-filter.jsonl"

# Filter-only mode preserves sentinel scores for zero or one emitted item.  It
# scores and sorts only when the emitted window contains at least two items.
printf '%s\n' 'a界界' 'aé' a beta zzz > "$work_dir/filter-limits.txt"
for limit in 0 1 3; do
  "$driver" --input "$work_dir/filter-limits.txt" --query a --workers 1 \
    --limit "$limit" --filter-only-min-pool 1 \
    > "$work_dir/filter-limit-$limit.jsonl"
  emitted=4
  if [[ "$limit" != 0 ]]; then emitted="$limit"; fi
  grep -q "\"event\":\"round\".*\"matched\":4,\"emitted\":$emitted.*\"filter_only\":true.*\"verified\":true" \
    "$work_dir/filter-limit-$limit.jsonl"
done

ready_revision="$(grep '"event":"ready"' "$work_dir/success.jsonl" | sed -E 's/.*"source_revision":"([0-9a-f]{40})".*/\1/')"
ready_build_id="$(grep '"event":"ready"' "$work_dir/success.jsonl" | sed -E 's/.*"source_build_id":"([0-9a-f]{40})".*/\1/')"
complete_revision="$(grep '"event":"complete"' "$work_dir/success.jsonl" | sed -E 's/.*"source_revision":"([0-9a-f]{40})".*/\1/')"
complete_build_id="$(grep '"event":"complete"' "$work_dir/success.jsonl" | sed -E 's/.*"source_build_id":"([0-9a-f]{40})".*/\1/')"
[[ "$ready_revision" =~ ^[0-9a-f]{40}$ ]]
[[ "$ready_build_id" =~ ^[0-9a-f]{40}$ ]]
[[ "$ready_revision" == "$complete_revision" ]]
[[ "$ready_build_id" == "$complete_build_id" ]]

if FZF_NATIVE_DRIVER_TEST_VERIFY_OOM=1 "$driver" \
    --input "$work_dir/input.txt" --query a --workers 2 --limit 3 \
    > "$work_dir/verify-oom.jsonl"; then
  echo "verification matcher OOM unexpectedly succeeded" >&2
  exit 1
fi
grep -q '"event":"error".*"session-or-verification-failed"' \
  "$work_dir/verify-oom.jsonl"
if grep -Eq '"event":"(round|complete)"' "$work_dir/verify-oom.jsonl"; then
  echo "verification matcher OOM emitted validated timing output" >&2
  exit 1
fi

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
