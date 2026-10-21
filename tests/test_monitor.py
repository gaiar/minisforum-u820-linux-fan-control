"""Exercise failure handling, identity gates, units, and real capture decoding."""

import json
from pathlib import Path

import pytest

import u820_monitor as monitor


def put(root: Path, relative: str, value: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)
    return path


def identity(root: Path) -> None:
    for name, value in {
        "product_name": "U820",
        "board_name": "CB9",
        "bios_version": "CB9_FV102",
    }.items():
        put(root, f"class/dmi/id/{name}", value)


@pytest.fixture
def ec_data() -> bytes:
    data = bytearray(256)
    data[:2] = bytes([1, 5])
    data[0x22:0x2A] = b"11:28:11"
    data[0x31:0x3B] = b"2021/01/20"
    data[0x62] = 53
    data[0x70:0x73] = bytes([49, 53, 52])
    return bytes(data)


@pytest.mark.parametrize(
    ("kind", "raw", "value", "unit"),
    [
        ("temp", -12500, -12.5, "°C"),
        ("fan", 2300, 2300, "RPM"),
        ("in", 12000, 12, "V"),
        ("curr", 250, 0.25, "A"),
        ("power", 18250000, 18.25, "W"),
        ("energy", 1500000, 1.5, "J"),
        ("humidity", 45000, 45, "%RH"),
    ],
)
def test_kernel_units(tmp_path, kind, raw, value, unit):
    path = put(tmp_path, "input", str(raw))
    reading = monitor.read_value(path, "device", "label", kind)
    assert (reading.raw, reading.value, reading.unit) == (raw, value, unit)
    assert reading.error is None


def test_failures_remain_visible(tmp_path, monkeypatch):
    path = put(tmp_path, "input", "bad")
    assert monitor.read_value(path, "d", "l", "temp").value is None
    with pytest.raises(ValueError, match="Unsupported sensor kind"):
        monitor.read_value(path, "d", "l", "invalid")

    def denied(self):
        raise PermissionError("permission denied")

    monkeypatch.setattr(Path, "read_text", denied)
    reading = monitor.read_value(path, "d", "l", "energy")
    assert reading.raw is None and "permission denied" in reading.error
    assert monitor.optional_text(path, "fallback") == "fallback"


def test_inventory_handles_duplicates_and_missing_labels(tmp_path):
    for number in (0, 1):
        put(tmp_path, f"class/hwmon/hwmon{number}/name", "jc42")
        put(tmp_path, f"class/hwmon/hwmon{number}/temp1_input", "45750")
    put(tmp_path, "class/hwmon/hwmon0/temp1_label", "DIMM A")
    put(tmp_path, "class/hwmon/hwmon0/unrelated_input", "999")
    put(tmp_path, "class/hwmon/hwmon0/temp1_max", "0")
    put(tmp_path, "class/powercap/intel-rapl:0/energy_uj", "1234000")
    put(tmp_path, "class/powercap/intel-rapl:0/name", "package-0")
    put(tmp_path, "class/powercap/intel-rapl:1/name", "")
    readings = monitor.collect_sensors(tmp_path)
    assert len(readings) == 3
    assert readings[0].label == "DIMM A"
    assert readings[1].label == "temp1"
    assert readings[0].device != readings[1].device
    assert readings[2].value == 1.234
    assert (
        monitor.optional_text(tmp_path / "class/powercap/intel-rapl:1/name", "x") == "x"
    )


def test_empty_inventory(tmp_path):
    assert monitor.collect_sensors(tmp_path) == []


def test_ec_decode_preserves_uncertainty(ec_data):
    report = monitor.decode_ec(ec_data)
    assert report["ec_version"] == "1.05"
    assert report["build_date_text"] == "2021/01/20"
    assert report["build_time_text"] == "11:28:11"
    assert report["fan_rpm"] is None
    assert bytes.fromhex(report["raw_hex"]) == ec_data
    assert [entry["raw"] for entry in report["candidates"]] == [53, 49, 53, 52]
    assert all("unverified" in entry["evidence"] for entry in report["candidates"])


def test_ec_version_matches_firmware_hex_format(ec_data):
    data = bytearray(ec_data)
    data[:2] = bytes([0xA, 0x1B])
    assert monitor.decode_ec(data)["ec_version"] == "a.1b"


@pytest.mark.parametrize("length", [0, 255, 257])
def test_reject_short_or_oversized_ec(length):
    with pytest.raises(ValueError, match="exactly 256"):
        monitor.decode_ec(bytes(length))


def test_live_ec_identity_gate_precedes_io(tmp_path, monkeypatch):
    def forbidden_open(*args, **kwargs):
        pytest.fail("EC must not open on unsupported hardware")

    monkeypatch.setattr(monitor, "optional_text", lambda *args: "not-U820")
    monkeypatch.setattr(Path, "open", forbidden_open)
    with pytest.raises(ValueError, match="Unsupported machine"):
        monitor.read_ec(tmp_path)


