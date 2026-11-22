"""Exercise DOC058 over TCP loopback and a Linux virtual serial port."""
import argparse
import ctypes as c
import json
import math
import os
from pathlib import Path
import select
import socket
import subprocess
import threading
import time
from tempfile import TemporaryDirectory



class Reading(c.Structure):
    _fields_ = [("actual_c", c.c_double), ("setpoint_c", c.c_double), ("stable", c.c_int32)]


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
        ("completed", c.c_int32), ("mean_dn", c.c_double), ("noise_dn", c.c_double),
        ("failed_step", c.c_int32), ("cleanup_status", c.c_int32),
    ]


class RecipeResult(c.Structure):
    _fields_ = [
        ("completed_frames", c.c_int32), ("failed_stage", c.c_int32),
        ("failed_step_index", c.c_int32), ("cleanup_status", c.c_int32),
    ]


class Controller:
    def __init__(self, tcp):
        self.tcp = tcp
        self.target = 24.875
        self.fail_read = False
        self.fail_write = False
        self.reject_write = False
        self.disconnect_reads = 0
        self.stable = True
        self.writes = 0
        self.error = None
        self.stable_reply = threading.Event()
        self.stable_reply_count = 0
        self.stable_reply_cond = threading.Condition()

    def reply(self, request, send):
        assert request[:1] == b"\x1b"
        direction, function = request[1:3]
        arg = request[3:]
        if function == 97 and direction == ord("1"):
            args = arg.split(b",")
            assert args[1:] == ([b"0"] if self.tcp else [])
            if self.reject_write:
                self.reject_write = False
                send(b"\x1b" + bytes([function]) + b"\x15\r")
                return True
            self.target = float(args[0])
            self.writes += 1
            if self.fail_write:
                self.fail_write = False
                send = None
            payload = b"\x06"
        else:
            assert direction == ord("0")
            head_reads = (56, 66, 67, 92, 97, 114) if self.tcp else (56,)
            assert arg == (b"0" if function in head_reads else b""), (function, arg)
            if function == 114 and self.fail_read:
                self.fail_read = False
                if not self.tcp:
                    return True  # Omit one response and trigger the serial deadline.
                return False  # Disconnect and require fresh controller identification.
            if function == 114 and self.disconnect_reads:
                self.disconnect_reads -= 1
                return not self.tcp
            payload = {
                135: b"PCN7-LT" if self.tcp else b"DCN1000",
                136: b"DOC058-MOCK",
                56: b"0,HEAD",
                73: b"0",
                114: str(self.target).encode(),
                97: str(self.target).encode(),
                67: b"0",
                66: b"50",
                92: b"1" if self.stable else b"0",
            }[function]

        if send is None:
            # The setpoint already took effect. Omit its acknowledgement.
            return True
        if function == 135:
            send(b"\x1b\x71unrelated\r\x1b" + bytes([function]))
            time.sleep(0.02)
            send(payload + b"\r")
        else:
            send(b"\x1b" + bytes([function]) + payload + b"\r")
        if function == 92:
            self.stable_reply.set()
            with self.stable_reply_cond:
                self.stable_reply_count += 1
                self.stable_reply_cond.notify_all()
        return True

    def serve(self, read, send, stop):
        pending = b""
        while not stop.is_set():
            block = read()
            if block is None:  # Stop waiting for the next TCP client.
                return
            pending += block
            while b"\r" in pending:
                request, pending = pending.split(b"\r", 1)
                if not self.reply(request, send):
                    return


def load_sdk(path):
    if os.name == "nt":
        with os.add_dll_directory(str(path.parent)):
            return c.CDLL(str(path))
    return c.CDLL(str(path))


