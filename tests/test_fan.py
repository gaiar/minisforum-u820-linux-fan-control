"""Validate the thermal policy, feedback failures, and daemon restoration paths."""

import json
from dataclasses import replace

import pytest
from test_monitor import identity, put

import u820_fan as fan


@pytest.mark.parametrize(
    "field,value",
    [
        ("hysteresis_c", 0),
        ("down_delay_s", 500),
        ("smoothing_s", float("nan")),
        ("rise_per_s", 0),
        ("fall_per_s", -1),
        ("emergency_c", 99),
        ("interval_s", 3),
        ("curve", ()),
        ("curve", ((45, 46), (45, 56))),
        ("curve", ((10, 46), (80, 95))),
        ("curve", ((45, 45), (80, 95))),
        ("curve", ((45, 56), (80, 46))),
        ("curve", ((45, 46), (80, 128))),
        ("curve", ((45, 46.5), (80, 95))),
    ],
)
def test_reject_unsafe_profile(field, value):
    with pytest.raises(ValueError):
        replace(fan.Profile(), **{field: value})


def test_profile_load_and_interpolation(tmp_path):
    profile = fan.load_profile(None)
    assert profile.demand(20) == 46
    assert profile.demand(90) == 68
    assert profile.demand(65) == 51
    path = put(tmp_path, "profile.json", '{"curve": [[45,46],[80,95]]}')
    assert fan.load_profile(path).curve == ((45, 46), (80, 95))
    for text in ("[]", '{"typo": 1}', '{"interval_s": "wrong"}'):
        path.write_text(text)
        with pytest.raises(ValueError):
            fan.load_profile(path)


def test_turbo_spike_and_sustained_heat():
    controller = fan.Controller(fan.Profile(down_delay_s=20, fall_per_s=1))
    assert controller.update(40, 0) == 46
    assert controller.update(70, 0.5) <= 52
    assert controller.update(40, 1) <= 52
    # Independent raw sensor bypasses smoothing at high temperatures.
    assert controller.update(80, 1.5) >= 68
    assert controller.update(88, 2) == 127
    assert controller.reason == "thermal override"
    for step in range(1, 42):
        assert controller.update(40, 2 + step * 0.5) == 127
    for step in range(42, 300):
        result = controller.update(40, 2 + step * 0.5)
    assert result == 46
    assert controller.reason == "hysteresis hold"
    assert controller.update(70, 152) > 46
    assert controller.reason == "temperature rising"


def test_hysteresis_resets_cooldown():
    controller = fan.Controller(fan.Profile())
    controller.update(88, 0)
    controller.update(40, 1)
    assert controller.reason == "cooldown hold"
    controller.update(88, 2)
    assert controller.cool_since is None
    assert controller.update(40, 3) == 127


@pytest.mark.parametrize(
    "temperature,now", [(float("nan"), 0), (-1, 0), (106, 0), (40, float("nan"))]
)
def test_invalid_temperature_or_clock(temperature, now):
    with pytest.raises(ValueError):
        fan.Controller(fan.Profile()).update(temperature, now)


@pytest.mark.parametrize("second", [0, -1, 3])
def test_missed_heartbeat(second):
    controller = fan.Controller(fan.Profile())
    controller.update(40, 0)
    with pytest.raises(ValueError, match="heartbeat"):
        controller.update(40, second)


@pytest.fixture
def hardware(tmp_path):
    identity(tmp_path)
    put(tmp_path, "class/hwmon/hwmon0/name", "coretemp")
    for number in range(1, 6):
        put(
            tmp_path,
            f"class/hwmon/hwmon0/temp{number}_input",
            str(40000 + 1000 * number),
        )
    put(tmp_path, "class/hwmon/hwmon1/name", "u820_ec_probe")
    put(tmp_path, "class/hwmon/hwmon1/fan1_input", "1540")
    put(tmp_path, "class/hwmon/hwmon1/fan_pwm_raw", "0")
    return fan.Hardware(tmp_path)


def test_hardware_uses_hottest_cpu_and_strict_writes(hardware):
    assert hardware.sample() == fan.Sample(45, 1540, 0)
    for value in (46, 127, 0):
        hardware.set_duty(value)
        assert hardware.sample().lease_duty == value
    for value in (-1, 45, 128, True, 46.5):
        with pytest.raises(ValueError):
            hardware.set_duty(value)
    hardware.temperatures[0].write_text("106000")
    with pytest.raises(ValueError, match="credible"):
        hardware.sample()
    hardware.temperatures[0].unlink()
    with pytest.raises(OSError):
        hardware.sample()


@pytest.mark.parametrize("rpm", [0, 499, 7000, 90000])
def test_bad_tachometer(hardware, rpm):
    (hardware.fan / "fan1_input").write_text(str(rpm))
    with pytest.raises(ValueError, match="feedback"):
        hardware.sample()


