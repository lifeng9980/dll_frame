#!/usr/bin/env python3
"""Composite virtual FPA test station coupling all 6 SDK instruments over TCP or serial COM."""

import sys

sys.dont_write_bytecode = True

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import signal
import socket
import threading

from ir_sim_devices import (
    Blackbody,
    CoolerModbus,
    DetectorController,
    Dmm6500,
    GwsStenOven,
    It6332a,
    Udp6730,
    _consume_modbus_buffer,
    _dispatch_buffer,
    close_serial,
    open_serial,
    serve_serial,
)


class VirtualStationState:
    """Thread-safe shared physical model linking power, cooler, DMM, blackbody, oven, and detector."""

    def __init__(self, ramp_c_per_s=0.0, log_io=False, *, blackbody_tcp=True,
                 interlock_detector_power=False, power_mode="it6332a"):
        if power_mode not in ("it6332a", "split"):
            raise ValueError(f"Unsupported power_mode: {power_mode}")
        self.lock = threading.RLock()
        self.cooler_fault_mv = None
        self.dmm_fault_value = None
        self.endpoints = {}

        self.power_mode = power_mode
        self.interlock_detector_power = interlock_detector_power
        self.blackbody = Blackbody(blackbody_tcp, 25.0, True, log_io=log_io, ramp_c_per_s=ramp_c_per_s)
        if power_mode == "split":
            self.power = None
            self.cooler_power = Udp6730(
                voltage=12.0, current_limit=2.0, output=False,
                log_io=log_io, on_output_changed=self._on_power_output_changed,
            )
            self.dp100_voltage = 12.0
            self.dp100_current_limit = 2.0
            self.dp100_output = False
        else:
            self.power = It6332a(log_io=log_io, cooler_profile=True)
            self.power.on_output_changed = self._on_power_output_changed
            self.cooler_power = None
            self.dp100_voltage = 0.0
            self.dp100_current_limit = 0.0
            self.dp100_output = False
        self.oven = GwsStenOven(
            address=1, temperature=25.0, setpoint=25.0, mode="STANDBY",
            log_io=log_io, auto_track=(ramp_c_per_s <= 0.0),
        )
        self.dmm = Dmm6500(mode="DIOD", value=0.500, log_io=log_io, value_fn=self.dmm_value)
        self.detector = DetectorController(
            log_io=log_io, current_ma_fn=self.detector_current_ma,
            powered_fn=self.detector_powered if interlock_detector_power else None,
        )
        self.cooler = CoolerModbus(
            slave_address=1, register_address=0x7A52, voltage_mv=500.0,
            mv_fn=self.cooler_mv, log_io=log_io,
        )

    def _clear_detector_state(self):
        with self.lock:
            self.detector.registers.clear()
            self.detector.gain_code = 0
            self.detector.channel_code = 0

    def _on_power_output_changed(self, channel, enabled):
        if self.power_mode == "it6332a" and self.interlock_detector_power and channel == 2 and not enabled:
            self._clear_detector_state()

    def cooler_powered(self):
        with self.lock:
            if self.power_mode == "split":
                return self.cooler_power.output
            return self.power.output[0]

    def detector_powered(self):
        with self.lock:
            if self.power_mode == "split":
                return self.dp100_output
            return self.power.output[1]

    def dp100_reading(self):
        with self.lock:
            enabled = bool(self.dp100_output)
            return {
                "setpoint_v": self.dp100_voltage,
                "current_limit_a": self.dp100_current_limit,
                "measured_v": self.dp100_voltage if enabled else 0.0,
                "measured_a": 0.25 if enabled else 0.0,
                "output_enabled": enabled,
            }

    def set_dp100_output(self, enabled):
        with self.lock:
            self.dp100_output = bool(enabled)
            if self.interlock_detector_power and not self.dp100_output:
                self._clear_detector_state()
            return self.dp100_reading()

    def cooler_mv(self):
        with self.lock:
            if self.cooler_fault_mv is not None:
                return float(self.cooler_fault_mv)
            return 1080.0 if self.cooler_powered() else 500.0

    def dmm_value(self):
        with self.lock:
            if self.dmm_fault_value is not None:
                return float(self.dmm_fault_value)
            return self.cooler_mv() / 1000.0

    def detector_current_ma(self, channel_code):
        with self.lock:
            if not self.detector_powered():
                return 0.0
            return 12.5 + float(channel_code & 0x0F) * 2.5

    def apply_fault(self, payload):
        with self.lock:
            if payload.get("reset"):
                self.blackbody.stable = True
                self.oven.alarm = ""
                self.detector.bad_ack = False
                self.cooler.bad_crc = False
                self.cooler_fault_mv = None
                self.dmm_fault_value = None
            if "blackbody_unstable" in payload:
                self.blackbody.stable = not bool(payload["blackbody_unstable"])
            if "oven_alarm" in payload:
                self.oven.alarm = str(payload["oven_alarm"])
            if "detector_bad_ack" in payload:
                self.detector.bad_ack = bool(payload["detector_bad_ack"])
            if "cooler_bad_crc" in payload:
                self.cooler.bad_crc = bool(payload["cooler_bad_crc"])
            if "cooler_mv" in payload:
                val = payload["cooler_mv"]
                self.cooler_fault_mv = None if val is None else float(val)
            if "dmm_value" in payload:
                val = payload["dmm_value"]
                self.dmm_fault_value = None if val is None else float(val)

    def snapshot(self):
        with self.lock:
            regs = dict(self.detector.registers)
            int_us = int.from_bytes(regs[0x50], "little") if 0x50 in regs and len(regs[0x50]) == 3 else None
            empty_us = int.from_bytes(regs[0x51], "little") if 0x51 in regs and len(regs[0x51]) == 3 else None
            gpol_v = None
            if 0x21 in regs and len(regs[0x21]) == 2:
                gpol_v = (int.from_bytes(regs[0x21], "little") * 5.0) / 65535.0
            if self.power_mode == "split":
                power_snap = {
                    "mode": "split",
                    "voltage": [self.cooler_power.voltage, self.dp100_voltage, 0.0],
                    "current_limit": [self.cooler_power.current_limit, self.dp100_current_limit, 0.0],
                    "output": [self.cooler_power.output, self.dp100_output, False],
                    "cooler_power": {
                        "voltage": self.cooler_power.voltage,
                        "current_limit": self.cooler_power.current_limit,
                        "output": self.cooler_power.output,
                    },
                    "dp100": self.dp100_reading(),
                }
            else:
                power_snap = {
                    "mode": "it6332a",
                    "voltage": list(self.power.voltage),
                    "current_limit": list(self.power.current_limit),
                    "output": list(self.power.output),
                }
            return {
                "endpoints": dict(self.endpoints),
                "blackbody": {
                    "target_c": self.blackbody.target,
                    "actual_c": self.blackbody.actual,
                    "stable": self.blackbody.stable,
                },
                "power": power_snap,
                "oven": {
                    "target_c": self.oven.setpoint,
                    "actual_c": self.oven.temperature,
                    "mode": self.oven.mode,
                    "alarm": self.oven.alarm,
                },
                "dmm": {
                    "mode": self.dmm.mode,
                    "value": self.dmm_value(),
                },
                "cooler": {
                    "voltage_mv": self.cooler_mv(),
                    "requests": len(self.cooler.requests),
                },
                "detector": {
                    "powered": self.detector_powered(),
                    "integration_us": int_us,
                    "empty_us": empty_us,
                    "gpol_v": gpol_v,
                    "gain_code": self.detector.gain_code,
                    "channel_code": self.detector.channel_code,
                    "frames_received": len(self.detector.frames),
                    "registers": {f"0x{k:02x}": v.hex() for k, v in sorted(regs.items())},
                },
            }