def exercise(sdk, controller, config):
    handle = c.c_uint64()
    error = c.create_string_buffer(1024)
    io_error, timeout, invalid_handle = 6, 7, 5
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *error_args]
    sdk.IR_CloseDevice.argtypes = [c.c_uint64, *error_args]
    sdk.IR_ReadBlackbody.argtypes = [c.c_uint64, c.POINTER(Reading), *error_args]
    sdk.IR_SetBlackbody.argtypes = [c.c_uint64, c.c_double, c.POINTER(c.c_double), *error_args]
    kind = 2 if controller.tcp else 3

    def check(function, *args, expected=0):
        status = function(*args, error, len(error))
        assert status == expected, (function.__name__, expected, status, error.value)

    if controller.tcp:
        bad_config = json.dumps(dict(config, expected_head_serial="WRONG")).encode()
        check(sdk.IR_OpenDevice, kind, bad_config, c.byref(handle), expected=io_error)
        assert not handle.value

    check(sdk.IR_OpenDevice, kind, json.dumps(config).encode(), c.byref(handle))
    if controller.tcp:
        alias_config = json.dumps(dict(config, host="localhost")).encode()
        alias_handle = c.c_uint64()
        check(sdk.IR_OpenDevice, kind, alias_config, c.byref(alias_handle), expected=9)
        assert not alias_handle.value
    reading, readback = Reading(), c.c_double()
    try:
        check(sdk.IR_ReadBlackbody, handle, c.byref(reading))
        assert (reading.actual_c, reading.setpoint_c, reading.stable) == (24.875, 24.875, 1)
        check(sdk.IR_SetBlackbody, handle, 51, c.byref(readback), expected=2)
        assert controller.writes == 0
        controller.reject_write = True
        check(sdk.IR_SetBlackbody, handle, 24.875, c.byref(readback), expected=io_error)
        assert controller.writes == 0
        check(sdk.IR_SetBlackbody, handle, 30.125, c.byref(readback))
        assert readback.value == 30.125

        controller.fail_read = True
        check(sdk.IR_ReadBlackbody, handle, c.byref(reading))
        assert reading.actual_c == reading.setpoint_c == 30.125

        controller.disconnect_reads = 4
        retry_exhausted_status = io_error if controller.tcp else timeout
        check(sdk.IR_ReadBlackbody, handle, c.byref(reading), expected=retry_exhausted_status)
        assert controller.disconnect_reads == 0

        # A call that starts with a broken link gets exactly three reconnect attempts.
        controller.disconnect_reads = 4
        check(sdk.IR_ReadBlackbody, handle, c.byref(reading), expected=retry_exhausted_status)
        assert controller.disconnect_reads == 1
        check(sdk.IR_ReadBlackbody, handle, c.byref(reading))
        assert reading.actual_c == reading.setpoint_c == 30.125

        controller.fail_write = True
        check(sdk.IR_SetBlackbody, handle, 31.25, c.byref(readback))
        assert readback.value == 31.25 and controller.writes == 2

    finally:
        check(sdk.IR_CloseDevice, handle)
    check(sdk.IR_ReadBlackbody, handle, c.byref(reading), expected=invalid_handle)


def exercise_wait_blackbody_stable(sdk, controller, config):
    handle = c.c_uint64()
    error = c.create_string_buffer(1024)
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_WaitBlackbodyStable.argtypes = [
        c.c_uint64, c.c_double, c.c_double, c.c_int32, c.c_int32, c.c_int32,
        c.POINTER(Reading), *error_args,
    ]
    sdk.IR_WaitBlackbodyStable.restype = c.c_int32
    sdk.IR_RequestStop.argtypes = []
    sdk.IR_RequestStop.restype = None

    kind = 2 if controller.tcp else 3
    status = sdk.IR_OpenDevice(kind, json.dumps(config).encode(), c.byref(handle), error, len(error))
    assert status == 0, (status, error.value)
    prev_stable = controller.stable
    try:
        target = float(controller.target)
        reading = Reading()

        controller.stable = True
        status = sdk.IR_WaitBlackbodyStable(
            handle, target, 0.05, 80, 1200, 1, c.byref(reading), error, len(error)
        )
        assert status == 0 and reading.actual_c == target and reading.stable == 1, (
            status, reading.actual_c, reading.stable, error.value
        )

        controller.stable = False
        status = sdk.IR_WaitBlackbodyStable(
            handle, target, 0.05, 100, 200, 1, c.byref(reading), error, len(error)
        )
        assert status == 7, (status, error.value)

        with controller.stable_reply_cond:
            before_replies = controller.stable_reply_count
        worker_status = []
        worker_err = c.create_string_buffer(1024)
        worker_reading = Reading()

        def wait_worker():
            worker_status.append(
                sdk.IR_WaitBlackbodyStable(
                    handle, target, 0.05, 500, 5000, 1,
                    c.byref(worker_reading), worker_err, len(worker_err),
                )
            )

        t = threading.Thread(target=wait_worker)
        t.start()
        try:
            with controller.stable_reply_cond:
                assert controller.stable_reply_cond.wait_for(
                    lambda: controller.stable_reply_count > before_replies, timeout=2.0
                ), "IR_WaitBlackbodyStable did not poll controller"
            sdk.IR_RequestStop()
        finally:
            t.join(5)
            assert not t.is_alive()
        assert worker_status == [8], (worker_status, worker_err.value)

        controller.stable = True
        status = sdk.IR_WaitBlackbodyStable(
            handle, target, 0.05, 50, 1200, 1, c.byref(reading), error, len(error)
        )
        assert status == 0 and reading.stable == 1, (status, error.value)
    finally:
        controller.stable = prev_stable
        close_status = sdk.IR_CloseDevice(handle, error, len(error))
        assert close_status == 0, (close_status, error.value)


