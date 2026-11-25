"""End-to-end hardware-in-the-loop test against simulators/ir_virtual_station.py."""
import sys

sys.dont_write_bytecode = True

import argparse
import ctypes as c
import json
import math
import os
from pathlib import Path
import socket
import subprocess
from tempfile import TemporaryDirectory
import urllib.request

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "simulators"))

from ir_virtual_station import build_host_blackbody_json, build_station_json  # noqa: E402

IR_PCN7LT = 2
IR_IT6332A = 4
IR_DMM6500 = 6
IR_DETECTOR_SERIAL = 7
IR_COOLER_MODBUS = 8
IR_DMM_MODE_DIODE = 4
IR_IO_ERROR = 6


class PowerReading(c.Structure):
    _fields_ = [
        ("setpoint_v", c.c_double),
        ("current_limit_a", c.c_double),
        ("measured_v", c.c_double),
        ("measured_a", c.c_double),
        ("output_enabled", c.c_int32),
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


def load_sdk(path: Path):
    if os.name == "nt":
        with os.add_dll_directory(str(path.parent)):
            sdk = c.CDLL(str(path))
    else:
        sdk = c.CDLL(str(path))
    err_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *err_args]
    sdk.IR_CloseDevice.argtypes = [c.c_uint64, *err_args]
    sdk.IR_SetPowerChannel.argtypes = [
        c.c_uint64, c.c_int32, c.c_double, c.c_double, c.POINTER(PowerReading), *err_args,
    ]
    sdk.IR_SetPowerOutput.argtypes = [
        c.c_uint64, c.c_int32, c.c_int32, c.POINTER(PowerReading), *err_args,
    ]
    sdk.IR_ReadPower.argtypes = [c.c_uint64, c.c_int32, c.POINTER(PowerReading), *err_args]
    sdk.IR_ReadDmm.argtypes = [c.c_uint64, c.c_int32, c.POINTER(DmmReading), *err_args]
    sdk.IR_MeasureDetectorCurrent.argtypes = [
        c.c_uint64, c.c_int32, c.c_uint16, c.POINTER(DetectorCurrentReading), *err_args,
    ]
    sdk.IR_ReadCooler.argtypes = [c.c_uint64, c.POINTER(CoolerReading), *err_args]
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


def query_doc058_fn116(host: str, port: int) -> str:
    with socket.create_connection((host, port), timeout=3) as s:
        s.sendall(b"\x1b0" + bytes([116]) + b"0\r")
        buf = b""
        while b"\r" not in buf:
            chunk = s.recv(1024)
            assert chunk, "EOF waiting for fn 116 reply"
            buf += chunk
        frame = buf.split(b"\r", 1)[0]
        assert frame[:2] == b"\x1b" + bytes([116]), frame
        return frame[2:].decode("ascii")


