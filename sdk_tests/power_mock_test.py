"""Exercise the IT6332A C API over TCP loopback (Windows/Linux) or Linux PTY serial."""

import sys

sys.dont_write_bytecode = True

import argparse
import copy
import ctypes as c
import json
import os
from pathlib import Path
import select
import socket
import tempfile
import threading
import time


SIMULATOR_DIR = Path(__file__).resolve().parents[1] / "simulators"
sys.path.insert(0, str(SIMULATOR_DIR))
from ir_sim_devices import It6332a, Udp6730  # noqa: E402

IR_BUSY = 9


class Reading(c.Structure):
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


class RecipeResult(c.Structure):
    _fields_ = [
        ("completed_frames", c.c_int32), ("failed_stage", c.c_int32),
        ("failed_step_index", c.c_int32), ("cleanup_status", c.c_int32),
    ]


class RecipeStepEvent(c.Structure):
    _fields_ = [
        ("struct_size", c.c_uint32), ("event_kind", c.c_int32), ("step_index", c.c_int32),
        ("stage", c.c_int32), ("status", c.c_int32), ("reserved", c.c_int32),
        ("duration_ms", c.c_double), ("step_id_utf8", c.c_char_p),
        ("step_type_utf8", c.c_char_p), ("detail_utf8", c.c_char_p),
    ]


RecipeStepCallback = c.CFUNCTYPE(None, c.POINTER(RecipeStepEvent), c.c_void_p)


class RecipeRunOptions(c.Structure):
    _fields_ = [
        ("struct_size", c.c_uint32), ("flags", c.c_uint32),
        ("on_step_event", RecipeStepCallback), ("user_data", c.c_void_p),
    ]