def test_live_ec_uses_read_only_open(tmp_path, ec_data, monkeypatch):
    identity(tmp_path)
    io_path = tmp_path / "kernel/debug/ec/ec0/io"
    io_path.parent.mkdir(parents=True)
    io_path.write_bytes(ec_data)
    put(tmp_path, "module/ec_sys/parameters/write_support", "N")
    original_open = Path.open

    def observe_open(self, mode="r", *args, **kwargs):
        assert mode in {"r", "rb"}
        return original_open(self, mode, *args, **kwargs)

    monkeypatch.setattr(Path, "open", observe_open)
    report = monitor.read_ec(tmp_path)
    assert report["write_support"] == "N"
    assert report["source"] == str(io_path)


def test_identity_mismatch(tmp_path):
    identity(tmp_path)
    put(tmp_path, "class/dmi/id/bios_version", "changed")
    with pytest.raises(ValueError, match="bios_version"):
        monitor.check_u820(tmp_path)


def test_display_unavailable_and_fan(capsys):
    monitor.print_sensors(
        [
            monitor.Reading("chip", "fan", "p", 1200, 1200, "RPM"),
            monitor.Reading("chip", "temp", "q", None, None, "°C", "offline"),
        ]
    )
    text = capsys.readouterr().out
    assert "1200.000 RPM" in text
    assert "unavailable (offline)" in text
    assert "1 fan inputs; 1 unavailable" in text


def test_cli_sensors_and_json(monkeypatch, capsys):
    readings = [monitor.Reading("cpu", "package", "p", 45000, 45.0, "°C")]
    monkeypatch.setattr(monitor, "collect_sensors", lambda: readings)
    assert monitor.main(["sensors", "--json"]) == 0
    captured = capsys.readouterr()
    assert json.loads(captured.out)[0]["raw"] == 45000
    assert "Read complete" in captured.err
    assert monitor.main(["sensors"]) == 0
    assert "No fan RPM interface exposed" in capsys.readouterr().out


def test_cli_empty_and_failure(monkeypatch, capsys):
    monkeypatch.setattr(monitor, "collect_sensors", lambda: [])
    assert monitor.main(["sensors", "--json"]) == 1
    assert "No supported Linux sensor inputs" in capsys.readouterr().err

    def failed_read():
        raise PermissionError("use root to read ec_sys")

    monkeypatch.setattr(monitor, "read_ec", failed_read)
    assert monitor.main(["ec"]) == 1
    assert "Read failed" in capsys.readouterr().err


def test_cli_ec(monkeypatch, capsys, ec_data):
    monkeypatch.setattr(monitor, "read_ec", lambda: monitor.decode_ec(ec_data))
    assert monitor.main(["ec", "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["ec_version"] == "1.05"


def test_boost_gates_and_scope(tmp_path):
    for invalid in (-1, 3, 128, True, "1"):
        with pytest.raises(ValueError, match="Boost must"):
            monitor.request_boost(invalid, tmp_path)
    with pytest.raises(ValueError, match="Unsupported machine"):
        monitor.request_boost(1, tmp_path)
    identity(tmp_path)
    with pytest.raises(ValueError, match="exactly one"):
        monitor.request_boost(1, tmp_path)
    put(tmp_path, "class/hwmon/hwmon0/name", "unrelated")
    put(tmp_path, "class/hwmon/hwmon1/name", "u820_ec_probe")
    with pytest.raises(ValueError, match="read-only"):
        monitor.request_boost(1, tmp_path)
    target = put(tmp_path, "class/hwmon/hwmon1/fan_boost", "0")
    result = monitor.request_boost(2, tmp_path)
    assert target.read_text() == "2\n" and result["timeout_seconds"] == 5
    result = monitor.request_boost(0, tmp_path)
    assert target.read_text() == "0\n" and result["timeout_seconds"] == 0
    put(tmp_path, "class/hwmon/hwmon2/name", "u820_ec_probe")
    with pytest.raises(ValueError, match="exactly one"):
        monitor.request_boost(1, tmp_path)


def test_boost_cli_and_failure(monkeypatch, capsys):
    monkeypatch.setattr(
        monitor, "request_boost", lambda level: {"result": "Requested", "preset": level}
    )
    assert monitor.main(["boost", "1"]) == 0
    assert "Requested" in capsys.readouterr().out
    assert monitor.main(["boost", "0", "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["preset"] == 0

    def denied(level):
        raise PermissionError("root required")

    monkeypatch.setattr(monitor, "request_boost", denied)
    assert monitor.main(["boost", "1"]) == 1
    assert "Boost failed" in capsys.readouterr().err
    for arguments in (["boost"], ["sensors", "1"], ["boost", "3"]):
        with pytest.raises(SystemExit) as error:
            monitor.main(arguments)
        assert error.value.code == 2
