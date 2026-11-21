#!/usr/bin/env python3
"""Small TCP and serial simulators for the IR SDK's device examples."""

import argparse
import os
import select
import signal
import socket
import threading


class Blackbody:
    def __init__(self, tcp, temperature, stable):
        self.tcp = tcp
        self.target = temperature
        self.stable = stable

    def handle(self, request):
        if len(request) < 3 or request[0] != 0x1B:
            return b""
        direction = request[1]
        function = request[2]
        argument = request[3:]
        if direction == ord("1") and function == 97:
            self.target = float(argument.split(b",", 1)[0])
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
        return b"\x1b" + bytes([function]) + payload + b"\r"


class It6332a:
    def __init__(self):
        self.channel = 1
        self.voltage = [0.0, 0.0, 0.0]
        self.current_limit = [1.0, 1.0, 1.0]
        self.output = [False, False, False]
        self.drop_command = ""
        self.drop_count = 0

    def handle(self, command):
        command = command.strip().decode("ascii", errors="replace")
        upper = command.upper()
        if upper == self.drop_command.upper() and self.drop_count > 0:
            self.drop_count -= 1
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


def serve_tcp(host, port, controller, stop):
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
                    if not block:
                        break
                    pending += block
                    while delimiter in pending:
                        request, pending = pending.split(delimiter, 1)
                        response = controller.handle(request)
                        if response:
                            connection.sendall(response)


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


def serve_serial(port, baud, controller, delimiter, stop):
    handle = open_serial(port, baud)
    print(f"Serial simulator attached to {port} at {baud} baud", flush=True)
    pending = b""
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
                response = controller.handle(request)
                if response:
                    if os.name == "nt":
                        handle.write(response)
                    else:
                        os.write(handle, response)
    finally:
        if os.name == "nt":
            handle.close()
        else:
            os.close(handle)


def serve_pty(controller, delimiter, baud, stop):
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
                response = controller.handle(request)
                if response:
                    os.write(master, response)
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
    args = parser.parse_args()

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    if args.mode == "blackbody-tcp":
        serve_tcp(args.host, args.port, Blackbody(True, args.temperature, not args.unstable), stop)
        return
    if args.mode == "blackbody-pty":
        serve_pty(Blackbody(False, args.temperature, not args.unstable), b"\r", 9600, stop)
        return
    if args.mode == "power-pty":
        serve_pty(It6332a(), b"\n", 115200, stop)
        return
    if not args.device:
        parser.error("--device is required for serial modes")
    if args.mode == "blackbody-serial":
        controller = Blackbody(False, args.temperature, not args.unstable)
        delimiter = b"\r"
        baud = 9600
    else:
        controller = It6332a()
        delimiter = b"\n"
        baud = args.baud
    serve_serial(args.device, baud, controller, delimiter, stop)


if __name__ == "__main__":
    main()
