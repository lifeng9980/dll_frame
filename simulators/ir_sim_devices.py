#!/usr/bin/env python3
"""Small TCP and serial simulators for the IR SDK's device examples."""

import argparse
import os
import select
import signal
import socket
import threading
import time


DOC058_ALIASES = {
    "name": (ord("0"), 135),
    "serial": (ord("0"), 136),
    "head": (ord("0"), 56),
    "mode": (ord("0"), 73),
    "temperature": (ord("0"), 114),
    "temp": (ord("0"), 114),
    "read_temp": (ord("0"), 114),
    "setpoint": (None, 97),
    "read_setpoint": (ord("0"), 97),
    "write_setpoint": (ord("1"), 97),
    "set": (ord("1"), 97),
    "set_temp": (ord("1"), 97),
    "low_limit": (ord("0"), 67),
    "high_limit": (ord("0"), 66),
    "stable": (ord("0"), 92),
}


def parse_doc058_drop_spec(spec):
    cleaned = spec.strip().lower()
    if not cleaned:
        return None
    if cleaned in ("*", "any"):
        return ("any", None)
    if cleaned in DOC058_ALIASES:
        return DOC058_ALIASES[cleaned]
    direction = None
    code_text = cleaned
    if ":" in cleaned:
        prefix, code_text = cleaned.split(":", 1)
        if prefix in ("0", "read"):
            direction = ord("0")
        elif prefix in ("1", "write"):
            direction = ord("1")
        else:
            raise ValueError(f"Unknown DOC058 drop direction: {spec}")
    try:
        function = int(code_text, 10)
    except ValueError as error:
        raise ValueError(f"Unknown DOC058 drop command: {spec}") from error
    if not 0 <= function <= 255:
        raise ValueError(f"DOC058 function code must be in 0..255: {spec}")
    return (direction, function)


class Blackbody:
    def __init__(self, tcp, temperature, stable, drop_command="", drop_count=0, log_io=False):
        self.tcp = tcp
        self.target = temperature
        self.stable = stable
        self.drop_command = drop_command
        self.drop_count = drop_count
        self.log_io = log_io
        self._drop_spec = parse_doc058_drop_spec(drop_command) if drop_command else None

    def _should_drop(self, direction, function):
        if not self.drop_command or self.drop_count <= 0:
            return False
        spec = self._drop_spec
        if spec is None:
            spec = parse_doc058_drop_spec(self.drop_command)
            self._drop_spec = spec
        if spec == ("any", None):
            return True
        expected_dir, expected_fn = spec
        if expected_fn != function:
            return False
        return expected_dir is None or expected_dir == direction

    def handle(self, request):
        if len(request) < 3 or request[0] != 0x1B:
            return b""
        direction = request[1]
        function = request[2]
        argument = request[3:]
        if direction == ord("1") and function == 97:
            try:
                self.target = float(argument.split(b",", 1)[0])
            except ValueError:
                return b"\x1b" + bytes([function]) + b"\x15\r"
            payload = b"\x06"
        elif direction == ord("0"):
            payloads = {
                135: b"PCN7-LT" if self.tcp else b"DCN1000",
                136: b"IR-SIMULATOR",
                56: b"0,HEAD-SIMULATOR",
                73: b"0",
                114: f"{self.target:.3f}".encode("ascii"),
                97: f"{self.target:.3f}".encode("ascii"),
                67: b"-60",
                66: b"100",
                92: b"1" if self.stable else b"0",
            }
            payload = payloads.get(function, b"")
        else:
            return b""
        if self._should_drop(direction, function):
            self.drop_count -= 1
            if self.log_io:
                print(
                    f"[DROP] dir={chr(direction)} fn={function} req={request!r} remaining={self.drop_count}",
                    flush=True,
                )
            return None
        return b"\x1b" + bytes([function]) + payload + b"\r"


