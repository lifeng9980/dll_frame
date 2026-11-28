#!/usr/bin/env python3
"""Combine CMake install prefixes into a checked Windows-only or dual-platform SDK archive."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import time
from urllib.parse import unquote
import zipfile


COMMON_ITEMS = ("include", "docs", "examples", "simulators", "third_party", "bindings", "SDK.md")
# Files copied out of the CMake install tree into the package-specific layouts.
CMAKE_PACKAGE_FILES = (
    "lib/cmake/IRSDK/IRSDKConfig.cmake",
    "lib/cmake/IRSDK/IRSDKConfigVersion.cmake",
)
REQUIRED_CUSTOMER_FILES = (
    "include/IR_types.h",
    "include/IR_plugin_abi.h",
    "include/IR_plugin_cxx.hpp",
    "include/IR_power_driver_abi.h",
    "SDK.md",
    "docs/客户接入说明.md",
    "bindings/python/pyproject.toml",
    "bindings/python/irsdk/__init__.py",
    "bindings/python/irsdk/abi.py",
    "bindings/python/irsdk/loader.py",
    "cmake/IRSDKConfig.cmake",
    "cmake/IRSDKConfigVersion.cmake",
)
FORBIDDEN_PACKAGE_FILES = (
    "AGENTS.md",
    "tools",
    "sdk_tests",
    "examples/data/tc002a_640",
    "examples/data/s07_1280",
    "实施计划.md",
    "验证记录.md",
    "docs/实施计划.md",
    "docs/验证记录.md",
    "docs/框架与二次开发实施计划.md",
    "docs/算法接入指南.md",
    "docs/开发规范.md",
    "docs/研发与算法指南.md",
)
FORBIDDEN_PATTERNS = (
    ("personal D: drive path", re.compile(r"(?i)(?<![A-Za-z0-9_])D:[\\/]")),
    ("personal user profile path", re.compile(r"(?i)(?<![A-Za-z0-9_])C:[\\/]Users[\\/]")),
    ("internal H-drive reference", re.compile(r"(?i)(?<![A-Za-z0-9_])H(?:\s*盘|:[\\/])")),
    ("internal sibling repository reference", re.compile(r"(?i)(?:oden_sln|optmeas-usb)")),
    ("internal codex branch reference", re.compile(r"(?i)codex/")),
    ("internal windows-iter branch reference", re.compile(r"(?i)windows-iter")),
    ("internal feat/plugin-arch branch reference", re.compile(r"(?i)feat/plugin-arch")),
    ("internal build or probe path", re.compile(
        r"""实验(?:[\\/]+|["']\s*[,/+]\s*["'](?:[\\/]+)?)(?:build-(?:debug|release)|debug_tc002a|hil_serial_map\.json)\b""",
        re.IGNORECASE,
    )),
)
HYGIENE_TEXT_EXTENSIONS = (
    ".md", ".txt", ".json", ".py", ".c", ".cpp", ".h", ".hpp", ".cs", ".csproj", ".ps1", ".sh",
    ".cmake", ".toml",
)
MARKDOWN_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
DEBUG_CRT_DLL_RE = re.compile(
    r"(?:ucrtbased|msvcr(?:t|[0-9]{2,3})d|msvcm[0-9]{2,3}d|"
    r"msvcp[0-9]{2,3}(?:_[0-9]+)?d(?:_(?:atomic_wait|codecvt_ids))?|"
    r"vcruntime[0-9]{3}(?:_(?:[0-9]+|threads))?d|"
    r"(?:concrt|vccorlib|vcamp|vcomp)[0-9]{2,3}d)\.dll",
    re.IGNORECASE,
)


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tree_hashes(path):
    if path.is_file():
        return {path.name: file_hash(path)}
    return {
        str(item.relative_to(path)).replace("\\", "/"): file_hash(item)
        for item in sorted(path.rglob("*")) if item.is_file()
    }


def copy_item(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination)
    elif source.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    else:
        raise FileNotFoundError(f"Required install content is missing: {source}")


