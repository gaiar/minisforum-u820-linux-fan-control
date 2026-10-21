# U820 sensor inventory

Baseline: Ubuntu 22.04.5, kernel 6.8.0-90-generic, U820 / CB9,
BIOS CB9_FV102, EC build 2021/01/20 11:28:11. Captures are in
`evidence/live-baseline.txt`, `evidence/live-followup.txt`, and
`evidence/ec-baseline.bin`. Values vary with workload. Hardware-monitor
numbers can change after reboot; identify devices by name and resolved path.
Raw captures remain private and are not part of this public repository.

| Interface | Evidence and meaning | Linux exposure | Validation or remaining work |
| --- | --- | --- | --- |
| CPU package + 4 cores | **Seen live**, coretemp | Five `temp*_input`, °C | Working; observed roughly 40–50 °C |
| PCH | **Seen live**, pch_cannonlake | One temperature, °C | Working; roughly 40–42 °C |
| NVMe composite + sensor 1 + sensor 2 | **Seen live**, NVMe controller | Three temperatures, °C | Working; roughly 58–63 °C; physical locations of numbered sensors unspecified |
| Memory sensor at I2C 0-0018 | **Seen live**, jc42 | One temperature, °C | Working; roughly 44–45 °C; exact DIMM/slot assignment unknown |
| Memory sensor at I2C 0-001a | **Seen live**, jc42 | One temperature, °C | Working; roughly 43–45 °C; exact DIMM/slot assignment unknown |
| ACPI thermal zone | **Seen live**, acpitz 27.8 °C | One temperature | **Inferred fallback**, SSDT9 `_TMP` uses `0x0bc2` under its fallback condition; not established as a physical reading |
| Wi-Fi temperature | **Seen live**, iwlwifi_1 attribute | Read returns `ENODATA` | Unavailable in baseline; retained as an error, not dropped or reported as zero |
| RAPL package-0 | **Seen live** | `intel-rapl:0/energy_uj` | Root-readable energy, converted to joules |
| RAPL core | **Seen live** | `intel-rapl:0:0/energy_uj` | Root-readable energy |
| RAPL uncore | **Seen live** | `intel-rapl:0:1/energy_uj` | Root-readable energy |
| RAPL dram | **Seen live** | `intel-rapl:0:2/energy_uj` | Root-readable energy |
| RAPL psys | **Seen live** | `intel-rapl:1/energy_uj` | Root-readable energy; not a measured wall-socket power claim |
| EC ACPI offset 62 / XRAM 0462 | **Seen live** raw 50–53; ACPI TER4 | Raw byte through existing ec_sys | File path filters ADC-like input; physical sensor, units, calibration pending |
| EC offset 70 / XRAM 0470 | **Seen live** raw 40–49 | Raw byte through ec_sys | **Seen in file** as fan-curve input; source/override logic traced; live physical calibration pending |
| EC offset 71 / XRAM 0471 | **Seen live** raw 50–53 | Raw byte through ec_sys | **Seen offline** as max(62,70), not a separate physical sensor |
| EC offset 72 / XRAM 0472 | **Seen live** raw 50–52; ACPI TER2 | Raw byte through ec_sys | File path filters a separate ADC-like input; physical sensor and calibration pending |
| EC tachometer 1 | **Seen in file** 181f:181e → 080b:080c | Driver `fan1_input`, RPM | Validated live on 6.8.0-138: ~1,545 RPM at duty 46, ~3,520 at duty 127 |
| EC tachometer 2 | **Seen in file** 1821:1820 → 083d:083e | Raw snapshot in experimental module | Both raw period and converted count read zero live; wiring remains unknown, not counted as a second fan |
| EC fan mode | **Seen in file** at 080a | Root-only `fan_pwm_raw` with a 3-second lease | Mode 84 / duties 46–127 and automatic restoration validated live; presets 1/2 also tested |
| EC PWM 1808 | **Seen in file**, automatic and fixed outputs | Raw snapshot in experimental module | Monotonic speed response validated; raw duty is not a percentage |
| EC PWM 1807 | **Seen in file**, updated with fixed outputs | Read-only snapshot; bounded presets can restore zero | Wiring unknown; idle zero and full preset rollback verified; direct mode 84 leaves it untouched |
| EC ADC-related channels | **Seen in file**, routines b326 onward read registers around 1900 | Filtered candidates above; other channels not mapped | Separate voltage/current interpretation or board resistor scaling unknown |
| ACPI PTID fan/power/thermistor labels | **Seen** in SSDT5 | OSDD empty, PSDD invalid placeholders, RPMD zeros | Labels alone do not establish working sensors; WPMD does not perform a hardware write |
| Firmware Setup fan/voltage/temperature labels | **Seen** in SetupUtility strings/IFR | No usable Linux interface established | Fan/temperature UI path reaches EFI_UNSUPPORTED helper; not evidence of implemented monitoring |
| Other board voltages or currents | **Unknown** | No native hwmon voltage/current input seen in baseline | Continue tracing ADC and board wiring; do not infer absent hardware |

The Python monitor follows the standard
[hwmon units](https://docs.kernel.org/hwmon/sysfs-interface.html) and keeps
unreadable inputs visible. Current RAPL readings are cumulative counters, not
watts. A future power calculation needs elapsed monotonic time and wrap handling
using `max_energy_range_uj`, as described by
[Linux powercap](https://docs.kernel.org/power/powercap/powercap.html).

Two baseline threshold anomalies need separate treatment: jc42 exposes zero
high/critical thresholds and corresponding alarm flags; NVMe exposes an
implausible maximum near 65261 °C. Neither is a reliable overheating conclusion
or a usable control threshold. They are not used for fan control.

The baseline accounts for 18 input attributes, including the ACPI fallback and
unavailable Wi-Fi attribute. This is not a claim of 18 independent physical
sensors or complete hardware coverage.


Current driver adds one validated fan-speed input to the 18-input baseline.
The daemon uses the hottest of the five coretemp inputs and fails back to
firmware cooling if any becomes unavailable or fan feedback is invalid.

**Seen live:** turbostat reads IA32_TEMPERATURE_TARGET=0x0f640000: nominal TjMax
100 C minus a 15 C offset gives an **85 C hardware throttling target**. Ordinary
coretemp `temp*_crit` still reads 100 C and does not disclose this offset. No
thermal target, power limit, or hardware protection was changed. This threshold
must be considered when evaluating full-performance cooling.
