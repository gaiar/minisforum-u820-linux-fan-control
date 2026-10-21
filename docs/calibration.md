# Quiet-profile calibration — 2026-10-21

Target: U820 i5-8279U, Ubuntu 22.04.5 / 6.8.0-138-generic. BIOS CB9_FV102,
EC 1.05 / 11:28:11. Turbo enabled, performance governor, max_perf_pct=100.
The existing **85°C TCC activation target** and thermald remain unchanged.
This is a measured operating compromise for this installation, not a proof of
a global acoustic optimum or unthrottled maximum CPU performance.

## Installation and measurement conditions

The owner places the U820 inside a woven IKEA basket, approximately halfway
up, with the Anker PowerConf C200 webcam on top. Exact microphone distance,
ambient sound level and air temperature were not measured. Moving the server
or microphone invalidates the acoustic comparison and can change cooling.

USB 291a:3369, ALSA `hw:C200,0`, 48 kHz S16_LE stereo, capture on, gain 100.
The channels were identical in the captured windows. No ALSA AGC switch is
exposed; internal processing/noise reduction is unknown. Reported levels are
relative, unweighted **dBFS**, not dBA or calibrated sound-pressure levels.
Audio is reduced to RMS/peak/clipping statistics without preserving recordings.

The complete idle sweep held each point for 14 seconds and captured eight
seconds after four seconds of settling. CPU and RPM were sampled each second.
An independent kernel lease restored cooling if renewal stopped; every test
also released control explicitly. Early sweeps conservatively aborted on
transient 80°C readings; the final sweep used a 90°C abort, retaining hardware
throttling at 85°C throughout. No fan-stop or raw-200 mode was used.

| Raw duty | Mean idle RPM | Microphone RMS dBFS |
| ---: | ---: | ---: |
| 46 | 1,547 | -26.54 |
| 56 | 1,859 | -26.61 |
| 68 | 2,211 | -26.63 |
| 82 | 2,573 | -26.31 |
| 95 | 2,875 | -25.92 |
| 127 | 3,521 | -25.45 |

Returning to duty 46 measured -27.30 dBFS, illustrating background/processing
variation. Differences among low speeds are not acoustically resolved by this
single sweep. Higher speeds showed more captured sound, but RPM is a more
repeatable measurement than small microphone-level differences here.

## Performance comparisons

Workload: four low-priority OpenSSL SHA-256 workers, 16,384-byte buffers,
wall-clock timing. CPU hottest-of-five temperature, fan RPM, lease state and
CPU0 frequency were sampled about every 0.5–0.6 seconds. Recorded thermal
throttling counters before/after; tests abort at 90°C. The server continued
running its ordinary services, so these are approximate comparisons with
uncontrolled background activity. CPU0 frequency is an instantaneous sample,
not an all-core performance measurement; throughput is the comparison metric.

The 30-second firmware and maximum-fan runs both achieved about 1,683 MB/s at
roughly 85°C. Both accumulated hardware thermal-throttling events. Raising fan
speed did not demonstrate a benefit in this comparison. Nominal Intel TjMax is
100°C, but the actual MSR target is 100 minus a 15°C offset. The daemon does not
raise that limit to obtain quieter benchmark results.

| Matched 45-second run | Steady RPM, approximate | SHA-256 MB/s | Peak sampled CPU |
| --- | ---: | ---: | ---: |
| Duty 46 | 1,550 | 1,584.7 | 86°C |
| Duty 68 | 2,230 | 1,614.2 | 85°C |
| Duty 82 | 2,590 | 1,614.8 | 86°C |
| Duty 127 | 3,540 | 1,609.2 | 86°C |

Duty 46 loses about 1.9% against the best matched result. Duty 68 is within
0.04% of duty 82, and slightly ahead of duty 127; these tiny differences are
not evidence of a performance advantage. It is the lowest tested point with
comparable throughput in this experiment. Different test lengths must not be
compared directly because initial Turbo behavior and heat buildup affect their
aggregate rates.

## Longer load check

At two minutes, duty 127 delivered 1,571.0 MB/s and duty 68 delivered
1,539.5 MB/s, approximately 2.0% lower. Average CPU0 frequency after the first
minute was about 3,102 versus 3,007 MHz. Peak sampled temperatures were 87°C
and 86°C respectively; the existing 85°C TCC setting remained active.
The lower speed is therefore a useful burst setting, not an equal-performance
replacement for all sustained workloads.

Eight-second C-meter windows under these loads read -25.91 dBFS at duty 127
and -27.98 dBFS at duty 68, a 2.07 dB relative reduction in this microphone
signal. This is not a calibrated SPL reduction; background variation and webcam
processing remain limitations.