def run_recipe(sdk, station, recipe, output, options=None):
    result = RecipeResult()
    error = c.create_string_buffer(1024)
    status = sdk.IR_RunRecipeJson(json.dumps(station).encode(), json.dumps(recipe).encode(),
                                  os.fsencode(output), options, c.byref(result), error, len(error))
    return (status, result.completed_frames, result.failed_stage,
            result.failed_step_index, result.cleanup_status), error.value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tcp", action="store_true", help="use loopback TCP transport instead of Linux PTY")
    parser.add_argument("library", type=Path)
    parser.add_argument("--fixture-parent", type=Path, default=Path.cwd())
    parser.add_argument("--power-driver", type=Path, default=None)
    args = parser.parse_args()

    if not args.tcp and os.name == "nt":
        raise RuntimeError("The PTY simulator test runs in Linux; pass --tcp on Windows")

    if os.name == "nt":
        with os.add_dll_directory(str(args.library.resolve().parent)):
            sdk = c.CDLL(str(args.library.resolve()))
    else:
        sdk = c.CDLL(str(args.library.resolve()))
    error_args = [c.c_char_p, c.c_int32]
    sdk.IR_OpenDevice.argtypes = [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *error_args]
    sdk.IR_CloseDevice.argtypes = [c.c_uint64, *error_args]
    sdk.IR_SetPowerChannel.argtypes = [c.c_uint64, c.c_int32, c.c_double, c.c_double,
                                       c.POINTER(Reading), *error_args]
    sdk.IR_SetPowerOutput.argtypes = [c.c_uint64, c.c_int32, c.c_int32,
                                      c.POINTER(Reading), *error_args]
    sdk.IR_ReadPower.argtypes = [c.c_uint64, c.c_int32, c.POINTER(Reading), *error_args]
    sdk.IR_WaitPowerCurrentStable.argtypes = [
        c.c_uint64, c.c_int32, c.POINTER(PowerCurrentStableSpec), c.POINTER(Reading), *error_args
    ]
    sdk.IR_CheckRecipeJson.argtypes = [c.c_char_p, c.c_char_p, *error_args]
    sdk.IR_RunRecipeJson.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_void_p, c.POINTER(RecipeResult), *error_args]
    sdk.IR_RequestStop.argtypes = []
    sdk.IR_RequestStop.restype = None

    controller = It6332a()
    stop = threading.Event()
    server_error = []
    commands = []
    command_hook = None
    response_delay = 0.0
    delay_command = b""
    listener = None
    master = slave = None

    if args.tcp:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.05)
        config_dict = {"host": "127.0.0.1", "tcp_port": listener.getsockname()[1], "timeout_ms": 500}

        def serve():
            nonlocal command_hook
            try:
                while not stop.is_set():
                    try:
                        conn, _ = listener.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    with conn:
                        conn.settimeout(0.05)
                        pending = b""
                        while not stop.is_set():
                            try:
                                block = conn.recv(4096)
                            except socket.timeout:
                                continue
                            except (ConnectionResetError, BrokenPipeError, OSError):
                                break
                            if not block:
                                break
                            pending += block
                            while b"\n" in pending:
                                command, pending = pending.split(b"\n", 1)
                                commands.append(command.strip())
                                response = controller.handle(command)
                                if response_delay and command.strip() == delay_command:
                                    time.sleep(response_delay)
                                if command_hook and command.strip() == command_hook[0]:
                                    _, callback = command_hook
                                    command_hook = None
                                    callback()
                                if response:
                                    conn.sendall(response)
            except Exception as error:
                if not stop.is_set():
                    server_error.append(error)
    else:
        import pty
        import tty

        master, slave = pty.openpty()
        tty.setraw(slave)
        config_dict = {"port": os.ttyname(slave), "baud_rate": 115200, "timeout_ms": 500}

        def serve():
            nonlocal command_hook
            pending = b""
            try:
                while not stop.is_set():
                    ready, _, _ = select.select([master], [], [], 0.05)
                    if not ready:
                        continue
                    pending += os.read(master, 4096)
                    while b"\n" in pending:
                        command, pending = pending.split(b"\n", 1)
                        commands.append(command.strip())
                        response = controller.handle(command)
                        if response_delay and command.strip() == delay_command:
                            time.sleep(response_delay)
                        if command_hook and command.strip() == command_hook[0]:
                            _, callback = command_hook
                            command_hook = None
                            callback()
                        if response:
                            os.write(master, response)
            except Exception as error:
                if not stop.is_set():
                    server_error.append(error)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    handle = c.c_uint64()
    error = c.create_string_buffer(1024)
    config = json.dumps(config_dict).encode()

    def check(status, expected=0):
        assert status == expected, (status, expected, error.value.decode(errors="replace"))

    try:
        check(sdk.IR_OpenDevice(4, config, c.byref(handle), error, len(error)))
        duplicate = c.c_uint64()
        check(sdk.IR_OpenDevice(4, config, c.byref(duplicate), error, len(error)), 9)
        assert duplicate.value == 0

        reading = Reading()
        check(sdk.IR_SetPowerChannel(handle, 1, 12.0, 2.0, c.byref(reading), error, len(error)))
        assert abs(reading.setpoint_v - 12.0) < 1e-9
        assert abs(reading.current_limit_a - 2.0) < 1e-9
        assert reading.output_enabled == 0 and reading.measured_v == 0.0

        check(sdk.IR_SetPowerOutput(handle, 1, 1, c.byref(reading), error, len(error)))
        assert reading.output_enabled == 1 and reading.measured_v == 12.0
        assert abs(reading.measured_a - 0.05) < 1e-9

        check(sdk.IR_ReadPower(handle, 1, c.byref(reading), error, len(error)))
        assert reading.output_enabled == 1 and reading.setpoint_v == 12.0
        controller.drop_command = "INST:NSEL?"
        controller.drop_count = 1
        check(sdk.IR_ReadPower(handle, 1, c.byref(reading), error, len(error)))
        assert controller.drop_count == 0 and reading.setpoint_v == 12.0
        controller.drop_count = 4
        check(sdk.IR_ReadPower(handle, 1, c.byref(reading), error, len(error)), 7)
        assert controller.drop_count == 0
        check(sdk.IR_ReadPower(handle, 1, c.byref(reading), error, len(error)))
        check(sdk.IR_ReadPower(handle, 4, c.byref(reading), error, len(error)), 2)
        check(sdk.IR_SetPowerOutput(handle, 1, 0, c.byref(reading), error, len(error)))
        assert reading.output_enabled == 0 and reading.measured_v == 0.0

        assert c.sizeof(PowerCurrentStableSpec) == 72
        controller.cooler_profile = True
        controller._ch1_on_reads = 0
        check(sdk.IR_SetPowerOutput(handle, 1, 1, c.byref(reading), error, len(error)))
        assert abs(reading.measured_a - 0.55) < 1e-9, reading.measured_a
        stable_spec = PowerCurrentStableSpec(
            expected_voltage_v=12.0,
            voltage_tolerance_v=0.2,
            max_current_a=2.0,
            startup_threshold_a=0.3,
            stable_threshold_a=0.3,
            max_variation_a=0.05,
            startup_required_samples=3,
            startup_timeout_ms=2000,
            stable_required_samples=3,
            sample_interval_ms=5,
            timeout_ms=3000,
            reserved=0,
        )
        check(sdk.IR_WaitPowerCurrentStable(handle, 1, c.byref(stable_spec), c.byref(reading), error, len(error)))
        assert reading.measured_a < 0.3, reading.measured_a

        controller.cooler_profile = False
        timeout_spec = PowerCurrentStableSpec(12.0, 0.2, 2.0, 0.3, 0.3, 0.05, 2, 60, 2, 10, 400, 0)
        check(sdk.IR_WaitPowerCurrentStable(handle, 1, c.byref(timeout_spec), c.byref(reading), error, len(error)), 7)

        over_spec = PowerCurrentStableSpec(12.0, 0.2, 0.02, 0.3, 0.3, 0.05, 0, 0, 2, 10, 400, 0)
        check(sdk.IR_WaitPowerCurrentStable(handle, 1, c.byref(over_spec), c.byref(reading), error, len(error)), 6)

        cancel_spec = PowerCurrentStableSpec(12.0, 0.2, 2.0, 0.3, 0.3, 0.05, 0, 0, 2, 10, 2000, 0)
        concurrent_statuses = []

        def try_concurrent_wait_and_cancel():
            other_reading = Reading()
            other_error = c.create_string_buffer(1024)
            concurrent_statuses.append(sdk.IR_WaitPowerCurrentStable(
                handle, 1, c.byref(cancel_spec), c.byref(other_reading), other_error, len(other_error)
            ))
            sdk.IR_RequestStop()

        command_hook = (b"MEAS:CURR?", try_concurrent_wait_and_cancel)
        check(sdk.IR_WaitPowerCurrentStable(handle, 1, c.byref(cancel_spec), c.byref(reading), error, len(error)), 8)
        assert concurrent_statuses == [IR_BUSY], concurrent_statuses

        check(sdk.IR_SetPowerOutput(handle, 1, 0, c.byref(reading), error, len(error)))
        assert reading.output_enabled == 0 and reading.measured_v == 0.0
        check(sdk.IR_CloseDevice(handle, error, len(error)))
        handle.value = 0

        # Verify that a reply arriving after the startup deadline is rejected,
        # without using this intentionally delayed connection for later checks.
        delayed_handle = c.c_uint64()
        check(sdk.IR_OpenDevice(4, config, c.byref(delayed_handle), error, len(error)))
        controller.cooler_profile = True
        controller._ch1_on_reads = 0
        check(sdk.IR_SetPowerOutput(
            delayed_handle, 1, 1, c.byref(reading), error, len(error)
        ))
        delay_command = b"MEAS:CURR?"
        response_delay = 0.05
        delayed_startup_spec = PowerCurrentStableSpec(
            12.0, 0.2, 2.0, 0.3, 0.3, 0.05, 1, 30, 2, 0, 1000, 0
        )
        delayed_status = sdk.IR_WaitPowerCurrentStable(
            delayed_handle, 1, c.byref(delayed_startup_spec), c.byref(reading), error, len(error)
        )
        response_delay = 0.0
        delay_command = b""
        assert delayed_status == 7 and b"startup load current" in error.value.lower(), (
            delayed_status, error.value, commands[-12:], server_error
        )
        check(sdk.IR_CloseDevice(delayed_handle, error, len(error)))
        controller.output[0] = False
        controller.cooler_profile = False

        with tempfile.TemporaryDirectory(prefix="ir-power-recipe-", dir=args.fixture_parent) as temporary:
            root = Path(temporary)
            raw = root / "raw16"
            raw.mkdir()
            (raw / "frames.raw").write_bytes(b"\x14\x00\x16\x00")
            (raw / "frames.csv").write_text(
                "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
                "0,1,1,2,RAW16_LE,16,little\n1,1,1,2,RAW16_LE,16,little\n", encoding="utf-8")
            station = {
                "schemav": 1,
                "acquisition": {"backend": "raw_replay", "source": str(raw), "valid_bits": 16},
                "devices": {"supply": {"kind": "it6332a", "connection": json.loads(config)}},
            }
            read_failure_recipe = {
                "schemav": 1,
                "recipe_id": "serial-power-reconnect-exhausted",
                "steps": [
                    {"id": "read", "type": "power_read", "device": "supply", "channel": 1},
                    {"id": "batch", "type": "capture", "frames": 2, "frame_timeout_ms": 0, "output": "batch"},
                    {"id": "noise", "type": "analyze", "method": "temporal_noise",
                     "name": "noise", "input": "batch"},
                    {"id": "archive", "type": "save_raw", "input": "batch"},
                ],
            }
            read_failure_json = json.dumps(read_failure_recipe).encode()
            check(sdk.IR_CheckRecipeJson(json.dumps(station).encode(), read_failure_json,
                                         error, len(error)))
            controller.drop_command = "INST:NSEL?"
            controller.drop_count = 4
            failed_output = root / "failed-run"
            report, detail = run_recipe(sdk, station, read_failure_recipe, failed_output)
            assert report == (7, 0, 4, 0, 0) and b"read" in detail.lower(), (report, detail)
            assert controller.drop_count == 0 and (failed_output / "result.json").is_file()

            recipe = {
                "schemav": 1,
                "recipe_id": "serial-power-recipe",
                "steps": [
                    {"id": "set", "type": "power_set", "device": "supply", "channel": 1,
                     "voltage_v": 12.0, "current_limit_a": 2.0},
                    {"id": "read", "type": "power_read", "device": "supply", "channel": 1},
                    {"id": "batch", "type": "capture", "frames": 2, "frame_timeout_ms": 0, "output": "batch"},
                    {"id": "noise", "type": "analyze", "method": "temporal_noise",
                     "name": "noise", "input": "batch"},
                    {"id": "archive", "type": "save_raw", "input": "batch"},
                ],
            }
            station_json = json.dumps(station).encode()
            recipe_json = json.dumps(recipe).encode()
            check(sdk.IR_CheckRecipeJson(station_json, recipe_json, error, len(error)))
            output = root / "run"
            report, detail = run_recipe(sdk, station, recipe, output)
            assert report == (0, 2, 0, -1, 0), (report, detail)
            record = json.loads((output / "result.json").read_text(encoding="utf-8"))
            assert record["run_status"] == "COMPLETED" and record["completed_frames"] == 2
            readings = record["instrument_readings"]
            assert readings[0]["reading"]["setpoint_v"] == 12.0
            assert readings[1]["reading"]["current_limit_a"] == 2.0

            enable = {"id": "enable", "type": "power_output", "device": "supply",
                      "channel": 1, "enabled": True, "off_after_run": True}
            capture_only = dict(recipe, steps=recipe["steps"][2:])
            powered_recipe = dict(recipe, steps=[enable, *capture_only["steps"]])

            missing_cleanup = dict(enable)
            del missing_cleanup["off_after_run"]
            incomplete_recipe = dict(recipe, steps=[missing_cleanup, *capture_only["steps"]])
            check(sdk.IR_CheckRecipeJson(station_json, json.dumps(incomplete_recipe).encode(), error, len(error)), 2)
            assert b"off_after_run" in error.value

            small_station = copy.deepcopy(station)
            small_station["acquisition"]["max_batch_bytes"] = 2
            commands.clear()
            report, detail = run_recipe(sdk, small_station, powered_recipe, root / "over-budget")
            assert report == (2, 0, 3, 1, 0) and b"max_batch_bytes" in detail, (report, detail)
            assert not commands and (root / "over-budget" / "result.json").is_file()

            invalid_roi = {"id": "invalid_roi", "type": "analyze", "name": "roi", "method": "roi_mean",
                           "input": "batch", "params": {"x": 1, "y": 0, "width": 1, "height": 1}}
            roi_recipe = dict(recipe, steps=[*powered_recipe["steps"][:-1], invalid_roi, powered_recipe["steps"][-1]])
            report, detail = run_recipe(sdk, station, roi_recipe, root / "outside-image")
            assert report == (2, 0, 3, 3, 0) and b"ROI" in detail, (report, detail)
            assert not commands and (root / "outside-image" / "result.json").is_file()

            read_then_enable = dict(recipe, steps=[recipe["steps"][1], *powered_recipe["steps"]])
            command_hook = (b"CHAN:OUTP?", sdk.IR_RequestStop)
            report, detail = run_recipe(sdk, station, read_then_enable, root / "cancelled-read")
            assert report == (8, 0, 4, 0, 0), (report, detail)
            assert not controller.output[0] and b"CHAN:OUTP ON" not in commands
            assert (root / "cancelled-read" / "result.json").is_file()

            commands.clear()
            command_hook = (b"CHAN:OUTP?", sdk.IR_RequestStop)
            report, detail = run_recipe(sdk, station, powered_recipe, root / "cancelled-enable")
            assert report == (8, 0, 4, 0, 0), (report, detail)
            assert commands.count(b"CHAN:OUTP ON") == commands.count(b"CHAN:OUTP OFF") == 1
            assert not controller.output[0] and (root / "cancelled-enable" / "result.json").is_file()

            short_recipe = copy.deepcopy(powered_recipe)
            short_recipe["steps"][1]["frames"] = 3
            commands.clear()
            report, detail = run_recipe(sdk, station, short_recipe, root / "failed-capture")
            assert report == (11, 2, 5, 1, 0) and b"batch" in detail, (report, detail)
            assert commands.count(b"CHAN:OUTP ON") == commands.count(b"CHAN:OUTP OFF") == 1
            assert not controller.output[0] and (root / "failed-capture" / "result.json").is_file()

            controller.drop_command = "CHAN:OUTP OFF"
            controller.drop_count = 1
            report, detail = run_recipe(sdk, station, powered_recipe, root / "failed-cleanup")
            assert report == (6, 2, 8, 0, 6) and b"enable" in detail, (report, detail)
            assert (root / "failed-cleanup" / "result.json").is_file()
            controller.drop_count = 1
            report, detail = run_recipe(sdk, station, short_recipe, root / "two-failures")
            assert report == (11, 2, 5, 1, 6), (report, detail)
            assert (root / "two-failures" / "result.json").is_file()

            @RecipeStepCallback
            def cancel_after_archive(event, _):
                step = event.contents
                if step.event_kind == 2 and step.step_id_utf8 == b"archive":
                    sdk.IR_RequestStop()

            options = RecipeRunOptions(c.sizeof(RecipeRunOptions), 0, cancel_after_archive, None)
            controller.drop_count = 1
            cancelled_output = root / "late-cancel-with-cleanup-failure"
            report, detail = run_recipe(sdk, station, powered_recipe, cancelled_output, c.byref(options))
            assert report == (8, 2, 7, -1, 6), (report, detail)
            record = json.loads((cancelled_output / "result.json").read_text(encoding="utf-8"))
            assert record["run_status"] == "CANCELLED" and record["cleanup_status"] == 6, record

            blocked = []
            offline = dict(station, devices={})
            second = root / "during-cleanup"
            command_hook = (b"CHAN:OUTP OFF", lambda: blocked.append(run_recipe(sdk, offline, capture_only, second)))
            report, detail = run_recipe(sdk, station, powered_recipe, root / "recovered")
            assert report == (0, 2, 0, -1, 0), (report, detail)
            assert blocked and blocked[0][0] == (9, 0, 2, -1, 0), blocked
            assert not controller.output[0] and not second.exists()
            report, detail = run_recipe(sdk, offline, capture_only, second)
            assert report == (0, 2, 0, -1, 0), (report, detail)
            test_udp6730_and_external_power(sdk, root, station, capture_only, args.power_driver)
    finally:
        stop.set()
        if listener is not None:
            try:
                listener.close()
            except OSError:
                pass
        thread.join(2)
        if master is not None:
            os.close(master)
        if slave is not None:
            os.close(slave)
    assert not thread.is_alive()
    assert not server_error, server_error


