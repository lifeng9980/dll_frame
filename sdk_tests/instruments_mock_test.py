"""Exercise GWS STEN oven, Keithley DMM6500, and detector serial drivers + Schema v2 integration."""
import sys

sys.dont_write_bytecode = True

import argparse
import ctypes as c
import json
import math
import os
from pathlib import Path
import socket
import threading
import time
from tempfile import TemporaryDirectory

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "simulators"))

from ir_sim_devices import (  # noqa: E402
    CoolerModbus,
    DetectorController,
    Dmm6500,
    GwsStenOven,
    _consume_modbus_buffer,
    _dispatch_buffer,
)

# Status codes (IR_status.h)
IR_OK = 0
IR_BAD_ARGUMENT = 2
IR_COMPUTATION_FAILED = 4
IR_IO_ERROR = 6
IR_TIMEOUT = 7
IR_CANCELLED = 8

# Device kinds (IR_device.h)
IR_RAW_REPLAY = 2
IR_OVEN_GWS_STEN = 5
IR_DMM6500 = 6
IR_DETECTOR_SERIAL = 7
IR_COOLER_MODBUS = 8

# Oven / DMM enums (IR_device.h)
IR_OVEN_MODE_STANDBY = 1
IR_OVEN_MODE_CONSTANT = 2
IR_DMM_MODE_ANY = 0
IR_DMM_MODE_DCV = 1
IR_DMM_MODE_DIODE = 4


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


class Geometry(c.Structure):
    _fields_ = [("width", c.c_int32), ("height", c.c_int32), ("valid_bits", c.c_int32)]


class NoiseRecipe(c.Structure):
    _fields_ = [
        ("recipe_id", c.c_char_p),
        ("detector", c.c_uint64),
        ("blackbody", c.c_uint64),
        ("integration_us", c.c_int32),
        ("frame_hz", c.c_int32),
        ("empty_us", c.c_int32),
        ("temperature_c", c.c_double),
        ("tolerance_c", c.c_double),
        ("stable_window_ms", c.c_int32),
        ("stable_timeout_ms", c.c_int32),
        ("require_device_stable", c.c_int32),
        ("frames", c.c_int32),
        ("frame_timeout_ms", c.c_int32),
    ]


class NoiseResult(c.Structure):
    _fields_ = [
        ("completed", c.c_int32),
        ("mean_dn", c.c_double),
        ("noise_dn", c.c_double),
        ("failed_step", c.c_int32),
        ("cleanup_status", c.c_int32),
    ]


class RecipeResult(c.Structure):
    _fields_ = [
        ("completed_frames", c.c_int32),
        ("failed_stage", c.c_int32),
        ("failed_step_index", c.c_int32),
        ("cleanup_status", c.c_int32),
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

    sdk.IR_SetOven.argtypes = [c.c_uint64, c.c_double, c.POINTER(c.c_double), *err_args]
    sdk.IR_ReadOven.argtypes = [c.c_uint64, c.POINTER(OvenReading), *err_args]
    sdk.IR_StopOven.argtypes = [c.c_uint64, *err_args]
    sdk.IR_WaitOvenStable.argtypes = [
        c.c_uint64, c.c_double, c.c_double, c.c_int32, c.c_int32, c.c_int32,
        c.POINTER(OvenReading), *err_args,
    ]

    sdk.IR_SetDmmMode.argtypes = [c.c_uint64, c.c_int32, *err_args]
    sdk.IR_ReadDmm.argtypes = [c.c_uint64, c.c_int32, c.POINTER(DmmReading), *err_args]

    sdk.IR_SetDetectorTiming.argtypes = [c.c_uint64, c.c_int32, c.c_int32, c.c_int32, *err_args]
    sdk.IR_SetDetectorGpol.argtypes = [c.c_uint64, c.c_double, *err_args]
    sdk.IR_SetDetectorRegister.argtypes = [
        c.c_uint64, c.c_uint8, c.POINTER(c.c_uint8), c.c_int32, *err_args,
    ]
    sdk.IR_MeasureDetectorCurrent.argtypes = [
        c.c_uint64, c.c_int32, c.c_uint16, c.POINTER(DetectorCurrentReading), *err_args,
    ]
    sdk.IR_ReadCooler.argtypes = [c.c_uint64, c.POINTER(CoolerReading), *err_args]

    sdk.IR_OpenFrames.argtypes = [c.c_int32, c.c_char_p, c.c_int32, c.POINTER(Geometry), *err_args]
    sdk.IR_CloseFrames.argtypes = err_args
    sdk.IR_RequestStop.argtypes = []
    sdk.IR_RequestStop.restype = None
    sdk.IR_RunNoise.argtypes = [c.POINTER(NoiseRecipe), c.c_char_p, c.POINTER(NoiseResult), *err_args]

    sdk.IR_CheckRecipeJson.argtypes = [c.c_char_p, c.c_char_p, *err_args]
    sdk.IR_RunRecipeJson.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.POINTER(RecipeResult), *err_args]
    return sdk


