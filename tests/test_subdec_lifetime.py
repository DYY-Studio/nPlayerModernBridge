"""Run the real iOS bridge with its FFmpeg closure in a temporary Catalyst host."""
import shutil
import struct
import subprocess
import sys
import platform
from pathlib import Path

import pytest

from npabridge import build_bridge, macho

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def lifetime_probe(tmp_path_factory):
    if sys.platform != "darwin" or platform.machine() != "arm64" or not shutil.which("xcrun"):
        pytest.skip("requires Apple arm64 Catalyst host")
    if not (ROOT / "build/deps/ffmpeg-core902-closure.txt").is_file():
        pytest.skip("requires the built FFmpeg 9.0.2 dependency closure")
    directory = tmp_path_factory.mktemp("subdec-lifetime")
    obj = directory / "probe.o"
    dylib = directory / "probe.dylib"
    exports = directory / "exports"
    exports.write_text("_npa_probe_extradata\n")
    compiler = macho.xcrun_find("clang")
    subprocess.run([
        str(compiler), "-target", build_bridge.TARGET, "-isysroot", str(macho.sdk_path()),
        "-O2", "-fvisibility=hidden", "-I" + str(ROOT / "bridge"),
        "-I" + str(ROOT / "build/deps/ffmpeg-core902/include"),
        "-c", str(ROOT / "dev/subdec_extradata_probe.c"), "-o", str(obj),
    ], check=True)
    archives, system = build_bridge.load_closure("ffmpeg-core902")
    build_bridge.link_dylib(build_bridge.manifest(), "ffmpeg-core902", macho.sdk_path(),
                           archives, system, output=dylib, export_list=exports,
                           object_file=obj)
    # Only this temporary test dylib changes platform; the shipped build stays iOS.
    data = bytearray(dylib.read_bytes())
    offset = 32
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x32:
            struct.pack_into("<III", data, offset + 8, 6, 0x000B0000, 0x000E0000)
            break
        offset += size
    else:
        raise AssertionError("test dylib has no LC_BUILD_VERSION")
    dylib.write_bytes(data)
    subprocess.run(["codesign", "-f", "-s", "-", str(dylib)], check=True)
    loader = directory / "loader.c"
    loader.write_text('''#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "%s\\n", dlerror()); return 2; }
    int (*probe)(int) = (int (*)(int))dlsym(lib, "npa_probe_extradata");
    if (!probe) return 2;
    return probe(atoi(argv[2]));
}
''')
    executable = directory / "loader"
    host_sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"],
                                       text=True).strip()
    subprocess.run([str(compiler), "-target", "arm64-apple-ios14.0-macabi",
                    "-isysroot", host_sdk,
                    str(loader), "-o", str(executable)], check=True)
    return executable, dylib


@pytest.mark.parametrize("mode", range(5), ids=[
    "subtitle", "context-to-params", "params-to-context", "flush", "close",
])
def test_extradata_stays_owned_after_repeated_crossings(lifetime_probe, mode):
    executable, dylib = lifetime_probe
    result = subprocess.run([str(executable), str(dylib), str(mode)],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("mode", range(5, 11), ids=[
    "context-extra", "params-extra", "av1-record", "av1-obus",
    "demux-extra", "demux-bufless-packet",
])
def test_copied_buffers_have_zero_padding(lifetime_probe, mode):
    executable, dylib = lifetime_probe
    result = subprocess.run([str(executable), str(dylib), str(mode)],
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
