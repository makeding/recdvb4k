#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=/tmp/recdvb-a865-pipe-test
# Task-owned disposable files: at most 2 MiB each, 10 MiB total, and always removed below.
trap 'rm -rf -- "$test_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ -e "$test_dir" ]; then
    du -sk "$test_dir"
    rm -rf -- "$test_dir"
fi
mkdir "$test_dir"
ulimit -f 4096

recbin=${1:-"$repo_dir/recdvb4k"}
read -r -a cc_cmd <<< "${CC:-cc}"
cc_flags=(-std=c99 -Wall -Wextra -Werror)
if [ "$(uname -s)" = "Darwin" ]; then
    cc_flags+=(-Wno-unused-command-line-argument)
fi
NIX_LDFLAGS= NIX_CFLAGS_COMPILE= "${cc_cmd[@]}" "${cc_flags[@]}" \
    "$repo_dir/tests/a865_pipe_mock.c" -o "$test_dir/a865-pipe-mock"

if [ -x "$recbin" ]; then
    source_name=recdvb
    set +e
    RECDVB_A865_BACKEND="$test_dir/a865-pipe-mock" \
    A865_MOCK_RELEASE_MARKER="$test_dir/released" \
        "$recbin" --dev a865 27 1 - >"$test_dir/recdvb.ts" 2>"$test_dir/recdvb.stderr"
    status=$?
    set -e
    if [ "$status" -ne 0 ]; then
        cat "$test_dir/recdvb.stderr" >&2
        exit "$status"
    fi
    output="$test_dir/recdvb.ts"
    grep -q 'A865 backend: Japanese ISDB-T tuner is locked; TS pipe ready' \
        "$test_dir/recdvb.stderr"
else
    source_name=a865_backend_harness
    NIX_LDFLAGS= NIX_CFLAGS_COMPILE= "${cc_cmd[@]}" "${cc_flags[@]}" \
        -I"$repo_dir" \
        "$repo_dir/a865_backend.c" "$repo_dir/tests/a865_pipe_harness.c" \
        -o "$test_dir/a865-pipe-harness"
    A865_MOCK_RELEASE_MARKER="$test_dir/released" \
    RECDVB_A865_BACKEND="$test_dir/a865-pipe-mock" \
        "$test_dir/a865-pipe-harness" >"$test_dir/recdvb.ts" 2>"$test_dir/recdvb.stderr"
    output="$test_dir/recdvb.ts"
fi

python3 - "$output" "$source_name" <<'PY'
from pathlib import Path
import sys

stream = Path(sys.argv[1]).read_bytes()
assert len(stream) >= 1880, f"expected TS output, received {len(stream)} bytes"
assert stream[0] == 0x47, "TS sync byte missing at offset 0"
assert stream[188] == 0x47, "TS sync byte missing at offset 188"
print(f"PASS: {sys.argv[2]} delivered the Japanese ch27 TS pipe ({len(stream)} bytes)")
PY

test "$(cat "$test_dir/released")" = "released"
rm -rf -- "$test_dir"
test ! -e "$test_dir"
echo "Cleaned $test_dir"