def write_dynamic_raw16(directory, state, frames=2, width=4, height=4):
    """Synthesize deterministic RAW16 frames whose DN reflects current virtual station conditions."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    snap = state.snapshot()
    ch2_on = snap["power"]["output"][1]
    bb_c = snap["blackbody"]["actual_c"]
    int_us = snap["detector"]["integration_us"] or 1000
    gpol_v = snap["detector"]["gpol_v"] if snap["detector"]["gpol_v"] is not None else 2.5

    if ch2_on:
        base_dn = 2000.0 + (bb_c + 20.0) * 150.0 + (int_us / 100.0) * 20.0 + gpol_v * 300.0
    else:
        base_dn = 100.0

    raw_bytes = bytearray()
    csv_lines = ["frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness"]
    stride = width * 2
    for f in range(frames):
        csv_lines.append(f"{f},{width},{height},{stride},RAW16_LE,16,little")
        for r in range(height):
            for c_idx in range(width):
                dn = int(round(base_dn + f * 4 + r * 2 + c_idx))
                dn = max(0, min(65535, dn))
                raw_bytes.extend(dn.to_bytes(2, "little"))

    (directory / "frames.raw").write_bytes(bytes(raw_bytes))
    (directory / "frames.csv").write_text("\n".join(csv_lines) + "\n", encoding="utf-8")


class BoundTcpDevice:
    def __init__(self, host, port, controller, delimiter, stop, log_io=False, consume_fn=None, lock=None):
        self.controller = controller
        self.delimiter = delimiter
        self.stop = stop
        self.log_io = log_io
        self.consume_fn = consume_fn
        self.lock = lock
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((host, port))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        try:
            self.listener.close()
        except OSError:
            pass
        self.thread.join(3)

    def _run(self):
        with self.listener:
            while not self.stop.is_set():
                try:
                    conn, _ = self.listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                t = threading.Thread(target=self._serve_client, args=(conn,), daemon=True)
                t.start()

    def _serve_client(self, conn):
        with conn:
            conn.settimeout(0.2)
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
                    if self.lock is not None:
                        with self.lock:
                            pending = _dispatch_buffer(
                                pending, self.controller, self.delimiter, conn.sendall,
                                fragment=False, log_io=self.log_io, consume_fn=self.consume_fn,
                            )
                    else:
                        pending = _dispatch_buffer(
                            pending, self.controller, self.delimiter, conn.sendall,
                            fragment=False, log_io=self.log_io, consume_fn=self.consume_fn,
                        )
                except (ConnectionResetError, BrokenPipeError, OSError):
                    break


def start_control_http(host, port, state, stop):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            return

        def _reply_json(self, code, obj):
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/state":
                self._reply_json(200, state.snapshot())
                return
            if self.path == "/dp100":
                if state.power_mode != "split":
                    self._reply_json(409, {"error": "dp100 is only available in split power mode"})
                    return
                self._reply_json(200, state.dp100_reading())
                return
            self._reply_json(404, {"error": "not found"})

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length > 0 else b"{}"
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
            except Exception as exc:
                self._reply_json(400, {"error": str(exc)})
                return
            if self.path == "/fault":
                state.apply_fault(payload)
                self._reply_json(200, state.snapshot())
                return
            if self.path == "/dp100/output":
                if state.power_mode != "split":
                    self._reply_json(409, {"error": "dp100 is only available in split power mode"})
                    return
                if "enabled" in payload:
                    enabled = bool(payload["enabled"])
                elif "output_enabled" in payload:
                    enabled = bool(payload["output_enabled"])
                else:
                    self._reply_json(400, {"error": "missing 'enabled' field"})
                    return
                self._reply_json(200, state.set_dp100_output(enabled))
                return
            if self.path == "/shutdown":
                self._reply_json(200, {"stopping": True})
                stop.set()
                return
            self._reply_json(404, {"error": "not found"})

    server = ThreadingHTTPServer((host, port), Handler)
    server.timeout = 0.2
    actual_port = server.server_address[1]
    thread = threading.Thread(target=lambda: _serve_http_until_stop(server, stop), daemon=True)
    thread.start()
    return server, actual_port, thread


def _serve_http_until_stop(server, stop):
    while not stop.is_set():
        server.handle_request()
    server.server_close()


def build_station_json(endpoints, raw_source, power_mode="it6332a", power_driver_path=None):
    devices = {}
    if "blackbody" in endpoints:
        ep = endpoints["blackbody"]
        if "tcp_port" in ep:
            devices["bb"] = {
                "kind": "pcn7-lt",
                "connection": {"host": ep["host"], "tcp_port": ep["tcp_port"], "head": 0,
                               "expected_head_serial": "HEAD-SIMULATOR", "timeout_ms": 1500},
            }
        else:
            devices["bb"] = {
                "kind": "dcn1000",
                "connection": {"port": ep.get("client_port", ep["port"]), "timeout_ms": 1500},
            }
    if "power" in endpoints:
        ep = endpoints["power"]
        if power_mode == "split":
            conn = {"host": ep["host"], "tcp_port": ep["tcp_port"], "timeout_ms": 1200} if "tcp_port" in ep \
                else {"port": ep.get("client_port", ep["port"]), "baud_rate": ep.get("baud_rate", 9600), "timeout_ms": 1200}
            devices["cooler_power"] = {"kind": "udp6730", "connection": conn}
            if power_driver_path and "control_port" in endpoints:
                devices["detector_power"] = {
                    "kind": "external-power",
                    "connection": {
                        "driver_path": str(power_driver_path),
                        "driver_id": "mock_power",
                        "driver_config": {"control_port": int(endpoints["control_port"])},
                    },
                }
        else:
            conn = {"host": ep["host"], "tcp_port": ep["tcp_port"], "timeout_ms": 1200} if "tcp_port" in ep \
                else {"port": ep.get("client_port", ep["port"]), "baud_rate": ep.get("baud_rate", 115200), "timeout_ms": 1200}
            devices["supply"] = {"kind": "it6332a", "connection": conn}
    if "oven" in endpoints:
        ep = endpoints["oven"]
        conn = {"host": ep["host"], "tcp_port": ep["tcp_port"], "address": 1, "delimiter": "crlf", "timeout_ms": 1500} if "tcp_port" in ep \
            else {"port": ep.get("client_port", ep["port"]), "baud_rate": ep.get("baud_rate", 9600), "address": 1, "delimiter": "crlf", "timeout_ms": 1500}
        devices["oven"] = {"kind": "gws-sten", "connection": conn}
    if "dmm" in endpoints:
        ep = endpoints["dmm"]
        conn = {"host": ep["host"], "tcp_port": ep["tcp_port"], "timeout_ms": 1500} if "tcp_port" in ep \
            else {"port": ep.get("client_port", ep["port"]), "baud_rate": ep.get("baud_rate", 115200), "timeout_ms": 1500}
        devices["dmm"] = {"kind": "dmm6500", "connection": conn}
    if "detector" in endpoints:
        ep = endpoints["detector"]
        conn = {"host": ep["host"], "tcp_port": ep["tcp_port"], "timeout_ms": 1200} if "tcp_port" in ep \
            else {"port": ep.get("client_port", ep["port"]), "baud_rate": ep.get("baud_rate", 115200), "timeout_ms": 1200}
        devices["det"] = {"kind": "detector-serial", "connection": conn}
    if "cooler" in endpoints:
        ep = endpoints["cooler"]
        conn = {"host": ep["host"], "tcp_port": ep["tcp_port"], "slave_address": 1, "register_address": 31314, "timeout_ms": 1500} if "tcp_port" in ep \
            else {"port": ep.get("client_port", ep["port"]), "baud_rate": ep.get("baud_rate", 9600),
                  "stop_bits": ep.get("stop_bits", 2), "slave_address": 1, "register_address": 31314, "timeout_ms": 1500}
        devices["cooler"] = {"kind": "cooler-modbus", "connection": conn}
    return {
        "schemav": 1,
        "acquisition": {
            "backend": "raw_replay",
            "source": str(raw_source),
            "valid_bits": 16,
        },
        "devices": devices,
    }


def build_host_blackbody_json(ep):
    if "tcp_port" in ep:
        return {
            "schema_version": 1,
            "driver": "pcn7lt",
            "transport": "tcp",
            "connection": {
                "host": ep["host"],
                "port": ep["tcp_port"],
            },
            "driver_options": {
                "head": 0,
                "expected_head_serial": "HEAD-SIMULATOR",
            },
            "safety": {
                "min_setpoint_c": -60.0,
                "max_setpoint_c": 100.0,
                "readback_tolerance_c": 0.01,
            },
            "telemetry": {
                "period_ms": 1000,
                "stale_ms": 8000,
                "io_timeout_ms": 3000,
            },
        }
    return {
        "schema_version": 1,
        "driver": "dcn1000",
        "transport": "serial",
        "connection": {
            "port": ep.get("client_port", ep["port"]),
        },
        "driver_options": {},
        "safety": {
            "min_setpoint_c": -5.0,
            "max_setpoint_c": 100.0,
            "readback_tolerance_c": 0.01,
        },
        "telemetry": {
            "period_ms": 1000,
            "stale_ms": 8000,
            "io_timeout_ms": 3000,
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("tcp", "serial-map"), default="tcp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--config", type=Path, help="JSON endpoint map for serial-map mode")
    parser.add_argument("--blackbody-port", type=int, default=21010)
    parser.add_argument("--power-port", type=int, default=5026)
    parser.add_argument("--oven-port", type=int, default=5000)
    parser.add_argument("--dmm-port", type=int, default=5025)
    parser.add_argument("--detector-port", type=int, default=2102)
    parser.add_argument("--cooler-port", type=int, default=5020)
    parser.add_argument("--control-port", type=int, default=None)
    parser.add_argument("--ramp-c-per-s", type=float, default=0.0)
    parser.add_argument("--power-mode", choices=("it6332a", "split"), default="it6332a",
                        help="power supply model: single dual-channel it6332a or split udp6730+dp100")
    parser.add_argument("--interlock-detector-power", action="store_true",
                        help="suppress detector responses and clear its registers while detector power is off")
    parser.add_argument("--emit-station-json", type=Path, default=None)
    parser.add_argument("--emit-blackbody-config", type=Path, default=None)
    parser.add_argument("--raw-source", type=Path, help="RAW replay directory for --emit-station-json")
    parser.add_argument("--log-io", action="store_true")
    args = parser.parse_args()
    if args.emit_station_json is not None and args.raw_source is None:
        parser.error("--emit-station-json requires --raw-source")

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    serial_map = {}
    if args.mode == "serial-map":
        if not args.config:
            parser.error("--config is required with --mode serial-map")
        serial_map = json.loads(args.config.read_text(encoding="utf-8"))

    state = VirtualStationState(
        ramp_c_per_s=args.ramp_c_per_s, log_io=args.log_io,
        blackbody_tcp="port" not in serial_map.get("blackbody", {}),
        interlock_detector_power=args.interlock_detector_power,
        power_mode=args.power_mode,
    )
    power_spec = (state.cooler_power, b"\n", None, 9600, 1, args.power_port) \
        if args.power_mode == "split" else (state.power, b"\n", None, 115200, 1, args.power_port)
    specs = {
        "blackbody": (state.blackbody, b"\r", None, 9600, 1, args.blackbody_port),
        "power": power_spec,
        "oven": (state.oven, b"\r\n", None, 9600, 1, args.oven_port),
        "dmm": (state.dmm, b"\n", None, 115200, 1, args.dmm_port),
        "detector": (state.detector, None, None, 115200, 1, args.detector_port),
        "cooler": (state.cooler, None, _consume_modbus_buffer, 9600, 2, args.cooler_port),
    }

    tcp_servers = []
    serial_threads = []
    endpoints = {}
    ctrl_thread = None
    try:
        for name, (controller, delimiter, consume_fn, default_baud, default_stop, tcp_default) in specs.items():
            if args.mode == "serial-map":
                cfg = serial_map.get(name)
                if cfg is None and name == "power":
                    cfg = serial_map.get("cooler_power")
            else:
                cfg = None
            if cfg and "port" in cfg:
                port_name = cfg["port"]
                client_port = cfg.get("client_port")
                baud = int(cfg.get("baud_rate", default_baud))
                stop_bits = int(cfg.get("stop_bits", default_stop))
                ep_info = {"port": port_name, "baud_rate": baud, "stop_bits": stop_bits}
                if client_port:
                    ep_info["client_port"] = client_port
                ser_handle = open_serial(port_name, baud, stop_bits=stop_bits)
                try:
                    endpoints[name] = ep_info
                    t = threading.Thread(
                        target=serve_serial,
                        args=(port_name, baud, controller, delimiter, stop),
                        kwargs={
                            "log_io": args.log_io,
                            "stop_bits": stop_bits,
                            "consume_fn": consume_fn,
                            "dispatch_lock": state.lock,
                            "preopened_handle": ser_handle,
                        },
                        daemon=True,
                    )
                    t.start()
                except Exception:
                    close_serial(ser_handle)
                    raise
                serial_threads.append(t)
            else:
                tcp_host = (cfg or {}).get("host", args.host)
                tcp_port = int((cfg or {}).get("tcp_port", tcp_default))
                srv = BoundTcpDevice(tcp_host, tcp_port, controller, delimiter, stop,
                                     log_io=args.log_io, consume_fn=consume_fn, lock=state.lock)
                tcp_servers.append(srv)
                srv.start()
                endpoints[name] = {"host": tcp_host, "tcp_port": srv.port}

        if args.control_port is not None:
            _, actual_ctrl_port, ctrl_thread = start_control_http(args.host, args.control_port, state, stop)
            endpoints["control_port"] = actual_ctrl_port
            endpoints["control_url"] = f"http://{args.host}:{actual_ctrl_port}"

        state.endpoints = endpoints
        if args.emit_station_json:
            args.emit_station_json.parent.mkdir(parents=True, exist_ok=True)
            args.emit_station_json.write_text(
                json.dumps(
                    build_station_json(endpoints, args.raw_source, power_mode=args.power_mode),
                    ensure_ascii=False,
                    indent=2,
                ) + "\n",
                encoding="utf-8",
            )
        if args.emit_blackbody_config and "blackbody" in endpoints:
            args.emit_blackbody_config.parent.mkdir(parents=True, exist_ok=True)
            args.emit_blackbody_config.write_text(
                json.dumps(build_host_blackbody_json(endpoints["blackbody"]), ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )

        print("VIRTUAL_STATION_READY=" + json.dumps(endpoints, ensure_ascii=False), flush=True)
        while not stop.wait(0.25):
            pass
    finally:
        stop.set()
        for srv in tcp_servers:
            srv.close()
        for t in serial_threads:
            t.join(2)
        if ctrl_thread is not None:
            ctrl_thread.join(2)


if __name__ == "__main__":
    main()
