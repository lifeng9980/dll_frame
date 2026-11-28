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
CMake ``…/cmake`` entries, with pair validation), and a handful of pure computation entry
points. No hardware, no live capture and no file output are involved.
"""
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch

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
