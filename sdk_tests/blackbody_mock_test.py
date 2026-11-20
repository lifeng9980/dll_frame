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


def load_sdk(path, qt_bin):
    if os.name == "nt":
        with os.add_dll_directory(str(path.parent)), os.add_dll_directory(str(qt_bin)):
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
        check(sdk.IR_ReadBlackbody, handle, c.byref(reading))
        assert reading.actual_c == reading.setpoint_c == 30.125

        controller.fail_write = True
        check(sdk.IR_SetBlackbody, handle, 31.25, c.byref(readback))
        assert readback.value == 31.25 and controller.writes == 2

    finally:
        check(sdk.IR_CloseDevice, handle)
    check(sdk.IR_ReadBlackbody, handle, c.byref(reading), expected=invalid_handle)


def exercise_recipe(args, controller, config):
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = os.pathsep.join([str(args.library.resolve().parent), str(args.qt_bin), env["PATH"]])
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["tcp", "serial"])
    parser.add_argument("library", type=Path)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--fixture-parent", type=Path, required=True)
    args = parser.parse_args()
    sdk = load_sdk(args.library.resolve(), args.qt_bin)
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
        exercise_recipe(args, controller, config)
        exercise_recipe_ownership(args, sdk, controller, config)
    finally:
        stop.set()
        server.join(5)
        assert not server.is_alive()
        if controller.error:
            raise controller.error


if __name__ == "__main__":
    main()
