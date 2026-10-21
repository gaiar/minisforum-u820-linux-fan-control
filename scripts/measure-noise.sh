#!/bin/bash
# Stream microphone levels without retaining raw audio; no fan writes.
set -euo pipefail
seconds=${1:-8}
if ! [[ "$seconds" =~ ^[0-9]+$ ]] || (( seconds < 1 || seconds > 60 )); then
    echo 'Duration must be 1–60 seconds.' >&2
    exit 2
fi
project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
task_build=$(mktemp -d)
trap 'rm -rf "$task_build"' EXIT
cc -std=c11 -O2 -Wall -Wextra -Werror "$project_root/tools/pcm_rms.c" -lm -o "$task_build/pcm-rms"
echo "Measuring $seconds seconds from hw:C200,0 at its current fixed gain…" >&2
for hwmon in /sys/class/hwmon/hwmon*; do
    if [[ $(cat "$hwmon/name") = u820_ec_probe ]]; then
        echo "Fan before: $(cat "$hwmon/fan1_input") RPM; leased raw duty: $(cat "$hwmon/fan_pwm_raw")" >&2
    fi
done
arecord -q -D hw:C200,0 -t raw -f S16_LE -r 48000 -c 2 -d "$seconds" | "$task_build/pcm-rms"
echo 'Complete: relative unweighted dBFS, not calibrated dBA. No audio saved.' >&2
