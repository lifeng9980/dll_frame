"""Compiler-verified ABI gate for the official Python binding (``bindings/python/irsdk``).

Independent opinions are compared 1:1:

1. the C compiler (``c_client --dump-abi-layout``) for size, alignment and **every field
   offset** of all 41 public structs in ``include/IR_*.h``;
2. :data:`irsdk.EXPECTED_SIZES`, asserted by ``irsdk.verify_struct_sizes()`` at import time;
3. ``ctypes.sizeof`` / ``ctypes.alignment`` / ``field.offset`` of the mirrored structs.

It also parses the public C declarations from ``include/IR_{station,device,recipe,analysis}.h``
and compares return type plus every parameter type 1:1 against the bound
``fn.restype``/``fn.argtypes`` of all 57 exports, exercises the writable ``char *`` callback
convention, the ``array_view`` guard, library-directory discovery (prefix, archive, and
CMake ``…/cmake`` entries, with pair validation), the SDK ``major.minor`` / mixed-pair version
gate, standalone ``load_station`` / ``load_analysis``, failed-load rollback, concurrent
``load_sdk`` caching, 32-bit interpreter rejection, and a handful of pure computation entry
points. No hardware, no live capture and no file output are involved.
"""
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from unittest.mock import MagicMock, patch

DECLARATION_RE = re.compile(
    r"^IR_(STATION|ANALYSIS)_API\s+(?P<ret>[^;()\n]*?)\s*(?P<name>IR_\w+)\s*"
    r"\((?P<params>[^;()]*)\)\s*IR_(?:STATION|ANALYSIS)_NOEXCEPT",
    re.S | re.M)
PUBLIC_HEADERS = ("IR_station.h", "IR_device.h", "IR_recipe.h", "IR_analysis.h")

# Scalar C spellings used by the public headers -> ctypes scalar types.
SCALAR_CTYPES = {
    "int": ctypes.c_int,
    "int32_t": ctypes.c_int32,
    "uint8_t": ctypes.c_uint8,
    "uint16_t": ctypes.c_uint16,
    "uint32_t": ctypes.c_uint32,
    "int64_t": ctypes.c_int64,
    "double": ctypes.c_double,
    "IRHandle": ctypes.c_uint64,
}


def import_binding(bindings_dir):
    sys.path.insert(0, str(bindings_dir))
    import irsdk  # noqa: PLC0415 - intentionally imported after sys.path is prepared

    return irsdk


# Child-process probe: load the real ``IR_analysis``, inject a failure inside the binding step
# (simulating a missing export *after* ``CDLL`` succeeded) and prove the native module was really
# unloaded - Windows ``FreeLibrary`` / POSIX ``_ctypes.dlclose`` - so a retry can load it again.
REAL_NATIVE_UNLOAD_PROBE = r'''
import sys
from pathlib import Path

library_dir, analysis_name, bindings_dir = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, bindings_dir)
import irsdk

loader = irsdk.loader
assert loader._list_resident_module_paths(analysis_name) == [], "precondition: not resident"

real_bind = loader.bind_analysis_dll


def failing_bind(library):
    # The CDLL load must be visible to the OS enumeration before we inject the failure, so the
    # post-rollback emptiness below really proves FreeLibrary/_ctypes.dlclose ran.
    expected = (Path(library_dir) / analysis_name).resolve()
    visible = loader._list_resident_module_paths(analysis_name)
    assert expected in visible, (expected, visible)
    raise RuntimeError("simulated missing export after CDLL succeeded")


loader.bind_analysis_dll = failing_bind
try:
    irsdk.load_analysis(library_dir)
except RuntimeError as error:
    assert "simulated missing export" in str(error), error
else:
    raise SystemExit("load_analysis must propagate the binding failure")
loader.bind_analysis_dll = real_bind

resident = loader._list_resident_module_paths(analysis_name)
assert resident == [], resident
assert not loader._IN_PROGRESS_DIRS, loader._IN_PROGRESS_DIRS
assert not loader._DLL_DIR_HANDLES, loader._DLL_DIR_HANDLES
analysis = irsdk.load_analysis(library_dir)
assert analysis.IR_AnalysisVersion()
print("native unload probe ok")
'''


def run_c_client(c_client, library_dir, flag):
    """Run ``c_client <flag>`` with the SDK libraries on the loader path."""
    search = [str(library_dir), str(Path(c_client).parent)]
    environment = dict(os.environ)
    if environment.get("PATH"):
        search.append(environment["PATH"])
    environment["PATH"] = os.pathsep.join(search)
    result = subprocess.run([str(c_client), flag], capture_output=True, text=True,
                            env=environment, check=False)
    assert result.returncode == 0, f"c_client {flag} failed: {result.stderr}"
    return json.loads(result.stdout)


def check_struct_layout(irsdk, compiler):
    expected = irsdk.EXPECTED_SIZES
    assert set(compiler) == set(expected), (
        f"compiler struct set mismatch: missing={sorted(set(expected) - set(compiler))} "
        f"unexpected={sorted(set(compiler) - set(expected))}")
    for name, declared in compiler.items():
        cls = getattr(irsdk, name)
        assert declared["size"] == expected[name], (
            f"{name}: C compiler sizeof={declared['size']} but EXPECTED_SIZES={expected[name]}")
        mirrored_size = ctypes.sizeof(cls)
        assert mirrored_size == expected[name], (
            f"{name}: ctypes.sizeof={mirrored_size} but EXPECTED_SIZES={expected[name]}")
        mirrored_align = ctypes.alignment(cls)
        assert mirrored_align == declared["alignment"], (
            f"{name}: ctypes.alignment={mirrored_align} but C compiler "
            f"alignof={declared['alignment']}")
        declared_fields = declared["fields"]
        mirrored_fields = [field for field, _ in cls._fields_]
        assert list(declared_fields) == mirrored_fields, (
            f"{name}: field order mismatch {list(declared_fields)} vs {mirrored_fields}")
        for field, offset in declared_fields.items():
            mirrored_offset = getattr(cls, field).offset
            assert mirrored_offset == offset, (
                f"{name}.{field}: ctypes offset={mirrored_offset} but offsetof={offset}")
    assert set(irsdk.verify_struct_sizes()) == set(compiler)
    return compiler