class EphemeralTcpServer:
    """Loopback TCP server wrapper around a simulator controller with ephemeral port 0."""

    def __init__(self, controller, delimiter, consume_fn=None):
        self.controller = controller
        self.delimiter = delimiter
        self.consume_fn = consume_fn
        self.stop = threading.Event()
        self.error = None
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop.set()
        self.thread.join(5)
        try:
            self.listener.close()
        except OSError:
            pass
        assert not self.thread.is_alive(), "TCP simulator server thread did not terminate"
        if self.error is not None:
            raise self.error

    def _run(self):
        try:
            with self.listener:
                while not self.stop.is_set():
                    try:
                        conn, _ = self.listener.accept()
                    except socket.timeout:
                        continue
                    with conn:
                        conn.settimeout(0.1)
                        pending = b""
                        while not self.stop.is_set():
                            try:
                                block = conn.recv(4096)
                            except socket.timeout:
                                continue
                            except (ConnectionResetError, BrokenPipeError, OSError):
                                break
                            if not block:
                                break
                            pending += block
                            try:
                                pending = _dispatch_buffer(
                                    pending, self.controller, self.delimiter, conn.sendall,
                                    fragment=False, log_io=False, consume_fn=self.consume_fn,
                                )
                            except (ConnectionResetError, BrokenPipeError, OSError):
                                break
        except Exception as exc:  # pragma: no cover
            self.error = exc


def write_raw_fixture(directory: Path):
    # 2 frames of 1x1 RAW16 little-endian pixels [20, 22] => mean_dn == 21.0
    (directory / "frames.raw").write_bytes(b"\x14\x00\x16\x00")
    (directory / "frames.csv").write_text(
        "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
        "0,1,1,2,RAW16_LE,16,little\n"
        "1,1,1,2,RAW16_LE,16,little\n",
        encoding="utf-8",
    )


def check(fn, *args, expected=IR_OK):
    err = c.create_string_buffer(1024)
    st = fn(*args, err, len(err))
    assert st == expected, f"{fn.__name__}: expected status {expected}, got {st} ({err.value.decode('utf-8', errors='replace')})"
    return err.value.decode("utf-8", errors="replace")


