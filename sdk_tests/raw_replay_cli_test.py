"""Generate a RAW replay fixture or exercise an SDK CLI in an isolated directory."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import struct
import sys
import time
from tempfile import TemporaryDirectory

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "simulators"))

from ir_virtual_station import VirtualStationState, write_dynamic_raw16  # noqa: E402


def write_raw_replay_fixture(directory, *, width=4, height=4):
    state = VirtualStationState()
    state.power.output[1] = True
    write_dynamic_raw16(directory, state, frames=2, width=width, height=height)


def run_cli(executable, *args, cwd):
    return subprocess.run(
        [str(executable), *args], cwd=cwd, capture_output=True, encoding="utf-8", timeout=20,
    )


def expect_exit(result, expected):
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)


def verify_capture_soak(cli, work):
    raw = work / "raw"
    report_path = work / "capture.json"
    write_raw_replay_fixture(raw)
    result = run_cli(cli, "--backend", "raw_replay", "--source", str(raw),
                     "--frames", "2", "--cycles", "3", "--preview-ms", "5",
                     "--report", str(report_path), cwd=work)
    expect_exit(result, 0)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["cycles_completed"] == 3 and report["total_frames"] == 6, report
    assert report["geometry"]["width"] == report["geometry"]["height"] == 4, report
    assert report["preview"]["errors"] == 0, report
    assert report["capture_stats"]["frames_delivered"] == 6, report
    assert report["capture_stats"]["frames_dropped"] == 0, report
    assert math.isclose(report["last_cycle_metrics"]["pop_std_dn"], 2.0), report


def verify_drift_example(script, library_dir, work):
    def run(*args):
        return subprocess.run(
            [sys.executable, "-B", str(script), "--mode", "self-test",
             "--library-dir", str(library_dir), "--interval-sec", "0", "--max-bursts", "3", *args],
            cwd=work, capture_output=True, encoding="utf-8", timeout=30,
            env={**os.environ, "PYTHONIOENCODING": "utf-8",
                 "TMPDIR": str(work), "TEMP": str(work), "TMP": str(work)},
        )

    first = run()
    expect_exit(first, 0)
    first_meta = Path(next(line.split("=", 1)[1] for line in first.stdout.splitlines()
                           if line.startswith("meta_path=")))
    metadata = json.loads(first_meta.read_text(encoding="utf-8"))
    spectrum = metadata["drift_series_fft"]
    assert spectrum["frequency_unit"] == "cycles/burst", spectrum
    assert math.isclose(spectrum["peak_cycles_per_burst"], 1 / 3), spectrum
    assert "peak_freq_hz" not in spectrum and "sample_rate_hz" not in spectrum

    second = run()
    expect_exit(second, 0)
    second_meta = Path(next(line.split("=", 1)[1] for line in second.stdout.splitlines()
                            if line.startswith("meta_path=")))
    assert second_meta != first_meta and second_meta.exists()

    output = work / "existing-drift-output"
    explicit = run("--output-dir", str(output))
    expect_exit(explicit, 0)
    csv_path = output / "drift_summary.csv"
    original_csv = csv_path.read_bytes()
    refused = run("--output-dir", str(output))
    assert refused.returncode != 0, refused.stdout
    assert csv_path.read_bytes() == original_csv


def verify_plugin_example(cli, plugin, work):
    raw = work / "plugin raw"
    write_raw_replay_fixture(raw, width=2, height=2)
    (raw / "frames.raw").write_bytes(struct.pack("<8H", *range(8)))
    station = {
        "schemav": 1,
        "acquisition": {"backend": "raw_replay", "source": str(raw), "valid_bits": 16},
        "devices": {},
        "plugins": [str(plugin)],
    }
    recipe = cli.parents[2] / "examples/recipes/plugin_mean_dn.json"
    station_path = work / "plugin-station.json"
    station_path.write_text(json.dumps(station), encoding="utf-8")

    check = run_cli(cli, "--check", "--station", str(station_path),
                    "--recipe", str(recipe), cwd=work)
    expect_exit(check, 0)
    assert check.stdout.strip() == "recipe=VALID", check.stdout

    output = work / "plugin output"
    result = run_cli(cli, "--run", "--station", str(station_path),
                     "--recipe", str(recipe), "--output", str(output), cwd=work)
    expect_exit(result, 0)
    record = json.loads((output / "result.json").read_text(encoding="utf-8"))
    metric = record["metrics"]["plugin_mean.mean_dn"]
    assert math.isclose(metric["value"], 3.5), metric
    assert metric["plugin_id"] == "example.mean_dn", metric


def verify_offline_recipe(cli, work):
    cwd = work / "运行 folder"
    inputs = work / "inputs"
    cwd.mkdir()
    inputs.mkdir()
    write_raw_replay_fixture(work / "raw frames", width=160, height=128)

    station = json.loads((REPO_ROOT / "examples/stations/offline.json").read_text(encoding="utf-8"))
    station["acquisition"]["source"] = "../raw frames"
    (inputs / "station.json").write_text(json.dumps(station), encoding="utf-8")
    recipe_text = (REPO_ROOT / "examples/recipes/noise_roi.json").read_text(encoding="utf-8")
    (inputs / "noise_roi.json").write_text(recipe_text, encoding="utf-8")
    station_args = ("--station", "../inputs/station.json")
    recipe_args = ("--recipe", "../inputs/noise_roi.json")

    check = run_cli(cli, "--check", *station_args, *recipe_args, cwd=cwd)
    expect_exit(check, 0)
    assert check.stdout.strip() == "recipe=VALID", check.stdout
    bad_check = run_cli(cli, "--check", *station_args, *recipe_args, "--output", "check", cwd=cwd)
    assert bad_check.returncode != 0, bad_check.stdout

    empty_output = run_cli(cli, "--run", *station_args, *recipe_args, "--output", "", cwd=cwd)
    expect_exit(empty_output, 2)
    assert "--run requires --output" in empty_output.stderr, empty_output.stderr
    empty_check = run_cli(cli, "--check", *station_args, *recipe_args, "--output", "", cwd=cwd)
    expect_exit(empty_check, 2)
    assert "--output is only used with --run" in empty_check.stderr, empty_check.stderr
    assert not any(cwd.iterdir())

    missing_parent = cwd / "missing-parent"
    bad_output = run_cli(cli, "--run", *station_args, *recipe_args,
                         "--output", "missing-parent/run", cwd=cwd)
    assert bad_output.returncode != 0 and not missing_parent.exists(), bad_output.stdout

    (cwd / "runs").mkdir()
    for run_status, exit_code in (("PASS", 0), ("NG", 20), ("ERROR", 11)):
        recipe = json.loads(recipe_text)
        if run_status == "NG":
            recipe["limits"][0]["value"] = 1.0
        elif run_status == "ERROR":
            recipe["steps"][0]["frames"] = 3
        recipe_filename = f"{run_status.lower()}.json"
        (inputs / recipe_filename).write_text(json.dumps(recipe), encoding="utf-8")
        output = f"runs/{run_status.lower()}"
        result = run_cli(cli, "--run", *station_args, "--recipe", f"../inputs/{recipe_filename}",
                         "--output", output, cwd=cwd)
        expect_exit(result, exit_code)
        assert f"run_status={run_status}" in result.stdout.splitlines(), result.stdout
        record = json.loads((cwd / output / "result.json").read_text(encoding="utf-8"))
        assert record["run_status"] == run_status, record

    burst_recipe = {
        "recipe_id": "burst-duration",
        "steps": [{
            "id": "loop",
            "type": "repeat_burst",
            "duration_sec": 0.1,
            "interval_sec": 2.0,
            "csv_file": "duration.csv",
            "record_metrics": ["mean.mean_dn"],
            "steps": [
                {"id": "capture", "type": "capture", "frames": 1,
                 "frame_timeout_ms": 0, "output": "batch"},
                {"id": "mean", "type": "analyze", "method": "roi_mean",
                 "name": "mean", "input": "batch",
                 "params": {"x": 0, "y": 0, "width": 1, "height": 1}},
            ],
        }],
    }
    burst_path = inputs / "burst.json"
    burst_path.write_text(json.dumps(burst_recipe), encoding="utf-8")
    output = cwd / "runs" / "burst-duration"
    started = time.monotonic()
    result = run_cli(cli, "--run", *station_args, "--recipe", "../inputs/burst.json",
                     "--output", "runs/burst-duration", cwd=cwd)
    expect_exit(result, 0)
    assert time.monotonic() - started < 1.0
    record = json.loads((output / "result.json").read_text(encoding="utf-8"))
    assert record["burst_runs"] == [{"id": "loop", "csv_file": "duration.csv", "iterations": 1}]

    failure_recipe = json.loads(json.dumps(burst_recipe))
    failure_recipe["steps"][0].pop("duration_sec")
    failure_recipe["steps"][0]["max_iterations"] = 5
    failure_recipe["steps"][0]["interval_sec"] = 0
    failure_recipe["steps"][0]["csv_file"] = "partial.csv"
    (inputs / "burst-failure.json").write_text(json.dumps(failure_recipe), encoding="utf-8")
    failure_output = cwd / "runs" / "burst-failure"
    failed = run_cli(cli, "--run", *station_args, "--recipe", "../inputs/burst-failure.json",
                     "--output", "runs/burst-failure", cwd=cwd)
    expect_exit(failed, 11)
    failure_record = json.loads((failure_output / "result.json").read_text(encoding="utf-8"))
    assert failure_record["run_status"] == "ERROR", failure_record
    assert failure_record["burst_runs"] == [{"id": "loop", "csv_file": "partial.csv", "iterations": 2}]
    assert len((failure_output / "partial.csv").read_text(encoding="utf-8").splitlines()) == 3

    no_rows_recipe = json.loads(json.dumps(burst_recipe))
    no_rows_recipe["steps"][0]["steps"][0]["frames"] = 3
    no_rows_recipe["steps"][0]["max_iterations"] = 1
    no_rows_recipe["steps"][0]["interval_sec"] = 0
    no_rows_recipe["steps"][0]["csv_file"] = "not-created.csv"
    (inputs / "burst-no-rows.json").write_text(json.dumps(no_rows_recipe), encoding="utf-8")
    no_rows_output = cwd / "runs" / "burst-no-rows"
    no_rows = run_cli(cli, "--run", *station_args, "--recipe", "../inputs/burst-no-rows.json",
                      "--output", "runs/burst-no-rows", cwd=cwd)
    expect_exit(no_rows, 11)
    no_rows_record = json.loads((no_rows_output / "result.json").read_text(encoding="utf-8"))
    assert no_rows_record["burst_runs"] == [], no_rows_record
    assert not (no_rows_output / "not-created.csv").exists()

    multiple_recipe = json.loads(json.dumps(burst_recipe))
    second_burst = json.loads(json.dumps(multiple_recipe["steps"][0]))
    multiple_recipe["steps"][0]["id"] = "loop-a"
    multiple_recipe["steps"][0]["steps"][0]["id"] = "capture-a"
    multiple_recipe["steps"][0]["steps"][1]["id"] = "mean-a"
    multiple_recipe["steps"][0]["steps"][1]["name"] = "mean-a"
    multiple_recipe["steps"][0]["record_metrics"] = ["mean-a.mean_dn"]
    multiple_recipe["steps"][0]["csv_file"] = "first.csv"
    second_burst["id"] = "loop-b"
    second_burst["steps"][0]["id"] = "capture-b"
    second_burst["steps"][1]["id"] = "mean-b"
    second_burst["steps"][1]["name"] = "mean-b"
    second_burst["record_metrics"] = ["mean-b.mean_dn"]
    second_burst["csv_file"] = "second.csv"
    multiple_recipe["steps"].append(second_burst)
    (inputs / "multiple-bursts.json").write_text(json.dumps(multiple_recipe), encoding="utf-8")
    multiple_output = cwd / "runs" / "multiple-bursts"
    multiple = run_cli(cli, "--run", *station_args, "--recipe", "../inputs/multiple-bursts.json",
                       "--output", "runs/multiple-bursts", cwd=cwd)
    expect_exit(multiple, 0)
    multiple_record = json.loads((multiple_output / "result.json").read_text(encoding="utf-8"))
    assert multiple_record["burst_runs"] == [
        {"id": "loop-a", "csv_file": "first.csv", "iterations": 1},
        {"id": "loop-b", "csv_file": "second.csv", "iterations": 1},
    ], multiple_record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write-fixture", type=Path, help="write RAW frames for the offline ROI recipe")
    mode.add_argument("--capture-accept", type=Path)
    mode.add_argument("--recipe-cli", type=Path)
    mode.add_argument("--drift-example", type=Path)
    mode.add_argument("--plugin-example", type=Path, metavar="CLI",
                      help="exercise the customer plugin with this recipe CLI")
    parser.add_argument("--plugin-dll", type=Path)
    parser.add_argument("--library-dir", type=Path)
    parser.add_argument("--fixture-parent", type=Path)
    args = parser.parse_args()
    if args.write_fixture is not None:
        write_raw_replay_fixture(args.write_fixture, width=160, height=128)
        return
    with TemporaryDirectory(prefix="raw-cli-", dir=args.fixture_parent) as tmp:
        work = Path(tmp)
        if args.capture_accept is not None:
            verify_capture_soak(args.capture_accept.resolve(), work)
        elif args.drift_example is not None:
            verify_drift_example(args.drift_example.resolve(), args.library_dir.resolve(), work)
        elif args.plugin_example is not None:
            verify_plugin_example(args.plugin_example.resolve(), args.plugin_dll.resolve(), work)
        else:
            verify_offline_recipe(args.recipe_cli.resolve(), work)


if __name__ == "__main__":
    main()
