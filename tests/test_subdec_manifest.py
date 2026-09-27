"""Pin the subtitle-decode face to the frozen redirect set.

The unit is sound only while it redirects exactly the app's subtitle-decode
call sites and nothing else: the decoder-wrapper class around
`sub_100A03CB0` / `sub_100A04BCC` and the external-subtitle demux cluster's
decode half around `sub_100AB4580` / `sub_100AB4F80`. A site belonging to the
encoder, mux or HLS face would be pulled onto the shadow translation, and a
missing site would leave a 4.4.5 call inside a unit the app expects to be
all-modern (the decode entry `0x100A04D14` is exactly such a site). This is a
data guard over the manifest, not a behaviour test; the device matrix is the
behaviour proof.

Only the ten entries that create or consume an object this shim owns are
redirected: the codec context, the codec parameters and the `AVSubtitle` the
decode call fills. The packet and dictionary calls in the same functions stay
on 4.4.5 - the unit never hands the app a packet or a dictionary of its own,
and the demux unit already showed legacy-owned packets survive the app's own
releases (see notes/ida-investigation-p2-site-attribution.md and
notes/ida-investigation-p2-field-surface.md).
"""

import unittest
from pathlib import Path

from npabridge.manifest import load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")

SUBDEC_FACE = {
    "npa_subdec_avcodec_alloc_context3": {
        0x100A03BE8, 0x100A03CF8, 0x100A03D68, 0x100AB4740,
    },
    "npa_subdec_avcodec_free_context": {
        0x100A03B28, 0x100A03E38, 0x100AB4884,
    },
    "npa_subdec_avcodec_find_decoder": {0x100A03D8C},
    "npa_subdec_avcodec_open2": {0x100A03DC8},
    "npa_subdec_avcodec_parameters_alloc": {0x100A03D98},
    "npa_subdec_avcodec_parameters_free": {0x100A03DB8},
    "npa_subdec_avcodec_parameters_from_context": {0x100A03DA4},
    "npa_subdec_avcodec_parameters_to_context": {0x100A03DB0, 0x100AB474C},
    "npa_subdec_avcodec_decode_subtitle2": {0x100A04D14},
    "npa_subdec_avsubtitle_free": {0x100A05788},
}
EXPECTED_SITES = frozenset().union(*SUBDEC_FACE.values())

# The poster path's MJPEG encoder, sub_100A469FC (size 0x1AC). It shares a caller
# with the thumbnail decoder, so a face drawn by address range would sweep it in.
ENCODER_FUNCTION = (0x100A469FC, 0x100A46BA8)


def _dylib(dylib_id):
    return next(d for d in MANIFEST.dylibs if d.id == dylib_id)


class SubdecodeManifestTests(unittest.TestCase):
    def setUp(self):
        self.dylib = _dylib("ffmpeg-core902")
        self.domain = next(d for d in self.dylib.domains if d.id == "ffmpeg-subdecode")
        self.apis = {api.symbol: api for api in self.domain.apis}

    def test_has_exactly_the_subdecode_symbols(self):
        self.assertEqual(set(self.apis), set(SUBDEC_FACE))

    def test_each_symbol_redirects_exactly_its_sites(self):
        for symbol, expected in SUBDEC_FACE.items():
            self.assertEqual(set(self.apis[symbol].call_sites), expected, symbol)

    def test_redirects_exactly_16_sites(self):
        sites = {site for api in self.domain.apis for site in api.call_sites}
        self.assertEqual(sites, EXPECTED_SITES)
        self.assertEqual(len(sites), 16)

    def test_no_site_is_already_claimed_in_this_dylib(self):
        """A site may be redirected once; the dylib carries these units together."""

        others = {
            domain.id: {
                site for api in domain.apis for site in api.call_sites
            }
            for domain in self.dylib.domains
            if domain.id != "ffmpeg-subdecode"
        }
        for domain_id, sites in others.items():
            self.assertEqual(EXPECTED_SITES & sites, set(), domain_id)

    def test_old_targets_match_the_ffmpeg_core_unit(self):
        core = _dylib("ffmpeg-core")
        core_targets = {
            api.symbol: api.old_target for dom in core.domains for api in dom.apis
        }
        for symbol, api in self.apis.items():
            core_symbol = "npa_" + symbol[len("npa_subdec_") :]
            self.assertEqual(api.old_target, core_targets[core_symbol], symbol)

    def test_excluded_functions_are_untouched(self):
        """The encoder that lives in the poster path stays on its own face."""

        start, end = ENCODER_FUNCTION
        swept = {site for site in EXPECTED_SITES if start <= site < end}
        self.assertEqual(swept, set())

        core = _dylib("ffmpeg-core")
        encoding = {
            api.symbol
            for dom in core.domains
            for api in dom.apis
            if api.symbol.endswith(("_find_encoder", "_send_frame", "_receive_packet"))
            or "_bsf_" in api.symbol
        }
        self.assertTrue(encoding, "expected the core unit to carry an encoding face")
        for symbol in self.apis:
            self.assertNotIn(symbol[len("npa_subdec_") :], {s[len("npa_") :] for s in encoding})


if __name__ == "__main__":
    unittest.main()
