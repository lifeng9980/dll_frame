"""Library loading, C ABI signatures and calling helpers for the IR SDK binding.

This module complements :mod:`irsdk.abi` (structs/enums/callbacks) with:

* the exact ``argtypes``/``restype`` of all 57 exported functions (35 station + 22 analysis),
* discovery of the two shared libraries (explicit path, environment, package layout),
* UTF-8 error-buffer helpers (``call_sdk`` / ``error_text`` / ``status_name``).

Standard library only; nothing here talks to hardware by itself.
"""
from __future__ import annotations

import ctypes as c
import os
from pathlib import Path

from .abi import (
    D,
    HANDLE,
    I,
    I64,
    P,
    U8,
    U16,
    IRArrayView,
    IRBlackbodyReading,
    IRCalcParameters,
    IRCaptureStats,
    IRCoolerReading,
    IRDetectorCurrentReading,
    IRDmmReading,
    IRFrameBatchView,
    IRFrameSourceInfo,
    IRFlickerSummary,
    IRFft2dSummary,
    IRGbBadPixelMasks,
    IRGbBadPixelSummary,
    IRGbCriteria,
    IRGbSummary,
    IRGjbBadPixelMasks,
    IRGjbBadPixelSummary,
    IRGjbBurstMetrics,
    IRGjbCriteria,
    IRGjbDriftContext,
    IRGjbSummary,
    IRMapSummary,
    IRNoise3DCalibOutputs,
    IRNoise3DCalibParams,
    IRNoise3DComputeOutputs,
    IRNoise3DComputeParams,
    IROvenReading,
    IRPowerCurrentStableSpec,
    IRPowerReading,
    IRRecipeResult,
    IRRecipeRunOptions,
    STATUS_NAMES as _STATUS_NAMES,
)

_ERR_ARGS = (c.c_char_p, I)

# name -> leading argtypes; the trailing (char *err, int32_t err_bytes) is appended.
STATION_SIGNATURES = (
    ("IR_OpenFrames", (I, c.c_char_p, I, P(IRFrameSourceInfo))),
    ("IR_StartFrames", ()),
    ("IR_ReadFrames", (I, P(IRFrameBatchView))),
    ("IR_FlushFrames", (I, I, P(I))),
    ("IR_StopFrames", ()),
    ("IR_CloseFrames", ()),
    ("IR_SetFrameQueueCapacity", (I,)),
    ("IR_TryReadLatestFrame", (P(IRFrameBatchView),)),
    ("IR_GetCaptureStats", (P(IRCaptureStats),)),
    ("IR_OpenDevice", (I, c.c_char_p, P(HANDLE))),
    ("IR_CloseDevice", (HANDLE,)),
    ("IR_SetBlackbody", (HANDLE, D, P(D))),
    ("IR_ReadBlackbody", (HANDLE, P(IRBlackbodyReading))),
    ("IR_WaitBlackbodyStable", (HANDLE, D, D, I, I, I, P(IRBlackbodyReading))),
    ("IR_SetPowerChannel", (HANDLE, I, D, D, P(IRPowerReading))),
    ("IR_SetPowerOutput", (HANDLE, I, I, P(IRPowerReading))),
    ("IR_ReadPower", (HANDLE, I, P(IRPowerReading))),
    ("IR_WaitPowerCurrentStable", (HANDLE, I, P(IRPowerCurrentStableSpec), P(IRPowerReading))),
    ("IR_ListSerialPorts", (c.c_char_p, I, P(I))),
    ("IR_ListCaptureDevices", (I, c.c_char_p, I, P(I))),
    ("IR_SetOven", (HANDLE, D, P(D))),
    ("IR_ReadOven", (HANDLE, P(IROvenReading))),
    ("IR_StopOven", (HANDLE,)),
    ("IR_WaitOvenStable", (HANDLE, D, D, I, I, I, P(IROvenReading))),
    ("IR_SetDmmMode", (HANDLE, I)),
    ("IR_ReadDmm", (HANDLE, I, P(IRDmmReading))),
    ("IR_SetDetectorTiming", (HANDLE, I, I, I)),
    ("IR_SetDetectorGpol", (HANDLE, D)),
    ("IR_SetDetectorRegister", (HANDLE, U8, P(U8), I)),
    ("IR_MeasureDetectorCurrent", (HANDLE, I, U16, P(IRDetectorCurrentReading))),
    ("IR_ReadCooler", (HANDLE, P(IRCoolerReading))),
    ("IR_CheckRecipeJson", (c.c_char_p, c.c_char_p)),
    ("IR_RunRecipeJson", (c.c_char_p, c.c_char_p, c.c_char_p, P(IRRecipeRunOptions),
                          P(IRRecipeResult))),
)

