"""Patch one nPlayer IPA with the prebuilt bridge dylibs.

The flow is linear and fails loudly: extract the executable, refuse an
encrypted dump, resolve the version by SHA-256, check every selected bridge
contract, freeze the layout, rewrite the call sites of the selected units,
assemble and pseudo-sign, then verify the shipped artifact before publishing
it atomically.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence
from zipfile import ZipFile

from . import macho, package, verify
from .manifest import Manifest, Unit, select_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = ROOT / "manifests"
MAIN_MEMBER = package.MAIN_MEMBER


@dataclass(frozen=True)
class PatchResult:
    source: Path
    output: Path
    app_version: str
    dylibs: tuple[str, ...]
    source_main_sha256: str
    packaged_main_sha256: str
    bridge_sha256s: dict[str, str]
    state_initial: int
    checks_passed: int

    def as_dict(self) -> dict:
        return {
            "source": str(self.source),
            "output": str(self.output),
            "app_version": self.app_version,
            "dylibs": list(self.dylibs),
            "source_main_sha256": self.source_main_sha256,
            "packaged_main_sha256": self.packaged_main_sha256,
            "bridge_sha256s": dict(self.bridge_sha256s),
            "state_initial": self.state_initial,
            "checks_passed": self.checks_passed,
        }


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _extract_main(source: Path, destination: Path) -> Path:
    with ZipFile(source) as archive:
        if MAIN_MEMBER not in set(archive.namelist()):
            raise ValueError(f"{source.name} does not carry {MAIN_MEMBER}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(archive.read(MAIN_MEMBER))
    return destination


def _reject_encrypted(main: Path) -> None:
    binary = macho.parse(main)
    if not binary.has_encryption_info:
        raise ValueError(
            "input executable carries no LC_ENCRYPTION_INFO; it is not an iOS app binary"
        )
    if int(binary.encryption_info.crypt_id) != 0:
        raise ValueError(
            "this IPA is still FairPlay-encrypted; a decrypted dump of your own "
            "copy is required (crypt_id != 0)"
        )


def selected_dylib_ids(units: Sequence[Unit]) -> tuple[str, ...]:
    """The dylib ids of these units, in manifest order."""

    return tuple(dict.fromkeys(unit.dylib_id for unit in units))


def default_output_name(source: Path, manifest: Manifest, units: Sequence[Unit]) -> Path:
    """`<stem>-<dylib id><library version>` for every selected dylib."""

    parts = [
        f"{dylib_id}{manifest.dylib(dylib_id).library_version}"
        for dylib_id in selected_dylib_ids(units)
    ]
    return source.with_name(f"{source.stem}-{'-'.join(parts)}.ipa")


def patch_ipa(
    source: Path | str,
    output: Path | str | None,
    dylibs_dir: Path | str,
    manifests: Path | str = MANIFESTS,
    dylibs: Sequence[str] | None = None,
    work: Path | str | None = None,
) -> PatchResult:
    source = Path(source).resolve()
    dylibs_dir = Path(dylibs_dir).resolve()
    manifests = Path(manifests).resolve()
    if not source.is_file():
        raise FileNotFoundError(f"source IPA is missing: {source}")
    if not dylibs_dir.is_dir():
        raise FileNotFoundError(f"bridge dylib directory is missing: {dylibs_dir}")
    if not manifests.is_dir():
        raise FileNotFoundError(f"manifest directory is missing: {manifests}")

    created_work = work is None
    work = Path(work) if work is not None else Path(tempfile.mkdtemp(prefix="npa-patch-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        source_main = _extract_main(source, work / "source-main")
        source_digest = _sha256(source_main)
        _reject_encrypted(source_main)
        manifest = select_manifest(manifests, source_main)
        units = manifest.units(dylibs)
        dylib_ids = selected_dylib_ids(units)

        bridges: dict[str, Path] = {}
        for dylib_id in dylib_ids:
            dylib = manifest.dylib(dylib_id)
            path = dylibs_dir / dylib.basename
            if not path.is_file():
                raise FileNotFoundError(
                    f"bridge dylib for {dylib_id} is missing: {path}"
                )
            bridges[dylib_id] = path
        basenames = tuple(manifest.dylib(dylib_id).basename for dylib_id in dylib_ids)

        output_path = (
            Path(output).resolve()
            if output is not None
            else default_output_name(source, manifest, units)
        )
        if output_path == source:
            raise ValueError("refusing to overwrite the source IPA; pass -o")
        if output_path in set(bridges.values()):
            raise ValueError("refusing to overwrite a bridge dylib; pass another -o")

        for dylib_id, path in bridges.items():
            verify.verify_bridge(path, manifest.dylib(dylib_id)).require()
        macho.preflight(source_main, manifest, units)

        macho.phase_a(source_main, work / "main-phase-a", manifest, units)
        macho.phase_b(work / "main-phase-a", work / "main-phase-b", manifest, units)

        temporary = output_path.with_name(f".tmp-{output_path.name}")
        temporary.unlink(missing_ok=True)
        try:
            package.package_ipa(
                source,
                temporary,
                work / "main-phase-b",
                {manifest.dylib(dylib_id).basename: path for dylib_id, path in bridges.items()},
                work=work / "package",
            )
            extracted = package.extract_for_verification(
                temporary, work / "shipped", basenames
            )
            shipped = {
                dylib_id: extracted[manifest.dylib(dylib_id).basename]
                for dylib_id in dylib_ids
            }
            report = verify.verify_artifact(
                source_main, extracted["main"], manifest, units, shipped
            )
            report.require()
            packaged_main_sha256 = _sha256(extracted["main"])
            bridge_sha256s = dict(report.bridge_sha256s)
            output_path.parent.mkdir(parents=True, exist_ok=True)
            temporary.replace(output_path)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
    finally:
        if created_work:
            shutil.rmtree(work, ignore_errors=True)

    return PatchResult(
        source=source,
        output=output_path,
        app_version=manifest.app_version,
        dylibs=dylib_ids,
        source_main_sha256=source_digest,
        packaged_main_sha256=packaged_main_sha256,
        bridge_sha256s=bridge_sha256s,
        state_initial=report.state_initial,
        checks_passed=len(report.checks),
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="npa-patch",
        description="Patch a decrypted nPlayer IPA with the prebuilt bridge dylibs.",
    )
    parser.add_argument("source", type=Path, help="your own decrypted nPlayer .ipa")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="where to write the patched IPA (default: next to the source)",
    )
    parser.add_argument(
        "--dylibs-dir",
        type=Path,
        default=Path.cwd(),
        help="directory holding the release bridge dylibs (default: the working directory)",
    )
    parser.add_argument(
        "--dylib",
        action="append",
        dest="dylibs",
        default=None,
        metavar="ID",
        help="install only this bridge dylib, by manifest id (repeatable; "
        "default: the manifest's default_dylibs)",
    )
    parser.add_argument("--manifests", type=Path, default=MANIFESTS)
    arguments = parser.parse_args(argv)
    try:
        result = patch_ipa(
            arguments.source,
            arguments.output,
            arguments.dylibs_dir,
            arguments.manifests,
            arguments.dylibs,
        )
        print(json.dumps(result.as_dict(), indent=2, sort_keys=True))
    except Exception as error:  # noqa: BLE001
        print(f"patch failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