def require_file(path):
    if not path.is_file():
        raise FileNotFoundError(f"Required SDK file is missing: {path}")


def require_item(path):
    if not path.exists():
        raise FileNotFoundError(f"Required install content is missing: {path}")


def source_metadata(source_root):
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True
        ).strip()
        dirty = bool(subprocess.check_output(
            ["git", "-C", str(source_root), "status", "--porcelain"], text=True
        ).strip())
    except (OSError, subprocess.CalledProcessError):
        commit = "unknown"
        dirty = None
    return commit, dirty


def scan_package_hygiene(stage):
    stage = stage.resolve()
    violations = []

    for relative in REQUIRED_CUSTOMER_FILES:
        if not (stage / relative).is_file():
            violations.append(f"missing required customer file: {relative}")

    for relative in FORBIDDEN_PACKAGE_FILES:
        if (stage / relative).exists():
            violations.append(f"internal file must not be packaged: {relative}")

    third_party = stage / "third_party"
    for path in sorted(stage.rglob("*")):
        relative = str(path.relative_to(stage)).replace("\\", "/")
        rel_parts = path.relative_to(stage).parts
        if "__pycache__" in rel_parts or path.suffix.lower() in (".pyc", ".pyo"):
            violations.append(f"python cache artifact must not be packaged: {relative}")
            continue
        if len(rel_parts) >= 3 and rel_parts[:2] == ("examples", "csharp") and rel_parts[2] in ("bin", "obj"):
            violations.append(f"C# build artifact must not be packaged: {relative}")
            continue
        if not path.is_file():
            continue
        if path.is_relative_to(third_party):
            continue
        if path.suffix.lower() not in HYGIENE_TEXT_EXTENSIONS:
            continue

        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            violations.append(f"{relative}: invalid UTF-8 ({error})")
            continue

        lines = content.split("\n")
        for label, pattern in FORBIDDEN_PATTERNS:
            for match in pattern.finditer(content):
                line_no = content.count("\n", 0, match.start()) + 1
                violations.append(f"{relative}:{line_no}: forbidden {label}: {lines[line_no - 1].strip()}")

        if path.suffix.lower() == ".md":
            for line_no, line in enumerate(content.splitlines(), start=1):
                for match in MARKDOWN_LINK_RE.finditer(line):
                    raw_target = match.group(1).strip()
                    if not raw_target:
                        continue
                    if raw_target.startswith("<") and ">" in raw_target:
                        target = raw_target[1:raw_target.index(">")].strip()
                    else:
                        target = raw_target.split()[0]
                    if target.startswith(("http://", "https://", "mailto:", "#")):
                        continue
                    target_path = unquote(target.split("#", 1)[0])
                    if not target_path:
                        continue
                    resolved = (path.parent / target_path).resolve()
                    if not resolved.is_relative_to(stage):
                        violations.append(f"{relative}:{line_no}: out-of-package link: {target}")
                    elif not resolved.exists():
                        violations.append(f"{relative}:{line_no}: broken package link: {target}")

        if path.suffix.lower() == ".json" and len(rel_parts) >= 3 and rel_parts[:2] in (
            ("examples", "stations"),
            ("examples", "recipes"),
        ):
            try:
                data = json.loads(content)
            except json.JSONDecodeError as error:
                violations.append(f"{relative}: invalid JSON ({error})")
                continue
            if not isinstance(data, dict):
                continue
            if rel_parts[1] == "stations":
                acquisition = data.get("acquisition")
                if isinstance(acquisition, dict) and acquisition.get("backend") == "raw_replay":
                    source_ref = acquisition.get("source")
                    if isinstance(source_ref, str):
                        resolved_source = (path.parent / source_ref).resolve()
                        if not resolved_source.is_relative_to(stage):
                            violations.append(f"{relative}: out-of-package raw_replay source: {source_ref}")
                        elif resolved_source.name not in ("tc002a_640", "s07_1280") and not resolved_source.exists():
                            violations.append(f"{relative}: missing raw_replay source: {source_ref}")
                if "plugins" in data:
                    plugins = data.get("plugins")
                    if not isinstance(plugins, list):
                        violations.append(f"{relative}: station.plugins must be an array")
                    else:
                        for plugin_ref in plugins:
                            if not isinstance(plugin_ref, str) or not plugin_ref.strip():
                                violations.append(f"{relative}: station.plugins entries must be non-empty strings")
                            else:
                                resolved_plugin = (path.parent / plugin_ref).resolve()
                                if not resolved_plugin.is_relative_to(stage):
                                    violations.append(f"{relative}: out-of-package plugin path: {plugin_ref}")
                                elif not resolved_plugin.exists():
                                    violations.append(f"{relative}: missing plugin path: {plugin_ref}")

    if violations:
        details = "\n  - ".join(violations)
        raise ValueError(f"SDK package hygiene check failed:\n  - {details}")


