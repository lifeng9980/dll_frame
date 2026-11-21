#!/usr/bin/env python3
"""Combine Windows and Linux CMake install prefixes into a checked SDK archive."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile


COMMON_ITEMS = ("include", "docs", "examples", "simulators", "third_party", "SDK.md")


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


def make_package(source_root, windows_install, linux_install, archive):
    source_root = source_root.resolve()
    windows_install = windows_install.resolve()
    linux_install = linux_install.resolve()
    archive = archive.resolve()
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
        linux_content = linux_install / item
        require_item(windows_content)
        require_item(linux_content)
        if tree_hashes(windows_content) != tree_hashes(linux_content):
            raise ValueError(f"Windows and Linux install content differs: {item}")

    for relative in (
        "bin/IR_station.dll", "bin/IR_analysis.dll", "bin/ir_run_recipe.exe",
        "lib/IR_station.lib", "lib/IR_analysis.lib",
    ):
        require_file(windows_install / relative)
    for relative in ("lib/libIR_station.so", "lib/libIR_analysis.so", "bin/ir_run_recipe"):
        require_file(linux_install / relative)

    archive_name = f"IR-SDK-{version}-Windows-Linux"
    commit, dirty = source_metadata(source_root)
    with tempfile.TemporaryDirectory(prefix="IR-SDK-package-", dir=archive.parent) as temporary:
        stage = Path(temporary) / archive_name
        stage.mkdir()

        for item in COMMON_ITEMS:
            copy_item(windows_install / item, stage / item)

        for relative in ("bin/IR_station.dll", "bin/IR_analysis.dll", "bin/ir_run_recipe.exe",
                         "lib/IR_station.lib", "lib/IR_analysis.lib"):
            copy_item(windows_install / relative, stage / "windows-x64" / relative)
        for relative in ("lib/libIR_station.so", "lib/libIR_analysis.so", "bin/ir_run_recipe"):
            copy_item(linux_install / relative, stage / "linux-x86_64" / relative)

        metadata = {
            "sdk_version": version,
            "source_commit": commit,
            "source_worktree_dirty": dirty,
            "platforms": ["windows-x64", "linux-x86_64"],
            "common_install_content_verified_equal": True,
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
                if relative == f"{archive_name}/linux-x86_64/bin/ir_run_recipe":
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--windows-install", type=Path, required=True)
    parser.add_argument("--linux-install", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new ZIP archive path")
    args = parser.parse_args()
    make_package(args.source_root, args.windows_install, args.linux_install, args.output)


if __name__ == "__main__":
    main()
