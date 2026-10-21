# Project status

## Initial public release

- U820 / CB9 fan RPM and bounded manual control demonstrated on Ubuntu.
- Temperature-based daemon, DKMS installation, systemd service and JSON tuning
  are implemented; firmware restoration is validated on stop and process death.
- Quiet profile, calibration limits, installation, tuning, rollback and sensor
  inventory are documented. Read DISCLAIMER.md before use.
- Python tests pass with 100% line/branch coverage. C control fault tests and
  local clangd checks pass. Public CI adds Ubuntu HWE kernel compilation.
- Live-tested kernel: 6.8.0-138-generic, Ubuntu 22.04.5. CI build success is not
  evidence of live compatibility on a new kernel or different hardware.
- Remaining: post-install reboot validation; physical ADC/second-tachometer
  mapping; calibrated acoustic measurement; broader live compatibility testing.
- An independent EC hardware timeout is unknown. Software cannot cover kernel
  crashes, stalled ECs or physical fan failure.

The public repository begins with a reviewed source snapshot. Private research
logs, host identifiers, firmware binaries and original development history are
not distributed. See CHANGELOG.md and docs/ for reproducible methods and limits.
