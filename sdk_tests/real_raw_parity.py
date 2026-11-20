"""Validate one RAW recording against the installed Windows DLLs and NumPy."""
import argparse
import csv
import ctypes as c
import hashlib
import json
from pathlib import Path

import numpy as np


class Geometry(c.Structure):
    _fields_ = [("width", c.c_int32), ("height", c.c_int32), ("valid_bits", c.c_int32)]


class NoiseRecipe(c.Structure):
    _fields_ = [
        ("recipe_id", c.c_char_p), ("detector", c.c_uint64), ("blackbody", c.c_uint64),
        ("integration_us", c.c_int32), ("frame_hz", c.c_int32), ("empty_us", c.c_int32),
        ("temperature_c", c.c_double), ("tolerance_c", c.c_double),
        ("stable_window_ms", c.c_int32), ("stable_timeout_ms", c.c_int32),
        ("require_device_stable", c.c_int32), ("frames", c.c_int32), ("frame_timeout_ms", c.c_int32),
    ]


class Result(c.Structure):
    _fields_ = [("completed", c.c_int32), ("mean_dn", c.c_double), ("temporal_noise_dn", c.c_double)]


def open_api(directory):
    import os

    if os.name == "nt":
        with os.add_dll_directory(str(directory)):
            sdk = c.CDLL(str(directory / "IR_station.dll"))
    else:
        sdk = c.CDLL(str(directory / "libIR_station.so"))
    error_args = [c.c_char_p, c.c_int32]
    sdk.OptMeas_OpenFrames.argtypes = [c.c_int32, c.c_char_p, c.c_int32, c.POINTER(Geometry), *error_args]
    sdk.OptMeas_RunNoise.argtypes = [c.POINTER(NoiseRecipe), c.c_char_p, c.POINTER(Result), *error_args]
    sdk.OptMeas_CloseFrames.argtypes = error_args
    return sdk


def run(sdk, directory, frames, output=None):
    error = c.create_string_buffer(2048)

    def checked(function, *args):
        code = function(*args, error, len(error))
        if code:
            raise RuntimeError(f"{function.__name__}: {code}: {error.value.decode('utf8', 'replace')}")

    g, result = Geometry(), Result()
    checked(sdk.OptMeas_OpenFrames, 2, str(directory).encode(), 0, c.byref(g))
    try:
        recipe = NoiseRecipe(recipe_id=b"raw-parity", frames=frames)
        name = str(output).encode() if output else None
        checked(sdk.OptMeas_RunNoise, c.byref(recipe), name, c.byref(result))
        return (g.width, g.height, g.valid_bits), result
    finally:
        checked(sdk.OptMeas_CloseFrames)


def reference(path, frames, width, height, stride):
    count = width * height
    totals = np.zeros(count, dtype=np.float64)
    squares = np.zeros_like(totals)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for _ in range(frames):
            raw = stream.read(height * stride)
            pixels = np.ndarray((height, width), dtype="<u2", buffer=raw, strides=(stride, 2))
            digest.update(pixels.tobytes())
            values = pixels.ravel().astype(np.float64)
            totals += values
            squares += values * values
    mean = np.mean(totals / frames)
    sample_noise = np.mean(np.sqrt(np.maximum(0, (squares - totals**2 / frames) / (frames - 1))))
    return mean, sample_noise, digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk_bin", type=Path)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, help="New output directory")
    args = parser.parse_args()
    with (args.input / "frames.csv").open(encoding="utf-8-sig", newline="") as index:
        rows = list(csv.DictReader(index))
    first = rows[0]
    if first["endianness"] != "little":
        raise ValueError("This parity check expects RAW16 little endian")
    frames = len(rows)
    width, height, stride, bits = (int(first[key]) for key in ("width", "height", "stride_bytes", "bit_depth"))
    sdk = open_api(args.sdk_bin.resolve())
    g, result = run(sdk, args.input, frames, args.output)
    g_replay, replay = run(sdk, args.output, frames)
    assert g == g_replay == (width, height, bits)
    assert result.completed == replay.completed == frames
    assert result.mean_dn == replay.mean_dn and result.temporal_noise_dn == replay.temporal_noise_dn

    checksum = args.input / "frames.sha256"
    saved_raw = args.output / "frames.raw"
    if checksum.is_file() and stride == width * 2:
        # The recorded digest verifies all bytes before the saved data is used as the NumPy input.
        expected_hash = checksum.read_text(encoding="utf-8-sig").split()[0].lower()
        mean, noise, saved_hash = reference(saved_raw, frames, width, height, width * 2)
        assert saved_hash == expected_hash
    else:
        mean, noise, expected_hash = reference(args.input / "frames.raw", frames, width, height, stride)
        with saved_raw.open("rb") as raw:
            assert hashlib.file_digest(raw, "sha256").hexdigest() == expected_hash
    np.testing.assert_allclose([result.mean_dn, result.temporal_noise_dn], [mean, noise], rtol=0, atol=1e-8)
    print(json.dumps({"size": [width, height], "frames": frames, "bits": bits, "mean_dn": result.mean_dn,
                      "noise_dn": result.temporal_noise_dn, "numpy_noise_dn": noise,
                      "noise_delta_dn": result.temporal_noise_dn - noise, "raw_sha256": expected_hash}), flush=True)


if __name__ == "__main__":
    main()
