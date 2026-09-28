"""Assemble and pseudo-sign a patched IPA.

One artifact is a patched main plus the selected bridge dylibs. Every
artifact is assembled in a scratch tree, signed with ldid and only
published after its contents were checked.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Mapping
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


ROOT = Path(__file__).resolve().parents[1]
APP_DIR = Path("Payload") / "nPlayer.app"
MAIN_MEMBER = (APP_DIR / "nPlayer").as_posix()
FRAMEWORKS = APP_DIR / "Frameworks"
LINKEDIT = "ldid"
TOOL_HINTS = {
    "ldid": "brew install ldid on macOS; on Linux use a prebuilt ldid binary",
}


def bridge_member(basename: str) -> str:
    return (FRAMEWORKS / basename).as_posix()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"{name} is required to assemble the IPA ({TOOL_HINTS[name]})")
    return path


def _run(command: list[object], cwd: Path | None = None) -> None:
    subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd) if cwd is not None else None,
        check=True,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sign(path: Path) -> None:
    _run([_tool(LINKEDIT), "-S", path])


def _write_ipa(
    source_ipa: Path,
    output: Path,
    main: Path,
    bridges: Mapping[str, Path],
) -> dict[str, Any]:
    """Rewrite an IPA with signed files while preserving source ZIP metadata."""

    with ZipFile(source_ipa) as source:
        infos = source.infolist()
        mains = [info for info in infos if info.filename == MAIN_MEMBER]
        _require(
            len(mains) == 1,
            f"{source_ipa.name} carries {len(mains)} main executables",
        )
        frameworks_member = FRAMEWORKS.as_posix()
        if any(info.filename == frameworks_member for info in infos):
            raise ValueError(
                f"source IPA carries {FRAMEWORKS.name} as a file, not a directory"
            )
        source_names = {info.filename for info in infos}
        for basename in bridges:
            member = bridge_member(basename)
            _require(
                member not in source_names,
                f"source IPA already carries selected bridge {basename}",
            )

        output.parent.mkdir(parents=True, exist_ok=True)
        output.unlink(missing_ok=True)
        try:
            with ZipFile(output, "w") as destination:
                destination.comment = source.comment
                for info in infos:
                    content = (
                        main.read_bytes()
                        if info.filename == MAIN_MEMBER
                        else source.read(info)
                    )
                    destination.writestr(info, content)
                for basename, bridge in bridges.items():
                    info = ZipInfo(bridge_member(basename))
                    info.compress_type = ZIP_DEFLATED
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | 0o755) << 16
                    destination.writestr(info, bridge.read_bytes())
            return inspect_ipa(output, tuple(bridges))
        except Exception:
            output.unlink(missing_ok=True)
            raise


def package_ipa(
    source_ipa: Path,
    output: Path,
    main: Path,
    bridges: Mapping[str, Path],
    work: Path | None = None,
) -> dict[str, Any]:
    """Assemble, pseudo-sign and publish one patched IPA."""

    if not bridges:
        raise ValueError("no bridge dylib was selected")
    created_scratch = work is None
    scratch_root = Path(work) if work is not None else Path(tempfile.mkdtemp(prefix="npa-patch-"))
    scratch = scratch_root / f"files-{output.name}"
    temporary = output.with_name(f".tmp-{output.name}")
    try:
        scratch_root.mkdir(parents=True, exist_ok=True)
        if scratch.exists():
            shutil.rmtree(scratch)
        scratch.mkdir()
        signed_main = scratch / "nPlayer"
        shutil.copy2(main, signed_main)
        signed_main.chmod(0o755)
        sign(signed_main)
        signed_bridges: dict[str, Path] = {}
        for basename, bridge in bridges.items():
            target = scratch / basename
            shutil.copy2(bridge, target)
            target.chmod(0o755)
            sign(target)
            signed_bridges[basename] = target
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary.unlink(missing_ok=True)
        try:
            report = _write_ipa(
                source_ipa,
                temporary,
                signed_main,
                signed_bridges,
            )
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
        shutil.rmtree(scratch, ignore_errors=True)
        if created_scratch:
            shutil.rmtree(scratch_root, ignore_errors=True)
    report.update(
        {
            "artifact": str(output),
            "main_sha256": _sha256(main),
            "bridge_sha256s": {
                basename: _sha256(bridge) for basename, bridge in bridges.items()
            },
        }
    )
    return report


def inspect_ipa(path: Path, basenames: tuple[str, ...]) -> dict[str, Any]:
    if not basenames:
        raise ValueError("no bridge dylib was selected")
    with ZipFile(path) as archive:
        names = archive.namelist()
    mains = [name for name in names if name == MAIN_MEMBER]
    _require(len(mains) == 1, f"{path.name} carries {len(mains)} main executables")
    bridges: dict[str, int] = {}
    for basename in basenames:
        member = bridge_member(basename)
        found = [name for name in names if name == member]
        _require(
            len(found) == 1,
            f"{path.name} carries {len(found)} copies of {basename}",
        )
        bridges[basename] = len(found)
    return {"main_members": len(mains), "bridge_members": bridges}


def extract_for_verification(
    ipa: Path, destination: Path, basenames: tuple[str, ...]
) -> dict[str, Path]:
    """Extract the shipped executable and bridge dylibs from one packaged IPA."""

    with ZipFile(ipa) as archive:
        names = set(archive.namelist())
        if MAIN_MEMBER not in names:
            raise ValueError(f"{ipa.name} does not carry {MAIN_MEMBER}")
        destination.mkdir(parents=True, exist_ok=True)
        main = destination / "nPlayer"
        main.write_bytes(archive.read(MAIN_MEMBER))
        extracted = {"main": main}
        for basename in basenames:
            member = bridge_member(basename)
            if member in names:
                bridge = destination / basename
                bridge.write_bytes(archive.read(member))
                extracted[basename] = bridge
    return extracted


def publish(
    source_ipa: Path,
    output: Path,
    main: Path,
    bridges: Mapping[str, Path],
) -> dict[str, Any]:
    return package_ipa(source_ipa, output, main, bridges)
