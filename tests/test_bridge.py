import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from npabridge import build_bridge, macho, verify
from npabridge.manifest import load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")
LIBASS = ROOT / "build" / "LibASSBridge.dylib"
FFMPEG = ROOT / "build" / "LibFFmpegBridge.dylib"
FFMPEG_INCLUDE = ROOT / "build" / "deps" / "ffmpeg" / "include"
FFMPEG_SOURCE = ROOT / "bridge" / "npa_ffmpeg_util_bridge.c"
ABI_PROBE = """\
#include <stdint.h>
#include "npa_ffmpeg_util_bridge.c"

/* The app's two swresample free sites pass the address of its context field
 * (FFmpeg 4.4 swr_free), and its setup site passes NULL plus the legacy int64
 * channel-layout masks. Both entry points must keep those exact prototypes. */
static void (*const probe_free)(struct SwrContext **) = npa_swr_free;
static struct SwrContext *(*const probe_opts)(
    struct SwrContext *, int64_t, int, int, int64_t, int, int, int, void *
) = npa_swr_alloc_set_opts;

int npa_abi_probe(void) { return probe_free != 0 && probe_opts != 0; }
"""
EXPECTED_LIBASS_SYMBOLS = tuple(
    api.symbol
    for domain in MANIFEST.dylib("libass").domains
    for api in domain.apis
)
EXPECTED_FFMPEG_SYMBOLS = (
    "npa_sws_alloc_context",
    "npa_sws_getContext",
    "npa_sws_getCachedContext",
    "npa_sws_scale",
    "npa_sws_freeContext",
    "npa_swr_alloc",
    "npa_swr_init",
    "npa_swr_free",
    "npa_swr_convert",
    "npa_swr_set_matrix",
    "npa_swr_alloc_set_opts",
)


