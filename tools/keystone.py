"""Build the host Keystone assembler library from a pinned commit.

Keystone 0.9.2 has no macOS arm64 wheel on PyPI, so the host library is
built from source. The commit pin is the integrity anchor: keystone is
not a build input of the bridge, only the host assembler used to encode
the payload.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from npabridge.toolchain import default_library  # noqa: E402


COMMIT = "dc7932ef2b2c4a793836caec6ecab485005139d6"
ARCHIVE_URL = f"https://codeload.github.com/keystone-engine/keystone/tar.gz/{COMMIT}"
SOURCES = ROOT / "build" / "sources"
ARCHIVE = SOURCES / f"keystone-{COMMIT[:8]}.tar.gz"
SOURCE = SOURCES / f"keystone-{COMMIT}"
BUILD_DIR = ROOT / "build" / "keystone-build"
# The assembler is loaded by npabridge.toolchain, so both sides derive its name
# from the same table: `make bootstrap` writes exactly what the patch flow loads.
OUTPUT = default_library()
POLICY_OLD = "cmake_policy(SET CMP0051 OLD)"
POLICY_NEW = "cmake_policy(SET CMP0051 NEW)"
POLICY_FILES = ("CMakeLists.txt", "llvm/CMakeLists.txt")
CONFIGURE_OPTIONS = (
    "-G",
    "Ninja",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DBUILD_SHARED_LIBS=ON",
    "-DBUILD_LIBS_ONLY=ON",
    "-DLLVM_TARGETS_TO_BUILD=AArch64",
) + (
    # Only Apple's CMake knows this variable; elsewhere the host ABI decides.
    ("-DCMAKE_OSX_ARCHITECTURES=arm64",)
    if sys.platform == "darwin"
    else ()
)


def _run(command: list[object]) -> None:
    subprocess.run([str(part) for part in command], check=True)


def fetch_archive() -> Path:
    if ARCHIVE.is_file():
        return ARCHIVE
    SOURCES.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(ARCHIVE_URL) as response, ARCHIVE.open("wb") as output:
        shutil.copyfileobj(response, output)
    return ARCHIVE


def extract_source() -> Path:
    if SOURCE.is_dir():
        return SOURCE
    archive = fetch_archive()
    with tarfile.open(archive, "r:gz") as stream:
        stream.extractall(SOURCES, filter="data")
    if not (SOURCE / "CMakeLists.txt").is_file():
        raise RuntimeError(f"unexpected Keystone archive layout: {archive}")
    return SOURCE


def apply_cmake_policy(source: Path) -> None:
    for relative in POLICY_FILES:
        path = source / relative
        text = path.read_text(encoding="utf-8")
        if POLICY_NEW in text:
            continue
        if text.count(POLICY_OLD) != 1:
            raise RuntimeError(f"Keystone compatibility policy is missing: {path}")
        path.write_text(text.replace(POLICY_OLD, POLICY_NEW), encoding="utf-8")


def _built_library(build_dir: Path, suffix: str) -> Path:
    """The shared library CMake just wrote, named for this host."""

    matches = sorted(
        path
        for path in (build_dir / "llvm" / "lib").glob(f"libkeystone{suffix}*")
        if path.is_file()
    )
    if not matches:
        raise RuntimeError(f"CMake did not produce a Keystone library in {build_dir}")
    return matches[0]


def build(source: Path, build_dir: Path = BUILD_DIR, output: Path = OUTPUT) -> Path:
    if build_dir.exists():
        shutil.rmtree(build_dir)
    _run(["cmake", "-S", source, "-B", build_dir, *CONFIGURE_OPTIONS])
    _run(["cmake", "--build", build_dir])
    built = _built_library(build_dir, output.suffix)
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, output)
    return output


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    if OUTPUT.is_file() and "--force" not in arguments:
        print(f"keystone: reusing {OUTPUT} (pass --force to rebuild)")
        return 0
    if sys.platform == "win32":
        raise RuntimeError(
            "Windows patching requires the official Keystone 0.9.2 "
            f"keystone.dll copied to the repository root: {OUTPUT}"
        )
    source = extract_source()
    apply_cmake_policy(source)
    print(build(source))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
