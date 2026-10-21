#!/bin/sh
# Test the shared kernel control policy locally, without hardware access.
set -eu
task_build=$(mktemp -d "${TMPDIR:-/tmp}/u820-control.XXXXXX")
trap 'rm -rf "$task_build"' EXIT HUP INT TERM
project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$project_root/tests/control_test.c" -o "$task_build/control_test"
"$task_build/control_test"