def exercise_oven(sdk, oven: GwsStenOven, oven_port: int):
    cfg = json.dumps({
        "host": "127.0.0.1",
        "tcp_port": oven_port,
        "address": 1,
        "delimiter": "crlf",
        "timeout_ms": 300,
    }).encode("utf-8")
    handle = c.c_uint64()
    check(sdk.IR_OpenDevice, IR_OVEN_GWS_STEN, cfg, c.byref(handle))
    assert handle.value != 0

    try:
        readback = c.c_double()
        check(sdk.IR_SetOven, handle, 28.5, c.byref(readback))
        assert math.isclose(readback.value, 28.5, abs_tol=1e-6)

        reading = OvenReading()
        check(sdk.IR_ReadOven, handle, c.byref(reading))
        assert math.isclose(reading.setpoint_c, 28.5, abs_tol=1e-6)
        assert reading.mode == IR_OVEN_MODE_CONSTANT
        assert reading.alarm_active == 0

        # Out-of-range setpoint rejected before I/O
        check(sdk.IR_SetOven, handle, 99.0, c.byref(readback), expected=IR_BAD_ARGUMENT)

        # WaitOvenStable pass
        oven.temperature = 28.5
        check(sdk.IR_WaitOvenStable, handle, 28.5, 0.2, 50, 2000, 1, c.byref(reading))
        assert math.isclose(reading.actual_c, 28.5, abs_tol=1e-6)
        assert reading.mode == IR_OVEN_MODE_CONSTANT

        # WaitOvenStable timeout when mode is STANDBY
        oven.mode = "STANDBY"
        check(sdk.IR_WaitOvenStable, handle, 28.5, 0.2, 50, 350, 1, c.byref(reading), expected=IR_TIMEOUT)
        oven.mode = "CONSTANT"

        # WaitOvenStable alarm rejection
        oven.alarm = "OVERTEMP"
        check(sdk.IR_WaitOvenStable, handle, 28.5, 0.2, 50, 1000, 1, c.byref(reading), expected=IR_IO_ERROR)
        oven.alarm = ""

        # WaitOvenStable cross-thread cancellation
        oven.temperature = 10.0
        base_cmds = len(oven.commands)
        wait_outcome = {}

        def waiter():
            local_reading = OvenReading()
            local_err = c.create_string_buffer(1024)
            st = sdk.IR_WaitOvenStable(handle, 28.5, 0.2, 50, 5000, 1,
                                       c.byref(local_reading), local_err, len(local_err))
            wait_outcome["status"] = (st, local_err.value)

        t = threading.Thread(target=waiter)
        t.start()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline and len(oven.commands) < base_cmds + 2:
            time.sleep(0.02)
        assert len(oven.commands) >= base_cmds + 2, "IR_WaitOvenStable did not enter polling loop"
        sdk.IR_RequestStop()
        t.join(5)
        assert not t.is_alive()
        assert wait_outcome["status"][0] == IR_CANCELLED, wait_outcome
        oven.temperature = 28.5

        # StopOven -> STANDBY
        check(sdk.IR_StopOven, handle)
        assert oven.mode == "STANDBY"
        check(sdk.IR_ReadOven, handle, c.byref(reading))
        assert reading.mode == IR_OVEN_MODE_STANDBY
    finally:
        check(sdk.IR_CloseDevice, handle)


def exercise_dmm6500(sdk, dmm: Dmm6500, dmm_port: int):
    cfg = json.dumps({"host": "127.0.0.1", "tcp_port": dmm_port, "timeout_ms": 300}).encode("utf-8")

    # Wrong *IDN? rejection on open
    bad_handle = c.c_uint64(123)
    dmm.idn = "KEITHLEY INSTRUMENTS,MODEL DMM7510,WRONG,1.0"
    check(sdk.IR_OpenDevice, IR_DMM6500, cfg, c.byref(bad_handle), expected=IR_IO_ERROR)
    assert bad_handle.value == 0
    dmm.idn = "KEITHLEY INSTRUMENTS,MODEL DMM6500,IR-SIM,1.0"

    handle = c.c_uint64()
    check(sdk.IR_OpenDevice, IR_DMM6500, cfg, c.byref(handle))
    assert handle.value != 0
    try:
        check(sdk.IR_SetDmmMode, handle, IR_DMM_MODE_DCV)
        check(sdk.IR_SetDmmMode, handle, IR_DMM_MODE_DIODE)

        reading = DmmReading()
        check(sdk.IR_ReadDmm, handle, IR_DMM_MODE_DIODE, c.byref(reading))
        assert math.isclose(reading.value, 0.6125, rel_tol=0.0, abs_tol=1e-9)
        assert reading.mode == IR_DMM_MODE_DIODE
        assert reading.valid == 1

        # Immediate mode mismatch failure
        check(sdk.IR_ReadDmm, handle, IR_DMM_MODE_DCV, c.byref(reading), expected=IR_COMPUTATION_FAILED)

        # Drop-1-reply reconnect recovery
        dmm.drop_command = ":READ?"
        dmm.drop_count = 1
        reading = DmmReading()
        check(sdk.IR_ReadDmm, handle, IR_DMM_MODE_DIODE, c.byref(reading))
        assert dmm.drop_count == 0
        assert math.isclose(reading.value, 0.6125, abs_tol=1e-9)
    finally:
        check(sdk.IR_CloseDevice, handle)


