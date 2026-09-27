"""Pin the demux face to the frozen redirect set.

The unit is only sound while it redirects exactly the app's libavformat demux
call sites and nothing else - the playback demuxer's and the external-subtitle
loader's: a site that belongs to class B/C or the mux face would be pulled onto
the shadow translation, and a missing site would leave a 4.4.5 call inside a
unit the app expects to be all-modern (the external-subtitle seek was exactly
such a missing site). This is a data guard over the manifest, not a behaviour
test; the device matrix is the behaviour proof.
"""

import unittest
from pathlib import Path

from npabridge.manifest import load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")

DEMUX_FACE = {
    "npa_demux_avformat_alloc_context": {
        0x100A3E390, 0x100AED780, 0x100AEF684, 0x100AB4DF4,
    },
    "npa_demux_avformat_open_input": {
        0x100A3E900, 0x100AED878, 0x100AEF778, 0x100AB451C, 0x100AB4E1C,
    },
    "npa_demux_avformat_find_stream_info": {
        0x100A3E910, 0x100AED8E8, 0x100AEF7A8, 0x100AB4E2C,
    },
    "npa_demux_av_read_frame": {
        0x100A46964, 0x100AEE510, 0x100AEEC60, 0x100AEFE28, 0x100AB4934, 0x100AB4FE0,
    },
    "npa_demux_av_seek_frame": {0x100A46944, 0x100AEFC4C, 0x100AEFCB4, 0x100AEFCCC},
    "npa_demux_avformat_seek_file": {0x100AB4F7C},
    "npa_demux_avformat_close_input": {0x100A415B4, 0x100AEFA04, 0x100AB4470},
    "npa_demux_avformat_free_context": {0x100A415D4},
    "npa_demux_avio_alloc_context": {0x100A3E388, 0x100AED778, 0x100AB4DCC},
    "npa_demux_av_find_input_format": {0x100AB4D74},
    "npa_demux_av_probe_input_buffer": {0x100AB4DEC},
    "npa_demux_avio_read": {0x100AB4B00},
    "npa_demux_avio_seek": {0x100AB4B14},
    "npa_demux_avio_size": {0x100A46874, 0x100AEFC18},
    "npa_demux_av_index_search_timestamp": {0x100AECEE0},
}
EXPECTED_SITES = frozenset().union(*DEMUX_FACE.values())


def _dylib(dylib_id):
    return next(d for d in MANIFEST.dylibs if d.id == dylib_id)


class DemuxManifestTests(unittest.TestCase):
    def setUp(self):
        self.dylib = _dylib("ffmpeg-core902")
        self.domain = next(d for d in self.dylib.domains if d.id == "ffmpeg-demux")
        self.apis = {api.symbol: api for api in self.domain.apis}

    def test_has_exactly_the_demux_face_symbols(self):
        self.assertEqual(set(self.apis), set(DEMUX_FACE))

    def test_each_symbol_redirects_exactly_its_sites(self):
        for symbol, expected in DEMUX_FACE.items():
            self.assertEqual(set(self.apis[symbol].call_sites), expected, symbol)

    def test_redirects_exactly_38_sites(self):
        sites = {site for api in self.domain.apis for site in api.call_sites}
        self.assertEqual(sites, EXPECTED_SITES)
        self.assertEqual(len(sites), 38)

    def test_overlapping_units_are_mutually_exclusive(self):
        """A call site may be shared only by dylibs a selection cannot combine.

        The demux domain deliberately restates the 38 demux-face sites the 4.4.8
        core domain also carries, and the tool refuses to select both. Any other
        overlap would let the patcher write two redirects into one call site, so
        every sharing pair has to declare the conflict.
        """

        sites = {
            dylib.id: {
                site
                for domain in dylib.domains
                for api in domain.apis
                for site in api.call_sites
            }
            for dylib in MANIFEST.dylibs
        }
        for dylib in MANIFEST.dylibs:
            for other in MANIFEST.dylibs:
                if other.id <= dylib.id:
                    continue
                if sites[dylib.id] & sites[other.id]:
                    self.assertTrue(
                        other.id in dylib.conflicts or dylib.id in other.conflicts,
                        f"{dylib.id} and {other.id} share call sites "
                        "without declaring a conflict",
                    )
        self.assertEqual(
            set(self.dylib.conflicts), {"ffmpeg", "ffmpeg-core", "ffmpeg-full"}
        )
        # the sites this unit takes over from the 4.4.8 core's demux face, except
        # 0x100AB4F7C: the core domain never bound avformat_seek_file, so the
        # 4.4.8 alternatives still reach that call through the app's own libavformat
        self.assertLessEqual(EXPECTED_SITES - {0x100AB4F7C}, sites["ffmpeg-core"])
        self.assertNotIn(0x100AB4F7C, sites["ffmpeg-core"])

    def test_old_targets_match_the_ffmpeg_core_unit(self):
        core = _dylib("ffmpeg-core")
        core_targets = {
            api.symbol: api.old_target
            for dom in core.domains
            for api in dom.apis
        }
        for symbol, api in self.apis.items():
            if symbol == "npa_demux_avformat_seek_file":
                continue
            core_symbol = "npa_" + symbol[len("npa_demux_") :]
            self.assertEqual(api.old_target, core_targets[core_symbol], symbol)
        # the 4.4.8 core unit never bound this call; the address is the app's own
        # avformat_seek_file, confirmed from the tail branch at 0x100AB4F7C in
        # notes/ida-investigation-subtitle-demux-face.md (Addendum A)
        self.assertEqual(self.apis["npa_demux_avformat_seek_file"].old_target, 0x100823AA8)


if __name__ == "__main__":
    unittest.main()
