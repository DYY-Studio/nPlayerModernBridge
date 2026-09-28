"""Cross-compile the static iOS 13 FFmpeg utility closure.

Only `libavutil`, `libswscale` and `libswresample` are built: the bridge that
carries the modern FFmpeg must not drag in a decoder, a demuxer or a muxer.
The closure is verified against the same rules as the libass closure (arm64,
iOS 13, non-thin, no host paths) and must contain exactly the three declared
archives; a fourth one means `configure` leaked a component.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

import build_deps


ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = Path(__file__).resolve().parent / "ffmpeg.lock.json"
SOURCE_NAME = "ffmpeg"
TARGET = build_deps.TARGET


def load_lock(path: Path = LOCK_PATH) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise ValueError("ffmpeg lock schema is invalid")
    if data.get("target") != TARGET:
        raise ValueError("ffmpeg lock target is not iOS 13 arm64")
    sources = data.get("sources")
    if not isinstance(sources, dict) or set(sources) != {SOURCE_NAME}:
        raise ValueError("ffmpeg lock must declare exactly the ffmpeg source")
    entry = sources[SOURCE_NAME]
    for field in ("version", "url", "archive_sha256", "archive_name"):
        if field not in entry:
            raise ValueError(f"ffmpeg lock entry is incomplete: {field}")
    if not entry["url"].startswith("https://"):
        raise ValueError("ffmpeg source URL is not HTTPS")
    build_deps._sha256(entry["archive_sha256"], SOURCE_NAME)
    if Path(entry["archive_name"]).name != entry["archive_name"]:
        raise ValueError("ffmpeg archive name is unsafe")
    prefix = Path(data["prefix"])
    for item in data["output_archives"]:
        path = Path(item)
        if path.parent != prefix / "lib" or path.suffix != ".a":
            raise ValueError(f"ffmpeg output path is unsafe: {item}")
    if not data["output_archives"]:
        raise ValueError("ffmpeg lock declares no output archive")
    return data


def archive_paths(lock: dict[str, Any]) -> tuple[Path, ...]:
    """The built archives, in static link order (dependents first)."""

    return tuple(ROOT / item for item in lock["output_archives"])


def configure_command(lock: dict[str, Any], sdk: str) -> list[str]:
    common = [
        "-target",
        TARGET,
        "-isysroot",
        sdk,
    ]
    placeholders = {
        "prefix": str(ROOT / lock["prefix"]),
        "cc": build_deps._xcrun("clang", "iphoneos"),
        "ar": build_deps._xcrun("ar", "iphoneos"),
        "ranlib": build_deps._xcrun("ranlib", "iphoneos"),
        "nm": build_deps._xcrun("nm", "iphoneos"),
        "strip": build_deps._xcrun("strip", "iphoneos"),
        "sdk": sdk,
        "cflags": " ".join(
            common
            + [
                "-miphoneos-version-min=13.0",
                "-g0",
                f"-ffile-prefix-map={ROOT}=.",
                f"-fdebug-prefix-map={ROOT}=.",
            ]
        ),
        "ldflags": " ".join(common),
    }
    return [argument.format(**placeholders) for argument in lock["configure_args"]]


def write_closure(lock: dict[str, Any]) -> None:
    path = ROOT / lock["closure"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "project_archives="
        + " ".join(Path(item).name for item in lock["output_archives"])
        + "\nsystem_link_args="
        + " ".join(lock["system_link_args"])
        + "\n",
        encoding="utf-8",
    )


def verify_closure(lock: dict[str, Any] | None = None) -> dict[str, Any]:
    lock = load_lock() if lock is None else lock
    env = build_deps.isolated_environment()
    archives = archive_paths(lock)
    lib_root = ROOT / lock["prefix"] / "lib"
    include_root = ROOT / lock["include_root"]
    if not lib_root.is_dir():
        raise FileNotFoundError(f"ffmpeg library directory is missing: {lib_root}")

    expected = sorted(path.name for path in archives)
    present = sorted(path.name for path in lib_root.iterdir() if path.is_file())
    if present != expected:
        raise ValueError(
            "ffmpeg closure archive set mismatch: "
            f"expected {expected}, found {present}"
        )
    report_archives = {
        path.name: build_deps._validate_archive(path, env) for path in archives
    }

    paths = list(archives) + [path for path in include_root.rglob("*") if path.is_file()]
    violations = [
        f"{path}: {prefix}"
        for path in paths
        for prefix in build_deps.FORBIDDEN_PATH_PREFIXES
        if prefix.encode() in path.read_bytes()
    ]
    if violations:
        raise ValueError(
            "forbidden host path in ffmpeg output: " + "; ".join(violations)
        )

    write_closure(lock)
    report = {
        "target": TARGET,
        "source": lock["sources"][SOURCE_NAME]["version"],
        "archives": report_archives,
        "include_root": str(include_root),
        "lib_root": str(lib_root),
        "system_link_args": list(lock["system_link_args"]),
        "path_hygiene": "passed",
    }
    verification = ROOT / lock["verification"]
    verification.parent.mkdir(parents=True, exist_ok=True)
    verification.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report


def build() -> dict[str, Any]:
    lock = load_lock()
    sdk = build_deps.sdk_path()
    source = build_deps.extract_source(SOURCE_NAME, lock)
    prefix = ROOT / lock["prefix"]
    if prefix.exists():
        shutil.rmtree(prefix)
    env = build_deps.isolated_environment({"LC_ALL": "C"})
    build_deps.run(["./configure", *configure_command(lock, sdk)], env, source)
    build_deps.sanitize_ffmpeg_configuration(source / "config.h")
    build_deps.run(["/usr/bin/make", f"-j{os.cpu_count() or 2}"], env, source)
    build_deps.run(["/usr/bin/make", "install"], env, source)
    return verify_closure(lock)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify-only", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        report = verify_closure() if arguments.verify_only else build()
        print(json.dumps(report, indent=2, sort_keys=True))
    except Exception as error:  # noqa: BLE001
        print(f"ffmpeg build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