def verify_windows_dll_commit(dll_path, expected_commit):
    needle = f"Source commit: {expected_commit}\0".encode("utf-16-le")
    if needle not in dll_path.read_bytes():
        raise RuntimeError(
            f"Windows DLL {dll_path.name} does not contain expected Source commit '{expected_commit}'; "
            "rebuild before packaging"
        )


def windows_imports(path):
    """Read direct and delay-loaded PE DLL names without using Windows tooling."""
    data = path.read_bytes()
    try:
        (pe,) = struct.unpack_from("<I", data, 0x3c)
        if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\x00\x00":
            raise ValueError("missing PE signature")
        (section_count,) = struct.unpack_from("<H", data, pe + 6)
        (optional_size,) = struct.unpack_from("<H", data, pe + 20)
        optional = pe + 24
        (magic,) = struct.unpack_from("<H", data, optional)
        if magic == 0x10b:
            directories_offset = 96
            (image_base,) = struct.unpack_from("<I", data, optional + 28)
        elif magic == 0x20b:
            directories_offset = 112
            (image_base,) = struct.unpack_from("<Q", data, optional + 24)
        else:
            raise ValueError(f"unknown PE optional header type {magic:#x}")
        directories = optional + directories_offset
        (directory_count,) = struct.unpack_from("<I", data, directories - 4)
        (headers_size,) = struct.unpack_from("<I", data, optional + 60)
        sections_offset = optional + optional_size
        if directories + 8 * directory_count > sections_offset or sections_offset + 40 * section_count > len(data):
            raise ValueError("truncated PE headers")
        # Section header fields: VirtualAddress, SizeOfRawData, PointerToRawData.
        sections = [struct.unpack_from("<III", data, sections_offset + i * 40 + 12)
                    for i in range(section_count)]
        sections.append((0, headers_size, 0))

        def rva_bounds(rva):
            for address, size, start in sections:
                if address <= rva < address + size and start + size <= len(data):
                    return start + rva - address, start + size
            raise ValueError(f"invalid PE import RVA {rva:#x}")

        imports = set()
        # Directory 1: IMAGE_IMPORT_DESCRIPTOR. Directory 13: delay-load descriptors.
        for directory, entry_words, name_field in ((1, 5, 3), (13, 8, 1)):
            if directory >= directory_count:
                continue
            rva, size = struct.unpack_from("<II", data, directories + 8 * directory)
            if not rva and not size:
                continue
            begin, end = rva_bounds(rva)
            if not rva or begin + size > end:
                raise ValueError("invalid PE import directory extent")
            entry = struct.Struct(f"<{entry_words}I")
            for offset in range(begin, begin + size - entry.size + 1, entry.size):
                fields = entry.unpack_from(data, offset)
                if not any(fields):
                    break
                name_rva = fields[name_field]
                if directory == 13 and not fields[0] & 1:
                    name_rva -= image_base
                if name_rva <= 0:
                    raise ValueError("invalid PE import name RVA")
                name, bound = rva_bounds(name_rva)
                name_end = data.find(b"\0", name, bound)
                if name_end <= name:
                    raise ValueError("invalid PE import name")
                imports.add(data[name:name_end].decode("ascii"))
            else:
                raise ValueError("missing PE import directory terminator")
        return imports
    except (ValueError, struct.error) as error:
        raise ValueError(f"Cannot inspect Windows imports of {path}: {error}") from error


