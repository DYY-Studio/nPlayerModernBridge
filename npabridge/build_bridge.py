"""Build the bridge dylibs declared in the manifest.

Each dylib wraps its own static dependency closure. It must stay a plain dylib: no implicit initializers, no third-party
dynamic dependency and exactly the exported symbols its units declare.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from . import macho, verify
from .manifest import Dylib, Manifest, load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "manifests" / "nplayer-3.13.0.json"
TARGET = "arm64-apple-ios13.0"
BUILD_ROOT = ROOT / "build"
OBJECT_ROOT = BUILD_ROOT / "bridge"
ASS_FONT_MODES = ("libass-patch", "bridge-isolation")


def manifest() -> Manifest:
    return load_manifest(MANIFEST_PATH)


def output_path(dylib: Dylib) -> Path:
    return BUILD_ROOT / dylib.basename


def object_path(dylib: Dylib) -> Path:
    return OBJECT_ROOT / f"{dylib.id}.o"


def report_path(dylib: Dylib) -> Path:
    return OBJECT_ROOT / f"{dylib.id}-verification.json"


def _run(command: list[object], cwd: Path | None = None) -> str:
    import subprocess

    result = subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd) if cwd is not None else None,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"command failed: {command}\n{result.stdout}")
    return result.stdout


def _build(manifest: Manifest, dylib_id: str) -> Dylib:
    dylib = manifest.dylib(dylib_id)
    if dylib.build is None:
        raise ValueError(f"dylib {dylib_id} has no build section")
    return dylib


def load_closure(
    dylib_id: str = "libass",
) -> tuple[tuple[Path, ...], tuple[str, ...]]:
    dylib = _build(manifest(), dylib_id)
    path = ROOT / dylib.build.closure
    lib_root = ROOT / dylib.build.lib_root
    if not path.is_file():
        raise FileNotFoundError(f"dependency closure is missing: {path}")
    values: dict[str, list[str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, _, raw = line.partition("=")
        values[key.strip()] = raw.split()
    archives = tuple(lib_root / name for name in values.get("project_archives", []))
    system_link_args = tuple(values.get("system_link_args", []))
    if not archives and not system_link_args:
        raise ValueError(f"dependency closure has no link inputs: {path}")
    for archive in archives:
        if not archive.is_file():
            raise FileNotFoundError(archive)
    return archives, system_link_args


def nm_exports(path: Path) -> list[str]:
    return macho.exported_symbols(macho.parse(path))


def verify_dylib(dylib_id: str, path: Path | None = None) -> dict[str, Any]:
    manifest_ = manifest()
    dylib = manifest_.dylib(dylib_id)
    report = verify.verify_bridge(path or output_path(dylib), dylib)
    report.require()
    report.write(report_path(dylib))
    return report.as_dict()


def compile_dylib(
    manifest: Manifest, dylib_id: str, sdk: Path,
    source: Path | None = None, object_file: Path | None = None,
) -> Path:
    dylib = _build(manifest, dylib_id)
    object_file = object_file or object_path(dylib)
    object_file.parent.mkdir(parents=True, exist_ok=True)
    object_file.unlink(missing_ok=True)
    _run(
        [
            macho.xcrun_find("clang"),
            "-target",
            TARGET,
            "-isysroot",
            str(sdk),
            "-arch",
            "arm64",
            "-miphoneos-version-min=13.0",
            "-fvisibility=hidden",
            "-fno-common",
            "-I" + str(ROOT / dylib.build.include_root),
            "-ffile-prefix-map=" + str(ROOT) + "=.",
            "-fdebug-prefix-map=" + str(ROOT) + "=.",
            "-O2",
            "-Werror=implicit-function-declaration",
            "-Werror=incompatible-pointer-types",
            "-Werror=return-type",
            "-c",
            str(source or ROOT / dylib.build.source),
            "-o",
            str(object_file),
        ]
    )
    return object_file


def link_dylib(
    manifest: Manifest,
    dylib_id: str,
    sdk: Path,
    archives: tuple[Path, ...],
    system_link_args: tuple[str, ...],
    output: Path | None = None,
    export_list: Path | None = None,
    object_file: Path | None = None,
) -> Path:
    dylib = _build(manifest, dylib_id)
    output = output or output_path(dylib)
    export_list = export_list or (ROOT / dylib.build.exports)
    object_file = object_file or object_path(dylib)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    _run(
        [
            macho.xcrun_find("clang"),
            "-target",
            TARGET,
            "-isysroot",
            str(sdk),
            "-arch",
            "arm64",
            "-miphoneos-version-min=13.0",
            "-dynamiclib",
            "-Wl,-install_name," + dylib.install_name,
            "-Wl,-exported_symbols_list," + str(export_list),
            "-Wl,-oso_prefix," + str(ROOT) + "/",
            str(object_file),
            *(str(archive) for archive in archives),
            *system_link_args,
            "-o",
            str(output),
        ]
    )
    return output


def build_dylib(
    dylib_id: str, output: Path | None = None, font_mode: str = "libass-patch",
) -> Path:
    if font_mode not in ASS_FONT_MODES:
        raise ValueError(f"unknown font mode: {font_mode}")
    manifest_ = manifest()
    dylib = _build(manifest_, dylib_id)
    output = output or output_path(dylib)
    source = None
    obj = None
    mode_report = None
    if dylib_id == "libass":
        # Remove the published output before any step that might fail.
        output.unlink(missing_ok=True)
        (OBJECT_ROOT / "libass-font-mode.json").unlink(missing_ok=True)
        report_path(dylib).unlink(missing_ok=True)
    archives, system_link_args = load_closure(dylib_id)
    if dylib_id == "libass":
        _run([sys.executable, ROOT / "deps/build_ass.py", "--font-mode", font_mode])
        mode_root = BUILD_ROOT / "deps/ass-font-modes" / font_mode
        archives = tuple(mode_root / "lib/libass.a" if p.name == "libass.a" else p for p in archives)
        mode_report = json.loads((mode_root / "build-report.json").read_text())
        source = ROOT / "bridge" / (
            "npa_ass_bridge.c" if font_mode == "libass-patch" else "npa_ass_isolated_bridge.c"
        )
        obj = OBJECT_ROOT / font_mode / "libass.o"
    sdk = macho.sdk_path()
    obj = compile_dylib(manifest_, dylib_id, sdk, source=source, object_file=obj)
    result = link_dylib(manifest_, dylib_id, sdk, archives, system_link_args,
                        output=output, object_file=obj)
    if mode_report is not None:
        verify.verify_bridge(result, dylib).require()
        mode_report.update({
            "bridge_source": str(source.relative_to(ROOT)),
            "bridge_source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "archives": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in archives},
            "output_sha256": hashlib.sha256(result.read_bytes()).hexdigest(),
        })
        (OBJECT_ROOT / "libass-font-mode.json").write_text(json.dumps(mode_report, indent=2) + "\n")
    return result


def verify_font_mode(font_mode: str) -> None:
    report = json.loads((OBJECT_ROOT / "libass-font-mode.json").read_text())
    output = output_path(manifest().dylib("libass"))
    if report["font_mode"] != font_mode:
        raise ValueError(f"libass was built with {report['font_mode']}, requested {font_mode}")
    if report["output_sha256"] != hashlib.sha256(output.read_bytes()).hexdigest():
        raise ValueError("libass output does not match its font mode report")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--font-mode", choices=ASS_FONT_MODES, default="libass-patch")
    parser.add_argument(
        "--dylib",
        action="append",
        dest="dylibs",
        default=None,
        metavar="ID",
        help="build only this bridge dylib (repeatable; default: all)",
    )
    arguments = parser.parse_args(argv)
    try:
        manifest_ = manifest()
        dylib_ids = arguments.dylibs or [dylib.id for dylib in manifest_.dylibs]
        reports = []
        for dylib_id in dylib_ids:
            if not arguments.verify_only:
                print(f"building {dylib_id}", flush=True)
                build_dylib(dylib_id, font_mode=arguments.font_mode)
            if dylib_id == "libass":
                verify_font_mode(arguments.font_mode)
            reports.append(verify_dylib(dylib_id))
        print(json.dumps(reports, indent=2, sort_keys=True))
    except Exception as error:  # noqa: BLE001
        print(f"bridge build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
