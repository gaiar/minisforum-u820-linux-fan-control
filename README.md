# Minisforum U820 Linux Fan Control — Quiet Cooling on Ubuntu

[![CI](https://github.com/gaiar/minisforum-u820-linux-fan-control/actions/workflows/ci.yml/badge.svg)](https://github.com/gaiar/minisforum-u820-linux-fan-control/actions/workflows/ci.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](LICENSE)
[![Python 3.10+](https://img.shields.io/badge/Python-3.10%2B-blue.svg)](pyproject.toml)

**Linux fan-speed control and temperature monitoring for the MINISFORUM U820
mini PC.** An experimental DKMS kernel driver exposes fan RPM and bounded fan
control; a lightweight Python daemon adjusts cooling with a configurable fan
curve, hysteresis, and delayed slowdowns. Developed and tested on **Ubuntu
Server 22.04.5 with an Intel Core i5-8279U**.

The goal is quieter idle and short Turbo Boost workloads, with more cooling
when heat persists. CPU Turbo remains available: the daemon does not change
CPU frequency, voltage, power limits, or hardware thermal protections.

> **Use entirely at your own risk and responsibility.** This experimental
> software writes to the embedded controller and affects cooling. Bugs,
> incompatible hardware, or unsuitable settings can cause overheating,
> instability, data loss, or hardware damage. It is provided without warranty;
> see the [risk disclaimer](DISCLAIMER.md) and [license](LICENSE). This is an
> independent community project, not affiliated with or endorsed by MINISFORUM.

## Features

- Fan RPM through Linux `hwmon`, plus a sensor CLI for CPU, chipset, NVMe, DIMM
  temperatures and RAPL energy counters where available.
- A JSON fan curve, temperature smoothing, hysteresis, and gradual cooldown.
- Separate settings for short bursts and sustained hot workloads.
- A three-second renewable kernel lease that restores firmware cooling if
  userspace stops renewing it; normal daemon exit also releases control.
- DKMS rebuilding for kernel updates, a systemd service, and live JSON status.
- Optional webcam-microphone measurements for relative noise comparisons.
- Automated Python, C, shell, and kernel-build checks in GitHub Actions.

## Supported hardware — read before installing

| Component | Validated configuration |
| --- | --- |
| Mini PC / DMI product | MINISFORUM U820 / `U820` |
| DMI board | `CB9` |
| CPU | Intel Core i5-8279U |
| BIOS | `CB9_FV102` |
| EC identity | Version 1.05, build `2021/01/20 11:28:11` |
| Live operating system | Ubuntu 22.04.5, x86-64 |
| Live-tested kernel | `6.8.0-138-generic` |

Control is deliberately restricted to this board, BIOS, and EC build. **Other
MINISFORUM models and other EC builds are unsupported.** Matching the model
name alone is insufficient. The driver validates identity before exposing
writable controls; do not remove that check to force an installation.

No BIOS flashing is required. Firmware archives, vendor tools, machine dumps,
and microphone recordings are not distributed here. Kernel compilation in CI
is separate from live hardware validation.

## Install on Ubuntu

Read the [installation guide](docs/installation.md) and [disclaimer](DISCLAIMER.md)
first. Run these commands on the U820, with root access and time to observe its
cooling. Python 3.10+ is required. Secure Boot must permit the DKMS module;
the tested machine used Secure Boot disabled. Signing and enrolling a key is
an alternative and requires your own platform setup.

```sh
sudo apt-get update
sudo apt-get install git dkms build-essential linux-headers-$(uname -r) python3-venv
git clone https://github.com/gaiar/minisforum-u820-linux-fan-control.git
cd minisforum-u820-linux-fan-control

# Inspect the installer and configuration before running as root.
less scripts/install-u820.sh
cat deploy/u820-fan.json
sudo sh scripts/install-u820.sh

# Check feedback, then start control explicitly.
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
sudo systemctl start u820-fan
systemctl status u820-fan --no-pager
cat /run/u820-fan/state.json
```

The installer builds and registers `u820-ec/0.2.0` with DKMS, installs the
isolated runtime in `/opt/u820-fan`, and enables startup at boot. It preserves
an existing `/etc/u820-fan.json`. It **does not start the daemon immediately**;
the explicit start above does. A post-install reboot has not yet been validated
on the reference server; verify startup at your next maintenance window.

To stop custom control and restore firmware cooling:

```sh
sudo systemctl stop u820-fan
```

To keep it stopped after reboot, use `sudo systemctl disable --now u820-fan`.
See [operation, recovery and removal](docs/maintenance.md).

## Tune the fan curve yourself

**Yes — edit `/etc/u820-fan.json`.** No recompilation is needed. See the
[configuration guide](docs/configuration.md) for every setting, accepted range,
examples, validation, and rollback.

```sh
sudo cp /etc/u820-fan.json /etc/u820-fan.json.bak
sudoedit /etc/u820-fan.json
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
# Continue only if the check succeeds.
sudo systemctl restart u820-fan
journalctl -u u820-fan -n 30 --no-pager
```

The shipped profile measured approximately:

| Condition | Raw duty | Fan speed on the reference unit |
| --- | ---: | ---: |
| Idle / CPU at or below 60°C | 46 | 1,550 RPM |
| Short hot burst, around 80–85°C | 68 | 2,230 RPM |
| Sustained heat for 30 seconds | 95 | 2,900 RPM |
| Temperature override at 88°C | 127 | 3,540 RPM |

Raw duty is an EC code, **not a percentage or a requested RPM**. Speeds vary
by unit and conditions. Supported manual duties are 46–127; fan-stop commands
and the problematic raw-200 preset are not exposed.

For more cooling during long workloads, set `"sustained_duty": 127`.
The default is 95. Hardware throttling on the reference machine starts at an
existing 85°C TCC target, before the daemon's 88°C override; this project does
not raise that limit.

## Measured noise and performance

In one matched two-minute, four-worker SHA-256 comparison, approximately
2,900 RPM delivered **99.45% of the throughput** measured at approximately
3,540 RPM. The webcam microphone signal was about **1.08 dB lower**.

These are installation-specific observations, not performance guarantees.
The acoustic result is relative, unweighted **dBFS**, not calibrated dBA or
sound-pressure level. Background services, ambient noise, webcam processing,
and placement affect the result. Both settings encountered the existing
hardware thermal limit. Read the [calibration report](docs/calibration.md)
for methods, limits, and the final daemon test.

## Monitoring and troubleshooting

```sh
cat /run/u820-fan/state.json
journalctl -u u820-fan -n 50 --no-pager
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_monitor.py sensors
dkms status
```

- **After a kernel update:** DKMS attempts a rebuild when matching headers are
  present. Check its result and the service after reboot. If the module cannot
  load, the daemon is skipped and firmware cooling is used.
- **After a daemon fault:** the unit stays stopped intentionally while the
  driver restores firmware control. Inspect the journal before restarting.
- **No writable fan interface:** check identity, driver options, and Secure Boot.
  Do not bypass the driver's compatibility checks.
- **Other sensors:** see the [sensor inventory](docs/sensors.md). Several EC ADC
  mappings remain unknown; full sensor coverage is not claimed.

A kernel crash, stalled EC, or failed hardware can defeat software recovery.
No independent EC hardware timeout has been established. Do not run another
vendor EC writer or fan controller concurrently.

## Development and evidence

See [CONTRIBUTING.md](CONTRIBUTING.md) for the virtual environment, full check
sequence, C/C++ clangd validation, and CI scope. The [firmware research](docs/ec_firmware.md)
records protocol evidence, offsets and hashes, distinguishing observed facts,
inferences and unknowns. Optional [microphone measurements](docs/configuration.md#relative-noise-measurements)
stream statistics without saving audio; the daemon itself does not use a microphone.

Licensed under **GPL-2.0-only**. See [LICENSE](LICENSE).