def test_missing_interface_and_bad_driver_state(tmp_path, hardware):
    hardware.control.write_text("200")
    with pytest.raises(ValueError, match="lease"):
        hardware.sample()
    hardware.control.unlink()
    with pytest.raises(ValueError, match="five CPU"):
        fan.Hardware(tmp_path)
    (hardware.fan / "name").write_text("other")
    with pytest.raises(ValueError, match="Exactly one"):
        fan.Hardware(tmp_path)


class ClockStop:
    def __init__(self, monkeypatch, after=2):
        self.now = 0
        self.after = after
        self.waits = 0
        monkeypatch.setattr(fan.time, "monotonic", lambda: self.now)

    def is_set(self):
        return self.waits >= self.after

    def wait(self, seconds):
        self.now += seconds
        self.waits += 1


def test_run_restores_on_normal_stop(hardware, tmp_path, monkeypatch, capsys):
    stop = ClockStop(monkeypatch)
    target = tmp_path / "state.json"
    fan.run(hardware, fan.Profile(), stop, target)
    assert hardware.control.read_text() == "0\n"
    assert json.loads(target.read_text())["mode"] == "firmware automatic"
    assert "Automatic cooling restored" in capsys.readouterr().out


def test_run_restores_on_duration(hardware, tmp_path, monkeypatch):
    stop = ClockStop(monkeypatch, after=100)
    fan.run(hardware, fan.Profile(), stop, tmp_path / "state.json", duration=1)
    assert stop.waits == 2
    assert hardware.control.read_text() == "0\n"


def test_run_stops_when_kernel_lease_lost(hardware, tmp_path, monkeypatch):
    stop = ClockStop(monkeypatch)
    original = hardware.sample
    calls = 0

    def sample():
        nonlocal calls
        calls += 1
        if calls == 2:
            hardware.control.write_text("0")
        return original()

    monkeypatch.setattr(hardware, "sample", sample)
    with pytest.raises(ValueError, match="expired"):
        fan.run(hardware, fan.Profile(), stop, tmp_path / "state.json")
    assert hardware.control.read_text() == "0\n"


def test_run_restores_when_status_write_fails(hardware, tmp_path, monkeypatch):
    stop = ClockStop(monkeypatch)
    with pytest.raises(OSError):
        fan.run(hardware, fan.Profile(), stop, tmp_path / "missing" / "state.json")
    assert hardware.control.read_text() == "0\n"


def test_main_check_run_and_error(hardware, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(fan, "Hardware", lambda: hardware)
    assert fan.main(["check"]) == 0
    assert "1540" in capsys.readouterr().out
    observed = []
    handlers = []
    monkeypatch.setattr(
        fan.signal, "signal", lambda number, handler: handlers.append(handler)
    )

    def run(hw, profile, stop, state, duration):
        observed.append((hw, duration))
        handlers[0](15, None)
        assert stop.is_set()

    monkeypatch.setattr(fan, "run", run)
    assert fan.main(["run", "--runtime", str(tmp_path), "--duration", "1"]) == 0
    assert observed == [(hardware, 1)]
    hardware.control.write_text("200")
    assert fan.main(["check"]) == 1
    assert "stopped" in capsys.readouterr().err
    with pytest.raises(SystemExit):
        fan.main(["run", "--duration", "-1"])


def test_single_instance_lock(hardware, tmp_path, monkeypatch):
    monkeypatch.setattr(fan, "Hardware", lambda: hardware)
    with (tmp_path / "lock").open("w") as held:
        fan.fcntl.flock(held, fan.fcntl.LOCK_EX | fan.fcntl.LOCK_NB)
        assert fan.main(["run", "--runtime", str(tmp_path)]) == 1
    assert hardware.control.read_text() == "0"


@pytest.mark.parametrize(
    "text", ['{"curve": null}', '{"curve": [[45, "bad"], [80, 95]]}']
)
def test_bad_curve_shape(tmp_path, text):
    path = put(tmp_path, "invalid.json", text)
    with pytest.raises(ValueError):
        fan.load_profile(path)


def test_sustained_heat_raises_cooling_after_delay():
    controller = fan.Controller(fan.Profile())
    for second in range(30):
        assert controller.update(85, second) == 68
    assert controller.update(85, 30) == 95
    assert controller.reason == "sustained thermal load"
    assert controller.update(79, 31) == 95
    controller.update(76, 32)
    assert controller.hot_since is None
    assert controller.update(85, 33) == 95
    assert controller.reason == "thermal headroom"


@pytest.mark.parametrize("value", [67, 128, 95.5, True])
def test_invalid_sustained_duty(value):
    with pytest.raises(ValueError):
        fan.Profile(sustained_duty=value)