class It6332a:
    def __init__(self, drop_command="", drop_count=0, log_io=False):
        self.channel = 1
        self.voltage = [0.0, 0.0, 0.0]
        self.current_limit = [1.0, 1.0, 1.0]
        self.output = [False, False, False]
        self.drop_command = drop_command
        self.drop_count = drop_count
        self.log_io = log_io

    def _matches_drop(self, upper):
        if not self.drop_command or self.drop_count <= 0:
            return False
        target = self.drop_command.strip().upper()
        if target in ("*", "ANY"):
            return True
        return upper == target or upper.startswith(target + " ")

    def handle(self, command):
        command = command.strip().decode("ascii", errors="replace")
        upper = command.upper()
        if self._matches_drop(upper):
            self.drop_count -= 1
            if self.log_io:
                print(f"[DROP] cmd={command!r} remaining={self.drop_count}", flush=True)
            return None
        if upper == "*IDN?":
            return b"ITECH,IT6332A,IR-SIMULATOR,1.0\n"
        if upper == "SYST:REM":
            return None
        if upper.startswith("INST:NSEL "):
            channel = int(command.split(maxsplit=1)[1])
            if 1 <= channel <= 3:
                self.channel = channel
            return None
        index = self.channel - 1
        if upper == "INST:NSEL?":
            return f"{self.channel}\n".encode("ascii")
        if upper == "VOLT?":
            return f"{self.voltage[index]:.6f}\n".encode("ascii")
        if upper.startswith("VOLT "):
            self.voltage[index] = float(command.split(maxsplit=1)[1])
            return None
        if upper == "CURR?":
            return f"{self.current_limit[index]:.6f}\n".encode("ascii")
        if upper.startswith("CURR "):
            self.current_limit[index] = float(command.split(maxsplit=1)[1])
            return None
        if upper == "MEAS:VOLT?":
            value = self.voltage[index] if self.output[index] else 0.0
            return f"{value:.6f}\n".encode("ascii")
        if upper == "MEAS:CURR?":
            value = 0.05 if self.output[index] else 0.0
            return f"{value:.6f}\n".encode("ascii")
        if upper == "CHAN:OUTP?":
            return (b"1\n" if self.output[index] else b"0\n")
        if upper == "CHAN:OUTP ON":
            self.output[index] = True
            return None
        if upper == "CHAN:OUTP OFF":
            self.output[index] = False
            return None
        return b""