ANALYSIS_SIGNATURES = (
    ("IR_FrameStatistics", (P(IRArrayView), I, P(D), P(D), I64, P(D), I)),
    ("IR_ROIMean", (P(IRArrayView), I, I, I, I, P(D))),
    ("IR_Noise3DBuildCalibDn", (P(D), P(D), P(D), P(D), I, I, I64,
                                P(IRNoise3DCalibParams), P(IRNoise3DCalibOutputs))),
    ("IR_Noise3DComputeFramesDn", (P(IRArrayView), P(U8), I64, P(D), P(D), I64,
                                   P(IRNoise3DComputeParams), P(IRNoise3DComputeOutputs))),
    ("IR_GbProcessBadPixels", (P(D), P(D), P(D), I, I, I64, P(IRGbCriteria),
                               P(IRGbBadPixelSummary), P(IRGbBadPixelMasks))),
    ("IR_GbEvaluateMaps", (P(D), P(D), P(D), I, I, I64, P(IRCalcParameters),
                           P(IRGbCriteria), P(IRGbSummary), P(IRGbBadPixelMasks))),
    ("IR_GjbProcessBadPixels", (P(D), P(D), P(D), P(D), P(D), I, I, I64, P(IRGjbCriteria),
                                P(IRGjbBadPixelSummary), P(IRGjbBadPixelMasks))),
    ("IR_GjbEvaluateMaps", (P(D), P(D), P(D), P(D), I, I, I64, P(IRCalcParameters),
                            P(IRGjbCriteria), P(IRGjbSummary), P(IRGjbBadPixelMasks))),
    ("IR_BuildTwoPointNucF64", (P(D), P(D), P(U8), P(D), I, I, I64, P(D), P(D), P(D), I)),
    ("IR_ApplyAffineMapF64", (P(D), P(D), P(D), I, I, I64, P(D))),
    ("IR_FrameStatisticsNuc", (P(IRArrayView), I, P(D), P(D), P(D), P(U8), P(D), P(D), I64,
                               P(D), I)),
    ("IR_MapSummaryF64", (P(D), P(U8), I, I, I64, I, I, I, I, D, D, P(IRMapSummary))),
    ("IR_SpatialLowHighPassF64", (P(D), P(U8), I, I, I64, I, I, I, P(D), P(D))),
    ("IR_MapBinaryOpF64", (P(D), P(D), I, I, I64, I, D, P(D))),
    ("IR_ThresholdMaskF64", (P(D), P(U8), I, I, I64, I, D, D, P(U8), P(I), P(I))),
    ("IR_MaskCombine", (P(U8), P(U8), I, I, I64, I, P(U8), P(I))),
    ("IR_ComputeRfft1D", (P(D), I, D, I, I, P(D), P(D), P(D), I)),
    ("IR_ChannelSpectrum", (P(IRArrayView), I, I, D, I, P(D), I, P(D), I64, P(D), I64,
                            P(D), P(D), I)),
    ("IR_GjbEvaluateBurstMetrics", (P(D), P(D), P(U8), I, I, I64, P(IRGjbDriftContext),
                                    P(IRGjbBurstMetrics))),
    ("IR_FlickerDetect", (P(IRArrayView), P(D), P(D), P(U8), D, I, P(IRFlickerSummary),
                          P(I), P(U8), I64, P(D), P(D), I)),
    ("IR_Fft2dMagnitudeF64", (P(D), P(U8), I, I, I64, I, I, I, P(IRFft2dSummary), P(D), P(D))),
)

