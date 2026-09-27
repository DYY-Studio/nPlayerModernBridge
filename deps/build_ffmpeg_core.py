"""Cross-compile the static iOS 13 FFmpeg *core* closure.

This is the closure that carries `libavformat`, `libavcodec` and `libavutil`
for the unit that replaces the FFmpeg the app links statically today
(4.4.5 -> 4.4.8). It is deliberately separate from `deps/build_ffmpeg.py`,
which builds the 9.0.2 scaler/resampler closure for the other bridge dylib.

Two hazards this file exists to prevent:

* the two closures must not share a source tree. `build_deps.extract_source`
  returns an existing `build/deps/sources/<name>/tree` verbatim, so reusing the
  name `ffmpeg` would silently build 4.4.8 out of a 9.0.2 tree. The core source
  is therefore keyed `ffmpeg-core` in the lock.
* the app's FFmpeg decodes AV1 through *libdav1d* (the binary contains
  `libdav1d` and `dav1d_apply_grain`, which dav1d 1.0 removed) and its DASH
  demuxer is absent (no libxml2), so the only external dependency is dav1d
  0.9.2. Anything else would silently change which decoder
  `avcodec_find_decoder(AV_CODEC_ID_AV1)` returns.
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
LOCK_PATH = Path(__file__).resolve().parent / "ffmpeg-core.lock.json"
DAV1D_PREFIX = "dav1d"
TARGET = build_deps.TARGET


def dav1d_source_key(lock: dict[str, Any]) -> str | None:
    """The lock's dav1d source key, if it declares one.

    The key is per-lock on purpose: extract_source() hands back an existing
    build/deps/sources/<key>/tree verbatim, so two closures that need different
    dav1d releases (the 4.4 core wants 0.9.2 for dav1d_apply_grain, the 9.0.2
    core wants >= 1.0.0) must not share the key.
    """

    return next(
        (name for name in lock["sources"] if name.startswith(DAV1D_PREFIX)), None
    )


def ffmpeg_source_key(lock: dict[str, Any]) -> str:
    """The lock's single non-dav1d source key."""

    return next(
        name for name in lock["sources"] if not name.startswith(DAV1D_PREFIX)
    )


def load_lock(path: Path = LOCK_PATH) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise ValueError("lock schema is invalid")
    if data.get("target") != TARGET:
        raise ValueError("lock target is not iOS 13 arm64")
    sources = data.get("sources")
    if (
        not isinstance(sources, dict)
        or not 1 <= len(sources) <= 2
        or (len(sources) == 2 and dav1d_source_key(data) is None)
    ):
        raise ValueError("lock must declare one FFmpeg source and optional dav1d")
    for name, entry in sources.items():
        for field in ("version", "url", "archive_sha256", "archive_name"):
            if field not in entry:
                raise ValueError(f"{name} lock entry is incomplete: {field}")
        if not entry["url"].startswith("https://"):
            raise ValueError(f"{name} source URL is not HTTPS")
        build_deps._sha256(entry["archive_sha256"], name)
        if Path(entry["archive_name"]).name != entry["archive_name"]:
            raise ValueError(f"{name} archive name is unsafe")
    prefix = Path(data["prefix"])
    if not data["output_archives"]:
        raise ValueError("lock declares no output archive")
    for item in data["output_archives"]:
        path = Path(item)
        if path.parent != prefix / "lib" or path.suffix != ".a":
            raise ValueError(f"output path is unsafe: {item}")
    if dav1d_source_key(data) is not None and not any(
        Path(item).name == "libdav1d.a" for item in data["output_archives"]
    ):
        raise ValueError("closure must carry libdav1d.a when it builds dav1d")
    return data


def archive_paths(lock: dict[str, Any]) -> tuple[Path, ...]:
    """The built archives, in static link order (consumers before providers)."""

    return tuple(ROOT / item for item in lock["output_archives"])


def build_dav1d(lock: dict[str, Any]) -> None:
    """Build libdav1d into the closure prefix (meson, arm64 iOS 13)."""

    key = dav1d_source_key(lock)
    assert key is not None
    source = build_deps.extract_source(key, lock)
    prefix = ROOT / lock["prefix"]
    build = ROOT / "build" / "deps" / "build" / "dav1d-core"
    if build.exists():
        shutil.rmtree(build)
    env = build_deps.isolated_environment(
        {
            "NINJA": build_deps.host_tool("ninja"),
            "MESON": build_deps.host_tool("meson"),
            "PATH": os.pathsep.join(
                [
                    str(Path(build_deps.host_tool("meson")).parent),
                    build_deps.SYSTEM_PATH,
                ]
            ),
            "PYTHON": sys.executable,
        }
    )
    arguments = [
        build_deps.host_tool("meson"),
        "setup",
        str(build),
        str(source),
        "--cross-file",
        str(build_deps.CROSS_FILE),
        "--native-file",
        str(build_deps.NATIVE_FILE),
        "--backend=ninja",
        "--buildtype=release",
        "--wrap-mode=nodownload",
        f"-Dprefix={prefix}",
        "-Dlibdir=lib",
        "-Dincludedir=include",
        "-Ddefault_library=static",
    ]
    for key, value in lock["sources"][dav1d_source_key(lock)]["build_options"].items():
        rendered = "true" if value is True else "false" if value is False else str(value)
        arguments.append(f"-D{key}={rendered}")
    build_deps.run(arguments, env)
    build_deps.run([build_deps.host_tool("ninja"), "-C", str(build), "install"], env)


