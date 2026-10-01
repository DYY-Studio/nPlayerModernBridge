import unittest
from pathlib import Path

from npabridge import macho
from npabridge.manifest import load_manifest, select_manifest

from support import PLUS_MAIN


ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = ROOT / "manifests"
PLUS_MANIFEST = MANIFESTS / "nplayer-plus-3.13.0.json"


@unittest.skipUnless(PLUS_MAIN.is_file(), "nPlayer Plus executable is absent")
class PlusManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_manifest(PLUS_MANIFEST)

    def test_pin_selects_the_plus_manifest(self):
        chosen = select_manifest(MANIFESTS, PLUS_MAIN)
        self.assertEqual(chosen.main_pin_sha256, self.manifest.main_pin_sha256)
        self.assertNotEqual(chosen.main_pin_sha256,
                            load_manifest(MANIFESTS / "nplayer-3.13.0.json").main_pin_sha256)

    def test_preflight_passes_for_every_selection(self):
        selections = [None] + [[dylib.id] for dylib in self.manifest.dylibs]
        for selection in selections:
            label = "default" if selection is None else ",".join(selection)
            with self.subTest(selection=label):
                macho.preflight(
                    PLUS_MAIN, self.manifest, self.manifest.units(selection)
                )


if __name__ == "__main__":
    unittest.main()