# Full export sets: the two version probes and IR_RequestStop are the only entries outside
# the (..., char *err, int32_t err_bytes) convention.
STATION_EXPORTS = tuple(name for name, _ in STATION_SIGNATURES) + (
    "IR_StationVersion", "IR_RequestStop")
ANALYSIS_EXPORTS = tuple(name for name, _ in ANALYSIS_SIGNATURES) + ("IR_AnalysisVersion",)

# Number of parameters declared in include/IR_*.h for the non-error-convention exports.
VERSION_PROBE_EXPORTS = ("IR_StationVersion", "IR_AnalysisVersion")
STOP_EXPORT = "IR_RequestStop"
ERROR_CONVENTION_ARGS = len(_ERR_ARGS)


def _bind(library, signatures, flag):
    if getattr(library, flag, False):
        return library
    for name, leading in signatures:
        function = getattr(library, name)
        function.argtypes = (*leading, *_ERR_ARGS)
        function.restype = I
    for name in VERSION_PROBE_EXPORTS:
        if hasattr(library, name):
            function = getattr(library, name)
            function.argtypes = ()
            function.restype = c.c_char_p
    if hasattr(library, STOP_EXPORT):
        function = getattr(library, STOP_EXPORT)
        function.argtypes = ()
        function.restype = None
    setattr(library, flag, True)
    return library


def bind_station_dll(station_dll):
    """Declare every ``IR_station.h`` / ``IR_device.h`` / ``IR_recipe.h`` signature."""
    return _bind(station_dll, STATION_SIGNATURES, "_irsdk_station_bound")


def bind_analysis_dll(analysis_dll):
    """Declare every ``IR_analysis.h`` signature."""
    return _bind(analysis_dll, ANALYSIS_SIGNATURES, "_irsdk_analysis_bound")


# --------------------------------------------------------------------------- error convention
def status_name(status):
    return _STATUS_NAMES.get(int(status), f"IR_STATUS_{int(status)}")


def error_text(buffer):
    """Decode a writable ``char *err`` buffer; the SDK NUL-terminates whenever it writes."""
    if isinstance(buffer, (bytes, bytearray)):
        return bytes(buffer).decode("utf-8", errors="replace")
    raw = buffer.value if getattr(buffer, "value", None) is not None else b""
    return raw.decode("utf-8", errors="replace")


def call_sdk(library, name, *args):
    """Invoke ``name(*args, err, err_bytes)`` and raise ``RuntimeError`` on a non-zero status.

    The message keeps the bare ``(<status>)`` group for legacy callers and appends the
    symbolic name in brackets, e.g. ``IR_SetBlackbody (7) [IR_TIMEOUT] ...``.
    """
    error = c.create_string_buffer(1024)
    status = getattr(library, name)(*args, error, len(error))
    if status != 0:
        raise RuntimeError(f"{name} ({status}) [{status_name(status)}] {error_text(error)}")
    return status


# --------------------------------------------------------------------------- library loading
_VERSION_MAX_CHARS = 128
# One retained os.add_dll_directory handle per successfully loaded directory. Dropping the
# handle too early breaks later plugin/backend loads from the same directory.
_DLL_DIR_HANDLES = {}
_LIBRARY_CACHE = {}


def _library_names():
    if os.name == "nt":
        return "IR_station.dll", "IR_analysis.dll"
    return "libIR_station.so", "libIR_analysis.so"


def _platform_library_subdirs():
    if os.name == "nt":
        return ("windows-x64/bin", "bin")
    return ("linux-x86_64/lib", "lib")


def _library_files(library):
    station_name, analysis_name = _library_names()
    if library == "analysis":
        return (analysis_name,)
    # "both" and "station": a station-only directory still needs the analysis runtime that
    # IR_station.dll/.so links against.
    return (station_name, analysis_name)


def _directory_matches(directory, library):
    return all((directory / name).is_file() for name in _library_files(library))