def verify_serial_map_config_builders():
    endpoints = {
        "blackbody": {"port": "COM21", "client_port": "COM20", "baud_rate": 9600, "stop_bits": 1},
        "power": {"port": "COM11", "client_port": "COM10", "baud_rate": 115200, "stop_bits": 1},
        "oven": {"port": "COM13", "client_port": "COM12", "baud_rate": 9600, "stop_bits": 1},
        "dmm": {"port": "COM15", "client_port": "COM14", "baud_rate": 115200, "stop_bits": 1},
        "detector": {"port": "COM17", "client_port": "COM16", "baud_rate": 115200, "stop_bits": 1},
        "cooler": {"port": "COM19", "client_port": "COM18", "baud_rate": 9600, "stop_bits": 2},
    }
    st = build_station_json(endpoints, REPO_ROOT / "examples/data/tc002a_640")
    assert st["devices"]["bb"]["kind"] == "dcn1000"
    assert st["devices"]["bb"]["connection"]["port"] == "COM20"
    assert st["devices"]["supply"]["connection"]["port"] == "COM10"
    assert st["devices"]["oven"]["connection"]["port"] == "COM12"
    assert st["devices"]["dmm"]["connection"]["port"] == "COM14"
    assert st["devices"]["det"]["connection"]["port"] == "COM16"
    assert st["devices"]["cooler"]["connection"]["port"] == "COM18"
    assert st["devices"]["cooler"]["connection"]["stop_bits"] == 2

    serial_bb = build_host_blackbody_json(endpoints["blackbody"])
    assert serial_bb["driver"] == "dcn1000" and serial_bb["transport"] == "serial"
    assert serial_bb["connection"]["port"] == "COM20"

    tcp_bb = build_host_blackbody_json({"host": "127.0.0.1", "tcp_port": 21010})
    assert tcp_bb["driver"] == "pcn7lt" and tcp_bb["transport"] == "tcp"
    assert tcp_bb["connection"]["port"] == 21010


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--fixture-parent", type=Path, default=None)
    args = parser.parse_args()

    verify_serial_map_config_builders()

    assert c.sizeof(PowerReading) == 40
    assert c.sizeof(DetectorCurrentReading) == 24
    assert c.sizeof(CoolerReading) == 16

    sdk = load_sdk(args.library.resolve())
    with TemporaryDirectory(prefix="vs-hil-", dir=args.fixture_parent) as tmp:
        work = Path(tmp)
        station_path = work / "station.json"
        host_bb_path = work / "host_blackbody.json"
        raw_dir = REPO_ROOT / "examples/data/tc002a_640"
        recipe_path = REPO_ROOT / "examples/recipes/virtual_station_hil.json"

        sim_cmd = [
            sys.executable, "-B",
            str(REPO_ROOT / "simulators/ir_virtual_station.py"),
            "--mode", "tcp",
            "--blackbody-port", "0",
            "--power-port", "0",
            "--oven-port", "0",
            "--dmm-port", "0",
            "--detector-port", "0",
            "--cooler-port", "0",
            "--control-port", "0",
            "--emit-station-json", str(station_path),
            "--emit-blackbody-config", str(host_bb_path),
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

            # Verify emitted host blackbody config schema & fn 116 ambient reply
            host_bb = json.loads(host_bb_path.read_text(encoding="utf-8"))
            assert host_bb["driver"] == "pcn7lt" and host_bb["transport"] == "tcp"
            assert host_bb["driver_options"]["expected_head_serial"] == "HEAD-SIMULATOR"
            amb = query_doc058_fn116(host_bb["connection"]["host"], host_bb["connection"]["port"])
            assert math.isclose(float(amb), 25.0, abs_tol=1e-3), amb

            # Step A: coupled HIL physics via direct C ABI
            h_pwr = c.c_uint64()
            h_dmm = c.c_uint64()
            h_det = c.c_uint64()
            h_cool = c.c_uint64()
            check(sdk.IR_OpenDevice, IR_IT6332A,
                  json.dumps({"host": "127.0.0.1", "tcp_port": endpoints["power"]["tcp_port"], "timeout_ms": 1200}).encode(),
                  c.byref(h_pwr))
            check(sdk.IR_OpenDevice, IR_DMM6500,
                  json.dumps({"host": "127.0.0.1", "tcp_port": endpoints["dmm"]["tcp_port"], "timeout_ms": 1200}).encode(),
                  c.byref(h_dmm))
            check(sdk.IR_OpenDevice, IR_DETECTOR_SERIAL,
                  json.dumps({"host": "127.0.0.1", "tcp_port": endpoints["detector"]["tcp_port"], "timeout_ms": 1200}).encode(),
                  c.byref(h_det))
            check(sdk.IR_OpenDevice, IR_COOLER_MODBUS,
                  json.dumps({"host": "127.0.0.1", "tcp_port": endpoints["cooler"]["tcp_port"], "timeout_ms": 1200}).encode(),
                  c.byref(h_cool))
            try:
                c_read = CoolerReading()
                d_read = DmmReading()
                det_curr = DetectorCurrentReading()

                # CH1 OFF, CH2 OFF baseline
                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read))
                assert math.isclose(c_read.voltage_mv, 500.0, abs_tol=1e-6), c_read.voltage_mv
                check(sdk.IR_ReadDmm, h_dmm, IR_DMM_MODE_DIODE, c.byref(d_read))
                assert math.isclose(d_read.value, 0.500, abs_tol=1e-6), d_read.value
                check(sdk.IR_MeasureDetectorCurrent, h_det, 0, 1, c.byref(det_curr))
                assert math.isclose(det_curr.current_ma, 0.0, abs_tol=1e-3), det_curr.current_ma

                # Turn CH1 ON -> observe startup >=0.3 A then steady <0.3 A, plus cooler/DMM transition
                p_read = PowerReading()
                check(sdk.IR_SetPowerChannel, h_pwr, 1, 12.0, 2.0, c.byref(p_read))
                check(sdk.IR_SetPowerOutput, h_pwr, 1, 1, c.byref(p_read))
                # First read in SetPowerOutput consumes startup sample 0 (0.55 A >= 0.3)
                assert p_read.measured_a >= 0.3, p_read.measured_a
                samples = [p_read.measured_a]
                for _ in range(5):
                    check(sdk.IR_ReadPower, h_pwr, 1, c.byref(p_read))
                    samples.append(p_read.measured_a)
                assert all(x >= 0.3 for x in samples[:4]), samples
                assert samples[-1] < 0.3, samples

                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read))
                assert math.isclose(c_read.voltage_mv, 1080.0, abs_tol=1e-6), c_read.voltage_mv
                check(sdk.IR_ReadDmm, h_dmm, IR_DMM_MODE_DIODE, c.byref(d_read))
                assert math.isclose(d_read.value, 1.080, abs_tol=1e-6), d_read.value

                # Turn CH2 ON -> detector ADC reads 12.5 + 1*2.5 = 15.0 mA
                check(sdk.IR_SetPowerChannel, h_pwr, 2, 12.0, 1.5, c.byref(p_read))
                check(sdk.IR_SetPowerOutput, h_pwr, 2, 1, c.byref(p_read))
                check(sdk.IR_MeasureDetectorCurrent, h_det, 0, 1, c.byref(det_curr))
                assert math.isclose(det_curr.current_ma, 15.0, abs_tol=1e-2), det_curr.current_ma

                # Fault injection probe check
                http_json(f"{control_url}/fault", {"cooler_bad_crc": True})
                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read), expected=IR_IO_ERROR)
                http_json(f"{control_url}/fault", {"reset": True})
                check(sdk.IR_ReadCooler, h_cool, c.byref(c_read))
                assert math.isclose(c_read.voltage_mv, 1080.0, abs_tol=1e-6)

                # Turn outputs OFF before recipe CLI run
                check(sdk.IR_SetPowerOutput, h_pwr, 2, 0, c.byref(p_read))
                check(sdk.IR_SetPowerOutput, h_pwr, 1, 0, c.byref(p_read))
            finally:
                check(sdk.IR_CloseDevice, h_cool)
                check(sdk.IR_CloseDevice, h_det)
                check(sdk.IR_CloseDevice, h_dmm)
                check(sdk.IR_CloseDevice, h_pwr)

            # Step B: full Schema v2 CLI execution with all 6 instruments
            cli_out = work / "hil-v2-out"
            rc_check = subprocess.run(
                [str(args.cli), "--check", "--station", str(station_path),
                 "--recipe", str(recipe_path), "--output", str(cli_out)],
                capture_output=True, text=True, check=False,
            )
            assert rc_check.returncode == 0 and rc_check.stdout.strip() == "recipe=VALID", (rc_check.returncode, rc_check.stdout, rc_check.stderr)

            rc_run = subprocess.run(
                [str(args.cli), "--run", "--station", str(station_path),
                 "--recipe", str(recipe_path), "--output", str(cli_out)],
                capture_output=True, text=True, check=False,
            )
            assert rc_run.returncode == 0, (rc_run.returncode, rc_run.stdout, rc_run.stderr)
            record = json.loads((cli_out / "result.json").read_text(encoding="utf-8"))
            assert record["run_status"] == "PASS", record
            assert math.isclose(record["metrics"]["dmm.value"]["value"], 1.080, abs_tol=1e-6)
            assert math.isclose(record["metrics"]["cooler.voltage_mv"]["value"], 1080.0, abs_tol=1e-6)
            assert math.isclose(record["metrics"]["det_curr.current_ma"]["value"], 15.0, abs_tol=1e-2)
            mean_dn = record["metrics"]["stats.mean_dn"]["value"]
            dc_mag = record["metrics"]["spatial_fft.dc_magnitude"]["value"]
            assert mean_dn > 1000.0
            assert math.isclose(dc_mag, mean_dn, rel_tol=1e-6)
            assert math.isfinite(record["metrics"]["spatial_fft.peak_ac_magnitude"]["value"])
            wait_rows = [
                r for r in record.get("instrument_readings", [])
                if r.get("step") == "pwr_ch1_wait" and r.get("operation") == "power_wait_stable"
            ]
            assert len(wait_rows) == 1 and wait_rows[0]["reading"]["measured_a"] < 0.3, wait_rows

            snap = http_json(f"{control_url}/state")
            assert snap["power"]["output"] == [False, False, False], snap["power"]
            assert snap["oven"]["mode"] == "STANDBY", snap["oven"]
            assert math.isclose(snap["blackbody"]["target_c"], 25.0, abs_tol=1e-6), snap["blackbody"]
            assert snap["detector"]["registers"].get("0x65") == "08", snap["detector"]
        finally:
            if control_url:
                try:
                    http_json(f"{control_url}/shutdown", {})
                except Exception:
                    pass
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=5)


if __name__ == "__main__":
    main()