def exercise_recipe(args, sdk, controller, config):
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = os.pathsep.join([str(args.library.resolve().parent), env["PATH"]])
    with TemporaryDirectory(prefix="blackbody-", dir=args.fixture_parent) as temporary:
        fixture = Path(temporary)
        (fixture / "frames.raw").write_bytes(b"\x14\x00\x16\x00")
        (fixture / "frames.csv").write_text(
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,1,1,2,RAW16_LE,16,little\n1,1,1,2,RAW16_LE,16,little\n", encoding="utf8")
        command = [str(args.client.resolve()), args.mode, json.dumps(config)]
        for stable in (True, False):
            controller.stable = stable
            run = subprocess.run(command, cwd=fixture, env=env, capture_output=True, text=True, timeout=12)
            print(f"{args.mode} stable={stable} status={run.returncode}\n{run.stdout}", flush=True)
            assert run.returncode == (0 if stable else 7), (run.returncode, run.stderr)
            values = dict(line.split("=", 1) for line in run.stdout.splitlines())
            if stable:
                assert controller.target == 32 and values["completed"] == "2"
                assert values["failed_step"] == "0" and values["cleanup_status"] == "0"
                assert float(values["mean_dn"]) == 21
                assert math.isclose(float(values["noise_dn"]), math.sqrt(2), rel_tol=0, abs_tol=1e-12)
            else:
                assert values == {"completed": "0", "failed_step": "4", "cleanup_status": "0"}

        exercise_saved_recipe(sdk, controller, config, fixture)
        exercise_json_recipe(sdk, controller, config, fixture)


def exercise_saved_recipe(sdk, controller, config, fixture):
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *error_args]
    sdk.IR_CloseDevice.argtypes = [c.c_uint64, *error_args]
    sdk.IR_OpenFrames.argtypes = [c.c_int32, c.c_char_p, c.c_int32, c.POINTER(Geometry), *error_args]
    sdk.IR_CloseFrames.argtypes = error_args
    sdk.IR_RunNoise.argtypes = [c.POINTER(NoiseRecipe), c.c_char_p, c.POINTER(NoiseResult), *error_args]

    error = c.create_string_buffer(1024)
    kind = 2 if controller.tcp else 3
    handle = c.c_uint64()
    status = sdk.IR_OpenDevice(kind, json.dumps(config).encode(), c.byref(handle), error, len(error))
    assert status == 0, error.value
    geometry = Geometry()
    try:
        status = sdk.IR_OpenFrames(2, str(fixture).encode(), 0, c.byref(geometry), error, len(error))
        assert status == 0, error.value
        try:
            controller.stable = True
            recipe = NoiseRecipe(b"saved-result", 0, handle.value, 0, 0, 0, 32.0, 0.01,
                                 100, 5000, 1, 2, 0)
            result = NoiseResult()
            output = fixture / "saved-run"
            status = sdk.IR_RunNoise(c.byref(recipe), str(output).encode(), c.byref(result), error, len(error))
            assert status == 0, error.value
            saved = json.loads((output / "result.json").read_text(encoding="utf8"))
            assert saved["station_version"] == saved["analysis_version"] and saved["station_version"]
            assert isinstance(saved["source_commit"], str) and saved["source_commit"]
            assert saved["started_utc"].endswith("Z") and saved["finished_utc"].endswith("Z")
            assert saved["started_utc"] <= saved["finished_utc"]
            blackbody = saved["blackbody"]
            assert blackbody["temperature_c"] == blackbody["setpoint_readback_c"] == 32
            assert blackbody["actual_c"] == 32 and blackbody["device_stable"] is True, blackbody
        finally:
            status = sdk.IR_CloseFrames(error, len(error))
            assert status == 0, error.value
    finally:
        status = sdk.IR_CloseDevice(handle, error, len(error))
        assert status == 0, error.value