def exercise_detector(sdk, det: DetectorController, det_port: int, fixture_dir: Path):
    cfg = json.dumps({"host": "127.0.0.1", "tcp_port": det_port, "timeout_ms": 300}).encode("utf-8")
    handle = c.c_uint64()
    check(sdk.IR_OpenDevice, IR_DETECTOR_SERIAL, cfg, c.byref(handle))
    assert handle.value != 0

    frames_open = False
    try:
        # Valid timing
        check(sdk.IR_SetDetectorTiming, handle, 1000, 50, 200)
        assert det.frames[-2:] == [
            (0x50, b"\xe8\x03\x00"),
            (0x51, b"\xc8\x00\x00"),
        ], det.frames

        # Invalid timing (integration_us > 1e6 / frame_hz)
        check(sdk.IR_SetDetectorTiming, handle, 25000, 50, 0, expected=IR_BAD_ARGUMENT)

        # GPOL 2.5 V -> lround(2.5 * 65535 / 5.0) = 32768 (0x8000 LE)
        check(sdk.IR_SetDetectorGpol, handle, 2.5)
        cmd, payload = det.frames[-1]
        assert cmd == 0x21 and len(payload) == 2
        assert int.from_bytes(payload, "little") == 32768

        # Custom register write
        reg_buf = (c.c_uint8 * 1)(0x01)
        check(sdk.IR_SetDetectorRegister, handle, 0x78, reg_buf, 1)
        assert det.frames[-1] == (0x78, b"\x01")

        # Bad ACK failure
        det.bad_ack = True
        reg_buf2 = (c.c_uint8 * 1)(0x02)
        check(sdk.IR_SetDetectorRegister, handle, 0x79, reg_buf2, 1, expected=IR_IO_ERROR)
        det.bad_ack = False

        # Current measurement (7-byte 55 00 03 41 ADC response)
        curr = DetectorCurrentReading()
        check(sdk.IR_MeasureDetectorCurrent, handle, 0, 0x0102, c.byref(curr))
        assert math.isclose(curr.current_ma, 25.0, abs_tol=1e-3), curr.current_ma
        assert math.isclose(curr.sense_voltage_v, 0.025, abs_tol=1e-6), curr.sense_voltage_v

        check(sdk.IR_MeasureDetectorCurrent, handle, 3, 0x0102, c.byref(curr))
        assert math.isclose(curr.current_ma, 25.0, abs_tol=1e-3), curr.current_ma
        assert det.gain_code == 3 and det.channel_code == 0x0102

        check(sdk.IR_MeasureDetectorCurrent, handle, 8, 0x0102, c.byref(curr), expected=IR_BAD_ARGUMENT)

        det.bad_ack = True
        check(sdk.IR_MeasureDetectorCurrent, handle, 0, 0x0102, c.byref(curr), expected=IR_IO_ERROR)
        det.bad_ack = False

        # IR_RunNoise with detector != 0
        det.frames.clear()
        geom = Geometry()
        check(sdk.IR_OpenFrames, IR_RAW_REPLAY, str(fixture_dir).encode("utf-8"), 0, c.byref(geom))
        frames_open = True
        recipe = NoiseRecipe(
            b"det-serial-noise", handle.value, 0,
            1200, 50, 150,
            25.0, 0.5, 50, 1000, 0,
            2, 0,
        )
        nres = NoiseResult()
        check(sdk.IR_RunNoise, c.byref(recipe), None, c.byref(nres))
        assert nres.completed == 2
        assert math.isclose(nres.mean_dn, 21.0, abs_tol=1e-9), nres.mean_dn
        assert nres.failed_step == 0 and nres.cleanup_status == 0
        assert det.frames == [
            (0x50, (1200).to_bytes(3, "little")),
            (0x51, (150).to_bytes(3, "little")),
        ], det.frames
    finally:
        if frames_open:
            check(sdk.IR_CloseFrames)
        check(sdk.IR_CloseDevice, handle)


