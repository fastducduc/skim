#!/usr/bin/env bash
set -euo pipefail

emit_fake_protocol() {
  local identity=0000000000000000000000000000000000000000
  printf '%s\n' "{\"event\":\"ready\",\"protocol\":1,\"items\":4,\"rounds\":4,\"workers\":2,\"limit\":3,\"source_revision\":\"$identity\",\"source_build_id\":\"$identity\"}"

  local query_hashes=(
    44bd9fd473cdbcb5
    9bf36600c697d570
    966b565173fef419
    975819681a395537
  )
  if [[ "$FZF_NATIVE_CLI_FAKE_DRIVER_MODE" == query ]]; then
    query_hashes[0]=0000000000000000
  fi
  for index in 0 1 2 3; do
    local round=$((index + 1))
    printf '%s\n' "{\"event\":\"round\",\"protocol\":1,\"round\":$round,\"request_id\":$round,\"query_hash\":\"${query_hashes[$index]}\",\"elapsed_ns\":1,\"matched\":4,\"emitted\":3,\"items\":4,\"filter_only\":false,\"checksum\":\"0000000000000000\",\"verified\":true}"
  done
  printf '%s\n' "{\"event\":\"complete\",\"protocol\":1,\"items\":4,\"rounds\":4,\"total_elapsed_ns\":4,\"aggregate_checksum\":\"0000000000000000\",\"source_revision\":\"$identity\",\"source_build_id\":\"$identity\",\"verified\":true}"
}

if [[ -n "${FZF_NATIVE_CLI_FAKE_DRIVER_MODE:-}" ]]; then
  emit_fake_protocol
  exit 0
fi

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

printf '%s\n' alpha alphabet beta gamma > "$work_dir/input.txt"

run_rejection_test() {
  local mode="$1"
  local expected_error="$2"
  if FZF_NATIVE_CLI_FAKE_DRIVER_MODE="$mode" cargo bench --quiet --bench cli -- native-session \
      --driver "$repo_dir/benches/test_fzf_native_cli.sh" -f "$work_dir/input.txt" -q test \
      -w 0 -r 1 --limit 3 > "$work_dir/$mode.stdout" 2> "$work_dir/$mode.stderr"; then
    echo "invalid $mode protocol unexpectedly succeeded" >&2
    exit 1
  fi
  if [[ -s "$work_dir/$mode.stdout" ]]; then
    echo "invalid $mode protocol forwarded timing output" >&2
    exit 1
  fi
  grep -q "$expected_error" "$work_dir/$mode.stderr"
}

run_rejection_test query "round query hash does not match the requested query"
run_rejection_test aggregate "completion aggregate checksum does not match the rounds"

if FZF_NATIVE_CLI_FAKE_DRIVER_MODE=aggregate cargo bench --quiet --bench cli -- native-session \
    --driver "$repo_dir/benches/test_fzf_native_cli.sh" -f "$work_dir/input.txt" -q test \
    -w 0 -r 1 --limit 3 -- --input "$work_dir/input.txt" \
    > "$work_dir/override.stdout" 2> "$work_dir/override.stderr"; then
  echo "benchmark-owned input override unexpectedly succeeded" >&2
  exit 1
fi
if [[ -s "$work_dir/override.stdout" ]]; then
  echo "input override forwarded timing output" >&2
  exit 1
fi
grep -q "controlled by native-session" "$work_dir/override.stderr"

: > "$work_dir/empty.txt"
if cargo bench --quiet --bench cli -- native-session \
    --driver "$repo_dir/benches/test_fzf_native_cli.sh" -f "$work_dir/empty.txt" \
    -q test -w 0 -r 1 --limit 3 > "$work_dir/empty.stdout" 2> "$work_dir/empty.stderr"; then
  echo "empty input unexpectedly succeeded" >&2
  exit 1
fi
if [[ -s "$work_dir/empty.stdout" ]]; then
  echo "empty input forwarded timing output" >&2
  exit 1
fi
grep -q "input corpus is empty" "$work_dir/empty.stderr"

echo "fzf-native CLI protocol rejection tests passed"
