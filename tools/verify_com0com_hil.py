#!/usr/bin/env python3
"""Run simultaneous 6-pair com0com virtual serial HIL checks against the IR SDK."""

import sys

sys.dont_write_bytecode = True

import argparse
import ctypes as c
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import time
import urllib.request

REPO_ROOT = Path(__file__).resolve().parents[1]

IR_DCN1000 = 3
IR_IT6332A = 4
IR_OVEN_GWS_STEN = 5
IR_DMM6500 = 6
IR_DETECTOR_SERIAL = 7
IR_COOLER_MODBUS = 8

IR_DMM_MODE_DIODE = 4
IR_BUFFER_TOO_SMALL = 3
IR_IO_ERROR = 6


class BlackbodyReading(c.Structure):
    _fields_ = [
        ("actual_c", c.c_double),
        ("setpoint_c", c.c_double),
        ("stable", c.c_int32),
    ]


class PowerReading(c.Structure):
    _fields_ = [
        ("setpoint_v", c.c_double),
        ("current_limit_a", c.c_double),
        ("measured_v", c.c_double),
        ("measured_a", c.c_double),
        ("output_enabled", c.c_int32),
    ]


class PowerCurrentStableSpec(c.Structure):
    _fields_ = [
        ("expected_voltage_v", c.c_double),
        ("voltage_tolerance_v", c.c_double),
        ("max_current_a", c.c_double),
        ("startup_threshold_a", c.c_double),
        ("stable_threshold_a", c.c_double),
        ("max_variation_a", c.c_double),
        ("startup_required_samples", c.c_int32),
        ("startup_timeout_ms", c.c_int32),
        ("stable_required_samples", c.c_int32),
        ("sample_interval_ms", c.c_int32),
        ("timeout_ms", c.c_int32),
        ("reserved", c.c_int32),
    ]


class OvenReading(c.Structure):
    _fields_ = [
        ("actual_c", c.c_double),
        ("setpoint_c", c.c_double),
        ("mode", c.c_int32),
        ("alarm_active", c.c_int32),
    ]


class DmmReading(c.Structure):
    _fields_ = [
        ("value", c.c_double),
        ("mode", c.c_int32),
        ("valid", c.c_int32),
    ]


class DetectorCurrentReading(c.Structure):
    _fields_ = [
        ("raw_code", c.c_uint32),
        ("reserved", c.c_uint32),
        ("sense_voltage_v", c.c_double),
        ("current_ma", c.c_double),
    ]


class CoolerReading(c.Structure):
    _fields_ = [
        ("raw_value", c.c_uint32),
        ("valid", c.c_int32),
        ("voltage_mv", c.c_double),
    ]


def load_sdk(library: Path):
    library = library.resolve()
    if os.name == "nt":
        os.add_dll_directory(str(library.parent))
    sdk = c.CDLL(str(library))
    err_args = [c.c_char_p, c.c_int32]
    fns = [
        ("IR_ListSerialPorts", [c.c_char_p, c.c_int32, c.POINTER(c.c_int32), *err_args]),
        ("IR_OpenDevice", [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *err_args]),
        ("IR_CloseDevice", [c.c_uint64, *err_args]),
        ("IR_SetBlackbody", [c.c_uint64, c.c_double, c.POINTER(c.c_double), *err_args]),
        ("IR_WaitBlackbodyStable", [c.c_uint64, c.c_double, c.c_double, c.c_int32, c.c_int32, c.c_int32, c.POINTER(BlackbodyReading), *err_args]),
        ("IR_SetPowerChannel", [c.c_uint64, c.c_int32, c.c_double, c.c_double, c.POINTER(PowerReading), *err_args]),
        ("IR_SetPowerOutput", [c.c_uint64, c.c_int32, c.c_int32, c.POINTER(PowerReading), *err_args]),
        ("IR_WaitPowerCurrentStable", [c.c_uint64, c.c_int32, c.POINTER(PowerCurrentStableSpec), c.POINTER(PowerReading), *err_args]),
        ("IR_SetOven", [c.c_uint64, c.c_double, c.POINTER(c.c_double), *err_args]),
        ("IR_ReadOven", [c.c_uint64, c.POINTER(OvenReading), *err_args]),
        ("IR_StopOven", [c.c_uint64, *err_args]),
        ("IR_WaitOvenStable", [c.c_uint64, c.c_double, c.c_double, c.c_int32, c.c_int32, c.c_int32, c.POINTER(OvenReading), *err_args]),
        ("IR_SetDmmMode", [c.c_uint64, c.c_int32, *err_args]),
        ("IR_ReadDmm", [c.c_uint64, c.c_int32, c.POINTER(DmmReading), *err_args]),
        ("IR_MeasureDetectorCurrent", [c.c_uint64, c.c_int32, c.c_uint16, c.POINTER(DetectorCurrentReading), *err_args]),
        ("IR_ReadCooler", [c.c_uint64, c.POINTER(CoolerReading), *err_args]),
    ]
    for name, argtypes in fns:
        fn = getattr(sdk, name)
        fn.argtypes = argtypes
        fn.restype = c.c_int32
    return sdk