def exercise_cooler(sdk, cooler: CoolerModbus, cooler_port: int):
    cfg = json.dumps({
        "host": "127.0.0.1",
        "tcp_port": cooler_port,
        "slave_address": 1,
        "register_address": 0x7A52,
        "timeout_ms": 250,
    }).encode("utf-8")
    handle = c.c_uint64()
    check(sdk.IR_OpenDevice, IR_COOLER_MODBUS, cfg, c.byref(handle))
    assert handle.value != 0

    try:
        assert cooler.requests[-1] == bytes.fromhex("01037a5200027cc2"), cooler.requests[-1].hex()

        reading = CoolerReading()
        check(sdk.IR_ReadCooler, handle, c.byref(reading))
        assert reading.valid == 1 and reading.raw_value == 108000
        assert math.isclose(reading.voltage_mv, 1080.0, abs_tol=1e-6)

        # Drop 1 reply -> reconnect recovery
        cooler.drop_count = 1
        reading = CoolerReading()
        check(sdk.IR_ReadCooler, handle, c.byref(reading))
        assert cooler.drop_count == 0
        assert math.isclose(reading.voltage_mv, 1080.0, abs_tol=1e-6)

        # Persistent CRC failure -> IR_IO_ERROR, then clean recovery
        cooler.bad_crc = True
        check(sdk.IR_ReadCooler, handle, c.byref(reading), expected=IR_IO_ERROR)
        cooler.bad_crc = False
        check(sdk.IR_ReadCooler, handle, c.byref(reading))
        assert math.isclose(reading.voltage_mv, 1080.0, abs_tol=1e-6)
    finally:
        check(sdk.IR_CloseDevice, handle)


