# Contributing and local validation

This is experimental cooling software. Keep identity gates, bounded writes,
feedback checks, and firmware restoration intact. New hardware support needs
independent firmware evidence and controlled live validation; a model name or
successful compilation is not sufficient. Read [DISCLAIMER.md](DISCLAIMER.md).

## Development environment

The project defaults to MIT. Contributions to the kernel source and shared
control header must preserve their `GPL-2.0-only OR MIT` dual-license choice;
other original contributions use MIT unless explicitly documented otherwise.
Retain third-party notices. See [LICENSING.md](LICENSING.md).

Python 3.10+ and a virtual environment are required. Runtime Python code uses
the standard library. Install a C compiler, ShellCheck and clangd separately.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -e '.[dev]'
.venv/bin/pre-commit install
.venv/bin/ruff format --check .
.venv/bin/ruff check .
.venv/bin/mypy
.venv/bin/pytest
shellcheck scripts/*.sh
sh scripts/check-control.sh
clangd --check=tools/pcm_rms.c
clangd --check=tests/control_test.c
```

Tests must maintain at least 90% line/branch coverage for the Python monitor
and controller. C tests exercise the same control helpers as the kernel and
inject partial write failures under AddressSanitizer and UndefinedBehaviorSanitizer.
The PCM meter is tested with known stereo signals, DC offsets and malformed input.
Use concise commit messages without AI attribution lines.

## Kernel build and C/C++ language-server validation

On Linux with matching kernel headers:

```sh
make -C kernel KCFLAGS=-Werror
.venv/bin/python /usr/src/linux-headers-$(uname -r)/scripts/clang-tools/gen_compile_commands.py \
  -d /usr/src/linux-headers-$(uname -r) -o kernel/compile_commands.json kernel
clangd --check=kernel/u820_ec_probe.c --compile-commands-dir=kernel
```

The compilation-database generator uses Python's standard library. The checked-in `kernel/.clangd`
removes specific GCC-only code-generation flags for clangd while preserving
kernel include paths and defines. The original validation also used actual
LSP initialize/didOpen/publishDiagnostics sessions for the driver and PCM meter.
Those local logs are not distributed. Building/checking does not load the driver.

## What GitHub Actions tests

The [CI workflow](.github/workflows/ci.yml) runs on pushes, pull requests, manual
dispatch, and weekly to catch changes in Ubuntu headers and dependencies:

- Python 3.10, 3.12 and 3.14: formatting, lint, strict types, tests and coverage.
- ShellCheck, C fault injection with sanitizers, C clangd checks, and default
  profile validation without accessing hardware.
- DKMS build/install against Ubuntu 22.04 and 24.04 HWE headers, with W=1 and
  warnings as errors. It prints the exact selected kernel version.

The kernel job installs the module file in the disposable runner but **never
loads it** and never runs the hardware installer. CI cannot test temperatures,
cooling, firmware restoration, Secure Boot enrollment, or service persistence
on a U820. Passing a newer-kernel compile does not extend the live-tested matrix.
Actions use commit pins, read-only repository permissions, and no project secrets.

## Firmware research

The optional offline harness requires a separately obtained, hash-matching EC
image and pinned emulator sources. See [firmware findings](docs/ec_firmware.md)
and `scripts/check-firmware.sh`. Vendor firmware is not redistributed and the
firmware execution suite is not part of public CI. Source hashes and offsets
remain documented; raw machine captures and one-off research scripts stay local.

Label observations as seen, inferred or unknown. Do not infer a safe write or
physical sensor mapping from a firmware label alone. Avoid publishing serial
numbers, private IP addresses, credentials, full system dumps, or recordings.
`scripts/collect-u820.sh` includes host/service information: inspect and redact
its output before sharing any excerpt.

## Reporting an issue

Include the project revision, Ubuntu and kernel version, non-serial DMI model /
board / BIOS, `dkms status`, and a small relevant journal excerpt. Explain
expected versus observed behavior and whether firmware cooling was restored.
For noise comparisons, include workload, duration, temperature, RPM, microphone
conditions and measurement units. Do not post recordings or firmware binaries.
