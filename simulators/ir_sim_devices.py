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
    def __init__(self, tcp, temperature, stable, drop_command="", drop_count=0, log_io=False,
                 ramp_c_per_s=0.0):
        self.tcp = tcp
        self.target = temperature
        self.actual = temperature
        self.stable = stable
        self.drop_command = drop_command
        self.drop_count = drop_count
        self.log_io = log_io
        self.ramp_c_per_s = float(ramp_c_per_s)
        self._last_mono = time.monotonic()
        self._drop_spec = parse_doc058_drop_spec(drop_command) if drop_command else None

    def _advance_ramp(self):
        now = time.monotonic()
        dt = max(0.0, now - self._last_mono)
        self._last_mono = now
        if self.ramp_c_per_s <= 0.0:
            self.actual = self.target
            return
        delta = self.target - self.actual
        step = self.ramp_c_per_s * dt
        if abs(delta) <= step:
            self.actual = self.target
        else:
            self.actual += step if delta > 0 else -step

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
        self._advance_ramp()
        direction = request[1]
        function = request[2]
        argument = request[3:]
        if direction == ord("1") and function == 97:
            try:
                self.target = float(argument.split(b",", 1)[0])
                if self.ramp_c_per_s <= 0.0:
                    self.actual = self.target
            except ValueError:
                return b"\x1b" + bytes([function]) + b"\x15\r"
            payload = b"\x06"
        elif direction == ord("0"):
            is_stable = self.stable and (abs(self.actual - self.target) <= 0.02)
            payloads = {
                135: b"PCN7-LT" if self.tcp else b"DCN1000",
                136: b"IR-SIMULATOR",
                56: b"0,HEAD-SIMULATOR",
                73: b"0",
                114: f"{self.actual:.3f}".encode("ascii"),
                116: b"25.000",
                97: f"{self.target:.3f}".encode("ascii"),
                67: b"-60",
                66: b"100",
                92: b"1" if is_stable else b"0",
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
    def __init__(self, drop_command="", drop_count=0, log_io=False, cooler_profile=False):
        self.channel = 1
        self.voltage = [0.0, 0.0, 0.0]
        self.current_limit = [1.0, 1.0, 1.0]
        self.output = [False, False, False]
        self.drop_command = drop_command
        self.drop_count = drop_count
        self.log_io = log_io
        self.cooler_profile = cooler_profile
        self._ch1_on_reads = 0
        self.on_output_changed = None

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
            if not self.output[index]:
                value = 0.0
            elif self.cooler_profile and index == 0:
                startup_curve = (0.55, 0.48, 0.42, 0.35)
                if self._ch1_on_reads < len(startup_curve):
                    value = startup_curve[self._ch1_on_reads]
                else:
                    value = 0.24
                self._ch1_on_reads += 1
            elif self.cooler_profile and index == 1:
                value = 0.42
            else:
                value = 0.05
            return f"{value:.6f}\n".encode("ascii")
        if upper == "CHAN:OUTP?":
            return (b"1\n" if self.output[index] else b"0\n")
        if upper == "CHAN:OUTP ON":
            if not self.output[index] and index == 0:
                self._ch1_on_reads = 0
            self.output[index] = True
            if self.on_output_changed:
                self.on_output_changed(index + 1, True)
            return None
        if upper == "CHAN:OUTP OFF":
            self.output[index] = False
            if index == 0:
                self._ch1_on_reads = 0
            if self.on_output_changed:
                self.on_output_changed(index + 1, False)
            return None
        return b""


class GwsStenOven:
    def __init__(self, address=1, temperature=25.0, setpoint=25.0, mode="STANDBY", alarm="",
                 drop_command="", drop_count=0, log_io=False, auto_track=False):
        self.address = address
        self.temperature = temperature
        self.setpoint = setpoint
        self.mode = mode
        self.alarm = alarm
        self.drop_command = drop_command
        self.drop_count = drop_count
        self.log_io = log_io
        self.auto_track = auto_track
        self.commands = []

    def _matches_drop(self, body_upper):
        if not self.drop_command or self.drop_count <= 0:
            return False
        target = self.drop_command.strip().upper()
        return target in ("*", "ANY") or body_upper == target or body_upper.startswith(target)

    def handle(self, request):
        text = request.decode("ascii", errors="replace").strip()
        prefix = f"{self.address},"
        if not text.startswith(prefix):
            return None
        body = text[len(prefix):].strip()
        body_upper = body.upper()
        self.commands.append(body)
        if self._matches_drop(body_upper):
            self.drop_count -= 1
            if self.log_io:
                print(f"[DROP] cmd={text!r} remaining={self.drop_count}", flush=True)
            return None
        if body_upper == "MODE?":
            return f"{self.mode}\r\n".encode("ascii")
        if body_upper == "TEMP?":
            return f"{self.temperature:.2f},{self.setpoint:.2f},0.00,0.00\r\n".encode("ascii")
        if body_upper == "ALARM?":
            return (f"{self.alarm}\r\n" if self.alarm else "0\r\n").encode("ascii")
        if body_upper.startswith("TEMP, S") or body_upper.startswith("TEMP,S"):
            try:
                val = float(body.split("S", 1)[1].strip())
            except ValueError:
                return b"NA\r\n"
            self.setpoint = val
            if self.auto_track:
                self.temperature = val
            return b"OK\r\n"
        if body_upper == "MODE, CONSTANT":
            self.mode = "CONSTANT"
            return b"OK\r\n"
        if body_upper == "MODE, STANDBY":
            self.mode = "STANDBY"
            return b"OK\r\n"
        return b"NA\r\n"


class Dmm6500:
    def __init__(self, mode="VOLT:DC", value=0.612345, idn="KEITHLEY INSTRUMENTS,MODEL DMM6500,IR-SIM,1.0",
                 lang="SCPI", drop_command="", drop_count=0, log_io=False, value_fn=None):
        self.mode = mode
        self.value = value
        self.idn = idn
        self.lang = lang
        self.drop_command = drop_command
        self.drop_count = drop_count
        self.log_io = log_io
        self.value_fn = value_fn
        self.commands = []

    def _matches_drop(self, upper):
        if not self.drop_command or self.drop_count <= 0:
            return False
        target = self.drop_command.strip().upper()
        return target in ("*", "ANY") or upper == target or upper.startswith(target + " ")

    def handle(self, request):
        cmd = request.decode("ascii", errors="replace").strip()
        upper = cmd.upper()
        self.commands.append(cmd)
        if self._matches_drop(upper):
            self.drop_count -= 1
            if self.log_io:
                print(f"[DROP] cmd={cmd!r} remaining={self.drop_count}", flush=True)
            return None
        if upper == "*IDN?":
            return f"{self.idn}\n".encode("ascii")
        if upper == "*LANG?":
            return f"{self.lang}\n".encode("ascii")
        if upper == ":SENS:FUNC?":
            return f'"{self.mode}"\n'.encode("ascii")
        if upper.startswith(":SENS:FUNC "):
            arg = cmd.split(None, 1)[1].strip().strip('"').upper()
            self.mode = arg
            return None
        if upper == ":READ?":
            val = float(self.value_fn()) if self.value_fn else self.value
            return f"{val:.9g}\n".encode("ascii")
        return None


class DetectorController:
    def __init__(self, bad_ack=False, log_io=False, current_ma_fn=None):
        self.bad_ack = bad_ack
        self.log_io = log_io
        self.current_ma_fn = current_ma_fn
        self.gain_code = 0
        self.channel_code = 0
        self.registers = {}
        self.frames = []

    def handle_frame(self, frame):
        cmd_code = frame[2]
        payload = frame[3:]
        self.frames.append((cmd_code, bytes(payload)))
        self.registers[cmd_code] = bytes(payload)
        if self.log_io:
            print(f"[DET RX] cmd=0x{cmd_code:02X} payload={payload.hex()}", flush=True)
        if self.bad_ack:
            return b"\x55\x00\x01\xEE\xFF"
        if cmd_code == 0x40 and len(payload) >= 1:
            self.gain_code = payload[0] & 0x07
        elif cmd_code == 0x30 and len(payload) >= 2:
            self.channel_code = (payload[0] << 8) | payload[1]
        elif cmd_code == 0x41 and len(payload) >= 1 and payload[0] == 0x03:
            current_ma = self.current_ma_fn(self.channel_code) if self.current_ma_fn else 25.0
            adc_v = (float(current_ma) / 1000.0) * float(1 << self.gain_code)
            adc_v = max(-2.5, min(2.499999, adc_v))
            raw_code = max(0, min(0xFFFFFF, round(((adc_v / 2.5) + 1.0) * 8388608.0)))
            return bytes((
                0x55, 0x00, 0x03, 0x41,
                (raw_code >> 16) & 0xFF,
                (raw_code >> 8) & 0xFF,
                raw_code & 0xFF,
            ))
        return bytes((0x55, 0x00, 0x01, cmd_code, 0xFF))


def modbus_crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
    return crc


class CoolerModbus:
    def __init__(self, slave_address=1, register_address=0x7A52, voltage_mv=1080.0,
                 bad_crc=False, mv_fn=None, drop_count=0, log_io=False):
        self.slave_address = slave_address
        self.register_address = register_address
        self.voltage_mv = float(voltage_mv)
        self.bad_crc = bad_crc
        self.mv_fn = mv_fn
        self.drop_count = drop_count
        self.log_io = log_io
        self.requests = []

    def current_mv(self):
        return float(self.mv_fn()) if self.mv_fn else self.voltage_mv

    def handle_frame(self, frame):
        self.requests.append(bytes(frame))
        if len(frame) != 8:
            return None
        crc_calc = modbus_crc16(frame[:6])
        crc_recv = int.from_bytes(frame[6:8], "little")
        if crc_calc != crc_recv:
            return None
        if frame[0] != self.slave_address:
            return None
        if self.drop_count > 0:
            self.drop_count -= 1
            if self.log_io:
                print(f"[DROP MODBUS] req={frame.hex()} remaining={self.drop_count}", flush=True)
            return None
        fn = frame[1]
        if fn != 0x03:
            ex = bytes((self.slave_address, fn | 0x80, 0x01))
            return ex + modbus_crc16(ex).to_bytes(2, "little")
        reg = int.from_bytes(frame[2:4], "big")
        cnt = int.from_bytes(frame[4:6], "big")
        if reg != self.register_address or cnt != 2:
            ex = bytes((self.slave_address, 0x83, 0x02))
            return ex + modbus_crc16(ex).to_bytes(2, "little")
        raw = max(0, min(0xFFFFFFFF, round(self.current_mv() * 100.0)))
        body = bytes((self.slave_address, 0x03, 0x04)) + raw.to_bytes(4, "big")
        crc_bytes = bytearray(modbus_crc16(body).to_bytes(2, "little"))
        if self.bad_crc:
            crc_bytes[1] ^= 0xFF
        return body + bytes(crc_bytes)


def _consume_modbus_buffer(pending, controller, write_fn, fragment=False, log_io=False):
    while len(pending) >= 8:
        frame = pending[:8]
        pending = pending[8:]
        if log_io:
            print(f"[MODBUS RX] {frame.hex()}", flush=True)
        resp = controller.handle_frame(frame)
        send_response(write_fn, resp, fragment=fragment, log_io=log_io)
    return pending


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


def _consume_detector_buffer(pending, controller, write_fn, fragment=False, log_io=False):
    while len(pending) >= 3:
        if pending[0] != 0xCC:
            pending = pending[1:]
            continue
        length = pending[1]
        total = 3 + length
        if len(pending) < total:
            break
        frame = pending[:total]
        pending = pending[total:]
        resp = controller.handle_frame(frame)
        send_response(write_fn, resp, fragment=fragment, log_io=log_io)
    return pending


def _dispatch_buffer(pending, controller, delimiter, write_fn, fragment=False, log_io=False, consume_fn=None):
    if consume_fn is not None:
        return consume_fn(pending, controller, write_fn, fragment=fragment, log_io=log_io)
    if delimiter is None:
        return _consume_detector_buffer(pending, controller, write_fn, fragment=fragment, log_io=log_io)
    while delimiter in pending:
        request, pending = pending.split(delimiter, 1)
        if log_io:
            print(f"[RX] {request!r}", flush=True)
        response = controller.handle(request)
        send_response(write_fn, response, fragment=fragment, log_io=log_io)
    return pending


def serve_tcp(host, port, controller, stop, delimiter=b"\r", fragment=False, log_io=False, consume_fn=None):
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
                    try:
                        pending = _dispatch_buffer(
                            pending, controller, delimiter, connection.sendall,
                            fragment=fragment, log_io=log_io, consume_fn=consume_fn,
                        )
                    except (ConnectionResetError, BrokenPipeError, OSError):
                        break


def open_serial(port, baud, data_bits=8, parity="none", stop_bits=1):
    if os.name == "nt":
        try:
            import serial
        except ImportError as error:
            raise RuntimeError("Windows COM simulation requires optional pyserial; TCP simulation needs no extra package") from error
        parity_map = {"none": serial.PARITY_NONE, "odd": serial.PARITY_ODD, "even": serial.PARITY_EVEN}
        stop_map = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}
        handle = serial.Serial(
            port,
            baudrate=baud,
            bytesize=data_bits,
            parity=parity_map.get(parity, serial.PARITY_NONE),
            stopbits=stop_map.get(stop_bits, serial.STOPBITS_ONE),
            timeout=0.1,
            write_timeout=1,
        )
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
    if stop_bits == 2 and hasattr(termios, "CSTOPB"):
        attributes[2] |= termios.CSTOPB
    termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
    return descriptor