# --------------------------------------------------------------------------- signatures
def _normalize_ctype(text, irsdk):
    """Normalize one C type spelling to the ctypes type the binding must declare."""
    text = " ".join(text.split())
    if text.startswith("const "):
        text = text[len("const "):].strip()
    if text.endswith("*"):
        base = text[:-1].strip()
        if base == "char":
            return ctypes.c_char_p        # const char * / char * (UTF-8 buffers)
        if base == "void":
            return ctypes.c_void_p
        scalar = SCALAR_CTYPES.get(base)
        if scalar is not None:
            return ctypes.POINTER(scalar)
        mirrored = getattr(irsdk, base, None)
        assert mirrored is not None, f"unknown C pointer target {base!r}"
        return ctypes.POINTER(mirrored)
    if text == "void":
        return None
    scalar = SCALAR_CTYPES.get(text)
    assert scalar is not None, f"unknown C type {text!r}"
    return scalar


def _split_parameters(params):
    params = " ".join(params.split())
    if params in ("", "void"):
        return []
    return [part.strip() for part in params.split(",")]


def _split_type_and_name(parameter):
    """Split ``const IRFoo *bar`` into (``const IRFoo *``, ``bar``)."""
    text = " ".join(parameter.split())
    if "*" in text:
        star = text.rindex("*")
        ctype = text[:star + 1].strip()
        name = text[star + 1:].strip()
    else:
        ctype, _, name = text.rpartition(" ")
    assert ctype and re.fullmatch(r"[A-Za-z_]\w*", name or ""), (
        f"unsupported parameter declaration {parameter!r}")
    return ctype, name


def parse_public_declarations(include_dir, irsdk):
    """``{symbol: (restype, [argtypes])}`` for every ``IR_*_API`` declaration."""
    declarations = {}
    for header in PUBLIC_HEADERS:
        text = (Path(include_dir) / header).read_text(encoding="utf-8")
        for match in DECLARATION_RE.finditer(text):
            name = match.group("name")
            assert name not in declarations, f"{name} declared twice"
            params = _split_parameters(match.group("params"))
            declarations[name] = (
                _normalize_ctype(match.group("ret"), irsdk),
                [_normalize_ctype(_split_type_and_name(param)[0], irsdk) for param in params])
    return declarations


def check_declarations(irsdk, include_dir, libraries):
    declarations = parse_public_declarations(include_dir, irsdk)
    all_exports = set(irsdk.STATION_EXPORTS) | set(irsdk.ANALYSIS_EXPORTS)
    assert len(all_exports) == 57, len(all_exports)
    assert set(declarations) == all_exports, (
        f"header declaration set mismatch: missing={sorted(all_exports - set(declarations))} "
        f"unexpected={sorted(set(declarations) - all_exports)}")

    for export in sorted(all_exports):
        declared_restype, declared_argtypes = declarations[export]
        library = libraries.station if export in irsdk.STATION_EXPORTS else libraries.analysis
        bound = getattr(library, export)
        assert bound.restype is declared_restype, (
            f"{export}: ctypes restype={bound.restype} but header declares {declared_restype}")
        bound_argtypes = tuple(bound.argtypes or ())
        assert len(bound_argtypes) == len(declared_argtypes), (
            f"{export}: ctypes declares {len(bound_argtypes)} parameter types, "
            f"header declares {len(declared_argtypes)}")
        for index, (declared, actual) in enumerate(zip(declared_argtypes, bound_argtypes)):
            assert actual is declared, (
                f"{export}: parameter {index} ctypes type={actual} but header declares {declared}")
        assert declared_restype is not None or export == irsdk.STOP_EXPORT, (
            f"{export}: only IR_RequestStop may return void")
    for name in irsdk.VERSION_PROBE_EXPORTS:
        assert declarations[name][1] == [], f"{name} must take no parameters"
    assert declarations[irsdk.STOP_EXPORT][1] == [], f"{irsdk.STOP_EXPORT} must take no parameters"
    return declarations


# --------------------------------------------------------------------------- runtime checks
def check_exports(irsdk, libraries, expected_version):
    assert libraries.station_version == expected_version, (
        f"IR_StationVersion={libraries.station_version} expected {expected_version}")
    assert libraries.analysis_version == expected_version, (
        f"IR_AnalysisVersion={libraries.analysis_version} expected {expected_version}")
    assert libraries.station_version == libraries.analysis_version, "library version mismatch"

    for name in irsdk.STATION_EXPORTS:
        assert hasattr(libraries.station, name), f"IR_station is missing export {name}"
    for name in irsdk.ANALYSIS_EXPORTS:
        assert hasattr(libraries.analysis, name), f"IR_analysis is missing export {name}"
    assert len(irsdk.STATION_EXPORTS) == 35, len(irsdk.STATION_EXPORTS)
    assert len(irsdk.ANALYSIS_EXPORTS) == 22, len(irsdk.ANALYSIS_EXPORTS)

    versions = irsdk.sdk_versions(libraries.library_dir)
    assert versions["error"] is None, versions
    assert versions["station"] == versions["analysis"] == expected_version, versions
    assert Path(irsdk.resolve_sdk_library_dir(libraries.library_dir)) == libraries.library_dir

    # Legacy ``(<status>)`` substring checks and the new symbolic tag must both be present.
    try:
        irsdk.call_sdk(libraries.analysis, "IR_ROIMean", None, 0, 0, 1, 1, None)
    except RuntimeError as error:
        message = str(error)
        assert "IR_ROIMean" in message and "(1)" in message, message
        assert "[IR_NULL_POINTER]" in message, message
    else:
        raise AssertionError("IR_ROIMean(NULL) must fail")


