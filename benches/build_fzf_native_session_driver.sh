#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${FZF_NATIVE_DIR:-}"
output="${FZF_NATIVE_DRIVER:-$repo_dir/target/fzf-native-session-driver}"

while (($#)); do
  case "$1" in
    --source)
      source_dir="${2:?--source requires a directory}"
      shift 2
      ;;
    --output)
      output="${2:?--output requires a path}"
      shift 2
      ;;
    --help|-h)
      echo "Usage: $0 [--source FZF_NATIVE_DIR] [--output DRIVER]"
      echo "FZF_NATIVE_DIR and FZF_NATIVE_DRIVER provide equivalent environment configuration."
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$source_dir" ]]; then
  echo "Set FZF_NATIVE_DIR or pass --source with an fzf-native checkout." >&2
  exit 2
fi

required=(
  emacs-module.h
  fzf-native-module.c
  fzf-additions.c
  fzf.c
  utf8proc-2.10.0/utf8proc.c
)
for file in "${required[@]}"; do
  if [[ ! -f "$source_dir/$file" ]]; then
    echo "Missing fzf-native source: $source_dir/$file" >&2
    exit 2
  fi
done

mkdir -p "$(dirname "$output")"
cc_args=(
  -std=gnu11 -O3 -DNDEBUG -Wall -Wextra -pthread
  -I"$source_dir" -I"$source_dir/utf8proc-2.10.0"
)
if [[ -n "${FZF_NATIVE_DRIVER_CFLAGS:-}" ]]; then
  read -r -a extra_flags <<< "$FZF_NATIVE_DRIVER_CFLAGS"
  cc_args+=("${extra_flags[@]}")
fi
cc_args+=(
  -o "$output"
  "$repo_dir/benches/fzf_native_session_driver.c"
  "$source_dir/fzf.c"
  "$source_dir/fzf-additions.c"
  "$source_dir/utf8proc-2.10.0/utf8proc.c"
)
"${CC:-cc}" "${cc_args[@]}"

echo "$output"
