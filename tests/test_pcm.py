"""Check the streaming C meter against known signals and malformed PCM."""

import json
import math
import struct
import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="module")
def meter(tmp_path_factory):
    output = tmp_path_factory.mktemp("meter") / "pcm-rms"
    source = Path(__file__).resolve().parents[1] / "tools/pcm_rms.c"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(source),
            "-lm",
            "-o",
            str(output),
        ],
        check=True,
    )
    return output


def test_independent_channels_and_dc_rejection(meter):
    pcm = b"".join(
        struct.pack("<hh", sign * 16384, sign * 8192 + 100) for sign in [-1, 1] * 100
    )
    result = subprocess.run([meter], input=pcm, capture_output=True, check=True)
    data = json.loads(result.stdout)
    assert data["frames"] == 200
    for channel, amplitude in zip(data["channels"], [16384, 8192], strict=True):
        assert channel["rms_dbfs"] == pytest.approx(
            20 * math.log10(amplitude / 32768), abs=0.0001
        )
    assert data["channels"][1]["dc_mean"] == 100


def test_silence_clipping_and_bad_frames(meter):
    for pcm in (b"", b"123", b"12345"):
        assert subprocess.run([meter], input=pcm, capture_output=True).returncode == 1
    result = subprocess.run(
        [meter],
        input=struct.pack("<hh", -32768, 32767) * 4,
        capture_output=True,
        check=True,
    )
    assert json.loads(result.stdout)["channels"][0]["clipped_samples"] == 4
    result = subprocess.run([meter], input=bytes(400), capture_output=True, check=True)
    assert json.loads(result.stdout)["channels"][0]["rms_dbfs"] < -200