class BridgeTests(unittest.TestCase):
    def test_every_dylib_declares_its_build_inputs(self):
        for dylib in MANIFEST.dylibs:
            with self.subTest(dylib=dylib.id):
                self.assertIsNotNone(dylib.build)
                exports = (ROOT / dylib.build.exports).read_text(encoding="utf-8")
                self.assertEqual(
                    set(exports.split()),
                    {
                        api.macho_name
                        for domain in dylib.domains
                        for api in domain.apis
                    },
                )
                self.assertTrue((ROOT / dylib.build.source).is_file())
                self.assertTrue((ROOT / dylib.build.closure).is_file())

    def test_exports_use_macho_underscore(self):
        if not LIBASS.is_file():
            self.skipTest("LibASSBridge.dylib is not built")
        self.assertEqual(
            set(build_bridge.nm_exports(LIBASS)),
            {f"_{name}" for name in EXPECTED_LIBASS_SYMBOLS},
        )

    def test_dlsym_names_have_no_underscore(self):
        for dylib in MANIFEST.dylibs:
            for domain in dylib.domains:
                for api in domain.apis:
                    with self.subTest(symbol=api.symbol):
                        self.assertFalse(api.symbol.startswith("_"))

    def test_ffmpeg_dylib_exports_exactly_the_expected_symbols(self):
        if not FFMPEG.is_file():
            self.skipTest("LibFFmpegBridge.dylib is not built")
        self.assertEqual(
            set(build_bridge.nm_exports(FFMPEG)),
            {f"_{name}" for name in EXPECTED_FFMPEG_SYMBOLS},
        )

    def test_ffmpeg_dylib_metadata(self):
        if not FFMPEG.is_file():
            self.skipTest("LibFFmpegBridge.dylib is not built")
        with mock.patch.object(
            macho.subprocess, "run", side_effect=AssertionError("shelled out")
        ):
            self.assertEqual(
                macho.install_name(FFMPEG), "@rpath/LibFFmpegBridge.dylib"
            )
            dependencies = macho.dependency_lines(FFMPEG)
        external = [item for item in dependencies if item != "@rpath/LibFFmpegBridge.dylib"]
        self.assertEqual(
            [
                item
                for item in external
                if not item.startswith(verify.SYSTEM_DEPENDENCY_PREFIXES)
            ],
            [],
        )
        self.assertIn("/usr/lib/libSystem.B.dylib", external)
        parsed = macho.parse(FFMPEG)
        self.assertEqual(macho.section_size(parsed, "__mod_init_func"), 0)
        self.assertEqual(
            [name for name in external if name.endswith("LibASSBridge.dylib")], []
        )
        self.assertNotIn(
            b"/Users/", FFMPEG.read_bytes()
        )
        self.assertNotIn(str(ROOT).encode(), FFMPEG.read_bytes())

    def test_metadata_helpers_match_the_built_bridge(self):
        if not LIBASS.is_file():
            self.skipTest("LibASSBridge.dylib is not built")
        with mock.patch.object(
            macho.subprocess, "run", side_effect=AssertionError("shelled out")
        ):
            self.assertEqual(macho.install_name(LIBASS), "@rpath/LibASSBridge.dylib")
            self.assertEqual(
                macho.dependency_lines(LIBASS),
                [
                    "@rpath/LibASSBridge.dylib",
                    "/usr/lib/libiconv.2.dylib",
                    "/usr/lib/libz.1.dylib",
                    "/usr/lib/libSystem.B.dylib",
                ],
            )

    def test_bridge_verifier_rejects_the_checkout_path(self):
        if not LIBASS.is_file():
            self.skipTest("LibASSBridge.dylib is not built")
        with tempfile.TemporaryDirectory() as directory:
            mutated = Path(directory) / LIBASS.name
            mutated.write_bytes(LIBASS.read_bytes() + str(ROOT / "build").encode())
            report = verify.verify_bridge(mutated, MANIFEST.dylib("libass"))

        with self.assertRaises(verify.VerificationError) as caught:
            report.require()
        self.assertIn("bridge.host_paths", caught.exception.codes)

    def test_ffmpeg_shim_keeps_the_legacy_swresample_abi(self):
        if not FFMPEG_SOURCE.is_file() or not FFMPEG_INCLUDE.is_dir():
            self.skipTest("the FFmpeg closure is not built")
        try:
            macho.sdk_path()
        except Exception as error:  # noqa: BLE001
            self.skipTest(f"iOS SDK is not available: {error}")
        probe = Path(__file__).resolve().parents[1] / "build" / "abi-probe.c"
        probe.parent.mkdir(parents=True, exist_ok=True)
        probe.write_text(ABI_PROBE, encoding="utf-8")
        result = subprocess.run(
            [
                macho.xcrun_find("clang"),
                "-fsyntax-only",
                "-target",
                "arm64-apple-ios13.0",
                "-isysroot",
                str(macho.sdk_path()),
                "-I",
                str(FFMPEG_INCLUDE),
                "-I",
                str(FFMPEG_SOURCE.parent),
                "-Wall",
                "-Werror=incompatible-pointer-types",
                "-Werror=implicit-function-declaration",
                str(probe),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_every_dylib_passes_its_artifact_checks(self):
        for dylib in MANIFEST.dylibs:
            with self.subTest(dylib=dylib.id):
                path = build_bridge.output_path(dylib)
                if not path.is_file():
                    self.skipTest(f"{dylib.basename} is not built")
                report = build_bridge.verify_dylib(dylib.id, path)
                codes = [check["code"] for check in report["checks"]]
                self.assertEqual(
                    codes,
                    [
                        "bridge.target",
                        "bridge.exports",
                        "bridge.install_name",
                        "bridge.dependencies",
                        "bridge.initializers",
                        "bridge.host_paths",
                        "bridge.callback",
                    ],
                )
                self.assertTrue(all(check["ok"] for check in report["checks"]))


if __name__ == "__main__":
    unittest.main()
