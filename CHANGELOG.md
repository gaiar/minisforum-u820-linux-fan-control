# Changelog

## License policy update

- MIT is now the default license for the project. Kernel source and the shared
  control header are additionally available under GPL-2.0-only, expressed as
  `GPL-2.0-only OR MIT`, with the `Dual MIT/GPL` module tag.
- Existing GPL grants in the initial public release remain available. No fan
  policy or runtime behavior changed. See [LICENSING.md](LICENSING.md).

## 0.2.0 — Initial public release

- U820 / CB9 EC driver with fan RPM, identity-gated raw duty 46–127, a renewable
  three-second lease, watchdog checks and firmware cooling restoration.
- Temperature-based Python daemon with configurable curves, hysteresis,
  asymmetric ramps, sustained-load behavior and a thermal override.
- DKMS, systemd installation, sensor monitoring and optional streaming C audio
  statistics without saved recordings.
- Measured quiet profile, installation/tuning/recovery guides and explicit
  hardware-risk disclaimer under GPL-2.0-only.
- GitHub Actions for Python, C, shell and Ubuntu kernel builds.

Live validation: Ubuntu 22.04.5, kernel 6.8.0-138-generic, U820 / CB9,
BIOS CB9_FV102, EC 1.05 build 2021/01/20 11:28:11. Other hardware/EC builds
are unsupported. Post-install reboot validation remains pending. No firmware
was flashed; unknown sensor mappings and software-watchdog limits remain
documented in the research and maintenance notes.