def check_callback_convention(irsdk):
    error = ctypes.create_string_buffer(32)

    def validate_config(config_json, err, err_bytes):
        assert config_json is None
        assert err_bytes == len(error)
        ctypes.memmove(err, b"err!\0", 5)
        return 2

    callback = irsdk.IRPowerDriverValidateConfigFn(validate_config)
    assert callback(None, error, len(error)) == 2
    assert error.value == b"err!", error.value

    writable = ctypes.POINTER(ctypes.c_char)
    for name in ("IRPluginPrepareFn", "IRPluginExecuteFn",
                 "IRPowerDriverValidateConfigFn", "IRPowerDriverReadFn",
                 "IRPowerDriverSetOutputFn", "IRPowerDriverSetChannelFn",
                 "IRPowerDriverCloseFn"):
        argtypes = getattr(irsdk, name)._argtypes_
        assert argtypes[-2] is writable, f"{name} err parameter must be POINTER(c_char)"
    open_args = irsdk.IRPowerDriverOpenFn._argtypes_
    assert open_args[2] is writable and open_args[4] is writable, open_args
    assert irsdk.IRPowerDriverOpenFn._argtypes_[1] is ctypes.POINTER(ctypes.c_void_p)
    query_args = irsdk.IRPowerDriverQueryFn._argtypes_
    assert query_args[-2] is writable, query_args
    # const strings stay c_char_p (read-only, copied by ctypes).
    assert irsdk.IRPluginPrepareFn._argtypes_[1] is ctypes.c_char_p


def check_array_view(irsdk):
    try:
        irsdk.array_view(ctypes.c_void_p(0), irsdk.IR_TYPE_U16, 1, 1)
    except ValueError as error:
        assert "byte_size" in str(error), error
    else:
        raise AssertionError("array_view must reject raw pointers without byte_size")
    view = irsdk.array_view(ctypes.c_void_p(0), irsdk.IR_TYPE_U16, 1, 1, byte_size=2)
    assert view.byte_size == 2
    pixels = (ctypes.c_uint16 * 4)(1, 2, 3, 4)
    assert irsdk.array_view(pixels, irsdk.IR_TYPE_U16, 2, 2).byte_size == 8


class _Environment:
    """Temporarily replace the library-discovery environment variables."""

    KEYS = ("IR_SDK_LIB_DIR", "IR_SDK_BIN_DIR", "IRSDK_DIR", "IR_SDK_ROOT")

    def __init__(self, **values):
        self.values = values

    def __enter__(self):
        self.saved = {key: os.environ.get(key) for key in self.KEYS}
        for key in self.KEYS:
            os.environ.pop(key, None)
        for key, value in self.values.items():
            os.environ[key] = str(value)
        return self

    def __exit__(self, *exc):
        for key, value in self.saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        return False


def _library_files():
    if os.name == "nt":
        return "IR_station.dll", "IR_analysis.dll"
    return "libIR_station.so", "libIR_analysis.so"


def _make_pair(directory, *, station=True, analysis=True):
    directory.mkdir(parents=True, exist_ok=True)
    station_name, analysis_name = _library_files()
    if station:
        (directory / station_name).write_bytes(b"")
    if analysis:
        (directory / analysis_name).write_bytes(b"")
    return directory


def check_library_discovery(irsdk):
    subdirs = (("windows-x64/bin", "bin") if os.name == "nt"
               else ("linux-x86_64/lib", "lib"))
    with tempfile.TemporaryDirectory(prefix="irsdk-resolve-") as temporary:
        root = Path(temporary)
        hot = _make_pair(root / "hot")
        cold = _make_pair(root / "cold")
        prefixed = _make_pair(root / "prefix" / subdirs[1])
        archived = _make_pair(root / "archive" / subdirs[0])

        # Explicit candidate wins over everything.
        with _Environment(IR_SDK_LIB_DIR=cold, IRSDK_DIR=root / "archive"):
            assert irsdk.resolve_sdk_library_dir(hot) == hot.resolve()
        # IR_SDK_LIB_DIR > IR_SDK_BIN_DIR > IRSDK_DIR > IR_SDK_ROOT.
        with _Environment(IR_SDK_LIB_DIR=hot, IR_SDK_BIN_DIR=cold,
                          IRSDK_DIR=root / "archive", IR_SDK_ROOT=root / "prefix"):
            assert irsdk.resolve_sdk_library_dir() == hot.resolve()
        with _Environment(IR_SDK_BIN_DIR=cold, IRSDK_DIR=root / "archive",
                          IR_SDK_ROOT=root / "prefix"):
            assert irsdk.resolve_sdk_library_dir() == cold.resolve()
        # IRSDK_DIR points at a packaged archive root -> platform sub-directory is probed.
        with _Environment(IRSDK_DIR=root / "archive"):
            assert irsdk.resolve_sdk_library_dir() == archived.resolve()
        # IRSDK_DIR points at the archive's CMake directory (<archive>/cmake).
        archive_cmake = root / "archive" / "cmake"
        archive_cmake.mkdir(parents=True, exist_ok=True)
        (archive_cmake / "IRSDKConfig.cmake").write_bytes(b"")
        with _Environment(IRSDK_DIR=archive_cmake):
            assert irsdk.resolve_sdk_library_dir() == archived.resolve()
        # ... or at an installed CMake package directory (<prefix>/lib/cmake/IRSDK).
        prefix_cmake = root / "prefix" / "lib" / "cmake" / "IRSDK"
        prefix_cmake.mkdir(parents=True, exist_ok=True)
        with _Environment(IRSDK_DIR=prefix_cmake):
            assert irsdk.resolve_sdk_library_dir() == prefixed.resolve()
        (prefix_cmake / "IRSDKConfig.cmake").write_bytes(b"")
        with _Environment(IRSDK_DIR=prefix_cmake):
            assert irsdk.resolve_sdk_library_dir() == prefixed.resolve()
        # IR_SDK_ROOT points at an install prefix -> <prefix>/bin (or <prefix>/lib).
        with _Environment(IR_SDK_ROOT=root / "prefix"):
            assert irsdk.resolve_sdk_library_dir() == prefixed.resolve()

        # Pair validation: "both"/"station" need both libraries, "analysis" only analysis.
        station_only = _make_pair(root / "station-only", analysis=False)
        analysis_only = _make_pair(root / "analysis-only", station=False)
        isolated = lambda candidate: [Path(candidate)]  # noqa: E731 - test isolation
        with _Environment(), patch.object(irsdk.loader, "_search_entries", isolated):
            for library in ("both", "station"):
                try:
                    irsdk.resolve_sdk_library_dir(station_only, library=library)
                except FileNotFoundError:
                    pass
                else:
                    raise AssertionError(f"{library} must reject a station-only directory")
            assert irsdk.resolve_sdk_library_dir(analysis_only, library="analysis") \
                == analysis_only.resolve()
            try:
                irsdk.resolve_sdk_library_dir(root / "missing")
            except FileNotFoundError:
                pass
            else:
                raise AssertionError("missing directory must not resolve")
            try:
                irsdk.resolve_sdk_library_dir(hot, library="plugin")
            except ValueError:
                pass
            else:
                raise AssertionError("unknown library kind must be rejected")


