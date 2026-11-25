#!/usr/bin/env python3
"""5-hour no-disk multi-burst thermal drift monitor built on IR_station and IR_analysis C ABI.

Supports offline verification (--mode self-test), recorded RAW16 replay (--mode replay),
and live FX3 cameras (--mode directshow on Windows / --mode v4l2 on Linux).
Writes only lightweight CSV/JSON burst summaries and never saves RAW image files.
"""

from __future__ import annotations

import argparse
import csv
import ctypes as c
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import signal
import sys
from tempfile import TemporaryDirectory
import time

_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from ir_analysis import (  # noqa: E402
    IRCalcParameters,
    IRGjbBadPixelMasks,
    IRGjbBurstMetrics,
    IRGjbCriteria,
    IRGjbDriftContext,
    IRGjbSummary,
    IRMapSummary,
    IRNoise3DComputeOutputs,
    IRNoise3DComputeParams,
    IR_MASK_AND,
    IR_MASK_NOT,
    IR_NOISE_SUBTRACT_NONE,
    IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE,
    IR_SPECTRUM_AXIS_COLUMNS,
    IR_THRESH_MEAN_SIGMA,
    IR_THRESH_UPPER_ONLY,
    IR_WINDOW_RECTANGULAR,
    load_analysis,
    sdk_library_directory,
    verify_abi_sizes,
)

IR_OK = 0
IR_CANCELLED = 8

IR_DIRECTSHOW = 1
IR_RAW_REPLAY = 2
IR_V4L2 = 3
IR_PCN7LT = 2

IR_NOISE3D_MODE_ORG = 0
IR_NOISE3D_MODE_NUC = 1
IR_NOISE3D_SCALAR_DELTA_I = 1
IR_NOISE3D_SCALAR_DELTA_J = 2
IR_NOISE3D_SCALAR_DELTA_TOTAL = 7
IR_NOISE3D_SCALAR_COUNT = 16
IR_NOISE3D_HIST_GROUP_COUNT = 4


class IRGeometry(c.Structure):
    _fields_ = [
        ("width", c.c_int32),
        ("height", c.c_int32),
        ("valid_bits", c.c_int32),
    ]


