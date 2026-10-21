"""Keep the U820 quiet with temperature feedback and a kernel cooling lease."""

from __future__ import annotations

import argparse
import fcntl
import json
import math
import signal
import sys
import threading
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from types import FrameType

from u820_monitor import check_u820, optional_text

MIN_DUTY = 46
MAX_DUTY = 127


@dataclass(frozen=True)
class Profile:
    """Define a bounded curve in Celsius and EC raw duty units."""

    curve: tuple[tuple[float, int], ...] = (
        (45, 46),
        (60, 46),
        (70, 56),
        (80, 68),
        (85, 68),
    )
    hysteresis_c: float = 3.0
    down_delay_s: float = 8.0
    smoothing_s: float = 3.0
    rise_per_s: float = 12.0
    fall_per_s: float = 3.0
    emergency_c: float = 88.0
    performance_delay_s: float = 30.0
    sustained_duty: int = 95
    interval_s: float = 0.5

    def __post_init__(self) -> None:
        if len(self.curve) < 2:
            raise ValueError("Curve requires at least two points")
        previous_temp, previous_duty = -math.inf, MIN_DUTY
        for temperature, duty in self.curve:
            if not math.isfinite(temperature) or not 20 <= temperature <= 90:
                raise ValueError("Curve temperatures must be within 20–90 C")
            if temperature <= previous_temp or type(duty) is not int:
                raise ValueError(
                    "Curve temperatures must increase; duties are integers"
                )
            if not previous_duty <= duty <= MAX_DUTY:
                raise ValueError("Curve duty must be nondecreasing within 46–127")
            previous_temp, previous_duty = temperature, duty
        bounds = {
            "hysteresis_c": (1, 5),
            "down_delay_s": (5, 120),
            "smoothing_s": (0.5, 5),
            "rise_per_s": (4, 100),
            "fall_per_s": (0.25, 3),
            "emergency_c": (75, 88),
            "performance_delay_s": (5, 120),
            "interval_s": (0.2, 1),
        }
        for name, (low, high) in bounds.items():
            value = getattr(self, name)
            if not math.isfinite(value) or not low <= value <= high:
                raise ValueError(f"{name} must be within {low}–{high}")
        if type(self.sustained_duty) is not int or not 68 <= self.sustained_duty <= 127:
            raise ValueError("Sustained duty must be an integer within 68–127")

    def demand(self, temperature: float) -> float:
        """Interpolate a monotonic fan curve, clamping at its endpoints."""
        if temperature <= self.curve[0][0]:
            return float(self.curve[0][1])
        for (left, low), (right, high) in zip(self.curve, self.curve[1:], strict=False):
            if temperature <= right:
                return low + (temperature - left) * (high - low) / (right - left)
        return float(self.curve[-1][1])


def load_profile(path: Path | None) -> Profile:
    """Load an optional JSON profile, rejecting unknown or unsafe settings."""
    if path is None:
        return Profile()
    data = json.loads(path.read_text())
    if not isinstance(data, dict):
        raise ValueError("Profile must be a JSON object")
    try:
        if "curve" in data:
            data["curve"] = tuple(tuple(point) for point in data["curve"])
        return Profile(**data)
    except (TypeError, KeyError) as error:
        raise ValueError(f"Invalid profile: {error}") from error


class Controller:
    """Filter brief heat spikes, increase promptly, and delay fan slowdowns."""

    def __init__(self, profile: Profile) -> None:
        self.profile = profile
        self.filtered: float | None = None
        self.previous_time: float | None = None
        self.cool_since: float | None = None
        self.hot_since: float | None = None
        self.duty = float(MIN_DUTY)
        self.reason = "starting"

    def update(self, temperature: float, now: float) -> int:
        """Compute the next duty from the hottest real CPU temperature."""
        if not math.isfinite(temperature) or not 0 <= temperature <= 105:
            raise ValueError("Invalid CPU temperature")
        if not math.isfinite(now):
            raise ValueError("Invalid monotonic time")
        elapsed = self.profile.interval_s
        if self.previous_time is not None:
            elapsed = now - self.previous_time
            if elapsed <= 0 or elapsed > 2.5:
                raise ValueError("Control loop missed its safe heartbeat window")
        self.previous_time = now
        alpha = 1 - math.exp(-elapsed / self.profile.smoothing_s)
        self.filtered = (
            temperature
            if self.filtered is None
            else (self.filtered + alpha * (temperature - self.filtered))
        )
        # Bound smoothing lag when Turbo produces a rapid temperature rise.
        effective = max(self.filtered, temperature - 8)
        demand = self.profile.demand(effective)
        if temperature < 77:
            self.hot_since = None
        elif temperature >= 80 and self.hot_since is None:
            self.hot_since = now
        sustained = self.hot_since is not None and (
            now - self.hot_since >= self.profile.performance_delay_s
        )
        if temperature >= self.profile.emergency_c:
            self.duty = float(MAX_DUTY)
            self.cool_since = None
            self.reason = "thermal override"
        elif sustained:
            self.duty = max(self.duty, self.profile.sustained_duty, demand)
            self.cool_since = None
            self.reason = "sustained thermal load"
        elif temperature >= 80:
            self.duty = max(self.duty, self.profile.demand(temperature), demand)
            self.cool_since = None
            self.reason = "thermal headroom"
        elif demand > self.duty:
            self.duty = min(demand, self.duty + self.profile.rise_per_s * elapsed)
            self.cool_since = None
            self.reason = "temperature rising"
        elif self.profile.demand(effective + self.profile.hysteresis_c) < self.duty:
            if self.cool_since is None:
                self.cool_since = now
            if now - self.cool_since >= self.profile.down_delay_s:
                self.duty = max(demand, self.duty - self.profile.fall_per_s * elapsed)
                self.reason = "cooling down"
            else:
                self.reason = "cooldown hold"
        else:
            self.cool_since = None
            self.reason = "hysteresis hold"
        return max(MIN_DUTY, min(MAX_DUTY, math.ceil(self.duty)))