def send_response(write_fn, response, fragment=False, log_io=False):
    if not response:
        return
    if log_io:
        print(f"[TX] {response!r}", flush=True)
    if not fragment or len(response) <= 1:
        write_fn(response)
        return
    step = max(1, min(3, len(response) // 2))
    for offset in range(0, len(response), step):
        write_fn(response[offset:offset + step])
        if offset + step < len(response):
            time.sleep(0.01)


def serve_tcp(host, port, controller, stop, fragment=False, log_io=False):
    delimiter = b"\r"
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((host, port))
        listener.listen()
        listener.settimeout(0.25)
        actual_port = listener.getsockname()[1]
        print(f"TCP simulator listening at {host}:{actual_port}", flush=True)
        while not stop.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(0.25)
                pending = b""
                while not stop.is_set():
                    try:
                        block = connection.recv(4096)
                    except socket.timeout:
                        continue
                    except (ConnectionResetError, BrokenPipeError, OSError):
                        break
                    if not block:
                        break
                    pending += block
                    disconnected = False
                    while delimiter in pending:
                        request, pending = pending.split(delimiter, 1)
                        if log_io:
                            print(f"[RX] {request!r}", flush=True)
                        response = controller.handle(request)
                        try:
                            send_response(connection.sendall, response, fragment=fragment, log_io=log_io)
                        except (ConnectionResetError, BrokenPipeError, OSError):
                            disconnected = True
                            break
                    if disconnected:
                        break


def open_serial(port, baud):
    if os.name == "nt":
        try:
            import serial
        except ImportError as error:
            raise RuntimeError("Windows COM simulation requires optional pyserial; TCP simulation needs no extra package") from error
        handle = serial.Serial(port, baudrate=baud, timeout=0.1, write_timeout=1)
        return handle

    import termios
    import tty

    descriptor = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(descriptor)
    attributes = termios.tcgetattr(descriptor)
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        os.close(descriptor)
        raise RuntimeError(f"This platform does not expose a {baud} baud termios setting")
    attributes[4] = speed
    attributes[5] = speed
    termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
    return descriptor


def serve_serial(port, baud, controller, delimiter, stop, fragment=False, log_io=False):
    handle = open_serial(port, baud)
    print(f"Serial simulator attached to {port} at {baud} baud", flush=True)
    pending = b""
    write_fn = handle.write if os.name == "nt" else (lambda data: os.write(handle, data))
    try:
        while not stop.is_set():
            if os.name == "nt":
                block = handle.read(4096)
            else:
                ready, _, _ = select.select([handle], [], [], 0.25)
                block = os.read(handle, 4096) if ready else b""
            if not block:
                continue
            pending += block
            while delimiter in pending:
                request, pending = pending.split(delimiter, 1)
                if log_io:
                    print(f"[RX] {request!r}", flush=True)
                response = controller.handle(request)
                send_response(write_fn, response, fragment=fragment, log_io=log_io)
    finally:
        if os.name == "nt":
            handle.close()
        else:
            os.close(handle)


def serve_pty(controller, delimiter, baud, stop, fragment=False, log_io=False):
    if os.name == "nt":
        raise RuntimeError("PTY mode is available on Linux; on Windows use a virtual COM pair and --device")
    import pty
    import termios
    import tty

    master, slave = pty.openpty()
    tty.setraw(slave)
    attributes = termios.tcgetattr(slave)
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        os.close(master)
        os.close(slave)
        raise RuntimeError(f"This platform does not expose a {baud} baud termios setting")
    attributes[4] = speed
    attributes[5] = speed
    termios.tcsetattr(slave, termios.TCSANOW, attributes)
    print(f"SDK_SERIAL_PORT={os.ttyname(slave)}", flush=True)
    pending = b""
    try:
        while not stop.is_set():
            ready, _, _ = select.select([master], [], [], 0.25)
            if not ready:
                continue
            pending += os.read(master, 4096)
            while delimiter in pending:
                request, pending = pending.split(delimiter, 1)
                if log_io:
                    print(f"[RX] {request!r}", flush=True)
                response = controller.handle(request)
                send_response(lambda data: os.write(master, data), response, fragment=fragment, log_io=log_io)
    finally:
        os.close(master)
        os.close(slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("blackbody-tcp", "blackbody-serial", "power-serial",
                                          "blackbody-pty", "power-pty"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=21010, help="TCP port for blackbody-tcp")
    parser.add_argument("--device", help="serial port path or COM name")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--temperature", type=float, default=24.875)
    parser.add_argument("--unstable", action="store_true", help="report the blackbody as not stable")
    parser.add_argument("--drop-command", default="",
                        help="command name or DOC058 function code whose reply is dropped (e.g. 114, set, VOLT?)")
    parser.add_argument("--drop-count", type=int, default=None,
                        help="number of matching commands to drop (defaults to 1 when --drop-command is set)")
    parser.add_argument("--fragment", action="store_true",
                        help="split replies into small delayed chunks to exercise transport reassembly")
    parser.add_argument("--log-io", action="store_true",
                        help="log received requests, dropped commands, and transmitted replies")
    args = parser.parse_args()

    drop_command = args.drop_command.strip()
    drop_count = args.drop_count
    if drop_count is None:
        drop_count = 1 if drop_command else 0
    if drop_count < 0:
        parser.error("--drop-count must be nonnegative")
    if drop_count > 0 and not drop_command:
        parser.error("--drop-count requires --drop-command")
    if args.mode.startswith("blackbody-") and drop_command:
        try:
            parse_doc058_drop_spec(drop_command)
        except ValueError as error:
            parser.error(str(error))

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    if args.mode == "blackbody-tcp":
        controller = Blackbody(True, args.temperature, not args.unstable,
                               drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_tcp(args.host, args.port, controller, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "blackbody-pty":
        controller = Blackbody(False, args.temperature, not args.unstable,
                               drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_pty(controller, b"\r", 9600, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "power-pty":
        controller = It6332a(drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_pty(controller, b"\n", 115200, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if not args.device:
        parser.error("--device is required for serial modes")
    if args.mode == "blackbody-serial":
        controller = Blackbody(False, args.temperature, not args.unstable,
                               drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        delimiter = b"\r"
        baud = 9600
    else:
        controller = It6332a(drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        delimiter = b"\n"
        baud = args.baud
    serve_serial(args.device, baud, controller, delimiter, stop, fragment=args.fragment, log_io=args.log_io)


if __name__ == "__main__":
    main()