def exercise_json_recipe(sdk, controller, config, fixture):
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_CheckRecipeJson.argtypes = [c.c_char_p, c.c_char_p, *error_args]
    sdk.IR_RunRecipeJson.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.POINTER(RecipeResult), *error_args]
    error = c.create_string_buffer(1024)
    kind = "pcn7-lt" if controller.tcp else "dcn1000"
    station = {
        "schema_version": 1,
        "acquisition": {"backend": "raw_replay", "source": str(fixture), "valid_bits": 16},
        "devices": {"bb": {"kind": kind, "connection": config}},
    }
    recipe = {
        "schema_version": 1,
        "recipe_id": "mock-json-blackbody-noise",
        "steps": [
            {"id": "stable", "type": "blackbody_stabilize", "device": "bb",
             "target_c": 32.0, "tolerance_c": 0.01, "stable_window_ms": 0,
             "timeout_ms": 1500, "require_device_stable": True},
            {"id": "batch", "type": "capture", "frames": 2, "frame_timeout_ms": 0},
            {"id": "noise", "type": "analyze", "method": "temporal_noise",
             "name": "noise", "input": "batch"},
            {"id": "roi", "type": "analyze", "method": "roi_mean", "name": "roi",
             "input": "batch", "x": 0, "y": 0, "width": 1, "height": 1},
            {"id": "archive", "type": "save_raw", "input": "batch"},
        ],
        "limits": [{"metric": "noise.temporal_noise_dn", "operator": "max",
                    "value": 2.0, "unit": "DN"}],
    }
    station_json = json.dumps(station).encode()
    recipe_json = json.dumps(recipe).encode()
    status = sdk.IR_CheckRecipeJson(station_json, recipe_json, error, len(error))
    assert status == 0, error.value

    output = fixture / "json-run"
    controller.stable = True
    result = RecipeResult()
    status = sdk.IR_RunRecipeJson(station_json, recipe_json, str(output).encode(),
                                  c.byref(result), error, len(error))
    assert status == 0, error.value
    assert (result.completed_frames, result.failed_stage, result.failed_step_index, result.cleanup_status) == (2, 0, -1, 0)
    record = json.loads((output / "result.json").read_text(encoding="utf8"))
    assert record["run_status"] == "PASS" and record["completed_frames"] == 2
    assert record["station_version"] == record["analysis_version"] and record["station_version"]
    assert isinstance(record["source_commit"], str) and record["source_commit"]
    assert record["started_utc"].endswith("Z") and record["finished_utc"].endswith("Z")
    assert record["started_utc"] <= record["finished_utc"]
    assert record["conditions"][0]["setpoint_readback_c"] == 32.0
    assert math.isclose(record["metrics"]["noise.temporal_noise_dn"]["value"],
                        math.sqrt(2), rel_tol=0, abs_tol=1e-12)
    assert record["metrics"]["roi.mean_dn"]["value"] == 21

    unstable = dict(recipe)
    unstable["recipe_id"] = "mock-json-blackbody-timeout"
    unstable["steps"] = [dict(step) for step in recipe["steps"]]
    unstable["steps"][0]["timeout_ms"] = 350
    controller.stable = False
    timeout_output = fixture / "json-timeout"
    status = sdk.IR_RunRecipeJson(station_json, json.dumps(unstable).encode(),
                                  str(timeout_output).encode(), c.byref(result), error, len(error))
    assert status == 7 and b"stable" in error.value.lower(), (status, error.value)
    assert (result.completed_frames, result.failed_stage, result.failed_step_index, result.cleanup_status) == (0, 4, 0, 0)
    assert not timeout_output.exists()
    controller.stable = True

    # Schema v2: multi-temperature capture (20 -> 30 °C), zero save_raw, and safety blackbody standby (24.5 °C)
    recipe_v2 = {
        "schema_version": 2,
        "recipe_id": "mock-v2-multitemp-standby",
        "steps": [
            {"id": "bb_cold", "type": "blackbody_stabilize", "device": "bb",
             "target_c": 20.0, "tolerance_c": 0.05, "stable_window_ms": 0,
             "timeout_ms": 1500, "require_device_stable": True},
            {"id": "cap_cold", "type": "capture", "frames": 1, "frame_timeout_ms": 0, "output": "b0"},
            {"id": "bb_hot", "type": "blackbody_stabilize", "device": "bb",
             "target_c": 30.0, "tolerance_c": 0.05, "stable_window_ms": 0,
             "timeout_ms": 1500, "require_device_stable": True},
            {"id": "cap_hot", "type": "capture", "frames": 1, "frame_timeout_ms": 0, "output": "b1"},
            {"id": "stats_hot", "type": "analyze", "method": "frame_stats", "name": "hot",
             "input": "b1", "params": {"sample_std": 0}}
        ],
        "safety": {
            "blackbody_standby": [{"device": "bb", "target_c": 24.5}]
        },
        "limits": [
            {"metric": "hot.mean_dn", "operator": "max", "value": 30.0, "unit": "DN"}
        ],
    }
    v2_json = json.dumps(recipe_v2).encode()
    status = sdk.IR_CheckRecipeJson(station_json, v2_json, error, len(error))
    assert status == 0, error.value

    v2_out = fixture / "json-v2-run"
    v2_result = RecipeResult()
    status = sdk.IR_RunRecipeJson(station_json, v2_json, str(v2_out).encode(),
                                  c.byref(v2_result), error, len(error))
    assert status == 0, error.value
    assert (v2_result.completed_frames, v2_result.failed_stage,
            v2_result.failed_step_index, v2_result.cleanup_status) == (2, 0, -1, 0)
    assert controller.target == 24.5, controller.target
    assert not list(v2_out.rglob("*.raw"))
    v2_rec = json.loads((v2_out / "result.json").read_text(encoding="utf8"))
    assert v2_rec["schema_version"] == 2 and v2_rec["run_status"] == "PASS" and v2_rec["raw_saved"] is False
    assert [c_row["target_c"] for c_row in v2_rec["conditions"]] == [20.0, 30.0]
    assert v2_rec["metrics"]["hot.mean_dn"]["value"] == 22.0