## Controller

The profile uses raw duties 46 up to 60°C, 56 at 70°C, and 68 at 80–85°C,
interpolated between points. A 3-second temperature filter and 3°C hysteresis
reduce reactions to short bursts. Raw temperatures near 80°C bypass filtering
for the normal curve; 88°C immediately requests the highest validated duty,
127. Hardware throttling still starts at its existing 85°C threshold.

After 30 seconds of temperatures at or above approximately 80°C, a separate
sustained-load floor increases cooling to protect throughput as heat builds up.
The hot timer resets below 77°C. The owner selected **sustained duty 95**,
approximately 2,900 RPM, after a
matched two-minute run delivered 1,562.4 MB/s versus 1,571.0 MB/s at duty 127
(99.45% of the measured throughput). The microphone window read -26.98 dBFS,
about 1.08 dB below the duty-127 window. This accepts the measured ~0.55%
throughput tradeoff for lower noise; it does not claim identical performance.
Duty 127 remains reserved for the 88°C temperature override. A performance-first
alternative is `"sustained_duty": 127` in `/etc/u820-fan.json`.

After temperatures fall, the controller holds for eight seconds, then reduces
at three raw units per second. The three-second kernel lease is renewed every
0.5 seconds plus I/O latency. Missing feedback, a missed heartbeat, an unexpected
lease change, or daemon exit returns cooling to firmware. CPU Turbo is not
capped by the daemon.

The initial aggressive profile used duty 127 at 85°C and a long cooldown.
The final policy uses the measured sustained-load compromise above and returns
to quiet idle sooner after bursts.

## Final installed-service validation

The selected daemon completed a 120-second, four-worker run without a fault.
It acquired duty 68 at the first 0.53-second sample and moved to duty 95 at
30.66 seconds. Mean RPM after the first minute was 2,908; peak sampled CPU
temperature was 86°C. Throughput was 1,532.6 MB/s. This run started warm after
the preceding workload and used both speeds, so it validates controller
behavior rather than repeating the fixed-duty comparison. The ~0.55% result
above is one measured comparison, not a guaranteed workload-independent loss.
Hardware thermal throttling remained active.

After cooldown, the daemon returned to duty 46. The final service-state sample
at 12:54:52 Europe/Berlin was 45°C / 1,573 RPM; temperatures vary between reads
with ordinary server activity. Turbo remained enabled. The service was active
and enabled, DKMS reported the driver installed for 6.8.0-138-generic, all
20 Docker containers were running, and systemd reported no failed units.
Installed Python files and profile hashes matched the local sources.

Normal service stop restored firmware cooling immediately. Killing the daemon
with SIGKILL restored automatic mode and cleared the lease at the +3-second
sample through the kernel watchdog. The service was restarted afterward and
left running. Boot configuration is installed; a post-install reboot check is
deferred to the owner's normal maintenance window.

Captures: `evidence/load-selected-daemon.jsonl`,
`evidence/live-daemon-recovery.txt`, and `evidence/final-state.txt`.

## Evidence and reproduction

Raw captures and one-off experiment scripts are retained locally, not published
in this repository. The tables report their measured results; contributors can
reproduce their own comparisons with the commands and conditions described here.

Local captures: `evidence/noise-sweep-full.jsonl`, `noise-sustained*.json`,
`load-*.jsonl`, `thermal-target-services.txt`, and the experiment scripts
`noise_sweep.py` / `load_probe.py`. The reusable microphone command is
`sudo scripts/measure-noise.sh 8`. The C meter is covered by known-signal tests.
A benchmark invocation is:

```sh
nice -n 19 openssl speed -elapsed -seconds 45 -multi 4 -evp sha256 -bytes 16384 -mr
```

For future calibration, monitor temperature and throttle counters while this
runs, retain the driver lease watchdog, and release any manual setting afterward.
Do not run a fixed-duty experiment concurrently with the daemon. For day-to-day
use, the installed service owns the fan and needs no microphone or benchmark.

Primary references: [Intel i5-8279U specifications](https://www.intel.com/content/www/us/en/products/sku/191070/intel-core-i58279u-processor-6m-cache-up-to-4-10-ghz/specifications.html),
[Linux Intel TCC source](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/thermal/intel/intel_tcc.c),
[Linux intel_pstate](https://docs.kernel.org/6.8/admin-guide/pm/intel_pstate.html).
Live MSR readings, not nominal specifications alone, establish this machine's
configured thermal threshold.
