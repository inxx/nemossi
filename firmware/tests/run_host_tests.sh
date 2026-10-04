#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/nemossi-face-tests.XXXXXX")
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
compiler=${CC:-clang}
if ! command -v "$compiler" >/dev/null 2>&1; then
    compiler=cc
fi

"$compiler" -std=c11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I"$script_dir/../model" \
    "$script_dir/../model/face.c" "$script_dir/test_face.c" \
    -o "$temp_dir/test_face"
"$temp_dir/test_face"

"$compiler" -std=c11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I"$script_dir/../main" \
    "$script_dir/../main/device_services.c" "$script_dir/test_services.c" \
    -o "$temp_dir/test_services"
"$temp_dir/test_services"