# --------------------------------------------------------------------------- binding gates
def check_binding_identity(irsdk, expected_version):
    """``BINDING_VERSION`` / ``EXPECTED_SDK_MAJOR_MINOR`` must describe the built SDK."""
    assert irsdk.BINDING_VERSION, "BINDING_VERSION must be declared"
    assert irsdk.__version__ == irsdk.BINDING_VERSION, (irsdk.__version__, irsdk.BINDING_VERSION)
    major_minor = ".".join(expected_version.split(".")[:2])
    assert irsdk.EXPECTED_SDK_MAJOR_MINOR == major_minor, (
        irsdk.EXPECTED_SDK_MAJOR_MINOR, major_minor)
    assert "BINDING_VERSION" in irsdk.abi.__all__, "abi.__all__ must export BINDING_VERSION"
    assert "EXPECTED_SDK_MAJOR_MINOR" in irsdk.abi.__all__, \
        "abi.__all__ must export EXPECTED_SDK_MAJOR_MINOR"
    # No name may be exported twice (a duplicated entry breaks ``from irsdk import *`` audits).
    assert len(irsdk.abi.__all__) == len(set(irsdk.abi.__all__)), \
        f"abi.__all__ has duplicates: {sorted(irsdk.abi.__all__)}"
    assert len(irsdk.__all__) == len(set(irsdk.__all__)), \
        f"irsdk.__all__ has duplicates: {sorted(irsdk.__all__)}"
    assert irsdk.STRUCT_SIZES == irsdk.verify_struct_sizes(), "STRUCT_SIZES must be verified"


def check_version_gate(irsdk, expected_version):
    """Matching versions pass; a mixed pair or a foreign ``major.minor`` raises ``RuntimeError``."""
    validate = irsdk.loader._validate_version_compatibility
    validate()
    validate(station_version=expected_version)
    validate(analysis_version=expected_version)
    validate(station_version=expected_version, analysis_version=expected_version)
    validate(station_version=expected_version, analysis_version=expected_version,
             expected_major_minor=irsdk.EXPECTED_SDK_MAJOR_MINOR)

    try:
        validate(station_version=expected_version, analysis_version="9.9.9")
    except RuntimeError as error:
        assert "9.9.9" in str(error), error
    else:
        raise AssertionError("a station/analysis version mismatch must raise RuntimeError")

    # Same series, different patch: the pair is still mixed and must be rejected.
    major, minor, patch_level = (int(part) for part in expected_version.split(".")[:3])
    same_series = f"{major}.{minor}.{patch_level + 1}"
    try:
        validate(station_version=expected_version, analysis_version=same_series)
    except RuntimeError as error:
        assert same_series in str(error), error
    else:
        raise AssertionError(
            f"{expected_version}/{same_series} must be rejected as a mixed pair")

    major, minor = (int(part) for part in irsdk.EXPECTED_SDK_MAJOR_MINOR.split("."))
    for wrong in (f"{major + 1}.{minor}.0", f"{major}.{minor + 1}.0", "1.7.4"):
        for kwargs in ({"station_version": wrong}, {"analysis_version": wrong},
                       {"station_version": wrong, "analysis_version": wrong}):
            try:
                validate(**kwargs)
            except RuntimeError as error:
                assert wrong in str(error), error
            else:
                raise AssertionError(f"SDK version {wrong} must be rejected ({kwargs})")

    for malformed in ("", "1", "1.8", "1.8.", "1.8.dev", "v1.8.0", "1.8.-1",
                      "1.8.\u00b2", "\uff11.\uff18.\uff10"):
        try:
            validate(analysis_version=malformed)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"malformed version {malformed!r} must raise RuntimeError")


def check_standalone_loaders(irsdk, library_dir, expected_version):
    """``load_station`` / ``load_analysis`` must load, bind and gate on their own."""
    analysis = irsdk.load_analysis(library_dir)
    assert analysis.IR_AnalysisVersion().decode() == expected_version
    assert hasattr(analysis, "IR_ROIMean"), "load_analysis must bind IR_analysis exports"
    assert not hasattr(analysis, "IR_OpenDevice"), "load_analysis must not bind station exports"

    station = irsdk.load_station(library_dir)
    assert station.IR_StationVersion().decode() == expected_version
    assert hasattr(station, "IR_OpenDevice"), "load_station must bind IR_station exports"
    # ``load_station`` never binds analysis exports. The ctypes ``__dict__`` is the platform-neutral
    # evidence: on ELF, ``dlsym`` on the station handle also walks its dependencies, so
    # ``IR_ROIMean`` resolves through ``libIR_analysis.so`` even though nothing was bound here.
    assert "IR_ROIMean" not in station.__dict__, "load_station must not bind analysis exports"
    if os.name == "nt":
        assert not hasattr(station, "IR_ROIMean"), "load_station must not bind analysis exports"
    elif os.name == "posix":
        # The imported analysis export must still belong to IR_analysis, never to IR_station.
        loader = irsdk.loader
        station_name, analysis_name = _library_files()
        symbol = loader._posix_dlsym(station, b"IR_ROIMean")
        assert symbol, "the ELF station handle must resolve the imported analysis export"
        owner = loader._posix_symbol_path(symbol)
        assert owner is not None, "cannot establish the on-disk identity of IR_ROIMean"
        assert loader._same_module_path(owner, Path(library_dir) / analysis_name), \
            f"IR_ROIMean resolved to '{owner}' instead of IR_analysis '{analysis_name}'"
        assert not loader._same_module_path(owner, Path(library_dir) / station_name), \
            f"IR_ROIMean must never live in IR_station '{station_name}'"

    # The failure path must release any provisional DLL directory handle it created.
    isolated = lambda candidate: [Path(candidate)]  # noqa: E731 - test isolation
    with _Environment(), patch.object(irsdk.loader, "_search_entries", isolated):
        try:
            irsdk.load_station(library_dir / "missing")
        except FileNotFoundError:
            pass
        else:
            raise AssertionError("load_station must reject a missing library directory")
    assert str(library_dir / "missing") not in irsdk.loader._DLL_DIR_HANDLES, \
        "a failed load must not retain a DLL directory handle"


