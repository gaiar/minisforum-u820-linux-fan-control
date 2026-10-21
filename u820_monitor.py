"""Monitor U820 sensors and request guarded, temporary fan boost presets."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

INPUT_PATTERN = re.compile(r"(temp|fan|in|curr|power|energy|humidity)\d+_input")
UNITS = {
    "temp": (1000, "°C"),
    "fan": (1, "RPM"),
    "in": (1000, "V"),
    "curr": (1000, "A"),
    "power": (1_000_000, "W"),
    "energy": (1_000_000, "J"),
    "humidity": (1000, "%RH"),
}
EC_CANDIDATES = {
    0x62: "TER4 in ACPI; physical sensor and unit unverified",
    0x70: "Fan-curve input in analyzed EC image; live unit unverified",
    0x71: "max(0x62, 0x70) in analyzed EC image; live mapping unverified",
    0x72: "TER2 in ACPI; physical sensor and unit unverified",
}


@dataclass(frozen=True)
class Reading:
    """Represent one kernel reading, including an explicit unavailable state."""

    device: str
    label: str
    path: str
    raw: int | None
    value: float | None
    unit: str
    error: str | None = None


def optional_text(path: Path, fallback: str) -> str:
    """Read a descriptive sysfs attribute or return a stable fallback."""
    try:
        return path.read_text().strip() or fallback
    except OSError:
        return fallback


def read_value(path: Path, device: str, label: str, kind: str) -> Reading:
    """Convert a standard Linux sensor input without interpreting its wiring."""
    if kind not in UNITS:
        raise ValueError(f"Unsupported sensor kind: {kind}")
    divisor, unit = UNITS[kind]
    try:
        raw = int(path.read_text().strip())
    except (OSError, ValueError) as error:
        return Reading(device, label, str(path), None, None, unit, str(error))
    return Reading(device, label, str(path), raw, raw / divisor, unit)


def collect_sensors(sysfs: Path = Path("/sys")) -> list[Reading]:
    """Read existing hwmon inputs and RAPL energy counters, retaining failures."""
    readings = []
    for device_path in sorted((sysfs / "class/hwmon").glob("hwmon*")):
        name = optional_text(device_path / "name", device_path.name)
        # The resolved path distinguishes identically named DIMM/NVMe devices.
        device = f"{name} ({device_path.resolve()})"
        for path in sorted(device_path.glob("*_input")):
            match = INPUT_PATTERN.fullmatch(path.name)
            if match is None:
                continue
            label = optional_text(
                path.with_name(path.name.replace("_input", "_label")),
                path.name.removesuffix("_input"),
            )
            readings.append(read_value(path, device, label, match[1]))
    for zone in sorted((sysfs / "class/powercap").glob("intel-rapl:*")):
        if not (zone / "energy_uj").exists():
            continue
        name = optional_text(zone / "name", zone.name)
        readings.append(read_value(zone / "energy_uj", zone.name, name, "energy"))
    return readings


def check_u820(sysfs: Path = Path("/sys")) -> None:
    """Reject live EC interpretation on a machine other than the validated BIOS."""
    expected = {
        "product_name": "U820",
        "board_name": "CB9",
        "bios_version": "CB9_FV102",
    }
    for field, value in expected.items():
        actual = optional_text(sysfs / "class/dmi/id" / field, "unknown")
        if actual != value:
            raise ValueError(
                f"Unsupported machine: {field}={actual!r}, expected {value!r}"
            )


def decode_ec(data: bytes) -> dict[str, object]:
    """Decode only established version fields; keep candidate sensor bytes raw."""
    if len(data) != 256:
        raise ValueError(f"Expected exactly 256 EC bytes, received {len(data)}")
    return {
        "ec_version_bytes": list(data[:2]),
        "ec_version": f"{data[0]:x}.{data[1]:02x}",
        "version_evidence": "SetupUtility read helper 0x2b1d4; callers 0x20bb0/0x2f86f",
        "build_time_text": data[0x22:0x2A].decode("ascii", errors="replace"),
        "build_date_text": data[0x31:0x3B].decode("ascii", errors="replace"),
        "candidates": [
            {"offset": f"0x{offset:02x}", "raw": data[offset], "evidence": description}
            for offset, description in EC_CANDIDATES.items()
        ],
        "fan_control": "Use driver fan_boost for control; ec_sys stays read-only",
        "fan_rpm": None,
        "raw_hex": data.hex(),
    }


def read_ec(sysfs: Path = Path("/sys")) -> dict[str, object]:
    """Read the existing ec_sys interface without loading modules or enabling writes."""
    check_u820(sysfs)
    path = sysfs / "kernel/debug/ec/ec0/io"
    with path.open("rb") as stream:
        data = stream.read(257)
    report = decode_ec(data)
    report["source"] = str(path)
    report["write_support"] = optional_text(
        sysfs / "module/ec_sys/parameters/write_support", "unknown"
    )
    return report


def request_boost(level: int, sysfs: Path = Path("/sys")) -> dict[str, object]:
    """Request preset 1 or 2 for five seconds, or zero to restore automatic mode."""
    if type(level) is not int or level not in (0, 1, 2):
        raise ValueError("Boost must be 0 (automatic), 1, or 2; preset 3 is disabled")
    check_u820(sysfs)
    matches = [
        path
        for path in (sysfs / "class/hwmon").glob("hwmon*")
        if optional_text(path / "name", "") == "u820_ec_probe"
    ]
    if len(matches) != 1:
        raise ValueError("Expected exactly one loaded U820 fan driver")
    target = matches[0] / "fan_boost"
    if not target.exists():
        raise ValueError("Driver is read-only; bounded boost is not enabled")
    target.write_text(f"{level}\n")
    return {
        "preset": level,
        "timeout_seconds": 5 if level else 0,
        "source": str(target),
        "result": (
            "Boost requested; kernel restores automatic mode after five seconds"
            if level
            else "Automatic mode restored"
        ),
    }


def print_sensors(readings: list[Reading]) -> None:
    """Display sensor values and failures without hiding missing fan support."""
    previous_device = None
    for reading in readings:
        if reading.device != previous_device:
            print(f"\n{reading.device}")
            previous_device = reading.device
        value = (
            f"{reading.value:.3f} {reading.unit}"
            if reading.value is not None
            else f"unavailable ({reading.error})"
        )
        print(f"  {reading.label:24} {value}")
    fans = sum(reading.unit == "RPM" for reading in readings)
    failures = sum(reading.error is not None for reading in readings)
    print(f"\nRead {len(readings)} inputs; {fans} fan inputs; {failures} unavailable.")
    if not fans:
        print("No fan RPM interface exposed; check that the U820 driver is loaded.")


def main(argv: list[str] | None = None) -> int:
    """Run sensor collection, EC inspection, or a bounded boost request."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("sensors", "ec", "boost"))
    parser.add_argument("preset", nargs="?", type=int, choices=(0, 1, 2))
    parser.add_argument(
        "--json", action="store_true", help="Emit machine-readable JSON"
    )
    args = parser.parse_args(argv)
    if (args.command == "boost") != (args.preset is not None):
        parser.error("boost requires a preset (0, 1, or 2); other commands take none")
    print(f"Running {args.command} through Linux interfaces…", file=sys.stderr)
    try:
        if args.command == "boost":
            result = request_boost(args.preset)
            print(json.dumps(result, indent=2) if args.json else result["result"])
        elif args.command == "sensors":
            readings = collect_sensors()
            if args.json:
                print(json.dumps([asdict(reading) for reading in readings], indent=2))
            else:
                print_sensors(readings)
            if not readings:
                print("No supported Linux sensor inputs found.", file=sys.stderr)
                return 1
        else:
            print(json.dumps(read_ec(), indent=2))
    except (OSError, ValueError) as error:
        print(
            f"{'Boost' if args.command == 'boost' else 'Read'} failed: {error}",
            file=sys.stderr,
        )
        return 1
    if args.command != "boost":
        print("Read complete; no control writes performed.", file=sys.stderr)
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
