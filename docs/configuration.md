# Configure the U820 fan curve, hysteresis and noise balance

Settings live in **`/etc/u820-fan.json`**. They are read when the daemon starts;
changes require a restart. No driver rebuild is needed. Tuning is at your own
risk: read the [disclaimer](../DISCLAIMER.md), preserve hardware protections,
and verify the result on your machine. The published values were measured on
one unit in one enclosure arrangement.

## Edit, validate, apply, and undo

```sh
sudo cp /etc/u820-fan.json /etc/u820-fan.json.bak
sudoedit /etc/u820-fan.json
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
```

Continue only if `check` succeeds. It validates settings and reads live feedback
without changing fan duty; it does not certify the thermal behavior of your
curve. If validation fails, the running daemon continues with its previously
loaded configuration until stopped.

```sh
sudo systemctl restart u820-fan
watch -n 1 cat /run/u820-fan/state.json
```

Use `journalctl -u u820-fan -n 50 --no-pager` to inspect decisions or errors.
Observe idle, a short ordinary workload, sustained activity, and cooldown.
Change one setting at a time. Do not introduce a heavy benchmark on a server
that cannot tolerate the extra load. If behavior is unexpected, stop the
service and inspect it under firmware cooling.

To restore your backup:

```sh
sudo systemctl stop u820-fan
sudo cp /etc/u820-fan.json.bak /etc/u820-fan.json
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
sudo systemctl start u820-fan
```

To restore the shipped profile, copy `deploy/u820-fan.json` from your checkout
instead of the backup, then validate and start it the same way.

## Default profile

```json
{
  "curve": [[45, 46], [60, 46], [70, 56], [80, 68], [85, 68]],
  "hysteresis_c": 3.0,
  "down_delay_s": 8.0,
  "smoothing_s": 3.0,
  "rise_per_s": 12.0,
  "fall_per_s": 3.0,
  "emergency_c": 88.0,
  "performance_delay_s": 30.0,
  "sustained_duty": 95,
  "interval_s": 0.5
}
```

Each curve pair is `[temperature_in_Celsius, raw_duty]`. Temperatures strictly
increase; duties must be nondecreasing integers. The controller linearly
interpolates between points and clamps to endpoints outside their range.
The sustained-load floor and temperature override can request higher cooling
than the curve alone. JSON does not allow comments or trailing commas.
Omitted fields use defaults; unknown field names are rejected.

## Every configurable setting

| Setting | Default | Accepted range | Effect |
| --- | ---: | --- | --- |
| `curve` | Above | At least 2 pairs; temperatures 20–90°C; integer duties 46–127 | Normal curve. Temperatures strictly increase; duties never decrease. |
| `hysteresis_c` | 3 | 1–5°C | Requires cooler conditions before reducing speed, helping avoid repeated up/down changes. |
| `down_delay_s` | 8 | 5–120 seconds | How long a qualifying cooler condition must persist before slowdown. |
| `smoothing_s` | 3 | 0.5–5 seconds | Temperature filter time constant. Higher values smooth short fluctuations more. |
| `rise_per_s` | 12 | 4–100 raw units/second | Normal upward ramp limit; thermal headroom, sustained-load and emergency paths bypass it. |
| `fall_per_s` | 3 | 0.25–3 raw units/second | Downward ramp after cooldown. Smaller values keep the fan elevated longer. |
| `emergency_c` | 88 | 75–88°C | Raw CPU temperature that immediately requests duty 127. |
| `performance_delay_s` | 30 | 5–120 seconds | Delay before the sustained floor after the hot timer starts. |
| `sustained_duty` | 95 | Integer 68–127 | Minimum duty while sustained heat persists. |
| `interval_s` | 0.5 | 0.2–1 second | Delay between samples, in addition to I/O time. Normally leave unchanged. |

The input is the hottest of all available `coretemp` package/core sensors,
with at least five required. The hot timer starts at 80°C or above, resets
below 77°C, and continues through the 77–80°C hysteresis band. These thresholds
are fixed in code. Raw readings at/above 80°C also bypass normal smoothing
for curve demand. Smoothing lag is limited to 8°C below the raw value.

## Practical tuning examples

**More sustained cooling:** set `"sustained_duty": 127` in the full profile.
This requests the highest validated duty after the hot delay, approximately
3,540 RPM on the reference unit. It does not guarantee a performance gain; the
measured two-minute advantage over duty 95 was about 0.55% in one comparison.

**Earlier response to long workloads:** set `"performance_delay_s": 15.0` while
keeping `"sustained_duty": 95`. This reaches the sustained setting sooner and
may make medium-length bursts louder.

**Less audible cycling during repeated bursts:** increase `"down_delay_s"` to
20.0, or reduce `"fall_per_s"` to 1.0. Either keeps the fan running faster
longer after work ends. A longer hold can sound steadier, but is not necessarily
quieter overall.

**More cooling before the sustained timer:** change the upper curve points to
`[80, 82]` and `[85, 95]`. Keep the curve monotonic. This requests more airflow
sooner; it has not been established as an optimum on other machines.

For lower noise, start with the measured default rather than raising temperature
limits. The accepted ranges cannot guarantee adequate cooling or performance
for your installation. Check NVMe and memory temperatures too: the sensor CLI
reports them, but they are not inputs to this CPU-based fan policy.

## Raw duty is not RPM or PWM percentage

| Raw duty | Approximate RPM on the measured unit |
| ---: | ---: |
| 46 | 1,550 |
| 56 | 1,860 |
| 68 | 2,230 |
| 82 | 2,590 |
| 95 | 2,900 |
| 127 | 3,540 |

Do not supply an RPM number or a percentage in a duty field. The lowest manual
duty is 46; zero is invalid in a daemon curve. Stopping the daemon releases
control to firmware, which chooses its own fan behavior.

## Controls that configuration cannot disable

- Three-second kernel lease and watchdog checks.
- Driver identity gating and the 46–127 raw-duty allowlist.
- Valid fan feedback and readable CPU sensors.
- The daemon's 2.5-second maximum control-loop gap.
- Existing CPU hardware throttling and operating-system thermal protection.

The kernel watches independently of the Python process. A failed loop or lost
feedback causes the service to exit and release control; it intentionally does
not restart automatically after a fault. Software cannot protect against all
kernel, EC, firmware or hardware failures. The reference unit's existing TCC
target is 85°C; the daemon does not change it or eliminate thermal throttling.

## Relative noise measurements

With the optional Anker C200 microphone setup:

```sh
sudo apt-get install alsa-utils
sudo sh scripts/measure-noise.sh 8
```

The script expects ALSA `hw:C200,0`, 48 kHz stereo S16_LE. The C meter reports
DC-removed RMS, peak and clipping without saving audio. Other microphones need
their own device selection and supported format; this script is not automatic
device discovery. Keep the microphone position, capture gain and surroundings
fixed across comparisons, and avoid clipping.

Results are relative unweighted dBFS, not dBA or calibrated sound pressure.
Room noise and webcam processing can dominate small differences. Compare noise
alongside throughput, temperatures, RPM and throttle counters. The daemon never
requires a microphone. See [calibration](calibration.md).