def check_resident_module_gate(irsdk, library_dir):
    """A same-named module resident from another directory must be rejected before loading."""
    loader = irsdk.loader
    station_name, analysis_name = _library_files()
    if not loader._list_resident_module_paths(analysis_name):
        return  # nothing resident yet (the enumeration is empty on exotic platforms)
    with tempfile.TemporaryDirectory(prefix="irsdk-resident-") as temporary:
        other = _make_pair(Path(temporary) / "other")
        try:
            loader._assert_foreign_modules_absent(other, (analysis_name, station_name))
        except RuntimeError as error:
            message = str(error)
            assert analysis_name in message and str(other.resolve()) in message, message
        else:
            raise AssertionError("a resident SDK module from another directory must be rejected")
    # The directory the resident pair actually came from must still pass the gate.
    loader._assert_foreign_modules_absent(library_dir, (analysis_name, station_name))
    # Exactly one copy of each module is resident, and it is the one we loaded.
    loader._assert_unique_resident_module(library_dir, analysis_name)
    loader._assert_unique_resident_module(library_dir, station_name)


def check_concurrent_loads(irsdk, library_dir, expected_version):
    """Concurrent ``load_sdk`` calls (cold cache) must yield one consistent instance."""
    key = str(irsdk.resolve_sdk_library_dir(library_dir, library="both"))
    with irsdk.loader._DLL_LOCK:
        irsdk.loader._LIBRARY_CACHE.pop(key, None)

    results = []
    errors = []
    barrier = threading.Barrier(8)

    def worker():
        try:
            barrier.wait(timeout=10)
            results.append(irsdk.load_sdk(library_dir))
        except BaseException as error:  # noqa: BLE001 - asserted by the main thread
            errors.append(error)

    threads = [threading.Thread(target=worker) for _ in range(8)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=30)

    assert not errors, errors
    assert len(results) == 8, len(results)
    assert all(libs is results[0] for libs in results), "load_sdk must cache one instance"
    assert results[0].station_version == results[0].analysis_version == expected_version
    assert results[0] is irsdk.load_sdk(library_dir), "the cache entry must survive the race"
    if hasattr(os, "add_dll_directory"):
        assert key in irsdk.loader._DLL_DIR_HANDLES, \
            "the shared DLL directory handle must be retained"
    assert not irsdk.loader._IN_PROGRESS_DIRS
    assert not [thread for thread in threads if thread.is_alive()]


def _fake_cdll():
    """A ``ctypes.CDLL`` instance that never loaded anything (identity-gate unit tests)."""
    return ctypes.CDLL.__new__(ctypes.CDLL)


def check_posix_pair_identity_gate(irsdk):
    """The POSIX ``dlsym``/``dladdr`` gate must reject a foreign ``IR_analysis`` dependency."""
    loader = irsdk.loader
    station, analysis = _fake_cdll(), _fake_cdll()
    directory = Path("/opt/irsdk-a")
    analysis_path = (directory / "libIR_analysis.so").resolve()
    station_path = (directory / "libIR_station.so").resolve()
    foreign_path = Path("/opt/irsdk-b/libIR_analysis.so").resolve()

    addresses = {"analysis": 0x1000, "foreign": 0x2000, "station": 0x3000}
    # Same symbol in the same object => same address; a foreign object yields another address.
    symbols = {"station_dep": addresses["analysis"]}
    paths = {addresses["analysis"]: analysis_path,
             addresses["foreign"]: foreign_path,
             addresses["station"]: station_path}

    def fake_dlsym(library, symbol_name):
        if library is analysis:
            return addresses["analysis"] if symbol_name == b"IR_AnalysisVersion" else None
        if symbol_name == b"IR_AnalysisVersion":
            return symbols["station_dep"]
        if symbol_name == b"IR_StationVersion":
            return addresses["station"]
        return None

    def verify_pair():
        loader._verify_posix_pair(station, analysis, directory, "libIR_station.so",
                                  "libIR_analysis.so")

    with patch.object(loader, "_posix_dlsym", fake_dlsym), \
            patch.object(loader, "_posix_symbol_path", lambda address: paths.get(address)), \
            patch.object(loader, "_posix_global_dlsym", lambda symbol_name: None), \
            patch.object(loader, "_assert_unique_resident_module", lambda *a, **k: None):
        # Matching identities pass.
        verify_pair()

        # A station that bound a *different* IR_analysis (same SONAME) must be rejected.
        symbols["station_dep"] = addresses["foreign"]
        try:
            verify_pair()
        except RuntimeError as error:
            message = str(error)
            assert str(foreign_path) in message and str(analysis_path) in message, message
        else:
            raise AssertionError("a foreign IR_analysis dependency must be rejected")
        symbols["station_dep"] = addresses["analysis"]

        # An object that ``dladdr`` cannot name must also fail closed.
        unidentifiable = dict(paths)
        unidentifiable[addresses["analysis"]] = None
        with patch.object(loader, "_posix_symbol_path",
                          lambda address: unidentifiable.get(address)):
            try:
                verify_pair()
            except RuntimeError as error:
                assert "cannot establish" in str(error), error
            else:
                raise AssertionError("an unidentifiable module must be rejected")

        # ... and so must a station loaded from another directory.
        paths[addresses["station"]] = foreign_path
        try:
            verify_pair()
        except RuntimeError as error:
            assert "IR_station was loaded from" in str(error), error
        else:
            raise AssertionError("a diverted IR_station must be rejected")
        paths[addresses["station"]] = station_path
        # load_analysis() gate: the analysis symbol must live in the requested file.
        with patch.object(loader, "_posix_symbol_path",
                          lambda address: foreign_path if address == addresses["analysis"]
                          else paths.get(address)):
            try:
                loader._verify_posix_analysis(analysis, directory, "libIR_analysis.so")
            except RuntimeError as error:
                assert str(foreign_path) in str(error), error
            else:
                raise AssertionError("a diverted IR_analysis must be rejected")


