#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/nemossi-muse-audio-tests.XXXXXX")
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
compiler=${CC:-clang}
if ! command -v "$compiler" >/dev/null 2>&1; then compiler=cc; fi
"$compiler" -std=c11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I"$test_dir" -I"$test_dir/../../main" \
    "$test_dir/test_voice_board_nemossi.c" -o "$temp_dir/test_audio"
"$temp_dir/test_audio"