def check(fn, *args, expected=0):
    err = c.create_string_buffer(1024)
    st = fn(*args, err, len(err))
    assert st == expected, f"{fn.__name__}: expected {expected}, got {st} ({err.value.decode('utf-8', errors='replace')})"


def http_json(url: str, body=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        method="POST" if data is not None else "GET",
        headers={"Content-Type": "application/json"} if data is not None else {},
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def list_serial_ports(sdk) -> set[str]:
    err = c.create_string_buffer(512)
    req_bytes = c.c_int32(0)
    st = sdk.IR_ListSerialPorts(None, 0, c.byref(req_bytes), err, len(err))
    assert st == IR_BUFFER_TOO_SMALL and req_bytes.value >= 2, (st, req_bytes.value, err.value)
    buf = c.create_string_buffer(req_bytes.value)
    st = sdk.IR_ListSerialPorts(buf, len(buf), c.byref(req_bytes), err, len(err))
    assert st == 0, (st, err.value.decode("utf-8", errors="replace"))
    entries = json.loads(buf.value.decode("utf-8"))
    return {item["port"] for item in entries}


def verify_v2_result(result_path: Path):
    record = json.loads(result_path.read_text(encoding="utf-8"))
    assert record["run_status"] == "PASS", record
    assert math.isclose(record["metrics"]["dmm.value"]["value"], 1.080, abs_tol=1e-6), record["metrics"]
    assert math.isclose(record["metrics"]["cooler.voltage_mv"]["value"], 1080.0, abs_tol=1e-6), record["metrics"]
    assert math.isclose(record["metrics"]["det_curr.current_ma"]["value"], 15.0, abs_tol=1e-2), record["metrics"]
    assert math.isclose(record["metrics"]["stats.mean_dn"]["value"], 21.0, abs_tol=1e-9), record["metrics"]
    assert math.isclose(record["metrics"]["spatial_fft.dc_magnitude"]["value"], 21.0, abs_tol=1e-9), record["metrics"]
    assert math.isclose(record["metrics"]["spatial_fft.peak_ac_magnitude"]["value"], 0.0, abs_tol=1e-9), record["metrics"]
    wait_rows = [
        r for r in record.get("instrument_readings", [])
        if r.get("step") == "pwr_ch1_wait" and r.get("operation") == "power_wait_stable"
    ]
    assert len(wait_rows) == 1 and wait_rows[0]["reading"]["measured_a"] < 0.3, wait_rows


def assert_safe_station_state(snap: dict):
    assert snap["power"]["output"] == [False, False, False], snap["power"]
    assert snap["oven"]["mode"] == "STANDBY", snap["oven"]
    assert math.isclose(snap["blackbody"]["target_c"], 25.0, abs_tol=1e-6), snap["blackbody"]


def find_dotnet10(explicit: Path | None) -> Path | None:
    if explicit is not None:
        return explicit if explicit.is_file() else None
    candidates = []
    which = shutil.which("dotnet")
    if which and "Program Files (x86)" not in which:
        candidates.append(Path(which))
    candidates.extend([
        Path(r"C:\Program Files\dotnet\dotnet.exe"),
        Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "dotnet" / "dotnet.exe",
        Path.home() / ".dotnet" / "dotnet.exe",
    ])
    for cand in candidates:
        if cand.is_file():
            try:
                out = subprocess.check_output([str(cand), "--list-sdks"], text=True, stderr=subprocess.DEVNULL)
                for line in out.splitlines():
                    head = line.strip().split(".", 1)[0]
                    if head.isdigit() and int(head) >= 10:
                        return cand
            except Exception:
                continue
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--station-lib", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--serial-map", type=Path, default=REPO_ROOT / "实验" / "hil_serial_map.json")
    parser.add_argument("--csharp-project", type=Path, default=REPO_ROOT / "examples" / "csharp" / "IRSdkSample.csproj")
    parser.add_argument("--python-recipe", type=Path, default=REPO_ROOT / "examples" / "python" / "ir_recipe.py")
    parser.add_argument("--dotnet", type=Path, default=None)
    parser.add_argument("--work-parent", type=Path, default=None)
    args = parser.parse_args()

    station_lib = args.station_lib.resolve()
    cli_path = args.cli.resolve()
    serial_map_path = args.serial_map.resolve()
    recipe_path = (REPO_ROOT / "examples" / "recipes" / "virtual_station_hil_v2.json").resolve()
    raw_dir = (REPO_ROOT / "examples" / "data" / "raw16").resolve()
    serial_map = json.loads(serial_map_path.read_text(encoding="utf-8"))

    required_roles = ("power", "oven", "dmm", "detector", "cooler", "blackbody")
    for role in required_roles:
        assert role in serial_map and "client_port" in serial_map[role], (role, serial_map)

    assert c.sizeof(PowerCurrentStableSpec) == 72
    sdk = load_sdk(station_lib)
    detected_ports = list_serial_ports(sdk)
    expected_client_ports = {serial_map[role]["client_port"] for role in required_roles}
    missing_ports = sorted(expected_client_ports - detected_ports)
    assert not missing_ports, f"Missing SDK-side com0com ports: {missing_ports}; detected={sorted(detected_ports)}"

    with TemporaryDirectory(prefix="com0com-hil-", dir=args.work_parent) as tmp:
        work = Path(tmp)
        station_path = work / "station_6com.json"
        sim_cmd = [
            sys.executable, "-B",
            str(REPO_ROOT / "simulators" / "ir_virtual_station.py"),
            "--mode", "serial-map",
            "--config", str(serial_map_path),
            "--control-port", "0",
            "--emit-station-json", str(station_path),
            "--raw-source", str(raw_dir),
        ]
        proc = subprocess.Popen(
            sim_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        control_url = None
        try:
            endpoints = None
            while True:
                line = proc.stdout.readline()
                if not line:
                    err = proc.stderr.read()
                    raise RuntimeError(f"ir_virtual_station exited early (rc={proc.poll()}): {err}")
                if line.startswith("VIRTUAL_STATION_READY="):
                    endpoints = json.loads(line.split("=", 1)[1])
                    break
            control_url = endpoints["control_url"]

            # Phase A: Simultaneous 6-COM C ABI handles + coupled verification + 3 faults
            handles = []
            h_bb = c.c_uint64()
            h_pwr = c.c_uint64()
            h_oven = c.c_uint64()
            h_dmm = c.c_uint64()
            h_det = c.c_uint64()
            h_cool = c.c_uint64()
            t0 = time.perf_counter()
            try:
                open_specs = [
                    (IR_DCN1000, {"port": serial_map["blackbody"]["client_port"], "baud_rate": 9600, "timeout_ms": 1500}, h_bb),
                    (IR_IT6332A, {"port": serial_map["power"]["client_port"], "baud_rate": 115200, "timeout_ms": 1200}, h_pwr),
                    (IR_OVEN_GWS_STEN, {"port": serial_map["oven"]["client_port"], "baud_rate": 9600, "address": 1, "delimiter": "crlf", "timeout_ms": 1500}, h_oven),
                    (IR_DMM6500, {"port": serial_map["dmm"]["client_port"], "baud_rate": 115200, "timeout_ms": 1500}, h_dmm),
                    (IR_DETECTOR_SERIAL, {"port": serial_map["detector"]["client_port"], "baud_rate": 115200, "timeout_ms": 1200}, h_det),
                    (IR_COOLER_MODBUS, {"port": serial_map["cooler"]["client_port"], "baud_rate": 9600, "stop_bits": 2, "slave_address": 1, "register_address": 31314, "timeout_ms": 1500}, h_cool),
                ]
                for kind, cfg, h in open_specs:
                    check(sdk.IR_OpenDevice, kind, json.dumps(cfg).encode("utf-8"), c.byref(h))
                    handles.append(h)
                open_ms = (time.perf_counter() - t0) * 1000.0

                bb_read = BlackbodyReading()
                p_read = PowerReading()
                o_read = OvenReading()
                d_read = DmmReading()
                det_curr = DetectorCurrentReading()
                c_read = CoolerReading()

                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read))
                assert math.isclose(c_read.voltage_mv, 500.0, abs_tol=1e-6), c_read.voltage_mv
                check(sdk.IR_SetDmmMode, h_dmm, IR_DMM_MODE_DIODE)
                check(sdk.IR_ReadDmm, h_dmm, IR_DMM_MODE_DIODE, c.byref(d_read))
                assert math.isclose(d_read.value, 0.500, abs_tol=1e-6), d_read.value
                check(sdk.IR_MeasureDetectorCurrent, h_det, 0, 1, c.byref(det_curr))
                assert math.isclose(det_curr.current_ma, 0.0, abs_tol=1e-3), det_curr.current_ma

                check(sdk.IR_SetPowerChannel, h_pwr, 1, 12.0, 2.0, c.byref(p_read))
                check(sdk.IR_SetPowerOutput, h_pwr, 1, 1, c.byref(p_read))
                assert abs(p_read.measured_a - 0.55) < 1e-9, p_read.measured_a
                wait_spec = PowerCurrentStableSpec(
                    expected_voltage_v=12.0,
                    voltage_tolerance_v=0.2,
                    max_current_a=2.0,
                    startup_threshold_a=0.3,
                    stable_threshold_a=0.3,
                    max_variation_a=0.05,
                    startup_required_samples=3,
                    startup_timeout_ms=5000,
                    stable_required_samples=3,
                    sample_interval_ms=10,
                    timeout_ms=10000,
                    reserved=0,
                )
                check(sdk.IR_WaitPowerCurrentStable, h_pwr, 1, c.byref(wait_spec), c.byref(p_read))
                assert p_read.measured_a < 0.3 and math.isclose(p_read.measured_v, 12.0, abs_tol=1e-6)

                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read))
                assert math.isclose(c_read.voltage_mv, 1080.0, abs_tol=1e-6)
                check(sdk.IR_ReadDmm, h_dmm, IR_DMM_MODE_DIODE, c.byref(d_read))
                assert math.isclose(d_read.value, 1.080, abs_tol=1e-6)

                check(sdk.IR_SetPowerChannel, h_pwr, 2, 12.0, 1.5, c.byref(p_read))
                check(sdk.IR_SetPowerOutput, h_pwr, 2, 1, c.byref(p_read))
                check(sdk.IR_MeasureDetectorCurrent, h_det, 0, 1, c.byref(det_curr))
                assert math.isclose(det_curr.current_ma, 15.0, abs_tol=1e-2)

                bb_rb = c.c_double()
                check(sdk.IR_SetBlackbody, h_bb, 32.0, c.byref(bb_rb))
                check(sdk.IR_WaitBlackbodyStable, h_bb, 32.0, 0.2, 20, 2000, 1, c.byref(bb_read))

                ov_rb = c.c_double()
                check(sdk.IR_SetOven, h_oven, 30.0, c.byref(ov_rb))
                check(sdk.IR_WaitOvenStable, h_oven, 30.0, 0.5, 20, 2000, 1, c.byref(o_read))

                # Inject 3 faults + verify error codes + recovery
                http_json(f"{control_url}/fault", {"cooler_bad_crc": True})
                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read), expected=IR_IO_ERROR)
                http_json(f"{control_url}/fault", {"reset": True})
                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read))
                assert math.isclose(c_read.voltage_mv, 1080.0, abs_tol=1e-6)

                http_json(f"{control_url}/fault", {"detector_bad_ack": True})
                check(sdk.IR_MeasureDetectorCurrent, h_det, 0, 1, c.byref(det_curr), expected=IR_IO_ERROR)
                http_json(f"{control_url}/fault", {"reset": True})
                check(sdk.IR_MeasureDetectorCurrent, h_det, 0, 1, c.byref(det_curr))
                assert math.isclose(det_curr.current_ma, 15.0, abs_tol=1e-2)

                http_json(f"{control_url}/fault", {"oven_alarm": "OVERTEMP"})
                check(sdk.IR_ReadOven, h_oven, c.byref(o_read))
                assert o_read.alarm_active == 1
                check(sdk.IR_WaitOvenStable, h_oven, 30.0, 0.5, 20, 1000, 1, c.byref(o_read), expected=IR_IO_ERROR)
                http_json(f"{control_url}/fault", {"reset": True})
                check(sdk.IR_WaitOvenStable, h_oven, 30.0, 0.5, 20, 2000, 1, c.byref(o_read))
                check(sdk.IR_StopOven, h_oven)

                check(sdk.IR_SetBlackbody, h_bb, 25.0, c.byref(bb_rb))
                check(sdk.IR_SetPowerOutput, h_pwr, 2, 0, c.byref(p_read))
                check(sdk.IR_SetPowerOutput, h_pwr, 1, 0, c.byref(p_read))
            finally:
                for h in handles:
                    if h.value:
                        try:
                            check(sdk.IR_CloseDevice, h)
                        except Exception:
                            pass
            print(f"COM0COM_HIL_PHASE_A=PASS open_all_6_ms={open_ms:.1f}")

            # Phase B: CLI Schema v2 PASS + mid-recipe fault rollback
            cli_out = work / "cli-v2-out"
            rc = subprocess.run(
                [str(cli_path), "--check", "--station", str(station_path),
                 "--recipe", str(recipe_path), "--output", str(cli_out)],
                capture_output=True, text=True, check=False,
            )
            assert rc.returncode == 0 and rc.stdout.strip() == "recipe=VALID", (rc.returncode, rc.stdout, rc.stderr)
            t1 = time.perf_counter()
            rc = subprocess.run(
                [str(cli_path), "--run", "--station", str(station_path),
                 "--recipe", str(recipe_path), "--output", str(cli_out)],
                capture_output=True, text=True, check=False,
            )
            cli_ms = (time.perf_counter() - t1) * 1000.0
            assert rc.returncode == 0, (rc.returncode, rc.stdout, rc.stderr)
            verify_v2_result(cli_out / "result.json")
            assert_safe_station_state(http_json(f"{control_url}/state"))

            http_json(f"{control_url}/fault", {"cooler_bad_crc": True})
            cli_fail_out = work / "cli-v2-fail"
            rc_fail = subprocess.run(
                [str(cli_path), "--run", "--station", str(station_path),
                 "--recipe", str(recipe_path), "--output", str(cli_fail_out)],
                capture_output=True, text=True, check=False,
            )
            assert rc_fail.returncode == IR_IO_ERROR, (rc_fail.returncode, rc_fail.stdout, rc_fail.stderr)
            assert not cli_fail_out.exists(), "Failed recipe run must not leave output directory"
            assert_safe_station_state(http_json(f"{control_url}/state"))
            http_json(f"{control_url}/fault", {"reset": True})
            print(f"COM0COM_HIL_PHASE_B=PASS cli_v2_ms={cli_ms:.1f}")

            # Phase C: Python ir_recipe.py over 6 COM
            py_env = dict(os.environ, IR_SDK_LIB_DIR=str(station_lib.parent))
            py_out = work / "py-v2-out"
            rc_py = subprocess.run(
                [sys.executable, "-B", str(args.python_recipe.resolve()),
                 "--run", str(station_path), str(recipe_path), str(py_out)],
                env=py_env, capture_output=True, text=True, check=False,
            )
            assert rc_py.returncode == 0, (rc_py.returncode, rc_py.stdout, rc_py.stderr)
            verify_v2_result(py_out / "result.json")
            assert_safe_station_state(http_json(f"{control_url}/state"))
            print("COM0COM_HIL_PHASE_C=PASS")

            # Phase D: C# .NET 10 in isolated temp sandbox (never pollutes repo examples/csharp)
            dotnet = find_dotnet10(args.dotnet)
            if dotnet is None:
                print("COM0COM_HIL_PHASE_D=SKIP")
            else:
                cs_sandbox = work / "csharp_sandbox"
                shutil.copytree(
                    args.csharp_project.resolve().parent,
                    cs_sandbox,
                    ignore=shutil.ignore_patterns("bin", "obj"),
                )
                cs_proj = cs_sandbox / args.csharp_project.name
                cs_out = work / "cs-v2-out"
                cs_env = dict(
                    os.environ,
                    IR_SDK_LIB_DIR=str(station_lib.parent),
                    DOTNET_ROOT=str(dotnet.parent),
                    DOTNET_NOLOGO="1",
                    DOTNET_CLI_TELEMETRY_OPTOUT="1",
                    DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
                )
                rc_cs = subprocess.run(
                    [str(dotnet), "run", "--project", str(cs_proj),
                     "--", "--recipe", str(station_path), str(recipe_path), str(cs_out)],
                    env=cs_env, cwd=str(cs_sandbox),
                    capture_output=True, text=True, check=False,
                )
                assert rc_cs.returncode == 0, (rc_cs.returncode, rc_cs.stdout, rc_cs.stderr)
                verify_v2_result(cs_out / "result.json")
                assert_safe_station_state(http_json(f"{control_url}/state"))
                print("COM0COM_HIL_PHASE_D=PASS")

            print("COM0COM_HIL=PASS")
        finally:
            if control_url:
                try:
                    http_json(f"{control_url}/shutdown", {})
                except Exception:
                    pass
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


if __name__ == "__main__":
    main()