REAL_POSIX_INTERPOSITION_PROBE = r'''
import ctypes as c
import sys

library_dir, bindings_dir, foreign_so = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, bindings_dir)
import irsdk

loader = irsdk.loader
analysis_name, station_name = "libIR_analysis.so", "libIR_station.so"

# Simulate ``LD_PRELOAD``: an unrelated object defining an SDK export in the global scope.
c.CDLL(foreign_so, mode=c.RTLD_GLOBAL)
assert loader._posix_global_dlsym(b"IR_ROIMean"), "the foreign RTLD_GLOBAL probe must win globally"

for name, call in (("load_station", lambda: irsdk.load_station(library_dir)),
                   ("load_sdk", lambda: irsdk.load_sdk(library_dir))):
    try:
        call()
    except RuntimeError as error:
        assert "interposition" in str(error).lower(), (name, error)
    else:
        raise SystemExit(f"{name} must reject an interposed SDK export")
    assert not loader._IN_PROGRESS_DIRS, (name, loader._IN_PROGRESS_DIRS)
    assert not loader._DLL_DIR_HANDLES, (name, loader._DLL_DIR_HANDLES)
    for module_name in (analysis_name, station_name):
        resident = loader._list_resident_module_paths(module_name)
        assert resident == [], (name, module_name, resident)

print("interposition probe ok")
'''


def check_posix_global_interposition_gate(irsdk):
    """A foreign object winning the global ELF scope for an SDK export must be rejected."""
    loader = irsdk.loader
    directory = Path("/opt/irsdk-a")
    analysis_name, station_name = "libIR_analysis.so", "libIR_station.so"
    analysis_path = (directory / analysis_name).resolve()
    station_path = (directory / station_name).resolve()
    foreign_path = Path("/opt/irsdk-b/libIR_analysis.so").resolve()
    addresses = {"IR_ROIMean": 0x1000}
    paths = {0x1000: analysis_path, 0x2000: foreign_path, 0x3000: None}

    def fake_global(symbol_name):
        return addresses.get(symbol_name.decode("ascii"))

    with patch.object(loader, "_posix_global_dlsym", fake_global), \
            patch.object(loader, "_posix_symbol_path", lambda address: paths.get(address)):
        # Exports resolving back to the requested SDK files are accepted.
        loader._assert_no_posix_global_interposition(directory, analysis_name)
        loader._assert_no_posix_global_interposition(directory, analysis_name, station_name)

        # A foreign global definition of an export must fail closed and name both paths.
        addresses["IR_ROIMean"] = 0x2000
        try:
            loader._assert_no_posix_global_interposition(directory, analysis_name)
        except RuntimeError as error:
            message = str(error)
            assert "interposition" in message.lower(), message
            assert "IR_ROIMean" in message, message
            assert str(foreign_path) in message and str(analysis_path) in message, message
        else:
            raise AssertionError("a globally interposed IR_ROIMean must be rejected")

        # An address whose backing object cannot be named also fails closed.
        addresses["IR_ROIMean"] = 0x3000
        try:
            loader._assert_no_posix_global_interposition(directory, analysis_name)
        except RuntimeError as error:
            message = str(error)
            assert "interposition" in message.lower() and "unknown" in message, message
        else:
            raise AssertionError("an unidentifiable global symbol must be rejected")

        # A station export interposed from another directory is rejected as well.
        addresses.clear()
        addresses["IR_OpenDevice"] = 0x2000
        try:
            loader._assert_no_posix_global_interposition(directory, analysis_name, station_name)
        except RuntimeError as error:
            message = str(error)
            assert "IR_OpenDevice" in message and str(station_path) in message, message
        else:
            raise AssertionError("a globally interposed station export must be rejected")

        # Nothing globally visible => nothing can interpose.
        addresses.clear()
        loader._assert_no_posix_global_interposition(directory, analysis_name, station_name)


def check_real_posix_interposition_subprocess(irsdk, library_dir, bindings_dir):
    """POSIX: a real ``RTLD_GLOBAL`` (``LD_PRELOAD``-style) object must break the load."""
    if os.name != "posix":
        return
    # CMake passes the configured C compiler so the probe matches the ABI the SDK was built with.
    compiler = (os.environ.get("IR_TEST_C_COMPILER")
                or shutil.which("cc")
                or shutil.which("gcc"))
    if compiler is None:
        raise AssertionError(
            "the POSIX interposition gate test needs a C compiler "
            "(IR_TEST_C_COMPILER, cc or gcc)")
    include_dir = Path(__file__).resolve().parents[1] / "include"
    if not (include_dir / "IR_analysis.h").is_file():
        raise AssertionError(f"the POSIX interposition probe needs the public headers in {include_dir}")
    library_dir, bindings_dir = Path(library_dir).resolve(), Path(bindings_dir).resolve()
    with tempfile.TemporaryDirectory(prefix="irsdk-interpose-") as temporary:
        source = Path(temporary) / "foreign_probe.c"
        # Including the shipped header keeps the exported prototype identical to the public ABI.
        source.write_text(
            '#include "IR_analysis.h"\n'
            "\n"
            "int32_t IR_ROIMean(const IRArrayView *frames, int32_t x, int32_t y, int32_t roi_width,\n"
            "                  int32_t roi_height, double *mean_dn, char *err, int32_t err_bytes)\n"
            "{\n"
            "    (void)frames; (void)x; (void)y; (void)roi_width; (void)roi_height;\n"
            "    (void)err_bytes;\n"
            "    if (mean_dn) *mean_dn = 0.0;\n"
            "    if (err) err[0] = '\\0';\n"
            "    return IR_OK;\n"
            "}\n", encoding="utf-8")
        foreign_so = Path(temporary) / "libforeign_probe.so"
        build = subprocess.run(
            [compiler, "-shared", "-fPIC", "-std=c11", f"-I{include_dir}",
             "-Wl,-soname,libforeign_probe.so",
             "-o", str(foreign_so), str(source)],
            capture_output=True, text=True, check=False)
        assert build.returncode == 0, (build.stdout, build.stderr)
        probe = subprocess.run(
            [sys.executable, "-B", "-c", REAL_POSIX_INTERPOSITION_PROBE,
             str(library_dir), str(bindings_dir), str(foreign_so)],
            capture_output=True, text=True, check=False)
        assert probe.returncode == 0, (probe.stdout, probe.stderr)
        assert "interposition probe ok" in probe.stdout, probe.stdout


