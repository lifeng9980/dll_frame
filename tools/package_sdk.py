#!/usr/bin/env python3
"""Combine CMake install prefixes into a checked Windows-only or dual-platform SDK archive."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
from urllib.parse import unquote
import zipfile


COMMON_ITEMS = ("include", "docs", "examples", "simulators", "third_party", "SDK.md")
REQUIRED_CUSTOMER_FILES = (
    "SDK.md",
    "docs/客户接入说明.md",
    "docs/测试人员配方指南.md",
    "docs/设备支持矩阵.md",
    "docs/第三方组件.md",
    "examples/schemas/station.schema.json",
    "examples/schemas/recipe.schema.json",
)
FORBIDDEN_PACKAGE_FILES = (
    "实施计划.md",
    "验证记录.md",
    "docs/实施计划.md",
    "docs/验证记录.md",
    "docs/框架与二次开发实施计划.md",
    "docs/算法接入指南.md",
    "docs/开发规范.md",
    "examples/power_mock_test.py",
)
FORBIDDEN_PATTERNS = (
    ("personal D: drive path", re.compile(r"(?i)(?<![A-Za-z0-9_])D:[\\/]")),
    ("personal user profile path", re.compile(r"(?i)(?<![A-Za-z0-9_])C:[\\/]Users[\\/]")),
    ("internal H-drive reference", re.compile(r"(?i)(?<![A-Za-z0-9_])H(?:\s*盘|:[\\/])")),
    ("internal sibling repository reference", re.compile(r"(?i)(?:oden_sln|optmeas-usb)")),
    ("internal codex branch reference", re.compile(r"(?i)codex/")),
    ("internal windows-iter branch reference", re.compile(r"(?i)windows-iter")),
)
HYGIENE_TEXT_EXTENSIONS = (
    ".md", ".txt", ".json", ".py", ".c", ".cpp", ".h", ".cs", ".csproj", ".ps1", ".sh", ".cmake",
)
MARKDOWN_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


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

        for line_no, line in enumerate(content.splitlines(), start=1):
            for label, pattern in FORBIDDEN_PATTERNS:
                if pattern.search(line):
                    violations.append(f"{relative}:{line_no}: forbidden {label}: {line.strip()}")

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
            schema_ref = data.get("$schema")
            if isinstance(schema_ref, str) and not schema_ref.startswith(("http://", "https://")):
                resolved_schema = (path.parent / schema_ref).resolve()
                if not resolved_schema.is_relative_to(stage):
                    violations.append(f"{relative}: out-of-package $schema path: {schema_ref}")
                elif not resolved_schema.is_file():
                    violations.append(f"{relative}: missing $schema target: {schema_ref}")
            if rel_parts[1] == "stations":
                acquisition = data.get("acquisition")
                if isinstance(acquisition, dict) and acquisition.get("backend") == "raw_replay":
                    source_ref = acquisition.get("source")
                    if isinstance(source_ref, str):
                        resolved_source = (path.parent / source_ref).resolve()
                        if not resolved_source.is_relative_to(stage):
                            violations.append(f"{relative}: out-of-package raw_replay source: {source_ref}")
                        elif not resolved_source.exists():
                            violations.append(f"{relative}: missing raw_replay source: {source_ref}")

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
    with tempfile.TemporaryDirectory(prefix="IR-SDK-package-", dir=archive.parent) as temporary:
        stage = Path(temporary) / archive_name
        stage.mkdir()

        for item in COMMON_ITEMS:
            copy_item(windows_install / item, stage / item)

        for relative in ("bin/IR_station.dll", "bin/IR_analysis.dll", "bin/ir_run_recipe.exe",
                         "bin/ir_capture_accept.exe", "lib/IR_station.lib", "lib/IR_analysis.lib"):
            copy_item(windows_install / relative, stage / "windows-x64" / relative)
        if not windows_only:
            for relative in ("lib/libIR_station.so", "lib/libIR_analysis.so",
                             "bin/ir_run_recipe", "bin/ir_capture_accept"):
                copy_item(linux_install / relative, stage / "linux-x86_64" / relative)

        scan_package_hygiene(stage)
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
                ):
                    # Windows 打包时也为 Linux 可执行文件写入 Unix 权限。
                    entry = zipfile.ZipInfo.from_file(path, relative)
                    entry.create_system = 3  # Unix ZIP entry
                    entry.external_attr = (stat.S_IFREG | 0o755) << 16
                    package.writestr(entry, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
                else:
                    package.write(path, relative)

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
