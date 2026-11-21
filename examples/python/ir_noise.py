#!/usr/bin/env python3
"""Call the IR SDK C ABI from Python using only the standard library."""

import argparse
import ctypes as c
import os
from pathlib import Path
import sys


class Geometry(c.Structure):
    _fields_ = [("width", c.c_int32), ("height", c.c_int32), ("valid_bits", c.c_int32)]


class NoiseRecipe(c.Structure):
    _fields_ = [
        ("recipe_id", c.c_char_p), ("detector", c.c_uint64), ("blackbody", c.c_uint64),
        ("integration_us", c.c_int32), ("frame_hz", c.c_int32), ("empty_us", c.c_int32),
        ("temperature_c", c.c_double), ("tolerance_c", c.c_double),
        ("stable_window_ms", c.c_int32), ("stable_timeout_ms", c.c_int32),
        ("require_device_stable", c.c_int32), ("frames", c.c_int32),
        ("frame_timeout_ms", c.c_int32),
    ]


class NoiseResult(c.Structure):
    _fields_ = [
        ("completed", c.c_int32), ("mean_dn", c.c_double),
        ("temporal_noise_dn", c.c_double), ("failed_step", c.c_int32),
        ("cleanup_status", c.c_int32),
    ]


def sdk_library_directory() -> Path:
    configured = os.environ.get("IR_SDK_LIB_DIR")
    if configured:
        return Path(configured).resolve()
    package_root = Path(__file__).resolve().parents[2]
    return package_root / ("windows-x64/bin" if os.name == "nt" else "linux-x86_64/lib")


def load_station(directory: Path):
    if os.name == "nt":
        dll_path = directory / "IR_station.dll"
        dll_directory = os.add_dll_directory(str(directory))
        try:
            return c.CDLL(str(dll_path)), dll_directory
        except Exception:
            dll_directory.close()
            raise
    return c.CDLL(str(directory / "libIR_station.so")), None


def error_text(buffer) -> str:
    return buffer.value.decode("utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw16_directory", type=Path)
    parser.add_argument("--output", type=Path, help="new output directory for RAW, metadata and metrics")
    parser.add_argument("--library-dir", type=Path, default=sdk_library_directory())
    args = parser.parse_args()

    if c.sizeof(NoiseRecipe) != 80 or c.sizeof(NoiseResult) != 32:
        raise RuntimeError("The ctypes structures do not match the IR SDK x64 C ABI")

    station, dll_directory = load_station(args.library_dir.resolve())
    error = c.create_string_buffer(1024)
    error_args = [c.c_char_p, c.c_int32]
    station.IR_OpenFrames.argtypes = [c.c_int32, c.c_char_p, c.c_int32,
                                      c.POINTER(Geometry), *error_args]
    station.IR_RunNoise.argtypes = [c.POINTER(NoiseRecipe), c.c_char_p,
                                    c.POINTER(NoiseResult), *error_args]
    station.IR_CloseFrames.argtypes = error_args

    geometry = Geometry()
    source = os.fsencode(args.raw16_directory.resolve())
    status = station.IR_OpenFrames(2, source, 0, c.byref(geometry), error, len(error))
    if status != 0:
        print(f"IR_OpenFrames status={status}: {error_text(error)}", file=sys.stderr)
        return status

    recipe_id = b"python-offline-noise-example-1"
    recipe = NoiseRecipe(recipe_id=recipe_id, frames=2, frame_timeout_ms=0)
    result = NoiseResult()
    output = os.fsencode(args.output.resolve()) if args.output else None
    status = station.IR_RunNoise(c.byref(recipe), output, c.byref(result), error, len(error))
    print(f"width={geometry.width} height={geometry.height} valid_bits={geometry.valid_bits}")
    print(f"status={status} completed={result.completed} failed_step={result.failed_step} cleanup_status={result.cleanup_status}")
    if status == 0:
        print(f"mean_dn={result.mean_dn:.17g}")
        print(f"temporal_noise_dn={result.temporal_noise_dn:.17g}")
    else:
        print(f"IR_RunNoise: {error_text(error)}", file=sys.stderr)

    close_status = station.IR_CloseFrames(error, len(error))
    if status == 0 and close_status != 0:
        print(f"IR_CloseFrames status={close_status}: {error_text(error)}", file=sys.stderr)
        status = close_status
    if dll_directory is not None:
        dll_directory.close()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