class IRFrameStamp(c.Structure):
    _fields_ = [
        ("sequence", c.c_uint64),
        ("driver_sequence", c.c_uint64),
        ("timestamp_ns", c.c_int64),
        ("lost_frames", c.c_uint32),
        ("fields", c.c_uint32),
        ("time_origin", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRBlackbodyReading(c.Structure):
    _fields_ = [
        ("actual_c", c.c_double),
        ("setpoint_c", c.c_double),
        ("stable", c.c_int32),
        ("reserved", c.c_int32),
    ]


class IRCaptureStats(c.Structure):
    _fields_ = [
        ("frames_received", c.c_uint64),
        ("frames_delivered", c.c_uint64),
        ("frames_dropped", c.c_uint64),
        ("preview_frames", c.c_uint64),
        ("queue_capacity", c.c_int32),
        ("queue_depth", c.c_int32),
        ("width", c.c_int32),
        ("height", c.c_int32),
        ("running", c.c_int32),
        ("reserved", c.c_int32),
        ("estimated_fps", c.c_double),
    ]


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def load_station(directory: Path):
    if os.name == "nt":
        dll_path = directory / "IR_station.dll"
        dll_directory = os.add_dll_directory(str(directory))
        try:
            return c.CDLL(str(dll_path)), dll_directory
        except Exception:
            dll_directory.close()
            raise
    return c.CDLL(str(directory / "libIR_station.so")), None


def bind_sdk(station, analysis) -> None:
    verify_abi_sizes()
    if (c.sizeof(IRGeometry) != 12 or c.sizeof(IRFrameStamp) != 40 or
            c.sizeof(IRBlackbodyReading) != 24 or c.sizeof(IRCaptureStats) != 64):
        raise RuntimeError("Station ctypes structure size mismatch against C ABI")

    err_args = [c.c_char_p, c.c_int32]

    station.IR_StationVersion.argtypes = []
    station.IR_StationVersion.restype = c.c_char_p
    station.IR_RequestStop.argtypes = []
    station.IR_RequestStop.restype = None

    station_bindings = [
        ("IR_OpenFrames", [c.c_int32, c.c_char_p, c.c_int32, c.POINTER(IRGeometry), *err_args]),
        ("IR_StartFrames", [*err_args]),
        ("IR_ReadFrames", [
            c.c_int32, c.c_int32, c.POINTER(c.c_uint16), c.c_int64,
            c.POINTER(IRFrameStamp), c.c_int32, c.POINTER(c.c_int32), *err_args,
        ]),
        ("IR_FlushFrames", [c.c_int32, c.c_int32, c.POINTER(c.c_int32), *err_args]),
        ("IR_StopFrames", [*err_args]),
        ("IR_CloseFrames", [*err_args]),
        ("IR_SetFrameQueueCapacity", [c.c_int32, *err_args]),
        ("IR_TryReadLatestFrame", [
            c.POINTER(c.c_uint16), c.c_int64, c.POINTER(IRGeometry), c.POINTER(IRFrameStamp),
            c.POINTER(c.c_int32), *err_args,
        ]),
        ("IR_GetCaptureStats", [c.POINTER(IRCaptureStats), *err_args]),
        ("IR_OpenDevice", [c.c_int32, c.c_char_p, c.POINTER(c.c_uint64), *err_args]),
        ("IR_CloseDevice", [c.c_uint64, *err_args]),
        ("IR_SetBlackbody", [c.c_uint64, c.c_double, c.POINTER(c.c_double), *err_args]),
        ("IR_ReadBlackbody", [c.c_uint64, c.POINTER(IRBlackbodyReading), *err_args]),
        ("IR_WaitBlackbodyStable", [
            c.c_uint64, c.c_double, c.c_double, c.c_int32, c.c_int32, c.c_int32,
            c.POINTER(IRBlackbodyReading), *err_args,
        ]),
    ]
    for name, argtypes in station_bindings:
        fn = getattr(station, name)
        fn.argtypes = argtypes
        fn.restype = c.c_int32

    analysis.IR_AnalysisVersion.argtypes = []
    analysis.IR_AnalysisVersion.restype = c.c_char_p

    analysis_bindings = [
        ("IR_FrameStatisticsU16", [
            c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32, c.c_int32,
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int64,
            c.POINTER(c.c_double), c.c_int32, *err_args,
        ]),
        ("IR_FrameStatisticsNucU16", [
            c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32, c.c_int32,
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8),
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int64,
            c.POINTER(c.c_double), c.c_int32, *err_args,
        ]),
        ("IR_GjbEvaluateMaps", [
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double),
            c.c_int32, c.c_int32, c.c_int64,
            c.POINTER(IRCalcParameters), c.POINTER(IRGjbCriteria),
            c.POINTER(IRGjbSummary), c.POINTER(IRGjbBadPixelMasks), *err_args,
        ]),
        ("IR_BuildTwoPointNucF64", [
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.POINTER(c.c_double),
            c.c_int32, c.c_int32, c.c_int64,
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32, *err_args,
        ]),
        ("IR_MapSummaryF64", [
            c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
            c.c_int32, c.c_int32, c.c_int32, c.c_int32, c.c_double, c.c_double,
            c.POINTER(IRMapSummary), *err_args,
        ]),
        ("IR_ThresholdMaskF64", [
            c.POINTER(c.c_double), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
            c.c_int32, c.c_double, c.c_double,
            c.POINTER(c.c_uint8), c.POINTER(c.c_int32), c.POINTER(c.c_int32), *err_args,
        ]),
        ("IR_MaskCombine", [
            c.POINTER(c.c_uint8), c.POINTER(c.c_uint8), c.c_int32, c.c_int32, c.c_int64,
            c.c_int32, c.POINTER(c.c_uint8), c.POINTER(c.c_int32), *err_args,
        ]),
        ("IR_ComputeRfft1D", [
            c.POINTER(c.c_double), c.c_int32, c.c_double, c.c_int32, c.c_int32,
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32, *err_args,
        ]),
        ("IR_ChannelSpectrumU16", [
            c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32,
            c.c_int32, c.c_int32, c.c_double, c.c_int32,
            c.POINTER(c.c_double), c.c_int32, c.POINTER(c.c_double), c.c_int64,
            c.POINTER(c.c_double), c.c_int64, c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int32,
            *err_args,
        ]),
        ("IR_Noise3DComputeFramesDn", [
            c.POINTER(c.c_uint16), c.c_int64, c.c_int32, c.c_int32, c.c_int32,
            c.POINTER(c.c_uint8), c.c_int64, c.POINTER(c.c_double), c.POINTER(c.c_double), c.c_int64,
            c.POINTER(IRNoise3DComputeParams), c.POINTER(IRNoise3DComputeOutputs), *err_args,
        ]),
        ("IR_GjbEvaluateBurstMetrics", [
            c.POINTER(c.c_double), c.POINTER(c.c_double), c.POINTER(c.c_uint8),
            c.c_int32, c.c_int32, c.c_int64,
            c.POINTER(IRGjbDriftContext), c.POINTER(IRGjbBurstMetrics), *err_args,
        ]),
    ]
    for name, argtypes in analysis_bindings:
        fn = getattr(analysis, name)
        fn.argtypes = argtypes
        fn.restype = c.c_int32


def check_status(status: int, action: str, err_buf) -> None:
    if status != IR_OK:
        msg = err_buf.value.decode("utf-8", errors="replace")
        raise RuntimeError(f"{action} failed (status={status}): {msg}")


def build_synthetic_fixture(fixture_dir: Path, rows: int, cols: int, calib_frames: int,
                            burst_frames: int, burst_count: int) -> None:
    """Create a deterministic continuous synthetic RAW16 fixture with inter-burst transition garbage frames."""
    raw_chunks = bytearray()

    def push_frame(pixel_fn) -> None:
        buf = (c.c_uint16 * (rows * cols))()
        idx = 0
        for r in range(rows):
            for col in range(cols):
                buf[idx] = pixel_fn(r, col)
                idx += 1
        raw_chunks.extend(bytes(buf))

    def gain_at(col: int) -> float:
        return 0.85 + 0.30 * (float(col) / float(cols - 1))

    # Cold T0 calibration frames
    for f in range(calib_frames):
        t_noise = -4.0 if (f % 2) == 0 else 4.0
        push_frame(lambda r, col, tn=t_noise: round(30000.0 + gain_at(col) * 100.0 + 40.0 * r + tn))

    # Garbage transition frame expected to be discarded by IR_FlushFrames(1)
    push_frame(lambda r, col: 65000)

    # Hot T1 calibration frames
    for f in range(calib_frames):
        t_noise = -4.0 if (f % 2) == 0 else 4.0
        push_frame(lambda r, col, tn=t_noise: round(30000.0 + gain_at(col) * 1100.0 + 40.0 * r + tn))

    # Continuous drift bursts, each preceded by a garbage transition frame
    for b in range(burst_count):
        push_frame(lambda r, col: 64000)
        signal_dn = 600.0 + 100.0 * b
        for f in range(burst_frames):
            t_noise = -4.0 if (f % 2) == 0 else 4.0
            push_frame(
                lambda r, col, sig=signal_dn, tn=t_noise: round(30000.0 + gain_at(col) * sig + 40.0 * r + tn)
            )

    total_frames = len(raw_chunks) // (rows * cols * 2)
    (fixture_dir / "frames.raw").write_bytes(bytes(raw_chunks))
    with (fixture_dir / "frames.csv").open("w", encoding="utf-8", newline="") as fp:
        fp.write("frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n")
        stride = cols * 2
        for idx in range(total_frames):
            fp.write(f"{idx},{cols},{rows},{stride},RAW16_LE,16,little\n")