def _search_entries(candidate):
    """Ordered search roots: explicit candidate, environment, then the package layout."""
    entries = []
    if candidate:
        entries.append(Path(candidate))
    for variable in ("IR_SDK_LIB_DIR", "IR_SDK_BIN_DIR", "IRSDK_DIR", "IR_SDK_ROOT"):
        value = os.environ.get(variable)
        if value:
            entries.append(Path(value))
    package_dir = Path(__file__).resolve().parent
    # <root>/bindings/python/irsdk/loader.py -> installed prefix or packaged archive root.
    for parent in package_dir.parents[2:4]:
        entries.append(parent)
    return entries


def _is_cmake_package_dir(entry):
    """True when ``entry`` looks like a CMake package directory rather than a root.

    Covers ``<archive>/cmake`` (``-DIRSDK_DIR=<archive>/cmake``),
    ``<prefix>/lib/cmake/IRSDK`` and any directory containing ``IRSDKConfig.cmake``.
    """
    if (entry / "IRSDKConfig.cmake").is_file():
        return True
    if entry.name.lower() == "cmake":
        return True
    tail = tuple(part.lower() for part in entry.parts[-3:])
    return tail == ("lib", "cmake", "irsdk")


def _expand_entry(entry):
    """Directories probed for one search entry (prefix, archive root, or package dir)."""
    roots = [entry]
    if _is_cmake_package_dir(entry):
        # <archive>/cmake -> <archive>; <prefix>/lib/cmake/IRSDK -> <prefix>.
        roots.append(entry.parent)
        if len(entry.parents) > 2:
            roots.append(entry.parents[2])
    directories = []
    seen = set()
    for root in roots:
        for directory in (root, *(root / subdir for subdir in _platform_library_subdirs())):
            if directory not in seen:
                seen.add(directory)
                directories.append(directory)
    return directories


def resolve_sdk_library_dir(candidate=None, *, library="both"):
    """Resolve the directory holding the requested SDK shared libraries.

    Search order: ``candidate`` -> ``IR_SDK_LIB_DIR`` -> ``IR_SDK_BIN_DIR`` -> ``IRSDK_DIR``
    -> ``IR_SDK_ROOT`` -> package-relative roots. Each entry is accepted directly or via its
    platform sub-directories (``windows-x64/bin``, ``bin`` / ``linux-x86_64/lib``, ``lib``).
    CMake-style entries (``<archive>/cmake``, ``<prefix>/lib/cmake/IRSDK``, any directory with
    ``IRSDKConfig.cmake``) are also expanded to their ``<archive>`` / ``<prefix>`` root.
    Pair validation: ``library="station"`` and ``"both"`` require the station *and* analysis
    library files, ``library="analysis"`` requires the analysis file only.
    """
    if library not in ("both", "station", "analysis"):
        raise ValueError(f"library must be 'both', 'station' or 'analysis', not {library!r}")
    searched = []
    for entry in _search_entries(candidate):
        for directory in _expand_entry(entry):
            searched.append(directory)
            if directory.is_dir() and _directory_matches(directory, library):
                return directory.resolve()
    raise FileNotFoundError(
        "IR SDK libraries (irsdk) not found; searched: "
        + ", ".join(str(entry) for entry in searched)
        + ". Pass the library directory explicitly or set IR_SDK_LIB_DIR.")


def _open_dll_directory(path):
    """Register and retain one OS DLL directory handle per directory.

    Returns ``True`` when this call created the handle (so a failed load can release it),
    ``False`` when the directory is already registered or the platform has no such API.
    """
    add = getattr(os, "add_dll_directory", None)
    if add is None:
        return False
    key = str(path)
    if key in _DLL_DIR_HANDLES:
        return False
    _DLL_DIR_HANDLES[key] = add(key)
    return True


def _close_dll_directory(path):
    key = str(path)
    handle = _DLL_DIR_HANDLES.pop(key, None)
    if handle is not None:
        handle.close()


