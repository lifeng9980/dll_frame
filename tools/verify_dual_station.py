"""Run the same application acceptance against either local virtual HIL profile."""

import argparse
import ctypes as c
import json
import math
import os
from pathlib import Path
import sys
from urllib.request import Request, urlopen


def _resolve_station_app(explicit):
    if explicit:
        return Path(explicit).resolve()
    env = os.environ.get("IR_STATION_APP")
    if env:
        return Path(env).resolve()
    return (Path(__file__).resolve().parents[2] / "dll_frame_use").resolve()


def control(base_url, path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    with urlopen(Request(base_url + path, data=data), timeout=3) as reply:
        return json.load(reply)


def reject(action, expected):
    try:
        action()
    except RuntimeError as error:
        if expected not in str(error):
            raise
        return str(error)
    raise AssertionError("expected RuntimeError: " + expected)


def check_virtual_config(cfg, layout, endpoints):
    def device(role):
        return cfg["devices"][cfg["roles"][role]["device"]]

    for role, client, server in (("detector", "COM16", "COM17"), ("dmm", "COM14", "COM15")):
        assert device(role)["connection"].get("port") == client
        assert endpoints[role]["port"] == server
    assert device("cooler_power")["connection"].get("port") == "COM10"
    assert endpoints["power"]["port"] == "COM11"
    det, bb = device("detector_power"), device("blackbody")
    channels = tuple(cfg["roles"][role]["channel"] for role in ("cooler_power", "detector_power"))
    assert cfg["capture_requires_detector_power"] and cfg["allow_simulated_capture"]
    if layout == "split":
        assert channels == (1, 1) and det["driver"] == "external-power"
        assert det["connection"]["driver_id"] == "mock_power"
        assert Path(det["connection"]["driver_path"]).name.lower() == "ir_test_power_driver.dll"
        assert device("cooler_power")["driver"] == "udp6730"
        assert device("cooler_power")["connection"]["baud_rate"] == 9600
        assert bb["driver"] == "pcn7-lt" and bb["connection"]["host"] == "127.0.0.1"
        assert bb["connection"]["tcp_port"] == 2101
    else:
        assert channels == (1, 2) and det["driver"] == "it6332a"
        assert det is device("cooler_power") and det["connection"]["baud_rate"] == 115200
        assert cfg["roles"]["detector_power"]["expected_current_a"] == 1.5
        assert bb["driver"] == "dcn1000" and bb["connection"]["port"] == "COM20"
        assert endpoints["blackbody"]["port"] == "COM21"


def verify(cfg, layout, station, base_url):
    settings = dict(cfg["settings"], discard=0)
    results = {"layout": layout, "raw_source": cfg["raw_source"]}
    bench = station.bench()
    roles = cfg["roles"]
    devices = cfg["devices"]

    def run(point, steps=(), count=8, **kwargs):
        _, report, attempt = station.run(
            list(steps), dict(settings, frames=count), cfg["profile"], point_id=point, **kwargs
        )
        assert report["valid_frames"] == count
        return report, attempt

    def detector_write_timeout():
        det_id = roles["detector"]["device"]
        handle = bench.open_device(det_id, devices[det_id])
        try:
            byte = (c.c_uint8 * 1)(8)
            return reject(lambda: bench.call("IR_SetDetectorRegister", handle, 0x65, byte, 1), "(7)")
        finally:
            bench.close_detector(det_id)

    frames_before = control(base_url, "/state")["detector"]["frames_received"]
    for role in ("cooler_power", "detector_power"):
        ref = roles[role]
        supply_id = ref["device"]
        if layout == "fpa":
            handle = bench.open_device(supply_id, devices[supply_id])
            first = PowerReading()
            bench.call("IR_SetPowerChannel", handle, ref["channel"], ref["expected_voltage_v"],
                       ref["expected_current_a"], c.byref(first))
        state = station.supply(role).read()
        assert not state["enabled"] and math.isclose(state["set_current"], ref["expected_current_a"])
    assert abs(station.telemetry()["dmm"] - 0.500) < 1e-6
    results["control_while_off"] = detector_write_timeout()

    cooler = station.output("cooler_power", True)["cooler_power"]
    assert cooler["enabled"] and not station.supply("detector_power").read()["enabled"]
    assert abs(station.telemetry()["dmm"] - 1.080) < 1e-6
    station.output("detector_power", True)
    power_id = roles["detector_power"]["device"]
    if layout == "split":
        bench.call("IR_CloseDevice", bench.handles.pop(power_id))
        assert station.supply("detector_power").read()["enabled"]
        assert control(base_url, "/state")["detector"]["powered"]
    else:
        assert list(bench.handles).count(power_id) == 1

    report, _ = run("initialize", cfg["steps"], read_dmm=True)
    assert report["dmm_voltage_v"] == 1.080
    assert report["blackbody"]["source"] == "simulated"
    assert report["capture_started_mono_ns"] >= report["blackbody"]["stabilized_mono_ns"]
    assert sum(map(len, report["acknowledged_commands"].values())) == 25
    assert control(base_url, "/state")["detector"]["frames_received"] == frames_before + 25
    results["power"] = report["power_telemetry"]
    results["blackbody"] = {key: report["blackbody"][key]
                            for key in ("target_c", "actual_c", "device_stable")}
    results["dmm_cooler_on_v"] = report["dmm_voltage_v"]
    repeat, _ = run("powered_retest")
    results["retest_off_delta"] = {
        role: repeat["power_commands"][role]["off_commands"] - report["power_commands"][role]["off_commands"]
        for role in ("cooler_power", "detector_power")
    }
    assert results["retest_off_delta"] == {"cooler_power": 0, "detector_power": 0}

    control(base_url, "/fault", {"blackbody_unstable": True})
    try:
        fault_cfg = dict(cfg["blackbody_target"], timeout_ms=600)
        results["blackbody_fault"] = reject(
            lambda: run("blackbody_fault", blackbody_target=fault_cfg), "IR_WaitBlackbodyStable"
        )
    finally:
        control(base_url, "/fault", {"reset": True})

    station.output("detector_power", False)
    assert station.supply("cooler_power").read()["enabled"]
    assert abs(station.telemetry()["dmm"] - 1.080) < 1e-6
    det = control(base_url, "/state")["detector"]
    assert not det["powered"] and not det["registers"] and not station.applied
    # Prior detector power-off closed the camera; start_camera() reopens raw_source from frame 0.
    assert bench.camera is None and roles["detector"]["device"] not in bench.handles
    results["control_after_power_off"] = detector_write_timeout()
    results["capture_while_off"] = reject(lambda: run("powered_off"), "detector_power")
    results["preview_while_off"] = reject(station.preview, "detector_power")

    station.output("detector_power", True)
    rerun, attempt = run("reinitialize_1000", cfg["steps"], count=1000)
    assert sum(map(len, rerun["acknowledged_commands"].values())) == 25
    assert (attempt / "frames.raw").stat().st_size == 655360000
    assert control(base_url, "/state")["detector"]["frames_received"] == frames_before + 50
    results["recovery"] = {"frames": rerun["valid_frames"], "init_ack": 25, "attempt": str(attempt)}
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("layout", choices=("split", "fpa"))
    parser.add_argument("station", type=Path)
    parser.add_argument("--station-app", default=None, help="Path to station app repo (or set IR_STATION_APP)")
    parser.add_argument("--control-port", type=int, default=29101, help="Virtual station HTTP control port")
    parser.add_argument("--output", type=Path, default=None, help="Optional summary JSON output path")
    args = parser.parse_args(argv)

    app_root = _resolve_station_app(args.station_app)
    sys.path.insert(0, str(app_root / "python" / "station"))
    from app import Station, load_station
    global PowerReading
    from bench import PowerReading

    base_url = f"http://127.0.0.1:{args.control_port}"
    cfg = load_station(args.station)
    initial = control(base_url, "/state")
    check_virtual_config(cfg, args.layout, initial["endpoints"])
    assert not initial["detector"]["powered"] and initial["dmm"]["value"] == 0.5
    station = Station(cfg, lambda _message: None)
    try:
        summary = verify(cfg, args.layout, station, base_url)
    finally:
        try:
            station.output("detector_power", False)
        finally:
            try:
                station.output("cooler_power", False)
            finally:
                station.close()

    final = control(base_url, "/state")
    assert not final["detector"]["powered"] and not final["detector"]["registers"]
    assert final["dmm"]["value"] == 0.5
    summary["final"] = {"detector_off": True, "cooler_off": True, "dmm_diode_v": final["dmm"]["value"]}
    path = args.output or (args.station.resolve().parent / f"verification.{args.layout}.json")
    path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"VIRTUAL_HIL_PASS {args.layout} {path}")


if __name__ == "__main__":
    main()