def parse_host_port(text: str) -> tuple[str, int]:
    host, port_text = text.rsplit(":", 1)
    port = int(port_text)
    if not host or not (1 <= port <= 65535):
        raise ValueError(f"Invalid --blackbody-tcp value: {text}")
    return host, port


def run_drift_session(args: argparse.Namespace, station, analysis, source_path: str) -> dict:
    err = c.create_string_buffer(1024)
    stop_flag = False
    bb_handle = c.c_uint64(0)
    frames_opened = False
    frames_started = False

    def handle_signal(_signum, _frame) -> None:
        nonlocal stop_flag
        stop_flag = True
        station.IR_RequestStop()

    prev_sigint = signal.getsignal(signal.SIGINT)
    signal.signal(signal.SIGINT, handle_signal)

    backend_map = {
        "directshow": IR_DIRECTSHOW,
        "replay": IR_RAW_REPLAY,
        "v4l2": IR_V4L2,
        "self-test": IR_RAW_REPLAY,
    }
    backend = backend_map[args.mode]

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "drift_summary.csv"
    meta_path = output_dir / "drift_meta.json"

    started_utc = utc_now_iso()
    rows_written: list[dict] = []
    calib_info: dict = {}
    drift_fft_summary: dict = {}
    capture_stats_summary: dict = {}
    geom = IRGeometry()

    try:
        # Stage 0: Optional blackbody + open & start stream + preallocate buffers
        if args.blackbody_tcp:
            host, port = parse_host_port(args.blackbody_tcp)
            bb_cfg = {
                "host": host,
                "tcp_port": port,
                "head": args.bb_head,
                "timeout_ms": args.bb_timeout_ms,
            }
            if args.bb_expected_serial:
                bb_cfg["expected_head_serial"] = args.bb_expected_serial
            check_status(
                station.IR_OpenDevice(IR_PCN7LT, json.dumps(bb_cfg).encode("utf-8"), c.byref(bb_handle), err, len(err)),
                "IR_OpenDevice",
                err,
            )

        check_status(
            station.IR_OpenFrames(backend, source_path.encode("utf-8"), args.valid_bits, c.byref(geom), err, len(err)),
            "IR_OpenFrames",
            err,
        )
        frames_opened = True
        rows, cols = geom.height, geom.width
        n_pix = rows * cols
        max_frames = max(args.calib_frames, args.burst_frames)

        check_status(station.IR_StartFrames(err, len(err)), "IR_StartFrames", err)
        frames_started = True

        # Constant-footprint reusable buffers (never write RAW to disk)
        raw_buf = (c.c_uint16 * (max_frames * n_pix))()
        stamps = (IRFrameStamp * max_frames)()
        completed = c.c_int32(0)
        discarded = c.c_int32(0)

        v2_mean = (c.c_double * n_pix)()
        vn2 = (c.c_double * n_pix)()
        v1_mean = (c.c_double * n_pix)()
        vn1 = (c.c_double * n_pix)()
        stat_sum = (c.c_double * 2)()

        def stabilize_bb(target_c: float) -> IRBlackbodyReading:
            readback = c.c_double(0.0)
            reading = IRBlackbodyReading()
            check_status(
                station.IR_SetBlackbody(bb_handle, target_c, c.byref(readback), err, len(err)),
                "IR_SetBlackbody",
                err,
            )
            check_status(
                station.IR_WaitBlackbodyStable(
                    bb_handle, target_c, args.bb_tol_c, args.bb_stable_ms, args.bb_timeout_ms,
                    1 if args.bb_require_stable else 0, c.byref(reading), err, len(err),
                ),
                "IR_WaitBlackbodyStable",
                err,
            )
            return reading

        # Stage 1: t=0 Two-point calibration (T0 cold -> transition -> T1 hot)
        if bb_handle.value:
            stabilize_bb(args.bb_t0_c)
        check_status(
            station.IR_ReadFrames(
                args.calib_frames, args.frame_timeout_ms, raw_buf, len(raw_buf),
                stamps, max_frames, c.byref(completed), err, len(err),
            ),
            "IR_ReadFrames(T0)",
            err,
        )
        check_status(
            analysis.IR_FrameStatisticsU16(
                raw_buf, completed.value * n_pix, completed.value, rows, cols, 0,
                v2_mean, vn2, n_pix, stat_sum, 2, err, len(err),
            ),
            "IR_FrameStatisticsU16(T0)",
            err,
        )

        check_status(
            station.IR_FlushFrames(args.flush_discard, args.frame_timeout_ms, c.byref(discarded), err, len(err)),
            "IR_FlushFrames(T0->T1)",
            err,
        )
        if bb_handle.value:
            stabilize_bb(args.bb_t1_c)
        check_status(
            station.IR_ReadFrames(
                args.calib_frames, args.frame_timeout_ms, raw_buf, len(raw_buf),
                stamps, max_frames, c.byref(completed), err, len(err),
            ),
            "IR_ReadFrames(T1)",
            err,
        )
        check_status(
            analysis.IR_FrameStatisticsU16(
                raw_buf, completed.value * n_pix, completed.value, rows, cols, 0,
                v1_mean, vn1, n_pix, stat_sum, 2, err, len(err),
            ),
            "IR_FrameStatisticsU16(T1)",
            err,
        )

        # GJB initial evaluation & valid pixel mask
        calc_params = IRCalcParameters(
            args.bb_t0_c, args.bb_t1_c, 15.0, 1.0, 1.0, 2.0, 1.0, 2.0,
            1000, 0, (1 << geom.valid_bits) - 1 if 1 <= geom.valid_bits <= 16 else 65535, 0,
        )
        gjb_crit = IRGjbCriteria(1.5, 1.5, 0.4, 0.4, 2, 0)
        valid_mask = (c.c_uint8 * n_pix)(*([1] * n_pix))
        gjb_masks = IRGjbBadPixelMasks()
        gjb_masks.valid_mask = valid_mask
        gjb_masks.pixel_capacity = n_pix
        gjb_init = IRGjbSummary()
        check_status(
            analysis.IR_GjbEvaluateMaps(
                v1_mean, v2_mean, vn1, v2_mean, rows, cols, n_pix,
                c.byref(calc_params), c.byref(gjb_crit), c.byref(gjb_init),
                c.byref(gjb_masks), err, len(err),
            ),
            "IR_GjbEvaluateMaps(init)",
            err,
        )

        k_map = (c.c_double * n_pix)()
        b_map = (c.c_double * n_pix)()
        nuc_meta = (c.c_double * 4)()
        check_status(
            analysis.IR_BuildTwoPointNucF64(
                v1_mean, v2_mean, valid_mask, None, rows, cols, n_pix,
                k_map, b_map, nuc_meta, 4, err, len(err),
            ),
            "IR_BuildTwoPointNucF64",
            err,
        )

        blind_mask = (c.c_uint8 * n_pix)()
        blind_count = c.c_int32(0)
        check_status(
            analysis.IR_MaskCombine(
                valid_mask, None, rows, cols, n_pix, IR_MASK_NOT,
                blind_mask, c.byref(blind_count), err, len(err),
            ),
            "IR_MaskCombine(NOT)",
            err,
        )

        noise_sub_mode = (
            IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE
            if args.noise_subtract == "quadrature"
            else IR_NOISE_SUBTRACT_NONE
        )
        drift_ctx = IRGjbDriftContext(
            gjb_init.volts_per_count,
            max(1e-6, abs(args.bb_t1_c - args.bb_t0_c)),
            gjb_init.before_avg_pixel_response_count,
            gjb_init.after_avg_responsivity,
            2.0,
            1.0,
            args.burst_frames,
            noise_sub_mode,
            args.lrnu_kernel,
            0,
        )

        calib_info = {
            "t0_c": args.bb_t0_c,
            "t1_c": args.bb_t1_c,
            "valid_pixels": int(nuc_meta[3]),
            "initial_bad_pixels": gjb_init.bad_pixels.total_bad_count,
            "volts_per_count": gjb_init.volts_per_count,
            "init_after_responsivity": gjb_init.after_avg_responsivity,
        }

        if bb_handle.value and args.bb_target_c is not None:
            stabilize_bb(args.bb_target_c)

        # Reusable burst buffers
        org_mean = (c.c_double * n_pix)()
        org_vn = (c.c_double * n_pix)()
        nuc_mean = (c.c_double * n_pix)()
        nuc_vn = (c.c_double * n_pix)()
        org_map_sum = IRMapSummary()
        nuc_map_sum = IRMapSummary()
        org_gjb = IRGjbBurstMetrics()
        nuc_gjb = IRGjbBurstMetrics()

        dyn_mask = (c.c_uint8 * n_pix)()
        comb_mask = (c.c_uint8 * n_pix)()
        pass_cnt = c.c_int32(0)
        fail_cnt = c.c_int32(0)
        comb_cnt = c.c_int32(0)

        fft_channels = args.fft_channels if (args.fft_channels > 0 and cols % args.fft_channels == 0) else 0
        fft_bins = args.burst_frames // 2 + 1
        ch_freqs = (c.c_double * fft_bins)()
        ch_amps = (c.c_double * (max(1, fft_channels) * fft_bins))()

        hist_interval = 4
        n3d_scalars_org = (c.c_double * IR_NOISE3D_SCALAR_COUNT)()
        n3d_scalars_nuc = (c.c_double * IR_NOISE3D_SCALAR_COUNT)()
        n3d_hist = (c.c_double * (IR_NOISE3D_HIST_GROUP_COUNT * hist_interval))()
        n3d_hist_meta = (c.c_double * (IR_NOISE3D_HIST_GROUP_COUNT * 3))()
        n3d_map_ij = (c.c_double * n_pix)()
        n3d_map_ti = (c.c_double * (args.burst_frames * rows))()
        n3d_map_tj = (c.c_double * (args.burst_frames * cols))()
        n3d_out = IRNoise3DComputeOutputs(
            None, n3d_scalars_org, n3d_hist, n3d_hist_meta,
            n3d_map_ij, n3d_map_ti, n3d_map_tj, None,
            0, len(n3d_hist), n_pix, len(n3d_map_ti), len(n3d_map_tj),
            IR_NOISE3D_SCALAR_COUNT, len(n3d_hist_meta), 0, 0,
        )
        n3d_params_org = IRNoise3DComputeParams(IR_NOISE3D_MODE_ORG, 1, hist_interval, 0, 0.0, -999.0)
        n3d_params_nuc = IRNoise3DComputeParams(IR_NOISE3D_MODE_NUC, 1, hist_interval, 0, nuc_meta[0], -999.0)

        fieldnames = [
            "burst_index", "timestamp_utc", "elapsed_s", "bb_actual_c", "bb_stable",
            "discarded_frames", "completed_frames", "first_seq", "last_seq", "lost_frames",
            "org_mean_dn", "org_pop_std_dn", "org_span_dn", "org_spatial_std_dn",
            "org_spatial_denoised_dn", "org_nonuniformity_pct", "org_avg_vn_dn",
            "org_netd_mk", "org_lrnu_pct", "org_hrnu_pct",
            "nuc_mean_dn", "nuc_span_dn", "nuc_spatial_std_dn",
            "nuc_spatial_denoised_dn", "nuc_nonuniformity_pct", "nuc_avg_vn_dn",
            "nuc_netd_mk", "nuc_lrnu_pct", "nuc_hrnu_pct",
            "thresh_pass_pixels", "thresh_fail_pixels", "ch0_peak_freq_hz", "ch0_peak_amp_dn",
            "n3d_org_total_dn", "n3d_nuc_total_dn", "n3d_org_ti_dn", "n3d_nuc_ti_dn",
            "n3d_org_tj_dn", "n3d_nuc_tj_dn",
        ]

        start_mono = time.monotonic()
        duration_s = args.duration_hours * 3600.0
        target_bb_c = args.bb_target_c if args.bb_target_c is not None else args.bb_t1_c

        with csv_path.open("w", encoding="utf-8", newline="") as csv_fp:
            writer = csv.DictWriter(csv_fp, fieldnames=fieldnames)
            writer.writeheader()
            csv_fp.flush()

            burst_idx = 0
            while not stop_flag:
                if args.max_bursts > 0 and burst_idx >= args.max_bursts:
                    break
                elapsed_s = time.monotonic() - start_mono
                if args.max_bursts == 0 and elapsed_s >= duration_s:
                    break

                bb_actual_str = ""
                bb_stable_str = ""
                if bb_handle.value:
                    bb_reading = IRBlackbodyReading()
                    st_bb = station.IR_WaitBlackbodyStable(
                        bb_handle, target_bb_c, args.bb_tol_c, args.bb_stable_ms, args.bb_timeout_ms,
                        1 if args.bb_require_stable else 0, c.byref(bb_reading), err, len(err),
                    )
                    if st_bb == IR_CANCELLED:
                        stop_flag = True
                        break
                    check_status(st_bb, f"IR_WaitBlackbodyStable(burst={burst_idx})", err)
                    bb_actual_str = f"{bb_reading.actual_c:.4f}"
                    bb_stable_str = str(bb_reading.stable)

                st_flush = station.IR_FlushFrames(
                    args.flush_discard, args.frame_timeout_ms, c.byref(discarded), err, len(err)
                )
                if st_flush == IR_CANCELLED:
                    stop_flag = True
                    break
                check_status(st_flush, f"IR_FlushFrames(burst={burst_idx})", err)

                st_read = station.IR_ReadFrames(
                    args.burst_frames, args.frame_timeout_ms, raw_buf, len(raw_buf),
                    stamps, max_frames, c.byref(completed), err, len(err),
                )
                if st_read == IR_CANCELLED:
                    stop_flag = True
                    break
                check_status(st_read, f"IR_ReadFrames(burst={burst_idx})", err)

                n_frames = completed.value
                total_samples = n_frames * n_pix
                lost_total = sum(stamps[i].lost_frames for i in range(n_frames))

                # Pre-NUC stats + map summary + GJB burst
                check_status(
                    analysis.IR_FrameStatisticsU16(
                        raw_buf, total_samples, n_frames, rows, cols, 0,
                        org_mean, org_vn, n_pix, stat_sum, 2, err, len(err),
                    ),
                    "IR_FrameStatisticsU16(ORG)",
                    err,
                )
                check_status(
                    analysis.IR_MapSummaryF64(
                        org_mean, valid_mask, rows, cols, n_pix, 0, 0, 0, 0, 5.0, 95.0,
                        c.byref(org_map_sum), err, len(err),
                    ),
                    "IR_MapSummaryF64(ORG)",
                    err,
                )
                check_status(
                    analysis.IR_GjbEvaluateBurstMetrics(
                        org_mean, org_vn, valid_mask, rows, cols, n_pix,
                        c.byref(drift_ctx), c.byref(org_gjb), err, len(err),
                    ),
                    "IR_GjbEvaluateBurstMetrics(ORG)",
                    err,
                )

                # Post-NUC lossless stats + map summary + GJB burst
                check_status(
                    analysis.IR_FrameStatisticsNucU16(
                        raw_buf, total_samples, n_frames, rows, cols, 0,
                        k_map, b_map, None, valid_mask,
                        nuc_mean, nuc_vn, n_pix, stat_sum, 2, err, len(err),
                    ),
                    "IR_FrameStatisticsNucU16(NUC)",
                    err,
                )
                check_status(
                    analysis.IR_MapSummaryF64(
                        nuc_mean, valid_mask, rows, cols, n_pix, 0, 0, 0, 0, 5.0, 95.0,
                        c.byref(nuc_map_sum), err, len(err),
                    ),
                    "IR_MapSummaryF64(NUC)",
                    err,
                )
                check_status(
                    analysis.IR_GjbEvaluateBurstMetrics(
                        nuc_mean, nuc_vn, valid_mask, rows, cols, n_pix,
                        c.byref(drift_ctx), c.byref(nuc_gjb), err, len(err),
                    ),
                    "IR_GjbEvaluateBurstMetrics(NUC)",
                    err,
                )

                # Dynamic noise threshold check
                t_mode = IR_THRESH_MEAN_SIGMA if args.vn_thresh_mode == "mean_sigma" else IR_THRESH_UPPER_ONLY
                t_low = args.vn_thresh_high if args.vn_thresh_mode == "mean_sigma" else 0.0
                check_status(
                    analysis.IR_ThresholdMaskF64(
                        org_vn, valid_mask, rows, cols, n_pix, t_mode, t_low, args.vn_thresh_high,
                        dyn_mask, c.byref(pass_cnt), c.byref(fail_cnt), err, len(err),
                    ),
                    "IR_ThresholdMaskF64",
                    err,
                )
                check_status(
                    analysis.IR_MaskCombine(
                        valid_mask, dyn_mask, rows, cols, n_pix, IR_MASK_AND,
                        comb_mask, c.byref(comb_cnt), err, len(err),
                    ),
                    "IR_MaskCombine(AND)",
                    err,
                )

                # Channel FFT check
                ch0_peak_freq = 0.0
                ch0_peak_amp = 0.0
                if fft_channels > 0 and n_frames >= 2:
                    bins = n_frames // 2 + 1
                    check_status(
                        analysis.IR_ChannelSpectrumU16(
                            raw_buf, total_samples, n_frames, rows, cols,
                            IR_SPECTRUM_AXIS_COLUMNS, fft_channels, args.sample_rate_hz,
                            IR_WINDOW_RECTANGULAR,
                            ch_freqs, len(ch_freqs), None, 0,
                            ch_amps, len(ch_amps), None, None, 0, err, len(err),
                        ),
                        "IR_ChannelSpectrumU16",
                        err,
                    )
                    if bins > 1:
                        best_k = max(range(1, bins), key=lambda k: ch_amps[k])
                        ch0_peak_freq = ch_freqs[best_k]
                        ch0_peak_amp = ch_amps[best_k]

                # Optional Noise3D Pre/Post-NUC check
                n3d_org_tot = n3d_nuc_tot = n3d_org_ti = n3d_nuc_ti = n3d_org_tj = n3d_nuc_tj = ""
                if args.enable_noise3d and n_frames >= 2:
                    n3d_out.scalars = c.cast(n3d_scalars_org, c.POINTER(c.c_double))
                    check_status(
                        analysis.IR_Noise3DComputeFramesDn(
                            raw_buf, total_samples, n_frames, rows, cols,
                            blind_mask, n_pix, None, None, 0,
                            c.byref(n3d_params_org), c.byref(n3d_out), err, len(err),
                        ),
                        "IR_Noise3DComputeFramesDn(ORG)",
                        err,
                    )
                    n3d_out.scalars = c.cast(n3d_scalars_nuc, c.POINTER(c.c_double))
                    check_status(
                        analysis.IR_Noise3DComputeFramesDn(
                            raw_buf, total_samples, n_frames, rows, cols,
                            blind_mask, n_pix, k_map, v1_mean, n_pix,
                            c.byref(n3d_params_nuc), c.byref(n3d_out), err, len(err),
                        ),
                        "IR_Noise3DComputeFramesDn(NUC)",
                        err,
                    )
                    n3d_org_tot = f"{n3d_scalars_org[IR_NOISE3D_SCALAR_DELTA_TOTAL]:.9g}"
                    n3d_nuc_tot = f"{n3d_scalars_nuc[IR_NOISE3D_SCALAR_DELTA_TOTAL]:.9g}"
                    n3d_org_ti = f"{n3d_scalars_org[IR_NOISE3D_SCALAR_DELTA_I]:.9g}"
                    n3d_nuc_ti = f"{n3d_scalars_nuc[IR_NOISE3D_SCALAR_DELTA_I]:.9g}"
                    n3d_org_tj = f"{n3d_scalars_org[IR_NOISE3D_SCALAR_DELTA_J]:.9g}"
                    n3d_nuc_tj = f"{n3d_scalars_nuc[IR_NOISE3D_SCALAR_DELTA_J]:.9g}"

                row = {
                    "burst_index": burst_idx,
                    "timestamp_utc": utc_now_iso(),
                    "elapsed_s": f"{time.monotonic() - start_mono:.3f}",
                    "bb_actual_c": bb_actual_str,
                    "bb_stable": bb_stable_str,
                    "discarded_frames": discarded.value,
                    "completed_frames": n_frames,
                    "first_seq": stamps[0].sequence,
                    "last_seq": stamps[n_frames - 1].sequence,
                    "lost_frames": lost_total,
                    "org_mean_dn": f"{org_gjb.mean_dn:.6f}",
                    "org_pop_std_dn": f"{org_map_sum.pop_std:.6f}",
                    "org_span_dn": f"{org_map_sum.span:.6f}",
                    "org_spatial_std_dn": f"{org_gjb.spatial_std_dn:.6f}",
                    "org_spatial_denoised_dn": f"{org_gjb.spatial_std_denoised_dn:.6f}",
                    "org_nonuniformity_pct": f"{org_gjb.nonuniformity_pct:.6f}",
                    "org_avg_vn_dn": f"{org_gjb.avg_temporal_noise_dn:.6f}",
                    "org_netd_mk": f"{org_gjb.estimated_netd_mk:.6f}",
                    "org_lrnu_pct": f"{org_gjb.lrnu_pct:.6f}",
                    "org_hrnu_pct": f"{org_gjb.hrnu_pct:.6f}",
                    "nuc_mean_dn": f"{nuc_gjb.mean_dn:.6f}",
                    "nuc_span_dn": f"{nuc_map_sum.span:.6f}",
                    "nuc_spatial_std_dn": f"{nuc_gjb.spatial_std_dn:.9g}",
                    "nuc_spatial_denoised_dn": f"{nuc_gjb.spatial_std_denoised_dn:.9g}",
                    "nuc_nonuniformity_pct": f"{nuc_gjb.nonuniformity_pct:.9g}",
                    "nuc_avg_vn_dn": f"{nuc_gjb.avg_temporal_noise_dn:.6f}",
                    "nuc_netd_mk": f"{nuc_gjb.estimated_netd_mk:.6f}",
                    "nuc_lrnu_pct": f"{nuc_gjb.lrnu_pct:.9g}",
                    "nuc_hrnu_pct": f"{nuc_gjb.hrnu_pct:.9g}",
                    "thresh_pass_pixels": comb_cnt.value,
                    "thresh_fail_pixels": fail_cnt.value,
                    "ch0_peak_freq_hz": f"{ch0_peak_freq:.6f}",
                    "ch0_peak_amp_dn": f"{ch0_peak_amp:.6f}",
                    "n3d_org_total_dn": n3d_org_tot,
                    "n3d_nuc_total_dn": n3d_nuc_tot,
                    "n3d_org_ti_dn": n3d_org_ti,
                    "n3d_nuc_ti_dn": n3d_nuc_ti,
                    "n3d_org_tj_dn": n3d_org_tj,
                    "n3d_nuc_tj_dn": n3d_nuc_tj,
                }
                writer.writerow(row)
                csv_fp.flush()
                rows_written.append(row)
                burst_idx += 1

                if args.mode in ("directshow", "v4l2") and args.interval_sec > 0.0:
                    if args.max_bursts == 0 or burst_idx < args.max_bursts:
                        deadline = time.monotonic() + args.interval_sec
                        while not stop_flag and time.monotonic() < deadline:
                            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))

        # Stage 3: Long-term mean drift 1D FFT across bursts
        if len(rows_written) >= 2:
            n_pts = len(rows_written)
            bins = n_pts // 2 + 1
            mean_series = (c.c_double * n_pts)(*[float(r["org_mean_dn"]) for r in rows_written])
            freqs = (c.c_double * bins)()
            amps = (c.c_double * bins)()
            drift_rate_hz = (1.0 / args.interval_sec) if args.interval_sec > 0.0 else 1.0
            check_status(
                analysis.IR_ComputeRfft1D(
                    mean_series, n_pts, drift_rate_hz, IR_WINDOW_RECTANGULAR, 1,
                    freqs, amps, None, bins, err, len(err),
                ),
                "IR_ComputeRfft1D(drift_series)",
                err,
            )
            best_k = max(range(1, bins), key=lambda k: amps[k]) if bins > 1 else 0
            drift_fft_summary = {
                "bursts": n_pts,
                "sample_rate_hz": drift_rate_hz,
                "dc_removed": True,
                "peak_freq_hz": freqs[best_k],
                "peak_amp_dn": amps[best_k],
            }
    finally:
        signal.signal(signal.SIGINT, prev_sigint)
        stats_c = IRCaptureStats()
        if station.IR_GetCaptureStats(c.byref(stats_c), err, len(err)) == IR_OK:
            capture_stats_summary = {
                "frames_received": stats_c.frames_received,
                "frames_delivered": stats_c.frames_delivered,
                "frames_dropped": stats_c.frames_dropped,
                "preview_frames": stats_c.preview_frames,
                "queue_capacity": stats_c.queue_capacity,
                "queue_depth": stats_c.queue_depth,
                "estimated_fps": stats_c.estimated_fps,
            }
        if frames_started:
            station.IR_StopFrames(err, len(err))
        if frames_opened:
            station.IR_CloseFrames(err, len(err))
        if bb_handle.value:
            station.IR_CloseDevice(bb_handle, err, len(err))

    meta = {
        "station_version": station.IR_StationVersion().decode("utf-8"),
        "analysis_version": analysis.IR_AnalysisVersion().decode("utf-8"),
        "mode": args.mode,
        "source": "<synthetic-self-test>" if args.mode == "self-test" else source_path,
        "raw_saved": False,
        "cancelled": stop_flag,
        "started_utc": started_utc,
        "finished_utc": utc_now_iso(),
        "geometry": {"width": geom.width, "height": geom.height, "valid_bits": geom.valid_bits},
        "config": {
            "duration_hours": args.duration_hours,
            "interval_sec": args.interval_sec,
            "burst_frames": args.burst_frames,
            "calib_frames": args.calib_frames,
            "flush_discard": args.flush_discard,
            "noise_subtract": args.noise_subtract,
            "lrnu_kernel": args.lrnu_kernel,
            "vn_thresh_mode": args.vn_thresh_mode,
            "vn_thresh_high": args.vn_thresh_high,
            "enable_noise3d": args.enable_noise3d,
            "fft_channels": args.fft_channels,
        },
        "calibration": calib_info,
        "burst_count": len(rows_written),
        "capture_stats": capture_stats_summary,
        "drift_series_fft": drift_fft_summary,
    }
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    return {"csv_path": csv_path, "meta_path": meta_path, "rows": rows_written, "meta": meta}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("self-test", "replay", "directshow", "v4l2"), default="self-test")
    parser.add_argument("--library-dir", type=Path, default=sdk_library_directory())
    parser.add_argument("--source", default="", help="camera name/device or RAW16 directory")
    parser.add_argument("--valid-bits", type=int, default=16)
    parser.add_argument("--output-dir", type=Path, default=Path("./drift_nodisk_out"))
    parser.add_argument("--duration-hours", type=float, default=5.0)
    parser.add_argument("--interval-sec", type=float, default=60.0)
    parser.add_argument("--max-bursts", type=int, default=0, help="0 = run until duration_hours")
    parser.add_argument("--calib-frames", type=int, default=0, help="0 = auto (4 in self-test, else 32)")
    parser.add_argument("--burst-frames", type=int, default=0, help="0 = auto (8 in self-test, else 100)")
    parser.add_argument("--flush-discard", type=int, default=-1, help="-1 = auto (1 in self-test, else 2)")
    parser.add_argument("--frame-timeout-ms", type=int, default=2000)
    parser.add_argument("--blackbody-tcp", default="", help="optional host:port for PCN7-LT blackbody")
    parser.add_argument("--bb-head", type=int, default=0)
    parser.add_argument("--bb-expected-serial", default="")
    parser.add_argument("--bb-t0-c", type=float, default=20.0)
    parser.add_argument("--bb-t1-c", type=float, default=30.0)
    parser.add_argument("--bb-target-c", type=float, default=None)
    parser.add_argument("--bb-tol-c", type=float, default=0.05)
    parser.add_argument("--bb-stable-ms", type=int, default=500)
    parser.add_argument("--bb-timeout-ms", type=int, default=5000)
    parser.add_argument("--bb-require-stable", action="store_true")
    parser.add_argument("--noise-subtract", choices=("none", "quadrature"), default="quadrature")
    parser.add_argument("--lrnu-kernel", type=int, default=15)
    parser.add_argument("--vn-thresh-mode", choices=("upper", "mean_sigma"), default="upper")
    parser.add_argument("--vn-thresh-high", type=float, default=10.0)
    parser.add_argument("--enable-noise3d", action="store_true", help="compute Noise3D before/after NUC")
    parser.add_argument("--fft-channels", type=int, default=4, help="0 to disable channel FFT")
    parser.add_argument("--sample-rate-hz", type=float, default=50.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    is_self_test = args.mode == "self-test"
    if args.calib_frames <= 0:
        args.calib_frames = 4 if is_self_test else 32
    if args.burst_frames <= 0:
        args.burst_frames = 8 if is_self_test else 100
    if args.flush_discard < 0:
        args.flush_discard = 1 if is_self_test else 2
    if is_self_test and args.max_bursts == 0:
        args.max_bursts = 3
    if is_self_test:
        args.enable_noise3d = True

    if not is_self_test and not args.source:
        raise SystemExit("--source is required when --mode is not self-test")

    lib_dir = args.library_dir.resolve()
    analysis, analysis_dll_dir = load_analysis(lib_dir)
    station, station_dll_dir = load_station(lib_dir)
    try:
        bind_sdk(station, analysis)
        if is_self_test:
            with TemporaryDirectory(prefix="ir_drift_selftest_") as tmp:
                fixture_dir = Path(tmp)
                build_synthetic_fixture(
                    fixture_dir, rows=16, cols=16,
                    calib_frames=args.calib_frames, burst_frames=args.burst_frames,
                    burst_count=args.max_bursts,
                )
                result = run_drift_session(args, station, analysis, str(fixture_dir))
                rows = result["rows"]
                meta = result["meta"]
                raw_files = list(Path(args.output_dir).rglob("*.raw"))
                assert not raw_files, f"Expected zero RAW files, found: {raw_files}"
                assert meta["raw_saved"] is False
                assert len(rows) == args.max_bursts, (len(rows), args.max_bursts)
                for idx, row in enumerate(rows):
                    org_std = float(row["org_spatial_std_dn"])
                    nuc_std = float(row["nuc_spatial_std_dn"])
                    assert org_std > 50.0, (idx, org_std)
                    assert nuc_std < 1e-10, (idx, nuc_std)
                    if idx > 0:
                        assert float(row["org_mean_dn"]) > float(rows[idx - 1]["org_mean_dn"]), rows
                    assert float(row["n3d_nuc_total_dn"]) < float(row["n3d_org_total_dn"]), row
                if args.blackbody_tcp:
                    expected_bb = args.bb_target_c if args.bb_target_c is not None else args.bb_t1_c
                    assert math.isclose(float(rows[-1]["bb_actual_c"]), expected_bb, abs_tol=1e-3), rows[-1]
        else:
            result = run_drift_session(args, station, analysis, args.source)

        meta = result["meta"]
        print(f"station_version={meta['station_version']}")
        print(f"analysis_version={meta['analysis_version']}")
        print(f"burst_count={meta['burst_count']}")
        print(f"raw_saved={str(meta['raw_saved']).lower()}")
        if result["rows"]:
            last = result["rows"][-1]
            print(f"last_org_mean_dn={last['org_mean_dn']}")
            print(f"last_org_spatial_std_dn={last['org_spatial_std_dn']}")
            print(f"last_nuc_spatial_std_dn={last['nuc_spatial_std_dn']}")
            if last["bb_actual_c"]:
                print(f"last_bb_actual_c={last['bb_actual_c']}")
        print(f"csv_path={result['csv_path']}")
        print(f"meta_path={result['meta_path']}")
        return 0
    finally:
        if station_dll_dir is not None:
            station_dll_dir.close()
        if analysis_dll_dir is not None:
            analysis_dll_dir.close()


if __name__ == "__main__":
    raise SystemExit(main())
