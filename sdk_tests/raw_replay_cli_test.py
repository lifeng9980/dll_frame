"""Generate a RAW replay fixture or exercise an SDK CLI in an isolated directory."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write-fixture", type=Path, help="write RAW frames for the offline ROI recipe")
    mode.add_argument("--capture-accept", type=Path)
    mode.add_argument("--recipe-cli", type=Path)
    parser.add_argument("--fixture-parent", type=Path)
    args = parser.parse_args()
    if args.write_fixture is not None:
        write_raw_replay_fixture(args.write_fixture, width=160, height=128)
        return
    with TemporaryDirectory(prefix="raw-cli-", dir=args.fixture_parent) as tmp:
        work = Path(tmp)
        if args.capture_accept is not None:
            verify_capture_soak(args.capture_accept.resolve(), work)
        else:
            verify_offline_recipe(args.recipe_cli.resolve(), work)


if __name__ == "__main__":
    main()
