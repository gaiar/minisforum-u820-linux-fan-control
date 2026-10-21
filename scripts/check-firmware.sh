#!/bin/sh
# Execute selected firmware routines locally, with no hardware or network access.
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 CB9EC_V1.06.BIN pinned-emu8051-source-directory" >&2
    exit 2
fi

firmware=$1
emulator=$2
expected=8b24e51229b1caa6023e30176f4aedba063facb54f0dc8d55fcb1bb34a7ebd5c
actual=$(shasum -a 256 "$firmware" | cut -d ' ' -f 1)
if [ "$actual" != "$expected" ]; then
    echo "Unsupported firmware hash: $actual" >&2
    exit 1
fi

task_build=$(mktemp -d "${TMPDIR:-/tmp}/u820-emulation.XXXXXX")
trap 'rm -rf "$task_build"' EXIT HUP INT TERM
project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
echo "Checking pinned emulator sources…"
(cd "$emulator" && shasum -a 256 -c "$project_root/tests/emu8051.sha256")
echo "Compiling the offline firmware harness…"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$emulator" \
    -c "$project_root/tests/firmware_harness.c" -o "$task_build/harness.o"
"${CC:-cc}" -std=c11 -O2 -I "$emulator" "$task_build/harness.o" \
    "$emulator/core.c" "$emulator/opcodes.c" "$emulator/disasm.c" \
    -o "$task_build/firmware_harness"
echo "Executing the supplied EC firmware locally…"
"$task_build/firmware_harness" "$firmware"
echo "Complete. This validates selected file routines, not the live controller."