def exercise_recipe_v2(sdk, oven: GwsStenOven, oven_port: int,
                       dmm: Dmm6500, dmm_port: int,
                       det: DetectorController, det_port: int,
                       cooler: CoolerModbus, cooler_port: int,
                       fixture_dir: Path, work_dir: Path):
    station = {
        "schema_version": 1,
        "acquisition": {
            "backend": "raw_replay",
            "source": str(fixture_dir),
            "valid_bits": 16,
        },
        "devices": {
            "oven": {
                "kind": "gws-sten",
                "connection": {"host": "127.0.0.1", "tcp_port": oven_port, "timeout_ms": 400},
            },
            "dmm": {
                "kind": "dmm6500",
                "connection": {"host": "127.0.0.1", "tcp_port": dmm_port, "timeout_ms": 400},
            },
            "det": {
                "kind": "detector-serial",
                "connection": {"host": "127.0.0.1", "tcp_port": det_port, "timeout_ms": 400},
            },
            "cooler": {
                "kind": "cooler-modbus",
                "connection": {"host": "127.0.0.1", "tcp_port": cooler_port, "timeout_ms": 400},
            },
        },
    }
    pass_recipe = {
        "schema_version": 2,
        "recipe_id": "multi-instrument-v2-pass",
        "steps": [
            {
                "id": "ov1", "type": "oven_stabilize", "device": "oven",
                "target_c": 35.0, "tolerance_c": 0.5, "stable_window_ms": 30,
                "timeout_ms": 1500, "require_no_alarm": True, "standby_after_run": True,
            },
            {"id": "dt1", "type": "detector_timing", "device": "det", "integration_us": 800, "frame_hz": 50, "empty_us": 100},
            {"id": "dg1", "type": "detector_gpol", "device": "det", "gpol_v": 2.5},
            {"id": "dr1", "type": "detector_register", "device": "det", "command_code": 0x65, "payload_hex": "08"},
            {"id": "dc1", "type": "detector_current", "device": "det", "name": "det_curr", "gain_code": 0, "channel_code": 1},
            {"id": "dm1", "type": "dmm_read", "device": "dmm", "name": "dmm", "set_mode": True, "mode": "diode"},
            {"id": "cl1", "type": "cooler_read", "device": "cooler", "name": "cooler"},
            {"id": "cap", "type": "capture", "frames": 2, "frame_timeout_ms": 0, "output": "batch0"},
            {"id": "st1", "type": "analyze", "method": "frame_stats", "name": "stats", "input": "batch0"},
        ],
        "safety": {"oven_standby": ["oven"]},
        "limits": [
            {"metric": "dmm.value", "operator": "min", "value": 0.5, "unit": "V"},
            {"metric": "dmm.value", "operator": "max", "value": 0.8, "unit": "V"},
            {"metric": "cooler.voltage_mv", "operator": "min", "value": 1000.0, "unit": "mV"},
            {"metric": "det_curr.current_ma", "operator": "min", "value": 20.0, "unit": "mA"},
        ],
    }
    station_bytes = json.dumps(station).encode("utf-8")
    pass_bytes = json.dumps(pass_recipe).encode("utf-8")
    check(sdk.IR_CheckRecipeJson, station_bytes, pass_bytes)

    oven.temperature = 35.0
    oven.mode = "STANDBY"
    oven.commands.clear()
    dmm.mode = "VOLT:DC"
    dmm.value = 0.6125
    det.frames.clear()

    pass_out = work_dir / "v2-pass-out"
    res = RecipeResult()
    check(sdk.IR_RunRecipeJson, station_bytes, pass_bytes, str(pass_out).encode("utf-8"), c.byref(res))
    assert res.cleanup_status == 0 and res.completed_frames == 2

    record = json.loads((pass_out / "result.json").read_text(encoding="utf-8"))
    assert record["run_status"] == "PASS", record["run_status"]
    assert record["raw_saved"] is False
    assert math.isclose(record["metrics"]["dmm.value"]["value"], 0.6125, abs_tol=1e-9)
    assert math.isclose(record["metrics"]["cooler.voltage_mv"]["value"], 1080.0, abs_tol=1e-6)
    assert math.isclose(record["metrics"]["det_curr.current_ma"]["value"], 25.0, abs_tol=1e-2)
    assert math.isclose(record["metrics"]["stats.mean_dn"]["value"], 21.0, abs_tol=1e-9)
    assert oven.mode == "STANDBY"
    assert "MODE, STANDBY" in oven.commands
    assert det.frames == [
        (0x50, (800).to_bytes(3, "little")),
        (0x51, (100).to_bytes(3, "little")),
        (0x21, (32768).to_bytes(2, "little")),
        (0x65, b"\x08"),
        (0x40, b"\x00"),
        (0x30, b"\x00\x01"),
        (0x41, b"\x03"),
    ], det.frames

    # Repeat-burst v2 check emitting dmm.value in CSV
    burst_recipe = {
        "schema_version": 2,
        "recipe_id": "multi-instrument-v2-burst",
        "steps": [
            {
                "id": "loop",
                "type": "repeat_burst",
                "max_iterations": 2,
                "csv_file": "drift.csv",
                "record_metrics": ["dmm.value"],
                "steps": [
                    {"id": "b_dm", "type": "dmm_read", "device": "dmm", "name": "dmm", "set_mode": False, "mode": "diode"},
                ],
            }
        ],
    }
    burst_bytes = json.dumps(burst_recipe).encode("utf-8")
    check(sdk.IR_CheckRecipeJson, station_bytes, burst_bytes)
    burst_out = work_dir / "v2-burst-out"
    res_burst = RecipeResult()
    check(sdk.IR_RunRecipeJson, station_bytes, burst_bytes, str(burst_out).encode("utf-8"), c.byref(res_burst))
    csv_lines = (burst_out / "drift.csv").read_text(encoding="utf-8").strip().splitlines()
    assert len(csv_lines) == 3 and csv_lines[0].endswith(",dmm.value"), csv_lines

    # Forced-failure case: DMM left in DIOD, recipe asserts expected DCV without set_mode;
    # oven_stabilize sets standby_after_run=True and must be returned to STANDBY in cleanup.
    fail_recipe = {
        "schema_version": 2,
        "recipe_id": "multi-instrument-v2-cleanup",
        "steps": [
            {
                "id": "ov_fail", "type": "oven_stabilize", "device": "oven",
                "target_c": 35.0, "tolerance_c": 0.5, "stable_window_ms": 20,
                "timeout_ms": 1500, "require_no_alarm": True, "standby_after_run": True,
            },
            {"id": "dm_bad", "type": "dmm_read", "device": "dmm", "name": "dmm", "set_mode": False, "mode": "dcv"},
        ],
        "safety": {"oven_standby": ["oven"]},
    }
    fail_bytes = json.dumps(fail_recipe).encode("utf-8")
    check(sdk.IR_CheckRecipeJson, station_bytes, fail_bytes)

    oven.temperature = 35.0
    oven.commands.clear()
    fail_out = work_dir / "v2-fail-out"
    res_fail = RecipeResult()
    check(
        sdk.IR_RunRecipeJson,
        station_bytes, fail_bytes, str(fail_out).encode("utf-8"), c.byref(res_fail),
        expected=IR_COMPUTATION_FAILED,
    )
    assert res_fail.cleanup_status == IR_OK
    assert not fail_out.exists()
    assert oven.mode == "STANDBY"
    assert "MODE, STANDBY" in oven.commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--fixture-parent", type=Path, default=None)
    args = parser.parse_args()

    assert c.sizeof(OvenReading) == 24, c.sizeof(OvenReading)
    assert c.sizeof(DmmReading) == 16, c.sizeof(DmmReading)
    assert c.sizeof(DetectorCurrentReading) == 24, c.sizeof(DetectorCurrentReading)
    assert c.sizeof(CoolerReading) == 16, c.sizeof(CoolerReading)

    sdk = load_sdk(args.library.resolve())
    oven = GwsStenOven(address=1, temperature=25.0, setpoint=25.0, mode="STANDBY")
    dmm = Dmm6500(mode="DIOD", value=0.6125)
    det = DetectorController()
    cooler = CoolerModbus(slave_address=1, register_address=0x7A52, voltage_mv=1080.0)

    with EphemeralTcpServer(oven, b"\r\n") as oven_srv, \
         EphemeralTcpServer(dmm, b"\n") as dmm_srv, \
         EphemeralTcpServer(det, None) as det_srv, \
         EphemeralTcpServer(cooler, None, consume_fn=_consume_modbus_buffer) as cooler_srv, \
         TemporaryDirectory(prefix="instruments-mock-", dir=args.fixture_parent) as tmp:
        work_dir = Path(tmp)
        fixture_dir = work_dir / "raw"
        fixture_dir.mkdir()
        write_raw_fixture(fixture_dir)

        exercise_oven(sdk, oven, oven_srv.port)
        exercise_dmm6500(sdk, dmm, dmm_srv.port)
        exercise_detector(sdk, det, det_srv.port, fixture_dir)
        exercise_cooler(sdk, cooler, cooler_srv.port)
        exercise_recipe_v2(
            sdk, oven, oven_srv.port, dmm, dmm_srv.port, det, det_srv.port,
            cooler, cooler_srv.port, fixture_dir, work_dir
        )


if __name__ == "__main__":
    main()