@dataclass(frozen=True)
class Sample:
    """Represent independent CPU sensor and fan feedback readings."""

    temperature_c: float
    rpm: int
    lease_duty: int


class Hardware:
    """Use only validated sysfs attributes; leave EC writes to the guarded driver."""

    def __init__(self, sysfs: Path = Path("/sys")) -> None:
        check_u820(sysfs)
        devices = list((sysfs / "class/hwmon").glob("hwmon*"))
        fans = [p for p in devices if optional_text(p / "name", "") == "u820_ec_probe"]
        cpus = [p for p in devices if optional_text(p / "name", "") == "coretemp"]
        if len(fans) != 1 or len(cpus) != 1:
            raise ValueError("Exactly one U820 driver and coretemp device are required")
        self.fan = fans[0]
        self.control = self.fan / "fan_pwm_raw"
        self.temperatures = tuple(sorted(cpus[0].glob("temp*_input")))
        if not self.control.exists() or len(self.temperatures) < 5:
            raise ValueError(
                "Manual lease interface and all five CPU sensors are required"
            )

    def sample(self) -> Sample:
        """Read every CPU sensor and reject missing, malformed, or invalid feedback."""
        temperatures = [int(path.read_text()) / 1000 for path in self.temperatures]
        if any(not 0 <= value <= 105 for value in temperatures):
            raise ValueError("CPU sensor outside its credible range")
        rpm = int((self.fan / "fan1_input").read_text())
        if not 500 <= rpm < 7000:
            raise ValueError(f"Fan feedback invalid: {rpm} RPM")
        duty = int(self.control.read_text())
        if duty != 0 and not MIN_DUTY <= duty <= MAX_DUTY:
            raise ValueError("Invalid driver lease state")
        return Sample(max(temperatures), rpm, duty)

    def set_duty(self, duty: int) -> None:
        """Renew the three-second lease or release it with zero."""
        if type(duty) is not int or (duty != 0 and not MIN_DUTY <= duty <= MAX_DUTY):
            raise ValueError("Duty must be zero (automatic) or 46–127")
        self.control.write_text(f"{duty}\n")


def write_state(path: Path, state: dict[str, object]) -> None:
    """Atomically publish the latest control status for local monitoring."""
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    temporary.replace(path)


def run(
    hardware: Hardware,
    profile: Profile,
    stop: threading.Event,
    state_path: Path,
    duration: float | None = None,
) -> None:
    """Run the feedback loop and restore firmware cooling on every exit."""
    controller = Controller(profile)
    expected = 0
    started = time.monotonic()
    last_log = -math.inf
    try:
        while not stop.is_set():
            now = time.monotonic()
            if duration is not None and now - started >= duration:
                break
            sample = hardware.sample()
            if sample.lease_duty != expected:
                raise ValueError(
                    "Cooling lease changed or expired; refusing to reacquire"
                )
            duty = controller.update(sample.temperature_c, now)
            hardware.set_duty(duty)
            state: dict[str, object] = {
                "timestamp": time.time(),
                "mode": "manual lease",
                **asdict(sample),
                "duty_raw": duty,
                "filtered_c": round(controller.filtered or 0, 2),
                "reason": controller.reason,
            }
            write_state(state_path, state)
            if duty != expected or now - last_log >= 15:
                print(json.dumps(state), flush=True)
                last_log = now
            expected = duty
            stop.wait(profile.interval_s)
    finally:
        hardware.set_duty(0)
        write_state(
            state_path, {"timestamp": time.time(), "mode": "firmware automatic"}
        )
        print("Automatic cooling restored.", flush=True)


def main(argv: list[str] | None = None) -> int:
    """Run the daemon or check sensor access without starting manual cooling."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("run", "check"))
    parser.add_argument("--config", type=Path)
    parser.add_argument("--runtime", type=Path, default=Path("/run/u820-fan"))
    parser.add_argument(
        "--duration", type=float, help="Optional bounded run in seconds"
    )
    args = parser.parse_args(argv)
    if args.duration is not None and (
        not math.isfinite(args.duration) or args.duration <= 0
    ):
        parser.error("duration must be positive and finite")
    stop = threading.Event()

    def request_stop(signum: int, frame: FrameType | None) -> None:
        stop.set()

    try:
        profile = load_profile(args.config)
        hardware = Hardware()
        if args.command == "check":
            print(json.dumps(asdict(hardware.sample()), indent=2))
            return 0
        args.runtime.mkdir(parents=True, exist_ok=True)
        with (args.runtime / "lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for signum in (signal.SIGTERM, signal.SIGINT):
                signal.signal(signum, request_stop)
            print(
                "Starting U820 temperature control with a three-second kernel lease.",
                flush=True,
            )
            run(hardware, profile, stop, args.runtime / "state.json", args.duration)
    except (OSError, ValueError) as error:
        print(f"Fan control stopped: {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
