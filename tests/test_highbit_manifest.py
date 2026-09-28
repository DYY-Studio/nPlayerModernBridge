"""Pin the standalone high-bit-depth renderer and compatible selections."""

import unittest
from pathlib import Path

from npabridge.build_bridge import load_closure
from npabridge.manifest import load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")


class HighBitManifestTests(unittest.TestCase):
    def test_renderer_is_optional_and_patches_only_its_callsite(self):
        self.assertNotIn("renderer-highbit", MANIFEST.default_dylibs)
        self.assertNotIn(
            "renderer-highbit", [unit.dylib_id for unit in MANIFEST.units()]
        )

        dylib = MANIFEST.dylib("renderer-highbit")
        self.assertEqual(dylib.basename, "LibRendererHighBitBridge.dylib")
        self.assertEqual([domain.id for domain in dylib.domains], ["renderer-highbit"])
        api = dylib.domains[0].apis[0]
        self.assertEqual(api.symbol, "npa_renderer_create_pixel_buffer")
        self.assertEqual(api.call_sites, (0x100A23314,))
        self.assertEqual(api.old_target, 0x100A22F2C)
        self.assertEqual(dylib.extra_sites, ())

    def test_renderer_can_be_selected_with_supported_ffmpeg_sets(self):
        for bridges in (
            ("libass",),
            ("libass", "ffmpeg-full"),
            ("libass", "ffmpeg-core", "ffmpeg"),
            ("libass", "ffmpeg-core902", "ffmpeg-out448"),
        ):
            with self.subTest(bridges=bridges):
                ids = (*bridges, "renderer-highbit")
                self.assertIn("renderer-highbit/renderer-highbit", {
                    unit.id for unit in MANIFEST.units(ids)
                })

    def test_renderer_closure_links_system_frameworks_without_archives(self):
        archives, link_args = load_closure("renderer-highbit")
        self.assertEqual(archives, ())
        self.assertEqual(link_args, ("-framework", "CoreVideo", "-framework", "CoreFoundation"))


if __name__ == "__main__":
    unittest.main()