def _read_version(library, name):
    raw = getattr(library, name)()
    if not raw:
        raise RuntimeError(f"{name}() returned a null pointer; the SDK version is unavailable")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise RuntimeError(f"{name}() did not return valid UTF-8: {raw!r}") from exc
    text = text.strip()
    if not text:
        raise RuntimeError(f"{name}() returned a blank version string")
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text):
        raise RuntimeError(f"{name}() version string contains control characters: {text!r}")
    if len(text) > _VERSION_MAX_CHARS:
        raise RuntimeError(f"{name}() version string is too long: {len(text)} chars")
    return text


def _load_library(directory, file_name):
    path = Path(directory) / file_name
    if not path.is_file():
        raise FileNotFoundError(f"missing IR SDK shared library: {path}")
    return c.CDLL(str(path))


def load_station(library_dir=None):
    """Load and bind ``IR_station`` only (the directory must also hold ``IR_analysis``)."""
    directory = resolve_sdk_library_dir(library_dir, library="station")
    opened = _open_dll_directory(directory)
    station_name, _ = _library_names()
    try:
        return bind_station_dll(_load_library(directory, station_name))
    except Exception:
        if opened:
            _close_dll_directory(directory)
        raise


def load_analysis(library_dir=None):
    """Load and bind ``IR_analysis`` only."""
    directory = resolve_sdk_library_dir(library_dir, library="analysis")
    opened = _open_dll_directory(directory)
    _, analysis_name = _library_names()
    try:
        return bind_analysis_dll(_load_library(directory, analysis_name))
    except Exception:
        if opened:
            _close_dll_directory(directory)
        raise


class SdkLibraries:
    """One loaded pair of SDK libraries with the versions reported by the DLLs themselves."""

    def __init__(self, library_dir):
        self.library_dir = Path(library_dir).resolve()
        if not self.library_dir.is_dir():
            raise FileNotFoundError(f"SDK library directory does not exist: {self.library_dir}")
        opened = _open_dll_directory(self.library_dir)
        station_name, analysis_name = _library_names()
        try:
            # Analysis first: IR_station links against IR_analysis on every platform.
            self.analysis = bind_analysis_dll(_load_library(self.library_dir, analysis_name))
            self.station = bind_station_dll(_load_library(self.library_dir, station_name))
            self.station_version = _read_version(self.station, "IR_StationVersion")
            self.analysis_version = _read_version(self.analysis, "IR_AnalysisVersion")
        except Exception:
            # Release the provisional directory handle when this directory never loaded.
            self.analysis = None
            self.station = None
            if opened:
                _close_dll_directory(self.library_dir)
            raise

    def __repr__(self):  # pragma: no cover - diagnostics only
        return (f"SdkLibraries(library_dir={str(self.library_dir)!r}, "
                f"station={self.station_version!r}, analysis={self.analysis_version!r})")


def load_sdk(library_dir=None):
    """Load (and cache per directory) the ``IR_station`` / ``IR_analysis`` pair."""
    directory = resolve_sdk_library_dir(library_dir, library="both")
    libraries = _LIBRARY_CACHE.get(str(directory))
    if libraries is None:
        libraries = _LIBRARY_CACHE[str(directory)] = SdkLibraries(directory)
    return libraries


def sdk_versions(library_dir=None):
    """Never-raising version probe: ``None`` values plus ``error`` on failure."""
    try:
        libraries = load_sdk(library_dir)
    except Exception as exc:  # noqa: BLE001 - probes must never fail a report
        return {"library_dir": str(library_dir) if library_dir is not None else None,
                "station": None, "analysis": None,
                "error": f"{type(exc).__name__}: {exc}"}
    return {"library_dir": str(libraries.library_dir),
            "station": libraries.station_version,
            "analysis": libraries.analysis_version,
            "error": None}


__all__ = [
    "STATION_SIGNATURES", "ANALYSIS_SIGNATURES", "STATION_EXPORTS", "ANALYSIS_EXPORTS",
    "VERSION_PROBE_EXPORTS", "STOP_EXPORT", "ERROR_CONVENTION_ARGS",
    "bind_station_dll", "bind_analysis_dll", "call_sdk", "error_text", "status_name",
    "resolve_sdk_library_dir", "load_sdk", "load_station", "load_analysis", "sdk_versions",
    "SdkLibraries",
]