def check_unique_resident_fail_closed(irsdk, library_dir):
    """An empty module enumeration is a hard error, never a silent skip."""
    loader = irsdk.loader
    analysis_name = _library_files()[1]
    with patch.object(loader, "_list_resident_module_paths", lambda module_name: []):
        try:
            loader._assert_unique_resident_module(library_dir, analysis_name)
        except RuntimeError as error:
            message = str(error)
            assert "expected exactly one resident" in message and "none" in message, message
        else:
            raise AssertionError("an empty module enumeration must fail closed")
    # The real enumeration must still accept the pair this process actually loaded.
    loader._assert_unique_resident_module(library_dir, analysis_name)


def check_reentrant_load_guard(irsdk, library_dir):
    """A nested load of an in-progress directory must fail without rolling back its handles."""
    loader = irsdk.loader
    key = str(irsdk.resolve_sdk_library_dir(library_dir, library="both"))
    committed = irsdk.load_sdk(library_dir)
    real_load = loader._load_library

    def nested_load(directory, name):
        # Same thread, same directory, while the outer attempt is still inside the lock.
        irsdk.SdkLibraries(directory)
        return real_load(directory, name)  # pragma: no cover - the nested load must raise

    with patch.object(loader, "_load_library", nested_load):
        try:
            irsdk.SdkLibraries(library_dir)
        except RuntimeError as error:
            assert "Reentrant" in str(error), error
        else:
            raise AssertionError("a reentrant load of an in-progress directory must be rejected")
    assert not loader._IN_PROGRESS_DIRS, loader._IN_PROGRESS_DIRS
    if hasattr(os, "add_dll_directory"):
        assert key in loader._DLL_DIR_HANDLES, \
            "the committed DLL directory handle must survive a rejected reentrant load"
    assert irsdk.load_sdk(library_dir) is committed
    assert committed.station_version and committed.analysis_version


def check_real_native_unload_rollback(irsdk, library_dir, bindings_dir):
    """A failure inside ``bind_analysis_dll`` must really unload the native module."""
    probe = subprocess.run(
        [sys.executable, "-B", "-c", REAL_NATIVE_UNLOAD_PROBE, str(library_dir),
         _library_files()[1], str(bindings_dir)],
        capture_output=True, text=True, check=False)
    assert probe.returncode == 0, (probe.stdout, probe.stderr)
    assert "native unload probe ok" in probe.stdout, probe.stdout


def check_failed_load_rollback(irsdk):
    """A version-gate failure *after* registration must unload natives and drop the handle."""
    loader = irsdk.loader
    with tempfile.TemporaryDirectory(prefix="irsdk-rollback-") as temporary:
        fresh = _make_pair(Path(temporary) / "sdk")
        key = str(fresh.resolve())
        assert key not in loader._DLL_DIR_HANDLES, key
        # ``_load_library`` is mocked, so the resident-module enumeration must be neutralized too.
        no_resident = lambda name: []  # noqa: E731 - keeps the mocked load consistent
        rollbacks = (
            (lambda: irsdk.load_station(fresh), 2),
            (lambda: irsdk.load_analysis(fresh), 1),
            (lambda: irsdk.SdkLibraries(fresh), 2),
        )
        for load_call, expected_unloads in rollbacks:
            unloaded = []
            with _Environment(), \
                    patch.object(loader, "_list_resident_module_paths", no_resident), \
                    patch.object(loader, "_assert_loaded_module_match",
                                 lambda *a, **k: None), \
                    patch.object(loader, "_load_library", lambda *a, **k: MagicMock()), \
                    patch.object(loader, "_read_version", lambda *a, **k: "1.7.0"), \
                    patch.object(loader, "_unload_library", unloaded.append):
                try:
                    load_call()
                except RuntimeError as error:
                    assert "1.7.0" in str(error) or "incompatible" in str(error), error
                else:
                    raise AssertionError("an SDK reporting 1.7.0 must be rejected")
            assert key not in loader._DLL_DIR_HANDLES, \
                f"a failed load leaked the DLL directory handle for {key}"
            assert len(unloaded) == expected_unloads, (len(unloaded), expected_unloads)


def check_32bit_rejection(bindings_dir):
    """A 32-bit interpreter must fail at import instead of mis-sizing every struct."""
    code = (
        "import ctypes, sys\n"
        f"sys.path.insert(0, {str(bindings_dir)!r})\n"
        "real_sizeof = ctypes.sizeof\n"
        "def fake_sizeof(obj, *args):\n"
        "    if obj is ctypes.c_void_p:\n"
        "        return 4\n"
        "    return real_sizeof(obj, *args)\n"
        "ctypes.sizeof = fake_sizeof\n"
        "try:\n"
        "    import irsdk.abi\n"
        "except RuntimeError as error:\n"
        "    assert '64-bit' in str(error), error\n"
        "else:\n"
        "    raise SystemExit('irsdk.abi must reject a 32-bit pointer size')\n"
    )
    result = subprocess.run([sys.executable, "-B", "-c", code], capture_output=True,
                            text=True, check=False)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)


