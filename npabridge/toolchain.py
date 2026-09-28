"""Keystone-backed arm64 assembler used to encode the dispatch payload."""

from __future__ import annotations

import ctypes
import sys
from pathlib import Path


# The host assembler is a build product / release asset, not a Python package:
# `make bootstrap` writes it next to the package, and release users drop the
# pinned asset there. Its name is the host's shared-library name, so the macOS
# and Linux builds are told apart by the platform, never guessed.
LIBRARY_NAMES = {
    "darwin": "libkeystone.dylib",
    "linux": "libkeystone.so",
    "win32": "keystone.dll",
}
ARCH_ARM64 = 2
MODE_LITTLE_ENDIAN = 0
SUPPORTED_VERSION = (0, 9)


def default_library(platform: str | None = None) -> Path:
    """Where this platform keeps the host assembler, or a loud failure."""

    host = sys.platform if platform is None else platform
    name = LIBRARY_NAMES.get(host)
    if name is None:
        raise RuntimeError(
            f"unsupported host platform for the Keystone assembler: {host}\n"
            f"  expected one of: {', '.join(sorted(LIBRARY_NAMES))}"
        )
    return Path(__file__).resolve().parents[1] / name


class Toolchain:
    def __init__(self, library_path: str | Path | None = None) -> None:
        selected = default_library() if library_path is None else Path(library_path)
        if not selected.is_file():
            raise FileNotFoundError(
                f"host assembler library is missing: {selected}\n"
                "  run `make bootstrap` to build it, or place the release asset "
                "next to the package (the patch flow runs from a repository checkout)"
            )
        self.library_path = selected.resolve()
        self._library = ctypes.CDLL(str(self.library_path))
        self._bind_api()
        major = ctypes.c_uint()
        minor = ctypes.c_uint()
        result = self._ks_version(ctypes.byref(major), ctypes.byref(minor))
        if result != (major.value << 8) + minor.value:
            raise RuntimeError("invalid Keystone version response")
        if (major.value, minor.value) != SUPPORTED_VERSION:
            raise RuntimeError(
                f"unsupported Keystone API version {major.value}.{minor.value}"
            )

    def _bind_api(self) -> None:
        self._ks_version = self._library.ks_version
        self._ks_version.argtypes = [
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint),
        ]
        self._ks_version.restype = ctypes.c_uint

        self._ks_open = self._library.ks_open
        self._ks_open.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self._ks_open.restype = ctypes.c_int

        self._ks_asm = self._library.ks_asm
        self._ks_asm.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self._ks_asm.restype = ctypes.c_int

        self._ks_close = self._library.ks_close
        self._ks_close.argtypes = [ctypes.c_void_p]
        self._ks_close.restype = ctypes.c_int

        self._ks_free = self._library.ks_free
        self._ks_free.argtypes = [ctypes.POINTER(ctypes.c_ubyte)]
        self._ks_free.restype = None

    def assemble(self, source: str, address: int) -> bytes:
        if not isinstance(source, str) or not source:
            raise ValueError("assembly source must be a non-empty string")
        if not isinstance(address, int) or not 0 <= address < 1 << 64:
            raise ValueError("assembly address must be a uint64")
        engine = ctypes.c_void_p()
        opened = self._ks_open(
            ARCH_ARM64,
            MODE_LITTLE_ENDIAN,
            ctypes.byref(engine),
        )
        if opened != 0 or not engine.value:
            raise RuntimeError(f"ks_open failed with error {opened}")
        encoding = ctypes.POINTER(ctypes.c_ubyte)()
        try:
            size = ctypes.c_size_t()
            count = ctypes.c_size_t()
            result = self._ks_asm(
                engine,
                source.encode("utf-8"),
                address,
                ctypes.byref(encoding),
                ctypes.byref(size),
                ctypes.byref(count),
            )
            if result != 0:
                raise RuntimeError(f"ks_asm failed with error {result}")
            if not encoding or size.value == 0:
                raise RuntimeError("ks_asm returned no instructions")
            return bytes(encoding[: size.value])
        finally:
            if encoding:
                self._ks_free(encoding)
            self._ks_close(engine)