def configure_command(lock: dict[str, Any], sdk: str) -> list[str]:
    common = ["-target", TARGET, "-isysroot", sdk]
    prefix = ROOT / lock["prefix"]
    placeholders = {
        "prefix": str(prefix),
        "cc": build_deps._xcrun("clang", "iphoneos"),
        "ar": build_deps._xcrun("ar", "iphoneos"),
        "ranlib": build_deps._xcrun("ranlib", "iphoneos"),
        "nm": build_deps._xcrun("nm", "iphoneos"),
        "strip": build_deps._xcrun("strip", "iphoneos"),
        "sdk": sdk,
        "dav1d_include": str(prefix / "include"),
        "dav1d_lib": str(prefix / "lib"),
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


def verify_closure(
    lock: dict[str, Any] | None = None, lock_path: Path = LOCK_PATH
) -> dict[str, Any]:
    lock = load_lock(lock_path) if lock is None else lock
    source_key = ffmpeg_source_key(lock)
    env = build_deps.isolated_environment()
    archives = archive_paths(lock)
    lib_root = ROOT / lock["lib_root"]
    include_root = ROOT / lock["include_root"]
    if not lib_root.is_dir():
        raise FileNotFoundError(f"closure library directory is missing: {lib_root}")

    expected = sorted(path.name for path in archives)
    present = sorted(path.name for path in lib_root.iterdir() if path.is_file())
    if present != expected:
        raise ValueError(
            f"closure archive set mismatch: expected {expected}, found {present}"
        )
    report_archives = {
        path.name: build_deps._validate_archive(path, env) for path in archives
    }
    if dav1d_source_key(lock) is not None and "libdav1d.a" not in report_archives:
        raise ValueError("closure is missing libdav1d.a")

    paths = list(archives) + [path for path in include_root.rglob("*") if path.is_file()]
    violations = [
        f"{path}: {prefix}"
        for path in paths
        for prefix in build_deps.FORBIDDEN_PATH_PREFIXES
        if prefix.encode() in path.read_bytes()
    ]
    if violations:
        raise ValueError("forbidden host path in closure output: " + "; ".join(violations))

    write_closure(lock)
    report = {
        "target": TARGET,
        "ffmpeg": lock["sources"][source_key]["version"],
        "archives": report_archives,
        "include_root": str(include_root),
        "lib_root": str(lib_root),
        "system_link_args": list(lock["system_link_args"]),
        "path_hygiene": "passed",
    }
    if dav1d_source_key(lock) is not None:
        report["dav1d"] = lock["sources"][dav1d_source_key(lock)]["version"]
    verification = ROOT / lock["verification"]
    verification.parent.mkdir(parents=True, exist_ok=True)
    verification.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report


def build(lock_path: Path = LOCK_PATH) -> dict[str, Any]:
    lock = load_lock(lock_path)
    source_key = ffmpeg_source_key(lock)
    sdk = build_deps.sdk_path()
    source = build_deps.extract_source(source_key, lock)
    if lock["sources"][source_key]["version"] not in (source / "RELEASE").read_text(
        encoding="utf-8"
    ):
        raise ValueError(f"source tree is not {lock['sources'][source_key]['version']}")
    prefix = ROOT / lock["prefix"]
    if prefix.exists():
        shutil.rmtree(prefix)
    if dav1d_source_key(lock) is not None:
        build_dav1d(lock)

    env = build_deps.isolated_environment(
        {
            "LC_ALL": "C",
            # FFmpeg's configure probes `which pkg-config` and silently degrades
            # to `false` when it is not on PATH. Put the host tool directory on
            # PATH instead of passing --pkg-config=<abs path>, because the
            # absolute path would be embedded in the configure string and trip
            # the host-path check below.
            "PATH": os.pathsep.join(
                [str(Path(build_deps.host_tool("pkg-config")).parent), build_deps.SYSTEM_PATH]
            ),
            "PKG_CONFIG_LIBDIR": str(prefix / "lib" / "pkgconfig"),
            "PKG_CONFIG_PATH": "",
        }
    )
    build_deps.run(["./configure", *configure_command(lock, sdk)], env, source)
    build_deps.run(["/usr/bin/make", f"-j{os.cpu_count() or 2}"], env, source)
    build_deps.run(["/usr/bin/make", "install"], env, source)
    return verify_closure(lock)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--lock", default=str(LOCK_PATH))
    arguments = parser.parse_args(argv)
    lock_path = Path(arguments.lock)
    try:
        report = verify_closure(lock_path=lock_path) if arguments.verify_only else build(lock_path)
        print(json.dumps(report, indent=2, sort_keys=True))
    except Exception as error:  # noqa: BLE001
        print(f"ffmpeg-core build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