def test_udp6730_and_external_power(sdk, root, base_station, base_recipe, power_driver=None):
    # 1. Verify IT6332A multi-channel shared handle via direct C API
    it_ctrl = It6332a()
    it_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    it_srv.bind(("127.0.0.1", 0))
    it_srv.listen(4)
    it_port = it_srv.getsockname()[1]

    udp_ctrl = Udp6730(voltage=12.0, current_limit=2.0, output=False)
    udp_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    udp_srv.bind(("127.0.0.1", 0))
    udp_srv.listen(4)
    udp_port = udp_srv.getsockname()[1]

    stop_srv = threading.Event()

    def serve(sock, handler):
        sock.settimeout(0.1)
        while not stop_srv.is_set():
            try:
                conn, _ = sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                conn.settimeout(0.1)
                buf = b""
                while not stop_srv.is_set():
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        reply = handler(line)
                        if reply:
                            conn.sendall(reply)

    t_it = threading.Thread(target=serve, args=(it_srv, it_ctrl.handle), daemon=True)
    t_udp = threading.Thread(target=serve, args=(udp_srv, udp_ctrl.handle), daemon=True)
    t_it.start()
    t_udp.start()
    try:
        err = c.create_string_buffer(1024)
        it_h = c.c_uint64()
        it_cfg = json.dumps({"host": "127.0.0.1", "tcp_port": it_port}).encode()
        assert sdk.IR_OpenDevice(4, it_cfg, c.byref(it_h), err, len(err)) == 0, err.value
        dup_h = c.c_uint64(77)
        assert sdk.IR_OpenDevice(4, it_cfg, c.byref(dup_h), err, len(err)) == IR_BUSY, err.value
        assert dup_h.value == 0
        r1 = Reading()
        r2 = Reading()
        assert sdk.IR_SetPowerChannel(it_h, 1, 12.0, 2.0, c.byref(r1), err, len(err)) == 0, err.value
        assert sdk.IR_SetPowerChannel(it_h, 2, 5.0, 1.5, c.byref(r2), err, len(err)) == 0, err.value
        assert sdk.IR_SetPowerOutput(it_h, 1, 1, c.byref(r1), err, len(err)) == 0, err.value
        assert sdk.IR_ReadPower(it_h, 2, c.byref(r2), err, len(err)) == 0, err.value
        assert r1.output_enabled == 1 and r2.output_enabled == 0
        assert abs(r1.setpoint_v - 12.0) < 0.01 and abs(r2.setpoint_v - 5.0) < 0.01
        assert sdk.IR_CloseDevice(it_h, err, len(err)) == 0, err.value

        # 2. Verify UDP6730 C API (read + output supported, set unsupported)
        udp_h = c.c_uint64()
        assert sdk.IR_OpenDevice(9, json.dumps({"host": "127.0.0.1", "tcp_port": udp_port}).encode(),
                                 c.byref(udp_h), err, len(err)) == 0, err.value
        ur = Reading()
        assert sdk.IR_ReadPower(udp_h, 1, c.byref(ur), err, len(err)) == 0, err.value
        assert ur.output_enabled == 0 and abs(ur.setpoint_v - 12.0) < 0.01
        assert sdk.IR_SetPowerOutput(udp_h, 1, 1, c.byref(ur), err, len(err)) == 0, err.value
        assert ur.output_enabled == 1 and abs(ur.measured_v - 12.0) < 0.01
        assert sdk.IR_SetPowerChannel(udp_h, 1, 12.0, 2.0, c.byref(ur), err, len(err)) == 10  # IR_UNSUPPORTED
        assert sdk.IR_ReadPower(udp_h, 2, c.byref(ur), err, len(err)) == 2  # IR_BAD_ARGUMENT
        assert sdk.IR_SetPowerOutput(udp_h, 1, 0, c.byref(ur), err, len(err)) == 0, err.value
        assert sdk.IR_CloseDevice(udp_h, err, len(err)) == 0, err.value

        # 3. Verify recipe capability precheck on UDP6730 & missing external power driver
        udp_station = dict(
            base_station,
            devices={"cooler_power": {"kind": "udp6730", "connection": {"host": "127.0.0.1", "tcp_port": udp_port}}},
        )
        udp_recipe_ok = dict(base_recipe, steps=[
            {"id": "p_on", "type": "power_output", "device": "cooler_power", "channel": 1, "enabled": True, "off_after_run": True},
            {"id": "p_rd", "type": "power_read", "device": "cooler_power", "channel": 1},
            *base_recipe["steps"],
        ])
        report, detail = run_recipe(sdk, udp_station, udp_recipe_ok, root / "udp-ok")
        assert report == (0, 2, 0, -1, 0), (report, detail)
        assert not udp_ctrl.output

        udp_recipe_set = dict(base_recipe, steps=[
            {"id": "p_set", "type": "power_set", "device": "cooler_power", "channel": 1, "voltage_v": 12.0, "current_limit_a": 2.0},
        ])
        status = sdk.IR_CheckRecipeJson(json.dumps(udp_station).encode(), json.dumps(udp_recipe_set).encode(), err, len(err))
        assert status == 10 and b"power_set" in err.value, (status, err.value)

        ext_bad = dict(
            base_station,
            devices={"dp": {"kind": "external-power", "connection": {"driver_path": str(root / "missing.dll"), "driver_config": {}}}},
        )
        status = sdk.IR_CheckRecipeJson(json.dumps(ext_bad).encode(), json.dumps(base_recipe).encode(), err, len(err))
        assert status != 0, status

        if power_driver is not None:
            # 4. Full external power driver lifecycle, driver_id mismatch, capability check, and failed-close endpoint release
            ext_h = c.c_uint64()
            bad_id_cfg = {"driver_path": str(power_driver), "driver_id": "wrong_id", "driver_config": {"ok": True}}
            assert sdk.IR_OpenDevice(10, json.dumps(bad_id_cfg).encode(), c.byref(ext_h), err, len(err)) == 2

            bad_cfg = {"driver_path": str(power_driver), "driver_id": "mock_power", "driver_config": {"invalid": True}}
            assert sdk.IR_OpenDevice(10, json.dumps(bad_cfg).encode(), c.byref(ext_h), err, len(err)) == 2

            good_cfg = {"driver_path": str(power_driver), "driver_id": "mock_power", "driver_config": {"fail_close": True}}
            assert sdk.IR_OpenDevice(10, json.dumps(good_cfg).encode(), c.byref(ext_h), err, len(err)) == 0, err.value
            er = Reading()
            assert sdk.IR_ReadPower(ext_h, 1, c.byref(er), err, len(err)) == 0, err.value
            assert er.output_enabled == 0
            assert sdk.IR_SetPowerOutput(ext_h, 1, 1, c.byref(er), err, len(err)) == 0, err.value
            assert er.output_enabled == 1 and abs(er.measured_v - 12.0) < 0.01
            assert sdk.IR_SetPowerChannel(ext_h, 1, 10.0, 1.0, c.byref(er), err, len(err)) == 10  # IR_UNSUPPORTED
            assert sdk.IR_CloseDevice(ext_h, err, len(err)) == 6  # IR_IO_ERROR propagated from mockClose
            # Endpoint must be released despite failed close, allowing immediate re-open
            assert sdk.IR_OpenDevice(10, json.dumps(good_cfg).encode(), c.byref(ext_h), err, len(err)) == 0, err.value
            good_cfg2 = {"driver_path": str(power_driver), "driver_id": "mock_power", "driver_config": {"ok": True}}
            ext_station = dict(base_station, devices={"ext": {"kind": "external-power", "connection": good_cfg2}})
            ext_set = dict(base_recipe, steps=[{"id": "s", "type": "power_set", "device": "ext", "channel": 1, "voltage_v": 5.0, "current_limit_a": 1.0}])
            assert sdk.IR_CheckRecipeJson(json.dumps(ext_station).encode(), json.dumps(ext_set).encode(), err, len(err)) == 10
            assert sdk.IR_CloseDevice(ext_h, err, len(err)) == 6

            if os.name == "nt":
                # 5. Verify control_port config validation (no connection during validate) & bounded error when offline
                for bad_ctrl in (
                    {"control_port": "29101"},
                    {"control_port": 0},
                    {"control_port": 70000},
                    {"control_port": 29101, "host": "127.0.0.1"},
                ):
                    bad_ctrl_cfg = {"driver_path": str(power_driver), "driver_id": "mock_power", "driver_config": bad_ctrl}
                    assert sdk.IR_OpenDevice(10, json.dumps(bad_ctrl_cfg).encode(), c.byref(ext_h), err, len(err)) == 2

                # Pick an unused ephemeral port; IR_CheckRecipeJson must pass without connecting
                probe_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                probe_sock.bind(("127.0.0.1", 0))
                unused_port = probe_sock.getsockname()[1]
                probe_sock.close()
                offline_cfg = {
                    "driver_path": str(power_driver),
                    "driver_id": "mock_power",
                    "driver_config": {"control_port": unused_port, "timeout_ms": 200},
                }
                offline_station = dict(base_station, devices={"ext": {"kind": "external-power", "connection": offline_cfg}})
                assert sdk.IR_CheckRecipeJson(json.dumps(offline_station).encode(), json.dumps(base_recipe).encode(), err, len(err)) == 0, err.value
                assert sdk.IR_CheckRecipeJson(json.dumps(offline_station).encode(), json.dumps(ext_set).encode(), err, len(err)) == 10
                t0 = time.monotonic()
                rc_offline = sdk.IR_OpenDevice(10, json.dumps(offline_cfg).encode(), c.byref(ext_h), err, len(err))
                elapsed = time.monotonic() - t0
                assert rc_offline in (6, 7), (rc_offline, err.value)
                assert elapsed < 3.0, elapsed
    finally:
        stop_srv.set()
        it_srv.close()
        udp_srv.close()
        t_it.join(2)
        t_udp.join(2)


if __name__ == "__main__":
    main()
