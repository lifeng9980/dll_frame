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
import threading

from .abi import (
    D,
    EXPECTED_SDK_MAJOR_MINOR,
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

# ``kernel32`` with ``use_last_error`` is bound once on Windows; the native module-path helpers
# below are the only OS-specific code in this module and are inert elsewhere.
_KERNEL32 = c.WinDLL("kernel32", use_last_error=True) if os.name == "nt" else None
_WINTYPES = None
if _KERNEL32 is not None:  # pragma: no cover - Windows only
    import ctypes.wintypes as _WINTYPES

    _KERNEL32.GetModuleFileNameW.argtypes = (_WINTYPES.HMODULE, _WINTYPES.LPWSTR, _WINTYPES.DWORD)
    _KERNEL32.GetModuleFileNameW.restype = _WINTYPES.DWORD
    _KERNEL32.GetCurrentProcess.argtypes = ()
    _KERNEL32.GetCurrentProcess.restype = _WINTYPES.HANDLE
    _KERNEL32.K32EnumProcessModules.argtypes = (_WINTYPES.HANDLE, c.POINTER(_WINTYPES.HMODULE),
                                                _WINTYPES.DWORD, c.POINTER(_WINTYPES.DWORD))
    _KERNEL32.K32EnumProcessModules.restype = _WINTYPES.BOOL
    _KERNEL32.FreeLibrary.argtypes = (_WINTYPES.HMODULE,)
    _KERNEL32.FreeLibrary.restype = _WINTYPES.BOOL

_MODULE_PATH_MAX_CHARS = 32768


# POSIX identity helpers: ``dlsym`` (symbol address inside a specific object) and ``dladdr``
# (object path owning an address) are the only way to prove which ``IR_analysis`` an
# ``IR_station`` import table actually bound. Both live in the global namespace of the process.
class _DlInfo(c.Structure):
    _fields_ = [("dli_fname", c.c_char_p), ("dli_fbase", c.c_void_p),
                ("dli_sname", c.c_char_p), ("dli_saddr", c.c_void_p)]


_LIBC = None
if os.name == "posix":  # pragma: no cover - POSIX only
    _LIBC = c.CDLL(None)
    _LIBC.dlsym.argtypes = (c.c_void_p, c.c_char_p)
    _LIBC.dlsym.restype = c.c_void_p
    _LIBC.dladdr.argtypes = (c.c_void_p, c.POINTER(_DlInfo))
    _LIBC.dladdr.restype = c.c_int

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
# Directories whose load is currently executing / already completed successfully. A nested load
# of an in-progress directory must fail instead of rolling back the outer attempt's handles.
_IN_PROGRESS_DIRS = set()
_COMMITTED_DIRS = set()
# Reentrant: the cache guard nests into _open_dll_directory / _close_dll_directory.
_DLL_LOCK = threading.RLock()


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
    with _DLL_LOCK:
        if key in _DLL_DIR_HANDLES:
            return False
        _DLL_DIR_HANDLES[key] = add(key)
    return True


def _close_dll_directory(path):
    key = str(path)
    with _DLL_LOCK:
        handle = _DLL_DIR_HANDLES.pop(key, None)
    # Closing outside the lock keeps the critical section free of foreign code.
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


def _same_module_path(left, right):
    """Windows paths compare case-insensitively; POSIX paths stay exact."""
    return os.path.normcase(str(left)) == os.path.normcase(str(right))


def _module_path_from_handle(handle):
    """On-disk path of a loaded module handle (``None`` off Windows)."""
    if _KERNEL32 is None or not handle:
        return None
    buffer = c.create_unicode_buffer(_MODULE_PATH_MAX_CHARS)
    written = _KERNEL32.GetModuleFileNameW(_WINTYPES.HMODULE(handle), buffer, len(buffer))
    if not written:
        return None
    return Path(buffer.value).resolve()


def _list_resident_module_paths_windows(module_name):
    """Enumerate every loaded module of the current process matching ``module_name``."""
    process = _KERNEL32.GetCurrentProcess()
    capacity = 64
    while True:
        modules = (_WINTYPES.HMODULE * capacity)()
        needed = _WINTYPES.DWORD(0)
        if not _KERNEL32.K32EnumProcessModules(process, modules, c.sizeof(modules),
                                              c.byref(needed)):
            # Never degrade to "nothing resident": an unreadable module list would silently
            # disable every identity gate below, so fail closed instead.
            raise RuntimeError(
                "cannot enumerate the loaded modules of this process (K32EnumProcessModules "
                f"failed with Win32 error {c.get_last_error()})")
        if needed.value <= c.sizeof(modules):
            break
        capacity *= 2
    wanted = str(module_name).lower()
    found = {}
    for index in range(needed.value // c.sizeof(_WINTYPES.HMODULE)):
        path = _module_path_from_handle(modules[index])
        if path is not None and path.name.lower() == wanted:
            found[os.path.normcase(str(path))] = path
    return list(found.values())


def _list_resident_module_paths_posix(module_name):
    """Parse ``/proc/self/maps`` for file-backed mappings matching ``module_name``."""
    try:
        maps = Path("/proc/self/maps").read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        # Fail closed: an unreadable map would silently disable the identity gates.
        raise RuntimeError(
            f"cannot read /proc/self/maps to enumerate loaded modules: {error}") from error
    found = {}
    for line in maps.splitlines():
        marker = line.find(" /")
        if marker < 0:
            continue
        token = line[marker + 1:].strip()
        if token.endswith(" (deleted)"):
            token = token[: -len(" (deleted)")]
        if not token.startswith("/"):
            continue
        try:
            path = Path(token).resolve()
        except OSError:
            continue
        if path.name == module_name or path.name.startswith(f"{module_name}."):
            found[os.path.normcase(str(path))] = path
    return list(found.values())


def _list_resident_module_paths(module_name):
    """Every in-process module path whose file name matches ``module_name`` (resolved, unique).

    Windows walks ``K32EnumProcessModules`` so N same-named modules loaded from different
    directories are all visible; POSIX parses ``/proc/self/maps``. Both raise ``RuntimeError``
    when the OS cannot answer, because an empty answer would weaken every identity gate below.
    """
    if _KERNEL32 is not None:
        return _list_resident_module_paths_windows(module_name)
    if os.name == "posix":
        return _list_resident_module_paths_posix(module_name)
    return []


def _posix_dlsym(library, symbol_name):
    """Address of ``symbol_name`` as bound by ``library`` (POSIX only; ``None`` elsewhere)."""
    if _LIBC is None or library is None:
        return None
    handle = getattr(library, "_handle", None)
    if not handle:
        return None
    return _LIBC.dlsym(c.c_void_p(handle), symbol_name)


def _posix_symbol_path(address):
    """On-disk path of the shared object owning ``address``, or ``None`` (POSIX only)."""
    if _LIBC is None or not address:
        return None
    info = _DlInfo()
    if not _LIBC.dladdr(c.c_void_p(address), c.byref(info)):
        return None
    if not info.dli_fname:
        return None
    return Path(os.fsdecode(info.dli_fname)).resolve()


def _posix_global_dlsym(symbol_name):
    """Address of ``symbol_name`` in the *global* scope (``RTLD_DEFAULT``), or ``None``.

    ``RTLD_DEFAULT`` is ``(void *)0`` on glibc/musl: exactly the scope ELF relocation consults
    before an object's own dependencies, i.e. where ``LD_PRELOAD`` / ``RTLD_GLOBAL`` objects win.
    ``dlsym``'s first parameter stays ``c_void_p`` so ``None`` / ``c_void_p(0)`` both query it.
    """
    if _LIBC is None:
        return None
    return _LIBC.dlsym(c.c_void_p(0), symbol_name)


def _assert_no_posix_global_interposition(directory, analysis_name, station_name=None):
    """POSIX: no SDK export may resolve to a foreign object through the global scope.

    A preloaded (``LD_PRELOAD``) or ``RTLD_GLOBAL`` object that defines e.g. ``IR_ROIMean`` wins
    over the library's own definition during ELF relocation - a per-object ``dlsym`` cannot see
    that. Every export of the libraries in use must therefore resolve *globally* back to the file
    we loaded. Symbols that are not globally resolvable cannot be interposed and are skipped.
    """
    targets = [(ANALYSIS_EXPORTS, analysis_name)]
    if station_name is not None:
        targets.append((STATION_EXPORTS, station_name))
    for exports, name in targets:
        expected = (Path(directory) / name).resolve()
        for symbol in exports:
            address = _posix_global_dlsym(symbol.encode("ascii"))
            if not address:
                continue  # not visible in the global scope, so nothing can interpose
            backing = _posix_symbol_path(address)
            if backing is None or not _same_module_path(backing, expected):
                raise RuntimeError(
                    f"Global symbol interposition detected for {symbol}: resolved to "
                    f"'{backing if backing is not None else 'unknown'}' instead of '{expected}'")


def _unload_library(library):
    """Best-effort release of a library opened by a load attempt that is being rolled back.

    Only ever called for handles this attempt opened, so the refcount stays balanced: a module
    that was already resident keeps its existing reference.
    """
    if library is None:
        return
    handle = getattr(library, "_handle", None)
    if not handle:
        return
    if _KERNEL32 is not None:
        _KERNEL32.FreeLibrary(_WINTYPES.HMODULE(handle))
        return
    try:
        import _ctypes

        _ctypes.dlclose(handle)
    except (AttributeError, ImportError, OSError):
        pass


def _assert_foreign_modules_absent(directory, names):
    """Refuse to load when a same-named SDK module is resident from another directory.

    A resident ``IR_analysis`` silently satisfies ``IR_station``'s import table, so a mixed pair
    has to be rejected *before* the loader runs instead of through a confusing version or
    calling-convention failure later. Every resident copy is checked, not just the first one.
    """
    for name in names:
        expected = (Path(directory) / name).resolve()
        for resident in _list_resident_module_paths(name):
            if not _same_module_path(resident, expected):
                raise RuntimeError(
                    f"{name} is already loaded from '{resident}', but the requested SDK "
                    f"directory is '{Path(directory).resolve()}' (expected '{expected}'); "
                    f"a mixed SDK pair is not supported - restart the process to load another build")
    if os.name == "posix":
        # A globally interposed export would silently replace the function we are about to bind.
        station_name, analysis_name = _library_names()
        _assert_no_posix_global_interposition(
            directory, analysis_name, station_name if station_name in names else None)


def _assert_unique_resident_module(directory, name):
    """After loading, exactly one module with this name may be resident, and it must be ours.

    Fails closed: an empty enumeration is a hard error, never a silent skip.
    """
    resident = _list_resident_module_paths(name)
    expected = (Path(directory) / name).resolve()
    if len(resident) != 1 or not _same_module_path(resident[0], expected):
        raise RuntimeError(
            f"expected exactly one resident {name} at '{expected}', found "
            + (", ".join(f"'{path}'" for path in resident) if resident else "none"))


def _assert_loaded_module_match(library, directory, name):
    """Verify the handle we bound is the file we asked for (Windows only; no-op elsewhere)."""
    if _KERNEL32 is None:
        return
    expected = (Path(directory) / name).resolve()
    actual = _module_path_from_handle(getattr(library, "_handle", None))
    if actual is None:
        raise RuntimeError(
            f"cannot verify the loaded module path for {name}; expected '{expected}'")
    if not _same_module_path(actual, expected):
        raise RuntimeError(
            f"{name} was loaded from '{actual}' instead of the requested "
            f"'{expected}'; the DLL search path was diverted")


def _verify_posix_analysis(analysis, directory, analysis_name):
    """POSIX: the ``IR_AnalysisVersion`` we call must live in the file we asked for."""
    symbol = _posix_dlsym(analysis, b"IR_AnalysisVersion")
    path = _posix_symbol_path(symbol) if symbol else None
    if path is None:
        raise RuntimeError(
            f"cannot establish the on-disk identity of IR_analysis '{analysis_name}' loaded from "
            f"'{Path(directory).resolve()}'")
    expected = (Path(directory) / analysis_name).resolve()
    if not _same_module_path(path, expected):
        raise RuntimeError(
            f"IR_analysis was loaded from '{path}' instead of the requested '{expected}'; "
            f"the shared-library search path was diverted")
    _assert_no_posix_global_interposition(directory, analysis_name)
    _assert_unique_resident_module(directory, analysis_name)


def _verify_posix_pair(station, analysis, directory, station_name, analysis_name):
    """POSIX: prove ``IR_station`` bound the exact ``IR_analysis`` object we loaded.

    ``IR_station`` declares ``IR_AnalysisVersion`` as an import, so ``dlsym`` on the station
    handle returns the address the dynamic linker actually resolved - which catches a foreign
    ``.so`` with a matching SONAME that ``/proc/self/maps`` alone cannot distinguish.
    """
    analysis_sym = _posix_dlsym(analysis, b"IR_AnalysisVersion")
    station_dep_sym = _posix_dlsym(station, b"IR_AnalysisVersion")
    station_sym = _posix_dlsym(station, b"IR_StationVersion")
    identities = (
        ("IR_analysis IR_AnalysisVersion", analysis_sym),
        ("IR_station dependency IR_AnalysisVersion", station_dep_sym),
        ("IR_station IR_StationVersion", station_sym),
    )
    for label, symbol in identities:
        if not symbol or _posix_symbol_path(symbol) is None:
            raise RuntimeError(
                f"cannot establish the on-disk identity of {label} for SDK directory "
                f"'{Path(directory).resolve()}'")
    if station_dep_sym != analysis_sym:
        raise RuntimeError(
            f"IR_station resolved IR_AnalysisVersion to '{_posix_symbol_path(station_dep_sym)}' "
            f"instead of the loaded IR_analysis at '{_posix_symbol_path(analysis_sym)}'; "
            f"a mixed SDK pair is not supported")
    expected_analysis = (Path(directory) / analysis_name).resolve()
    expected_station = (Path(directory) / station_name).resolve()
    analysis_path = _posix_symbol_path(analysis_sym)
    station_path = _posix_symbol_path(station_sym)
    if not _same_module_path(analysis_path, expected_analysis):
        raise RuntimeError(
            f"IR_analysis was loaded from '{analysis_path}' instead of the requested "
            f"'{expected_analysis}'; the shared-library search path was diverted")
    if not _same_module_path(station_path, expected_station):
        raise RuntimeError(
            f"IR_station was loaded from '{station_path}' instead of the requested "
            f"'{expected_station}'; the shared-library search path was diverted")
    _assert_no_posix_global_interposition(directory, analysis_name, station_name)
    _assert_unique_resident_module(directory, analysis_name)
    _assert_unique_resident_module(directory, station_name)


def _verify_loaded_pair(station, analysis, directory, station_name, analysis_name):
    """Verify a loaded ``IR_station`` / ``IR_analysis`` pair; fail closed on real handles.

    Only non-``ctypes.CDLL`` objects (unit-test doubles) skip the native checks - a real load is
    always verified, and an OS enumeration failure raises instead of weakening the gate.
    """
    if not isinstance(analysis, c.CDLL):
        return
    if os.name == "nt":
        _assert_loaded_module_match(analysis, directory, analysis_name)
        _assert_loaded_module_match(station, directory, station_name)
        _assert_unique_resident_module(directory, analysis_name)
        _assert_unique_resident_module(directory, station_name)
    elif os.name == "posix":
        _verify_posix_pair(station, analysis, directory, station_name, analysis_name)


def _verify_loaded_analysis(analysis, directory, analysis_name):
    """Verify a loaded ``IR_analysis``; fail closed on real handles."""
    if not isinstance(analysis, c.CDLL):
        return
    if os.name == "nt":
        _assert_loaded_module_match(analysis, directory, analysis_name)
        _assert_unique_resident_module(directory, analysis_name)
    elif os.name == "posix":
        _verify_posix_analysis(analysis, directory, analysis_name)


def _validate_version_compatibility(station_version=None, analysis_version=None,
                                    expected_major_minor=EXPECTED_SDK_MAJOR_MINOR):
    """Reject SDK builds this binding was not written against.

    Every supplied version must be a numeric dot-separated string starting with
    ``<expected_major_minor>.``; when both libraries report a version they must be identical
    (a mixed ``IR_station`` / ``IR_analysis`` pair is never supported).
    """
    prefix = f"{expected_major_minor}."
    for label, version in (("IR_station", station_version), ("IR_analysis", analysis_version)):
        if version is None:
            continue
        segments = version.split(".")
        if len(segments) < 2 or not all(segment.isascii() and segment.isdigit()
                                        for segment in segments):
            raise RuntimeError(
                f"{label} version {version!r} is not a numeric dot-separated version; "
                f"this irsdk binding expects SDK {expected_major_minor}.x")
        if not version.startswith(prefix):
            raise RuntimeError(
                f"{label} version {version!r} is incompatible with this irsdk binding, "
                f"which requires SDK {expected_major_minor}.x")
    if station_version is not None and analysis_version is not None \
            and station_version != analysis_version:
        raise RuntimeError(
            f"IR_station version {station_version!r} does not match IR_analysis version "
            f"{analysis_version!r}; a mixed SDK pair is not supported")


def load_station(library_dir=None):
    """Load and bind ``IR_station`` (plus the ``IR_analysis`` it links against) and gate versions."""
    directory = resolve_sdk_library_dir(library_dir, library="station")
    key = str(directory)
    station_name, analysis_name = _library_names()
    # One critical section covers registration, loading, validation and rollback: a concurrent
    # thread must never observe (or reuse) a directory handle this attempt is about to release.
    with _DLL_LOCK:
        if key in _IN_PROGRESS_DIRS:
            raise RuntimeError(f"Reentrant load of SDK directory in progress: {directory}")
        _assert_foreign_modules_absent(directory, (analysis_name, station_name))
        _IN_PROGRESS_DIRS.add(key)
        opened = False
        loaded = []
        try:
            opened = _open_dll_directory(directory)
            # Analysis first: IR_station links against IR_analysis on every platform.
            raw_analysis = _load_library(directory, analysis_name)
            loaded.append(raw_analysis)
            analysis = bind_analysis_dll(raw_analysis)
            raw_station = _load_library(directory, station_name)
            loaded.append(raw_station)
            station = bind_station_dll(raw_station)
            _verify_loaded_pair(station, analysis, directory, station_name, analysis_name)
            station_version = _read_version(station, "IR_StationVersion")
            analysis_version = _read_version(analysis, "IR_AnalysisVersion")
            _validate_version_compatibility(station_version=station_version,
                                            analysis_version=analysis_version)
            _COMMITTED_DIRS.add(key)
        except BaseException:
            # Release this attempt's native handles before the provisional directory handle.
            for library in reversed(loaded):
                _unload_library(library)
            if opened and key not in _COMMITTED_DIRS:
                _close_dll_directory(directory)
            raise
        finally:
            _IN_PROGRESS_DIRS.discard(key)
    return station


def load_analysis(library_dir=None):
    """Load and bind ``IR_analysis`` only, gated on the reported version."""
    directory = resolve_sdk_library_dir(library_dir, library="analysis")
    key = str(directory)
    _, analysis_name = _library_names()
    with _DLL_LOCK:
        if key in _IN_PROGRESS_DIRS:
            raise RuntimeError(f"Reentrant load of SDK directory in progress: {directory}")
        _assert_foreign_modules_absent(directory, (analysis_name,))
        _IN_PROGRESS_DIRS.add(key)
        opened = False
        loaded = []
        try:
            opened = _open_dll_directory(directory)
            raw_analysis = _load_library(directory, analysis_name)
            loaded.append(raw_analysis)
            analysis = bind_analysis_dll(raw_analysis)
            _verify_loaded_analysis(analysis, directory, analysis_name)
            analysis_version = _read_version(analysis, "IR_AnalysisVersion")
            _validate_version_compatibility(analysis_version=analysis_version)
            _COMMITTED_DIRS.add(key)
        except BaseException:
            for library in reversed(loaded):
                _unload_library(library)
            if opened and key not in _COMMITTED_DIRS:
                _close_dll_directory(directory)
            raise
        finally:
            _IN_PROGRESS_DIRS.discard(key)
    return analysis


class SdkLibraries:
    """One loaded pair of SDK libraries with the versions reported by the DLLs themselves."""

    def __init__(self, library_dir):
        self.library_dir = Path(library_dir).resolve()
        if not self.library_dir.is_dir():
            raise FileNotFoundError(f"SDK library directory does not exist: {self.library_dir}")
        station_name, analysis_name = _library_names()
        self.analysis = None
        self.station = None
        key = str(self.library_dir)
        with _DLL_LOCK:
            if key in _IN_PROGRESS_DIRS:
                raise RuntimeError(
                    f"Reentrant load of SDK directory in progress: {self.library_dir}")
            _assert_foreign_modules_absent(self.library_dir, (analysis_name, station_name))
            _IN_PROGRESS_DIRS.add(key)
            opened = False
            loaded = []
            try:
                opened = _open_dll_directory(self.library_dir)
                # Analysis first: IR_station links against IR_analysis on every platform.
                raw_analysis = _load_library(self.library_dir, analysis_name)
                loaded.append(raw_analysis)
                self.analysis = bind_analysis_dll(raw_analysis)
                raw_station = _load_library(self.library_dir, station_name)
                loaded.append(raw_station)
                self.station = bind_station_dll(raw_station)
                _verify_loaded_pair(self.station, self.analysis, self.library_dir,
                                    station_name, analysis_name)
                self.station_version = _read_version(self.station, "IR_StationVersion")
                self.analysis_version = _read_version(self.analysis, "IR_AnalysisVersion")
                _validate_version_compatibility(station_version=self.station_version,
                                                analysis_version=self.analysis_version)
                _COMMITTED_DIRS.add(key)
            except BaseException:
                # Drop this attempt's native handles, then the provisional directory handle.
                for library in reversed(loaded):
                    _unload_library(library)
                self.analysis = None
                self.station = None
                if opened and key not in _COMMITTED_DIRS:
                    _close_dll_directory(self.library_dir)
                raise
            finally:
                _IN_PROGRESS_DIRS.discard(key)

    def __repr__(self):  # pragma: no cover - diagnostics only
        return (f"SdkLibraries(library_dir={str(self.library_dir)!r}, "
                f"station={self.station_version!r}, analysis={self.analysis_version!r})")


def load_sdk(library_dir=None):
    """Load (and cache per directory) the ``IR_station`` / ``IR_analysis`` pair.

    The cache read/insert and both DLL-directory handle maps are guarded by :data:`_DLL_LOCK`,
    so concurrent first loads share exactly one :class:`SdkLibraries` instance.
    """
    directory = resolve_sdk_library_dir(library_dir, library="both")
    key = str(directory)
    with _DLL_LOCK:
        libraries = _LIBRARY_CACHE.get(key)
        if libraries is not None:
            return libraries
        libraries = _LIBRARY_CACHE[key] = SdkLibraries(directory)
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
