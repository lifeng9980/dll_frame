"""Official thin Python binding for the IR SDK pure C ABI (``IR_station`` + ``IR_analysis``).

The package is a pure ``ctypes`` mirror of ``include/IR_*.h``: enumerators, public structs,
callbacks, function signatures, ``sizeof`` invariants and the UTF-8 error-buffer convention.
Standard library only (``ctypes`` / ``os`` / ``pathlib``) — no numpy, no Qt, no third-party
package — and no hardware access by itself.

Layout:
    ``irsdk.abi``     structs, enums, callbacks, ``EXPECTED_SIZES``, ``array_view``
    ``irsdk.loader``  signatures, exports, library discovery, ``call_sdk`` helpers

Quick start::

    import ctypes
    import irsdk

    libs = irsdk.load_sdk(r"<package>/windows-x64/bin")
    print(libs.station_version, libs.analysis_version)

    pixels = (ctypes.c_uint16 * 2)(20, 22)
    view = irsdk.array_view(pixels, irsdk.IR_TYPE_U16, 1, 1, frames=2)
    mean_map = (ctypes.c_double * 1)()
    std_map = (ctypes.c_double * 1)()
    summary = (ctypes.c_double * 2)()
    irsdk.call_sdk(libs.analysis, "IR_FrameStatistics", ctypes.byref(view), 1,
                   mean_map, std_map, 1, summary, 2)
    print(summary[0], summary[1])   # 21.0 (DN mean), ~1.4142 (sample std N-1: sqrt(2))
"""
from __future__ import annotations

from . import abi as _abi
from . import loader as _loader
from .abi import *  # noqa: F401,F403  (public surface declared in abi.__all__)
from .loader import *  # noqa: F401,F403  (public surface declared in loader.__all__)

__version__ = "1.8.0"

__all__ = [*_abi.__all__, *_loader.__all__, "__version__"]
