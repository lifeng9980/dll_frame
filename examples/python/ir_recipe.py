#!/usr/bin/env python3
"""Validate or run a JSON measurement recipe through the IR C ABI."""

import argparse
import ctypes as c
import json
import os
from pathlib import Path
import sys


class RecipeResult(c.Structure):
    _fields_ = [
        ("completed_frames", c.c_int32),
        ("failed_stage", c.c_int32),
        ("failed_step_index", c.c_int32),
        ("cleanup_status", c.c_int32),
    ]


def sdk_library_directory():
    configured = os.environ.get("IR_SDK_LIB_DIR")
    if configured:
        return Path(configured).resolve()
    package_root = Path(__file__).resolve().parents[2]
    packaged = package_root / ("windows-x64/bin" if os.name == "nt" else "linux-x86_64/lib")
    if packaged.is_dir():
        return packaged
    build_debug = package_root / "实验" / "build-debug" / ("bin" if os.name == "nt" else "lib")
    if build_debug.is_dir():
        return build_debug
    return packaged


def load_station(directory):
    if os.name == "nt":
        dll_directory = os.add_dll_directory(str(directory))
        try:
            _analysis = c.CDLL(str(directory / "IR_analysis.dll"))
            return (c.CDLL(str(directory / "IR_station.dll")), _analysis), dll_directory
        except Exception:
            dll_directory.close()
            raise
    _analysis = c.CDLL(str(directory / "libIR_analysis.so"))
    return (c.CDLL(str(directory / "libIR_station.so")), _analysis), None


def station_text(path):
    station = json.loads(path.read_text(encoding="utf-8-sig"))
    acquisition = station["acquisition"]
    if acquisition["backend"] == "raw_replay":
        source = Path(acquisition["source"])
        if not source.is_absolute():
            source = path.parent / source
        acquisition["source"] = str(source.resolve())
    plugins = station.get("plugins")
    if isinstance(plugins, list):
        resolved_plugins = []
        for entry in plugins:
            if isinstance(entry, str) and entry:
                p = Path(entry)
                if not p.is_absolute():
                    p = path.parent / p
                resolved_plugins.append(str(p.resolve()))
            else:
                resolved_plugins.append(entry)
        station["plugins"] = resolved_plugins
    return json.dumps(station, ensure_ascii=False, separators=(",", ":"))


def check_output_directory(path):
    if path.exists():
        raise ValueError(f"Output already exists: {path}")
    if not path.parent.is_dir():
        raise ValueError(f"Output parent directory does not exist: {path.parent}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", dest="mode", action="store_const", const="check")
    mode.add_argument("--run", dest="mode", action="store_const", const="run")
    parser.add_argument("station", type=Path)
    parser.add_argument("recipe", type=Path)
    parser.add_argument("output", type=Path, help="new output directory; resolved relative to the recipe")
    parser.add_argument("--library-dir", type=Path, default=sdk_library_directory())
    args = parser.parse_args()

    station_path = args.station.resolve()
    recipe_path = args.recipe.resolve()
    output = args.output if args.output.is_absolute() else recipe_path.parent / args.output
    output = output.resolve()
    check_output_directory(output)

    (sdk, _analysis), dll_directory = load_station(args.library_dir.resolve())
    sdk.IR_StationVersion.argtypes = []
    sdk.IR_StationVersion.restype = c.c_char_p
    station_version = sdk.IR_StationVersion()
    if not station_version or not station_version.decode("utf-8"):
        raise RuntimeError("IR_StationVersion returned an empty string")
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_CheckRecipeJson.argtypes = [c.c_char_p, c.c_char_p, *error_args]
    sdk.IR_RunRecipeJson.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_void_p, c.POINTER(RecipeResult), *error_args]
    station_json = station_text(station_path).encode("utf-8")
    recipe_json = recipe_path.read_text(encoding="utf-8-sig").encode("utf-8")
    error = c.create_string_buffer(2048)

    if args.mode == "check":
        status = sdk.IR_CheckRecipeJson(station_json, recipe_json, error, len(error))
        if status != 0:
            print(f"IR_CheckRecipeJson status={status}: {error.value.decode('utf-8', errors='replace')}", file=sys.stderr)
            if dll_directory:
                dll_directory.close()
            return status
        print("recipe=VALID")
        if dll_directory:
            dll_directory.close()
        return 0

    run = RecipeResult()
    status = sdk.IR_RunRecipeJson(station_json, recipe_json, str(output).encode("utf-8"), None, c.byref(run), error, len(error))
    if status != 0:
        print(f"IR_RunRecipeJson status={status} stage={run.failed_stage} step_index={run.failed_step_index} "
              f"completed_frames={run.completed_frames} cleanup_status={run.cleanup_status} "
              f"{error.value.decode('utf-8', errors='replace')}", file=sys.stderr)
        if dll_directory:
            dll_directory.close()
        return status

    result = json.loads((output / "result.json").read_text(encoding="utf-8"))
    print(f"run_status={result['run_status']}")
    print(f"completed_frames={result['completed_frames']}")
    for name, metric in result["metrics"].items():
        print(f"metric.{name}={metric['value']:.17g} {metric['unit']}")
    if dll_directory:
        dll_directory.close()
    return 20 if result["run_status"] == "NG" else 0


if __name__ == "__main__":
    raise SystemExit(main())
