"""Pin the playback/probe decode face to the frozen redirect set.

The unit is sound only while it redirects exactly the app's software-decode call
sites and nothing else: the video decoder wrappers (`sub_100A80F14`,
`sub_100A810CC`, the open path `sub_100A81000`), the audio wrappers
(`sub_100A897D4`), the thumbnail/poster decoder (`sub_100A46774`) and, since the
2026-09-27 amendment, the call sites that share the temporary source context
built by `sub_100A8A4F8`. A site belonging to the encoder, mux or HLS face would
be pulled onto the shadow translation, and a missing site would leave a 4.4.5
call inside a unit the app expects to be all-modern (the open/close partners of
the context were exactly such missing sites).

The amendment matters: `avcodec_alloc_context3` runs inside `sub_100A8A4F8`,
which stays on 4.4.5, while its `avcodec_free_context` runs in five callers,
three of them this unit's and two in the SPDIF path. Taking the callers without
the allocation would leave the object allocated on one side and released on the
other, which the design forbids, so the helper's two sites and the three SPDIF
sites belong to this unit too. The helper's parameter chain and `av_malloc` stay
on 4.4.5, so its claim is deliberately partial. The AudioToolboxDecoder's own
builder (`sub_100B63FA4`, same class) is a different case: its parameters object
crosses into this unit through the claimed `avcodec_parameters_from_context`
site, so its allocation and release are claimed here as well.

This is a data guard over the manifest, not a behaviour test; the device matrix
is the behaviour proof. See notes/ida-investigation-p2-site-attribution.md.
"""

import unittest
from pathlib import Path

from npabridge.manifest import load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")

CODEC_FACE = {
    "npa_codec_avcodec_alloc_context3": {
        0x100A80F9C, 0x100A898D4, 0x100A467B8, 0x100A8A514,
    },
    "npa_codec_avcodec_free_context": {
        0x100A80ED8, 0x100A80F4C, 0x100A81078, 0x100A81104,
        0x100A89770, 0x100A89880, 0x100A899D0, 0x100A468EC,
        0x100A8A684, 0x100B300F0, 0x100B63F88,
    },
    "npa_codec_avcodec_find_decoder": {0x100A80F8C, 0x100A898C4, 0x100A467B0},
    "npa_codec_avcodec_open2": {0x100A81020, 0x100A89914, 0x100A467DC},
    "npa_codec_avcodec_close": {0x100A468E0},
    "npa_codec_avcodec_flush_buffers": {0x100A815AC, 0x100A89FF4},
    "npa_codec_avcodec_parameters_alloc": {0x100A80FA8, 0x100A898E0, 0x100B63FFC},
    "npa_codec_avcodec_parameters_free": {0x100A80FCC, 0x100A89904, 0x100B64020},
    "npa_codec_avcodec_parameters_from_context": {
        0x100A80FB8, 0x100A898F0, 0x100B6400C,
    },
    "npa_codec_avcodec_parameters_to_context": {
        0x100A80FC4, 0x100A898FC, 0x100A467C8,
    },
    "npa_codec_avcodec_send_packet": {
        0x100A81280, 0x100A812AC, 0x100A89BD8, 0x100A89C04, 0x100A468A8, 0x100A46998,
    },
    "npa_codec_avcodec_receive_frame": {
        0x100A812F8, 0x100A89C8C, 0x100A468B4, 0x100A469AC,
    },
}
EXPECTED_SITES = frozenset().union(*CODEC_FACE.values())

# The 2026-09-27 amendment: the temporary source context's own allocation, the
# helper's error-path release, and the SPDIF callers that release/read the same
# context. Dropping any of these silently breaks the one-owner rule.
AMENDED_SITES = frozenset({
    0x100A8A514, 0x100A8A684, 0x100B300F0, 0x100B63F88, 0x100B6400C,
    # 2026-09-27: the AudioToolboxDecoder's own temporary parameters object. Its
    # avcodec_parameters_from_context site (0x100B6400C) is claimed here (it reads
    # the source context built by sub_100A8A4F8), so the parameters that call is
    # handed - built at 0x100B63FFC and released at 0x100B64020, both inside
    # sub_100B63FA4 - have to be this unit's too. Both were on 4.4.5 and the shim
    # aborted (no shadow entry) while playing an AC3 file.
    0x100B63FFC, 0x100B64020,
})

# The poster path's MJPEG encoder, sub_100A469FC (size 0x1AC). It shares a caller
# with the thumbnail decoder, so a face drawn by address range would sweep it in.
ENCODER_FUNCTION = (0x100A469FC, 0x100A46BA8)


def _dylib(dylib_id):
    return next(d for d in MANIFEST.dylibs if d.id == dylib_id)


class CodecManifestTests(unittest.TestCase):
    def setUp(self):
        self.dylib = _dylib("ffmpeg-core902")
        self.domain = next(d for d in self.dylib.domains if d.id == "ffmpeg-codec")
        self.apis = {api.symbol: api for api in self.domain.apis}

    def test_has_exactly_the_codec_symbols(self):
        self.assertEqual(set(self.apis), set(CODEC_FACE))

    def test_each_symbol_redirects_exactly_its_sites(self):
        for symbol, expected in CODEC_FACE.items():
            self.assertEqual(set(self.apis[symbol].call_sites), expected, symbol)

    def test_redirects_exactly_46_sites(self):
        sites = {site for api in self.domain.apis for site in api.call_sites}
        self.assertEqual(sites, EXPECTED_SITES)
        self.assertEqual(len(sites), 46)

    def test_source_context_lifetime_is_claimed_whole(self):
        """The amendment has to survive edits: alloc, both releases and the read."""

        self.assertEqual(EXPECTED_SITES & AMENDED_SITES, AMENDED_SITES)
        for site in AMENDED_SITES:
            owners = {
                symbol
                for symbol, expected in CODEC_FACE.items()
                if site in expected
            }
            self.assertEqual(len(owners), 1, hex(site))

    def test_no_site_is_already_claimed_in_this_dylib(self):
        """A site may be redirected once; the dylib carries these units together."""

        others = {
            domain.id: {site for api in domain.apis for site in api.call_sites}
            for domain in self.dylib.domains
            if domain.id != "ffmpeg-codec"
        }
        for domain_id, sites in others.items():
            self.assertEqual(EXPECTED_SITES & sites, set(), domain_id)

    def test_old_targets_match_the_ffmpeg_core_unit(self):
        core = _dylib("ffmpeg-core")
        core_targets = {
            api.symbol: api.old_target for dom in core.domains for api in dom.apis
        }
        for symbol, api in self.apis.items():
            core_symbol = "npa_" + symbol[len("npa_codec_") :]
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
        decoded = {symbol[len("npa_codec_") :] for symbol in self.apis}
        self.assertEqual(decoded & {s[len("npa_") :] for s in encoding}, set())


if __name__ == "__main__":
    unittest.main()