def serve_serial(port, baud, controller, delimiter, stop, fragment=False, log_io=False,
                 stop_bits=1, consume_fn=None):
    handle = open_serial(port, baud, stop_bits=stop_bits)
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
            pending = _dispatch_buffer(
                pending, controller, delimiter, write_fn,
                fragment=fragment, log_io=log_io, consume_fn=consume_fn,
            )
    finally:
        if os.name == "nt":
            handle.close()
        else:
            os.close(handle)


def serve_pty(controller, delimiter, baud, stop, fragment=False, log_io=False, stop_bits=1, consume_fn=None):
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
    if stop_bits == 2 and hasattr(termios, "CSTOPB"):
        attributes[2] |= termios.CSTOPB
    termios.tcsetattr(slave, termios.TCSANOW, attributes)
    print(f"SDK_SERIAL_PORT={os.ttyname(slave)}", flush=True)
    pending = b""
    try:
        while not stop.is_set():
            ready, _, _ = select.select([master], [], [], 0.25)
            if not ready:
                continue
            pending += os.read(master, 4096)
            pending = _dispatch_buffer(
                pending, controller, delimiter, lambda data: os.write(master, data),
                fragment=fragment, log_io=log_io, consume_fn=consume_fn,
            )
    finally:
        os.close(master)
        os.close(slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=(
        "blackbody-tcp", "blackbody-serial", "power-tcp", "power-serial", "blackbody-pty", "power-pty",
        "oven-tcp", "oven-serial", "oven-pty",
        "dmm6500-tcp", "dmm6500-serial", "dmm6500-pty",
        "detector-tcp", "detector-serial", "detector-pty",
        "cooler-tcp", "cooler-serial", "cooler-pty",
    ))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=21010, help="TCP port for TCP simulator modes")
    parser.add_argument("--device", help="serial port path or COM name")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--temperature", type=float, default=24.875)
    parser.add_argument("--unstable", action="store_true", help="report the blackbody as not stable")
    parser.add_argument("--cooler-profile", action="store_true",
                        help="simulate IT6332A CH1 startup current (>0.3 A) settling to <0.3 A")
    parser.add_argument("--address", type=int, default=1, help="oven station address (1..255)")
    parser.add_argument("--alarm", default="", help="oven alarm status string")
    parser.add_argument("--dmm-mode", default="VOLT:DC", help="initial DMM6500 SCPI function")
    parser.add_argument("--dmm-value", type=float, default=0.612345, help="DMM6500 :READ? value")
    parser.add_argument("--bad-ack", action="store_true", help="return mismatched detector ACK frames")
    parser.add_argument("--cooler-mv", type=float, default=1080.0, help="cooler Modbus voltage in mV")
    parser.add_argument("--slave-address", type=int, default=1, help="cooler Modbus slave address (1..247)")
    parser.add_argument("--register-address", type=lambda s: int(s, 0), default=0x7A52,
                        help="cooler Modbus holding register address")
    parser.add_argument("--bad-crc", action="store_true", help="corrupt Modbus CRC in cooler replies")
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
        serve_tcp(args.host, args.port, controller, stop, delimiter=b"\r",
                  fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "oven-tcp":
        controller = GwsStenOven(args.address, args.temperature, args.temperature, "CONSTANT",
                                 args.alarm, drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_tcp(args.host, args.port, controller, stop, delimiter=b"\r\n",
                  fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "dmm6500-tcp":
        controller = Dmm6500(args.dmm_mode, args.dmm_value,
                             drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_tcp(args.host, args.port, controller, stop, delimiter=b"\n",
                  fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "detector-tcp":
        controller = DetectorController(bad_ack=args.bad_ack, log_io=args.log_io)
        serve_tcp(args.host, args.port, controller, stop, delimiter=None,
                  fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "power-tcp":
        controller = It6332a(drop_command=drop_command, drop_count=drop_count,
                             log_io=args.log_io, cooler_profile=args.cooler_profile)
        serve_tcp(args.host, args.port, controller, stop, delimiter=b"\n",
                  fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "cooler-tcp":
        controller = CoolerModbus(args.slave_address, args.register_address, args.cooler_mv,
                                  bad_crc=args.bad_crc, drop_count=drop_count, log_io=args.log_io)
        serve_tcp(args.host, args.port, controller, stop, delimiter=None,
                  fragment=args.fragment, log_io=args.log_io, consume_fn=_consume_modbus_buffer)
        return
    if args.mode == "blackbody-pty":
        controller = Blackbody(False, args.temperature, not args.unstable,
                               drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_pty(controller, b"\r", 9600, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "oven-pty":
        controller = GwsStenOven(args.address, args.temperature, args.temperature, "CONSTANT",
                                 args.alarm, drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_pty(controller, b"\r\n", 9600, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "dmm6500-pty":
        controller = Dmm6500(args.dmm_mode, args.dmm_value,
                             drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        serve_pty(controller, b"\n", 115200, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "detector-pty":
        controller = DetectorController(bad_ack=args.bad_ack, log_io=args.log_io)
        serve_pty(controller, None, 115200, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if args.mode == "cooler-pty":
        controller = CoolerModbus(args.slave_address, args.register_address, args.cooler_mv,
                                  bad_crc=args.bad_crc, drop_count=drop_count, log_io=args.log_io)
        serve_pty(controller, None, 9600, stop, fragment=args.fragment, log_io=args.log_io,
                  stop_bits=2, consume_fn=_consume_modbus_buffer)
        return
    if args.mode == "power-pty":
        controller = It6332a(drop_command=drop_command, drop_count=drop_count,
                             log_io=args.log_io, cooler_profile=args.cooler_profile)
        serve_pty(controller, b"\n", 115200, stop, fragment=args.fragment, log_io=args.log_io)
        return
    if not args.device:
        parser.error("--device is required for serial modes")
    stop_bits = 1
    consume_fn = None
    if args.mode == "blackbody-serial":
        controller = Blackbody(False, args.temperature, not args.unstable,
                               drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        delimiter = b"\r"
        baud = 9600
    elif args.mode == "oven-serial":
        controller = GwsStenOven(args.address, args.temperature, args.temperature, "CONSTANT",
                                 args.alarm, drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        delimiter = b"\r\n"
        baud = 9600
    elif args.mode == "dmm6500-serial":
        controller = Dmm6500(args.dmm_mode, args.dmm_value,
                             drop_command=drop_command, drop_count=drop_count, log_io=args.log_io)
        delimiter = b"\n"
        baud = args.baud
    elif args.mode == "detector-serial":
        controller = DetectorController(bad_ack=args.bad_ack, log_io=args.log_io)
        delimiter = None
        baud = args.baud
    elif args.mode == "cooler-serial":
        controller = CoolerModbus(args.slave_address, args.register_address, args.cooler_mv,
                                  bad_crc=args.bad_crc, drop_count=drop_count, log_io=args.log_io)
        delimiter = None
        baud = args.baud if args.baud != 115200 else 9600
        stop_bits = 2
        consume_fn = _consume_modbus_buffer
    else:
        controller = It6332a(drop_command=drop_command, drop_count=drop_count,
                             log_io=args.log_io, cooler_profile=args.cooler_profile)
        delimiter = b"\n"
        baud = args.baud
    serve_serial(args.device, baud, controller, delimiter, stop, fragment=args.fragment,
                 log_io=args.log_io, stop_bits=stop_bits, consume_fn=consume_fn)


if __name__ == "__main__":
    main()