def verify_windows_runtime_dependencies(stage):
    violations = []
    for path in sorted(stage.rglob("*")):
        if path.suffix.lower() not in (".exe", ".dll") or not path.is_file():
            continue
        debug_imports = sorted(name for name in windows_imports(path) if DEBUG_CRT_DLL_RE.fullmatch(name))
        if debug_imports:
            violations.append(f"{path.relative_to(stage).as_posix()}: {', '.join(debug_imports)}")
    if violations:
        details = "\n  - ".join(violations)
        raise RuntimeError(f"Windows package contains Debug CRT imports; use the windows-release preset:\n  - {details}")


def make_package(source_root, windows_install, linux_install, archive, windows_only=False, require_clean=False):
    source_root = source_root.resolve()
    windows_install = windows_install.resolve()
    linux_install = linux_install.resolve() if linux_install is not None else None
    archive = archive.resolve()

    if windows_only and linux_install is not None:
        raise ValueError("--linux-install cannot be combined with --windows-only")
    if not windows_only and linux_install is None:
        raise ValueError("--linux-install is required unless --windows-only is specified")

    commit, dirty = source_metadata(source_root)
    if require_clean and dirty is not False:
        raise RuntimeError(
            f"Refusing to package with --require-clean (source_commit={commit}, source_worktree_dirty={dirty})"
        )

    if archive.exists() or Path(str(archive) + ".sha256").exists():
        raise FileExistsError(f"Refusing to overwrite package output: {archive}")
    archive.parent.mkdir(parents=True, exist_ok=True)

    cmake = (source_root / "CMakeLists.txt").read_text(encoding="utf-8")
    version_match = re.search(r"project\(IRSDK VERSION ([0-9]+\.[0-9]+\.[0-9]+)", cmake)
    if not version_match:
        raise ValueError("Cannot read the SDK version from CMakeLists.txt")
    version = version_match.group(1)

    for item in COMMON_ITEMS:
        windows_content = windows_install / item
        require_item(windows_content)
        if not windows_only:
            linux_content = linux_install / item
            require_item(linux_content)
            if tree_hashes(windows_content) != tree_hashes(linux_content):
                raise ValueError(f"Windows and Linux install content differs: {item}")

    # The CMake package files are platform independent; a dual-platform package must ship
    # byte-identical config files.
    for relative in CMAKE_PACKAGE_FILES:
        require_file(windows_install / relative)
        if not windows_only:
            require_file(linux_install / relative)
            if file_hash(windows_install / relative) != file_hash(linux_install / relative):
                raise ValueError(f"Windows and Linux CMake package files differ: {relative}")

    for relative in (
        "bin/IR_station.dll", "bin/IR_analysis.dll", "bin/ir_run_recipe.exe",
        "bin/ir_capture_accept.exe",
        "lib/IR_station.lib", "lib/IR_analysis.lib",
    ):
        require_file(windows_install / relative)
    if not windows_only:
        for relative in (
            "lib/libIR_station.so", "lib/libIR_analysis.so",
            "bin/ir_run_recipe", "bin/ir_capture_accept",
        ):
            require_file(linux_install / relative)

    archive_name = f"IR-SDK-{version}-Windows" if windows_only else f"IR-SDK-{version}-Windows-Linux"
    temporary = tempfile.mkdtemp(prefix="IR-SDK-package-", dir=archive.parent)
    try:
        stage = Path(temporary) / archive_name
        stage.mkdir()

        for item in COMMON_ITEMS:
            copy_item(windows_install / item, stage / item)

        for relative in CMAKE_PACKAGE_FILES:
            copy_item(windows_install / relative, stage / "cmake" / Path(relative).name)

        for relative in ("bin/IR_station.dll", "bin/IR_analysis.dll", "bin/ir_run_recipe.exe",
                         "bin/ir_capture_accept.exe", "lib/IR_station.lib", "lib/IR_analysis.lib"):
            copy_item(windows_install / relative, stage / "windows-x64" / relative)
        if not windows_only:
            for relative in ("lib/libIR_station.so", "lib/libIR_analysis.so",
                             "bin/ir_run_recipe", "bin/ir_capture_accept"):
                copy_item(linux_install / relative, stage / "linux-x86_64" / relative)

        scan_package_hygiene(stage)
        verify_windows_runtime_dependencies(stage)
        if commit != "unknown":
            expected_commit = f"{commit}-dirty" if dirty else commit
            verify_windows_dll_commit(stage / "windows-x64/bin/IR_station.dll", expected_commit)
            verify_windows_dll_commit(stage / "windows-x64/bin/IR_analysis.dll", expected_commit)

        metadata = {
            "sdk_version": version,
            "source_commit": commit,
            "source_worktree_dirty": dirty,
            "package_mode": "windows-only" if windows_only else "dual-platform",
            "platforms": ["windows-x64"] if windows_only else ["windows-x64", "linux-x86_64"],
            "common_install_content_verified_equal": None if windows_only else True,
            "hygiene_check_passed": True,
        }
        (stage / "manifest.json").write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        manifest_lines = []
        for path in sorted(item for item in stage.rglob("*") if item.is_file()):
            if path.name == "MANIFEST.sha256":
                continue
            relative = str(path.relative_to(stage)).replace("\\", "/")
            manifest_lines.append(f"{file_hash(path)}  {relative}")
        (stage / "MANIFEST.sha256").write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")

        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as package:
            for path in sorted(item for item in stage.rglob("*") if item.is_file()):
                relative = str(path.relative_to(stage.parent)).replace("\\", "/")
                if relative in (
                    f"{archive_name}/linux-x86_64/bin/ir_run_recipe",
                    f"{archive_name}/linux-x86_64/bin/ir_capture_accept",
                    f"{archive_name}/linux-x86_64/lib/libIR_station.so",
                    f"{archive_name}/linux-x86_64/lib/libIR_analysis.so",
                ):
                    # Windows 打包时也为 Linux 可执行文件与动态库写入 Unix 权限。
                    entry = zipfile.ZipInfo.from_file(path, relative)
                    entry.create_system = 3  # Unix ZIP entry
                    entry.external_attr = (stat.S_IFREG | 0o755) << 16
                    package.writestr(entry, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
                else:
                    package.write(path, relative)
    finally:
        for attempt in range(5):
            try:
                shutil.rmtree(temporary)
                break
            except PermissionError:
                if attempt == 4:
                    shutil.rmtree(temporary, ignore_errors=True)
                else:
                    time.sleep(0.2)

    digest = file_hash(archive)
    Path(str(archive) + ".sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    print(f"archive={archive}")
    print(f"sha256={digest}")
    print(f"source_commit={commit}")
    print(f"source_worktree_dirty={dirty}")
    print(f"package_mode={'windows-only' if windows_only else 'dual-platform'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--windows-install", type=Path, required=True)
    parser.add_argument("--linux-install", type=Path, default=None)
    parser.add_argument("--windows-only", action="store_true",
                        help="package Windows x64 only (IR-SDK-<version>-Windows.zip) without --linux-install")
    parser.add_argument("--require-clean", action="store_true",
                        help="refuse to package when the Git source worktree is dirty or unavailable")
    parser.add_argument("--output", type=Path, required=True, help="new ZIP archive path")
    args = parser.parse_args()
    make_package(
        args.source_root,
        args.windows_install,
        args.linux_install,
        args.output,
        windows_only=args.windows_only,
        require_clean=args.require_clean,
    )


if __name__ == "__main__":
    main()