def exercise_recipe_ownership(args, sdk, controller, config):
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *error_args]
    sdk.IR_CloseDevice.argtypes = [c.c_uint64, *error_args]
    sdk.IR_SetBlackbody.argtypes = [c.c_uint64, c.c_double, c.POINTER(c.c_double), *error_args]
    sdk.IR_ReadBlackbody.argtypes = [c.c_uint64, c.POINTER(Reading), *error_args]
    sdk.IR_OpenFrames.argtypes = [c.c_int32, c.c_char_p, c.c_int32, c.c_void_p, *error_args]
    sdk.IR_CloseFrames.argtypes = error_args
    sdk.IR_RunNoise.argtypes = [c.POINTER(NoiseRecipe), c.c_char_p, c.POINTER(NoiseResult), *error_args]
    sdk.IR_RequestStop.argtypes = []
    sdk.IR_RequestStop.restype = None

    with TemporaryDirectory(prefix="ownership-", dir=args.fixture_parent) as temporary:
        fixture = Path(temporary)
        (fixture / "frames.raw").write_bytes(b"\x14\x00\x16\x00")
        (fixture / "frames.csv").write_text(
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,1,1,2,RAW16_LE,16,little\n1,1,1,2,RAW16_LE,16,little\n", encoding="utf8")
        controller.stable = True
        controller.stable_reply.clear()
        error = c.create_string_buffer(1024)
        handle = c.c_uint64()
        kind = 2 if controller.tcp else 3
        status = sdk.IR_OpenDevice(kind, json.dumps(config).encode(), c.byref(handle), error, len(error))
        assert status == 0, error.value

        recipe = NoiseRecipe(b"ownership", 0, handle.value, 0, 0, 0, 32.0, 0.01,
                             1500, 6000, 1, 2, 0)
        outcome = {}
        first_complete = threading.Event()
        retry_allowed = threading.Event()

        def run_recipe():
            def execute_attempt(name):
                worker_error = c.create_string_buffer(1024)
                geometry = (c.c_int32 * 3)()
                fixture_name = str(fixture).encode()
                opened = sdk.IR_OpenFrames(2, fixture_name, 0, geometry, worker_error, len(worker_error))
                if opened != 0:
                    outcome[name] = {"open": opened, "error": worker_error.value}
                    return
                result = NoiseResult()
                status = sdk.IR_RunNoise(c.byref(recipe), None, c.byref(result),
                                         worker_error, len(worker_error))
                outcome[name] = {"status": status, "error": worker_error.value,
                                 "result": result, "thread": threading.get_ident()}
                closed = sdk.IR_CloseFrames(worker_error, len(worker_error))
                assert closed == 0, worker_error.value

            execute_attempt("first")
            first_complete.set()
            if outcome["first"].get("status") != 8:
                return
            retry_allowed.wait(5)
            recipe.stable_window_ms = 0
            execute_attempt("retry")

        worker = threading.Thread(target=run_recipe)
        worker.start()
        assert controller.stable_reply.wait(4), "procedure did not reach blackbody stabilization"
        time.sleep(0.05)

        readback = c.c_double()
        reading = Reading()
        assert sdk.IR_SetBlackbody(handle, 40.0, c.byref(readback), error, len(error)) == 9
        assert sdk.IR_ReadBlackbody(handle, c.byref(reading), error, len(error)) == 9
        assert sdk.IR_CloseDevice(handle, error, len(error)) == 9

        sdk.IR_RequestStop()
        assert first_complete.wait(4), "procedure did not release its resources after cancellation"
        assert "open" not in outcome["first"], outcome["first"]
        assert outcome["first"]["status"] == 8, outcome["first"]
        assert outcome["first"]["result"].failed_step == 4
        assert outcome["first"]["result"].cleanup_status == 0
        assert sdk.IR_ReadBlackbody(handle, c.byref(reading), error, len(error)) == 0, error.value

        # Retry on the same worker after proving the cancelled run released its reservation.
        retry_allowed.set()
        worker.join(8)
        assert not worker.is_alive(), "retry did not finish"
        assert "open" not in outcome["retry"], outcome["retry"]
        assert outcome["retry"]["status"] == 0, outcome["retry"]
        assert outcome["retry"]["result"].completed == 2
        assert outcome["retry"]["result"].failed_step == 0
        assert outcome["retry"]["result"].cleanup_status == 0
        assert outcome["retry"]["thread"] == outcome["first"]["thread"]
        assert sdk.IR_CloseDevice(handle, error, len(error)) == 0, error.value


def exercise_json_recipe_ownership(args, sdk, controller, config):
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *error_args]
    sdk.IR_RunRecipeJson.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.POINTER(RecipeResult), *error_args]
    sdk.IR_RequestStop.argtypes = []
    sdk.IR_RequestStop.restype = None

    with TemporaryDirectory(prefix="json-ownership-", dir=args.fixture_parent) as temporary:
        fixture = Path(temporary)
        (fixture / "frames.raw").write_bytes(b"\x14\x00\x16\x00")
        (fixture / "frames.csv").write_text(
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,1,1,2,RAW16_LE,16,little\n1,1,1,2,RAW16_LE,16,little\n", encoding="utf8")

        kind = "pcn7-lt" if controller.tcp else "dcn1000"
        station = {
            "schema_version": 1,
            "acquisition": {"backend": "raw_replay", "source": str(fixture), "valid_bits": 16},
            "devices": {"bb": {"kind": kind, "connection": config}},
        }
        recipe = {
            "schema_version": 1,
            "recipe_id": "json-cancel-and-retry",
            "steps": [
                {"id": "stable", "type": "blackbody_stabilize", "device": "bb",
                 "target_c": 32.0, "tolerance_c": 0.01, "stable_window_ms": 3000,
                 "timeout_ms": 5000, "require_device_stable": True},
                {"id": "batch", "type": "capture", "frames": 2, "frame_timeout_ms": 0},
                {"id": "noise", "type": "analyze", "method": "temporal_noise",
                 "name": "noise", "input": "batch"},
                {"id": "archive", "type": "save_raw", "input": "batch"},
            ],
        }
        retry_recipe = json.loads(json.dumps(recipe))
        retry_recipe["recipe_id"] = "json-retry-after-cancel"
        retry_recipe["steps"][0]["stable_window_ms"] = 0
        station_json = json.dumps(station).encode()
        recipe_json = json.dumps(recipe).encode()
        retry_json = json.dumps(retry_recipe).encode()

        controller.stable = True
        controller.stable_reply.clear()
        first_output = fixture / "cancelled-run"
        retry_output = fixture / "retry-run"
        outcome = {}
        first_finished = threading.Event()
        retry_allowed = threading.Event()

        def run_recipe():
            error = c.create_string_buffer(1024)
            details = RecipeResult()
            status = sdk.IR_RunRecipeJson(station_json, recipe_json, str(first_output).encode(),
                                          c.byref(details), error, len(error))
            outcome["first"] = status, error.value, details
            first_finished.set()
            if status != 8:
                return

            if not retry_allowed.wait(5):
                outcome["retry"] = -1, b"test did not allow the retry"
                return
            error = c.create_string_buffer(1024)
            status = sdk.IR_RunRecipeJson(station_json, retry_json, str(retry_output).encode(),
                                          None, error, len(error))
            outcome["retry"] = status, error.value

        worker = threading.Thread(target=run_recipe)
        worker.start()
        assert controller.stable_reply.wait(4), "JSON recipe did not reach blackbody stabilization"

        blocked_handle = c.c_uint64()
        error = c.create_string_buffer(1024)
        status = sdk.IR_OpenDevice(2 if controller.tcp else 3, json.dumps(config).encode(),
                                   c.byref(blocked_handle), error, len(error))
        assert status == 9 and blocked_handle.value == 0, (status, error.value)

        second = RecipeResult()
        second_output = fixture / "concurrent-run"
        status = sdk.IR_RunRecipeJson(station_json, retry_json, str(second_output).encode(),
                                      c.byref(second), error, len(error))
        assert status == 9 and second.failed_stage == 2 and not second_output.exists(), (status, error.value)

        sdk.IR_RequestStop()
        assert first_finished.wait(5), "JSON recipe did not finish cancellation"
        assert outcome["first"][0] == 8 and b"stable" in outcome["first"][1].lower(), outcome["first"]
        details = outcome["first"][2]
        assert (details.completed_frames, details.failed_stage, details.failed_step_index, details.cleanup_status) == (0, 4, 0, 0)
        assert not first_output.exists()

        retry_allowed.set()
        worker.join(8)
        assert not worker.is_alive(), "JSON recipe did not finish after cancellation"
        assert outcome["retry"][0] == 0, outcome["retry"]
        result = json.loads((retry_output / "result.json").read_text(encoding="utf8"))
        assert result["completed_frames"] == 2 and result["run_status"] == "COMPLETED"


def exercise_concurrent_polling_regression(args, sdk, controller, config):
    """Hammer IR_ReadBlackbody, IR_SetBlackbody, and IR_CloseDevice from other threads during stabilization."""
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *error_args]
    sdk.IR_CloseDevice.argtypes = [c.c_uint64, *error_args]
    sdk.IR_SetBlackbody.argtypes = [c.c_uint64, c.c_double, c.POINTER(c.c_double), *error_args]
    sdk.IR_ReadBlackbody.argtypes = [c.c_uint64, c.POINTER(Reading), *error_args]
    sdk.IR_OpenFrames.argtypes = [c.c_int32, c.c_char_p, c.c_int32, c.POINTER(Geometry), *error_args]
    sdk.IR_CloseFrames.argtypes = error_args
    sdk.IR_RunNoise.argtypes = [c.POINTER(NoiseRecipe), c.c_char_p, c.POINTER(NoiseResult), *error_args]

    with TemporaryDirectory(prefix="polling-race-", dir=args.fixture_parent) as temporary:
        fixture = Path(temporary)
        (fixture / "frames.raw").write_bytes(b"\x14\x00\x16\x00")
        (fixture / "frames.csv").write_text(
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,1,1,2,RAW16_LE,16,little\n1,1,1,2,RAW16_LE,16,little\n", encoding="utf8")

        error = c.create_string_buffer(1024)
        handle = c.c_uint64()
        kind = 2 if controller.tcp else 3
        status = sdk.IR_OpenDevice(kind, json.dumps(config).encode(), c.byref(handle), error, len(error))
        assert status == 0, error.value

        controller.stable = False
        with controller.stable_reply_cond:
            controller.stable_reply_count = 0

        recipe = NoiseRecipe(b"polling-race-regression", 0, handle.value, 0, 0, 0, 32.0, 0.01,
                             200, 10000, 1, 2, 0)
        outcome = {}

        def run_worker():
            worker_error = c.create_string_buffer(1024)
            geometry = Geometry()
            opened = sdk.IR_OpenFrames(2, str(fixture).encode(), 0, c.byref(geometry),
                                       worker_error, len(worker_error))
            if opened != 0:
                outcome["open"] = (opened, worker_error.value)
                return
            try:
                result = NoiseResult()
                run_status = sdk.IR_RunNoise(c.byref(recipe), None, c.byref(result),
                                             worker_error, len(worker_error))
                outcome["run"] = (run_status, worker_error.value, result)
            finally:
                closed = sdk.IR_CloseFrames(worker_error, len(worker_error))
                outcome["close_frames"] = (closed, worker_error.value)

        worker = threading.Thread(target=run_worker)
        worker.start()
        closed_cleanly = False

        try:
            with controller.stable_reply_cond:
                reached = controller.stable_reply_cond.wait_for(
                    lambda: controller.stable_reply_count >= 1, timeout=5.0)
            assert reached, "procedure did not enter stabilization polling"

            stop_attackers = threading.Event()
            attacker_errors = []
            counts = {"read": 0, "set": 0, "close": 0}

            def hammer_read():
                local_error = c.create_string_buffer(256)
                reading = Reading()
                while not stop_attackers.is_set():
                    code = sdk.IR_ReadBlackbody(handle, c.byref(reading), local_error, len(local_error))
                    if code != 9:
                        attacker_errors.append(("IR_ReadBlackbody", code, local_error.value))
                        break
                    counts["read"] += 1

            def hammer_set():
                local_error = c.create_string_buffer(256)
                readback = c.c_double()
                while not stop_attackers.is_set():
                    code = sdk.IR_SetBlackbody(handle, 35.0, c.byref(readback), local_error, len(local_error))
                    if code != 9:
                        attacker_errors.append(("IR_SetBlackbody", code, local_error.value))
                        break
                    counts["set"] += 1

            def hammer_close():
                local_error = c.create_string_buffer(256)
                while not stop_attackers.is_set():
                    code = sdk.IR_CloseDevice(handle, local_error, len(local_error))
                    if code != 9:
                        attacker_errors.append(("IR_CloseDevice", code, local_error.value))
                        break
                    counts["close"] += 1

            reader = threading.Thread(target=hammer_read)
            setter = threading.Thread(target=hammer_set)
            closer = threading.Thread(target=hammer_close)
            reader.start()
            setter.start()
            closer.start()
            try:
                with controller.stable_reply_cond:
                    polled = controller.stable_reply_cond.wait_for(
                        lambda: controller.stable_reply_count >= 5 or not worker.is_alive(),
                        timeout=6.0)
                assert polled, f"stabilization polling stalled at {controller.stable_reply_count}"
                deadline = time.monotonic() + 5.0
                while (counts["read"] < 20 or counts["set"] < 20 or counts["close"] < 20) and time.monotonic() < deadline and not attacker_errors:
                    time.sleep(0.01)
            finally:
                stop_attackers.set()
                reader.join(3)
                setter.join(3)
                closer.join(3)
                assert not reader.is_alive() and not setter.is_alive() and not closer.is_alive()

            assert not attacker_errors, f"concurrent calls did not return IR_BUSY: {attacker_errors}"
            assert counts["read"] >= 20 and counts["set"] >= 20 and counts["close"] >= 20, counts
            assert worker.is_alive(), f"worker aborted prematurely during concurrent polling: {outcome}"

            controller.stable = True
            worker.join(8)
            assert not worker.is_alive(), "worker did not finish after blackbody stabilized"
            assert "open" not in outcome, outcome
            assert outcome["close_frames"][0] == 0, outcome["close_frames"]
            run_status, run_err, result = outcome["run"]
            assert run_status == 0, (run_status, run_err, result.failed_step, result.cleanup_status)
            assert (result.completed, result.failed_step, result.cleanup_status) == (2, 0, 0)

            reading = Reading()
            assert sdk.IR_ReadBlackbody(handle, c.byref(reading), error, len(error)) == 0, error.value
            assert sdk.IR_CloseDevice(handle, error, len(error)) == 0, error.value
            closed_cleanly = True
        finally:
            controller.stable = True
            if worker.is_alive():
                worker.join(8)
            if not closed_cleanly:
                sdk.IR_CloseDevice(handle, error, len(error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["tcp", "serial"])
    parser.add_argument("library", type=Path)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--fixture-parent", type=Path, required=True)
    args = parser.parse_args()
    sdk = load_sdk(args.library.resolve())
    controller = Controller(args.mode == "tcp")
    stop = threading.Event()

    if controller.tcp:
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.1)
        config = dict(host="127.0.0.1", tcp_port=listener.getsockname()[1], head=0,
                      expected_head_serial="HEAD", timeout_ms=400)

        def serve():
            with listener:
                while not stop.is_set():
                    try:
                        conn, _ = listener.accept()
                    except socket.timeout:
                        continue
                    with conn:
                        conn.settimeout(0.1)

                        def read():
                            try:
                                return conn.recv(4096) or None
                            except socket.timeout:
                                return b""

                        controller.serve(read, conn.sendall, stop)
    else:
        import pty

        master, slave = pty.openpty()
        config = dict(port=os.ttyname(slave), timeout_ms=400)

        def serve():
            def read():
                ready, _, _ = select.select([master], [], [], 0.1)
                return os.read(master, 4096) if ready else b""

            try:
                controller.serve(read, lambda data: os.write(master, data), stop)
            finally:
                os.close(master)
                os.close(slave)

    def worker():
        try:
            serve()
        except Exception as exc:
            controller.error = exc

    server = threading.Thread(target=worker, daemon=True)
    server.start()
    try:
        exercise(sdk, controller, config)
        exercise_wait_blackbody_stable(sdk, controller, config)
        exercise_recipe(args, sdk, controller, config)
        exercise_recipe_ownership(args, sdk, controller, config)
        exercise_json_recipe_ownership(args, sdk, controller, config)
        exercise_concurrent_polling_regression(args, sdk, controller, config)
    finally:
        stop.set()
        server.join(5)
        assert not server.is_alive()
        if controller.error:
            raise controller.error


if __name__ == "__main__":
    main()