def smoke_calls(irsdk, libraries, examples_dir):
    analysis = libraries.analysis
    station = libraries.station

    # IR_ROIMean: 2x2 U16 single frame, full-frame ROI.
    pixels = (ctypes.c_uint16 * 4)(1, 2, 3, 4)
    view = irsdk.array_view(pixels, irsdk.IR_TYPE_U16, 2, 2)
    roi_mean = ctypes.c_double()
    irsdk.call_sdk(analysis, "IR_ROIMean", ctypes.byref(view), 0, 0, 2, 2,
                   ctypes.byref(roi_mean))
    assert roi_mean.value == 2.5, roi_mean.value

    # IR_FrameStatistics: 2 frames, 1x1, sample standard deviation (N - 1).
    frames = (ctypes.c_uint16 * 2)(20, 22)
    frame_view = irsdk.array_view(frames, irsdk.IR_TYPE_U16, 1, 1, frames=2)
    mean_map = (ctypes.c_double * 1)()
    std_map = (ctypes.c_double * 1)()
    summary = (ctypes.c_double * 2)()
    irsdk.call_sdk(analysis, "IR_FrameStatistics", ctypes.byref(frame_view), 1,
                   mean_map, std_map, 1, summary, 2)
    assert math.isclose(summary[0], 21.0, abs_tol=1e-12), summary[0]
    assert math.isclose(summary[1], math.sqrt(2.0), abs_tol=1e-12), summary[1]
    assert math.isclose(std_map[0], math.sqrt(2.0), abs_tol=1e-12), std_map[0]

    # IR_BuildTwoPointNucF64: k == 1 for both pixels, b == {5, -5}.
    v1 = (ctypes.c_double * 2)(11.0, 21.0)
    v2 = (ctypes.c_double * 2)(10.0, 20.0)
    k_map = (ctypes.c_double * 2)()
    b_map = (ctypes.c_double * 2)()
    nuc_summary = (ctypes.c_double * 4)()
    irsdk.call_sdk(analysis, "IR_BuildTwoPointNucF64", v1, v2, None, None, 1, 2, 2,
                   k_map, b_map, nuc_summary, 4)
    assert list(k_map) == [1.0, 1.0], list(k_map)
    assert list(b_map) == [5.0, -5.0], list(b_map)
    assert list(nuc_summary) == [16.0, 15.0, 16.0, 2.0], list(nuc_summary)

    # IR_ApplyAffineMapF64: out = k * in + b.
    in_map = (ctypes.c_double * 2)(1.0, 2.0)
    affine_k = (ctypes.c_double * 2)(2.0, 3.0)
    affine_b = (ctypes.c_double * 2)(1.0, 1.0)
    out_map = (ctypes.c_double * 2)()
    irsdk.call_sdk(analysis, "IR_ApplyAffineMapF64", in_map, affine_k, affine_b, 1, 2, 2,
                   out_map)
    assert list(out_map) == [3.0, 7.0], list(out_map)

    # IR_FlickerDetect: one pixel exceeds the 1x per-frame sigma threshold.
    flicker_frames = (ctypes.c_uint16 * 8)(10, 10, 10, 10, 10, 50, 10, 10)
    flicker_view = irsdk.array_view(flicker_frames, irsdk.IR_TYPE_U16, 2, 2, frames=2)
    flicker = irsdk.IRFlickerSummary()
    exceed = (ctypes.c_int32 * 4)()
    flicker_mask = (ctypes.c_uint8 * 4)()
    frame_means = (ctypes.c_double * 2)()
    frame_sigmas = (ctypes.c_double * 2)()
    irsdk.call_sdk(analysis, "IR_FlickerDetect", ctypes.byref(flicker_view), None, None, None,
                   1.0, 0, ctypes.byref(flicker), exceed, flicker_mask, 4,
                   frame_means, frame_sigmas, 2)
    assert flicker.frame_count == 2 and flicker.valid_pixel_count == 4, flicker.frame_count
    assert flicker.flicker_count == 1, flicker.flicker_count
    assert list(exceed) == [0, 1, 0, 0], list(exceed)
    assert list(flicker_mask) == [0, 1, 0, 0], list(flicker_mask)

    # IR_CheckRecipeJson: packaged offline station + recipe templates must preflight.
    examples_dir = Path(examples_dir)
    station_json = (examples_dir / "stations" / "offline.json").read_bytes()
    recipe_json = (examples_dir / "recipes" / "noise_roi.json").read_bytes()
    irsdk.call_sdk(station, "IR_CheckRecipeJson", station_json, recipe_json)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c-client", type=Path, required=True,
                        help="built sdk_tests/c_client executable (compiler ABI dump)")
    parser.add_argument("--library-dir", type=Path, required=True,
                        help="directory holding IR_station / IR_analysis")
    parser.add_argument("--expected-version", required=True, help="PROJECT_VERSION")
    parser.add_argument("--bindings-dir", type=Path, required=True,
                        help="bindings/python containing the irsdk package")
    parser.add_argument("--examples-dir", type=Path, required=True,
                        help="examples/ templates used by the recipe preflight smoke call")
    parser.add_argument("--include-dir", type=Path, default=None,
                        help="include/ with the public headers (defaults to <repo>/include)")
    args = parser.parse_args()

    include_dir = (args.include_dir or Path(__file__).resolve().parents[1] / "include").resolve()
    irsdk = import_binding(args.bindings_dir.resolve())
    compiler = run_c_client(args.c_client.resolve(), args.library_dir.resolve(),
                            "--dump-abi-layout")
    sizes = check_struct_layout(irsdk, compiler)
    check_array_view(irsdk)
    check_callback_convention(irsdk)
    check_library_discovery(irsdk)
    check_binding_identity(irsdk, args.expected_version)
    check_version_gate(irsdk, args.expected_version)
    check_standalone_loaders(irsdk, args.library_dir.resolve(), args.expected_version)
    check_resident_module_gate(irsdk, args.library_dir.resolve())
    check_unique_resident_fail_closed(irsdk, args.library_dir.resolve())
    check_posix_pair_identity_gate(irsdk)
    check_posix_global_interposition_gate(irsdk)
    check_real_posix_interposition_subprocess(irsdk, args.library_dir.resolve(),
                                              args.bindings_dir.resolve())
    check_reentrant_load_guard(irsdk, args.library_dir.resolve())
    check_real_native_unload_rollback(irsdk, args.library_dir.resolve(),
                                      args.bindings_dir.resolve())
    check_failed_load_rollback(irsdk)
    check_32bit_rejection(args.bindings_dir.resolve())
    check_concurrent_loads(irsdk, args.library_dir.resolve(), args.expected_version)
    libraries = irsdk.load_sdk(args.library_dir.resolve())
    declarations = check_declarations(irsdk, include_dir, libraries)
    check_exports(irsdk, libraries, args.expected_version)
    smoke_calls(irsdk, libraries, args.examples_dir.resolve())
    field_count = sum(len(entry["fields"]) for entry in compiler.values())
    print(f"python_binding_abi: {len(sizes)} structs / {field_count} field offsets, "
          f"{len(declarations)} C declarations (return + parameter types), "
          f"{len(irsdk.STATION_EXPORTS) + len(irsdk.ANALYSIS_EXPORTS)} exports, "
          f"version {args.expected_version}")


if __name__ == "__main__":
    main()
