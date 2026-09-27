"""Pin the 4.4.8 output-side unit to its frozen redirect sets.

The unit carries three faces on one 4.4.8 dylib: the HLS session's transmux and
transcode path (which includes that session's own demux and decode calls), the
SPDIF / IEC 61937 mux, and the poster path's MJPEG encoder. The sets below are
the design's frozen station table (the appendix of
docs/superpowers/specs/2026-09-27-ffmpeg-out448-unit-design.md), derived from
the app's functions and confirmed read-only against the database in
notes/ida-investigation-out448-sites.md.

Two boundaries this guard protects:

* the 16 sites of media::ios::AudioToolboxDecoder (0x100B63xxx-0x100B64xxx) are
  the *main playback* audio path, not SPDIF, so they must stay off this unit -
  the decoder factory sub_100B911F8 only picks SPDIF when the passthrough
  setting is on, and AudioToolbox otherwise;
* disjointness with the five 9.0.2 domains is compared by address only. The
  9.0.2 side spells the same API `npa_codec_avcodec_free_context` while this unit
  spells it `npa_avcodec_free_context`, so a (site, symbol) comparison misses a
  real clash - measured while building this table.

This is a data guard over the manifest, not a behaviour test; the drivers and
the device rows are the behaviour proof.
"""

import unittest
from pathlib import Path

from npabridge.manifest import load_manifest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")

# ---------------------------------------------------------------------------
# Frozen station table (generated from the spec appendix, edited by hand only
# when the spec changes).
# ---------------------------------------------------------------------------

HLS_FACE = {
    "npa_av_bsf_alloc": {0x100B9593C},
    "npa_av_bsf_free": {0x100B96CB8},
    "npa_av_bsf_get_by_name": {0x100B95930},
    "npa_av_bsf_init": {0x100B95954},
    "npa_av_bsf_receive_packet": {0x100B999E0},
    "npa_av_bsf_send_packet": {0x100B999CC},
    "npa_av_codec_get_tag": {0x100B99078},
    "npa_av_dict_copy": {0x100B98F2C, 0x100B99094},
    "npa_av_dict_free": {0x100B95114, 0x100B99140},
    "npa_av_dict_get": {0x100B95F94, 0x100B95FE0},
    "npa_av_dict_set": {0x100B950A4, 0x100B950C0, 0x100B99100, 0x100B9911C},
    "npa_av_frame_alloc": {0x100B960B8, 0x100B960E4},
    "npa_av_frame_free": {0x100B96D38, 0x100B96D40},
    "npa_av_freep": {0x100B96C90, 0x100B96D08},
    "npa_av_get_default_channel_layout": {0x100B9A330},
    "npa_av_guess_format": {0x100B98EC8},
    "npa_av_init_packet": {0x100B954EC, 0x100B99290, 0x100B99590, 0x100B99968, 0x100B99B48, 0x100B99D68},
    "npa_av_malloc": {0x100B958D4},
    "npa_av_new_packet": {0x100B99B5C, 0x100B99D7C},
    "npa_av_opt_set": {0x100B99F94, 0x100B99FCC, 0x100B99FE8},
    "npa_av_packet_copy_props": {0x100B99B68, 0x100B99D88},
    "npa_av_packet_move_ref": {0x100B99B8C, 0x100B99DAC},
    "npa_av_packet_ref": {0x100B99974},
    "npa_av_packet_rescale_ts": {0x100B99BC0, 0x100B99EA0},
    "npa_av_packet_unref": {0x100B958C0, 0x100B99358, 0x100B999D4, 0x100B99B80, 0x100B99C00, 0x100B99C08, 0x100B99DA0, 0x100B99EB4},
    "npa_av_read_frame": {0x100B954FC, 0x100B9929C},
    "npa_av_rescale_q": {0x100B99408},
    "npa_av_samples_get_buffer_size": {0x100B99428, 0x100B99548},
    "npa_av_seek_frame": {0x100B98C54, 0x100B98CA4, 0x100B98CBC},
    "npa_av_write_frame": {0x100B99BF8, 0x100B99EAC, 0x100B9A65C, 0x100B9A700, 0x100B9A7D8},
    "npa_av_write_trailer": {0x100B98E84},
    "npa_avcodec_alloc_context3": {0x100B95D4C, 0x100B96088, 0x100B9A2BC},
    "npa_avcodec_close": {0x100B965A0, 0x100B96D10, 0x100B96D28, 0x100B98CD8},
    "npa_avcodec_fill_audio_frame": {0x100B99568},
    "npa_avcodec_find_decoder": {0x100B96078},
    "npa_avcodec_find_encoder": {0x100B9A2B0},
    "npa_avcodec_flush_buffers": {0x100B98CC8},
    "npa_avcodec_free_context": {0x100B95DC4, 0x100B96D18, 0x100B96D30, 0x100B98CE0, 0x100B9A36C},
    "npa_avcodec_open2": {0x100B960A4, 0x100B9A358},
    "npa_avcodec_parameters_copy": {0x100B9594C, 0x100B98FCC},
    "npa_avcodec_parameters_from_context": {0x100B98FB0},
    "npa_avcodec_parameters_to_context": {0x100B95D58, 0x100B96094},
    "npa_avcodec_receive_frame": {0x100B9934C, 0x100B995F4},
    "npa_avcodec_receive_packet": {0x100B995A8},
    "npa_avcodec_send_frame": {0x100B99578},
    "npa_avcodec_send_packet": {0x100B99334},
    "npa_avformat_alloc_context": {0x100B95010},
    "npa_avformat_alloc_output_context2": {0x100B98EE4},
    "npa_avformat_close_input": {0x100B96C70},
    "npa_avformat_find_stream_info": {0x100B95130},
    "npa_avformat_free_context": {0x100B96CE4, 0x100B98E8C},
    "npa_avformat_init_output": {0x100B99128},
    "npa_avformat_new_stream": {0x100B98F6C},
    "npa_avformat_open_input": {0x100B950F8},
    "npa_avformat_write_header": {0x100B99150},
    "npa_avio_alloc_context": {0x100B95008},
    "npa_avio_close": {0x100B9A69C, 0x100B9A818},
    "npa_avio_close_dyn_buf": {0x100B9A67C, 0x100B9A7F8},
    "npa_avio_closep": {0x100B96CDC, 0x100B9A70C},
    "npa_avio_flush": {0x100B99138, 0x100B9A668, 0x100B9A7E4},
    "npa_avio_open2": {0x100B9924C, 0x100B9A648, 0x100B9A768},
    "npa_avio_open_dyn_buf": {0x100B990DC, 0x100B991B4, 0x100B9A6A8, 0x100B9A824},
    "npa_avio_size": {0x100B98C28},
    "npa_avio_wb32": {0x100B9A780, 0x100B9A7AC},
    "npa_avio_wl32": {0x100B9A790, 0x100B9A7A0, 0x100B9A7BC, 0x100B9A7CC},
    "npa_avio_write": {0x100B9A68C, 0x100B9A808},
}

SPDIF_FACE = {
    "npa_av_freep": {0x100B303FC},
    "npa_av_init_packet": {0x100B30458},
    "npa_av_malloc": {0x100B301C4},
    "npa_av_write_frame": {0x100B304B0},
    "npa_av_write_trailer": {0x100B303DC},
    "npa_avformat_alloc_output_context2": {0x100B30174},
    "npa_avformat_free_context": {0x100B30404},
    "npa_avformat_new_stream": {0x100B30180},
    "npa_avformat_write_header": {0x100B30248},
    "npa_avio_alloc_context": {0x100B30234},
    "npa_avio_flush": {0x100B304F8},
}

MJPEG_FACE = {
    "npa_av_frame_alloc": {0x100A46A3C},
    "npa_av_frame_free": {0x100A46B88},
    "npa_av_freep": {0x100A46B80},
    "npa_av_image_alloc": {0x100A46A6C},
    "npa_av_init_packet": {0x100A46B10},
    "npa_av_packet_ref": {0x100A46B48},
    "npa_av_packet_unref": {0x100A46B50},
    "npa_avcodec_alloc_context3": {0x100A46ACC},
    "npa_avcodec_find_encoder": {0x100A46AC0},
    "npa_avcodec_free_context": {0x100A46B70},
    "npa_avcodec_open2": {0x100A46B04},
    "npa_avcodec_receive_packet": {0x100A46B30},
    "npa_avcodec_send_frame": {0x100A46B24},
}

FACES = {
    "ffmpeg-hls448": HLS_FACE,
    "ffmpeg-spdif448": SPDIF_FACE,
    "ffmpeg-mjpeg448": MJPEG_FACE,
}
EXPECTED_SITES = frozenset().union(*(set(sites) for face in FACES.values() for sites in face.values()))

# media::ios::AudioToolboxDecoder: the main playback audio path.
AUDIO_TOOLBOX_LAYER = (0x100B63000, 0x100B65000)
# The two sites that must keep running through ffmpeg-codec: the SPDIF-path
# release of the shared helper's temporary source context, and the poster path's
# neighbouring function sites.
CODEC_UNIT_SITES = {0x100B300F0}
PROBE_FACE_SITES = {0x100A469C0, 0x100A469E0, 0x100A469E8}

# Each face's entry points carry their own prefix: the payload resolves a call
# site through its unit's name and the export-set check compares the built dylib
# against the manifest, so a name shared by two domains would be declared twice
# and that check's expected list would grow past the dylib's exports.
PREFIX = {
    "ffmpeg-hls448": "npa_hls_",
    "ffmpeg-spdif448": "npa_spdif_",
    "ffmpeg-mjpeg448": "npa_mjpeg_",
}


def prefixed(domain_id, symbol):
    return PREFIX[domain_id] + symbol[len("npa_") :]
NINE_O2_DOMAINS = (
    "ffmpeg-demux",
    "libswscale",
    "libswresample",
    "ffmpeg-subdecode",
    "ffmpeg-codec",
)


def _dylib(dylib_id):
    return next(d for d in MANIFEST.dylibs if d.id == dylib_id)


class Out448ManifestTests(unittest.TestCase):
    def setUp(self):
        self.dylib = _dylib("ffmpeg-out448")
        self.domains = {}

    def _domain(self, domain_id):
        if domain_id not in self.domains:
            self.domains[domain_id] = next(d for d in self.dylib.domains if d.id == domain_id)
        return self.domains[domain_id]

    def test_dylib_carries_exactly_the_three_faces(self):
        self.assertEqual([d.id for d in self.dylib.domains], list(FACES))

    def test_each_domain_has_exactly_its_symbols(self):
        for domain_id, face in FACES.items():
            symbols = {api.symbol for api in self._domain(domain_id).apis}
            self.assertEqual(symbols, {prefixed(domain_id, s) for s in face}, domain_id)

    def test_each_symbol_redirects_exactly_its_sites(self):
        for domain_id, face in FACES.items():
            apis = {api.symbol: api for api in self._domain(domain_id).apis}
            for symbol, expected in face.items():
                self.assertEqual(set(apis[prefixed(domain_id, symbol)].call_sites), expected, symbol)

    def test_redirects_exactly_154_sites(self):
        sites = {site for domain_id in FACES for api in self._domain(domain_id).apis for site in api.call_sites}
        self.assertEqual(sites, EXPECTED_SITES)
        self.assertEqual(len(sites), 154)

    def test_the_three_faces_are_disjoint(self):
        seen = {}
        for domain_id in FACES:
            for api in self._domain(domain_id).apis:
                for site in api.call_sites:
                    self.assertNotIn(site, seen, f"{hex(site)} in {seen.get(site)} and {domain_id}")
                    seen[site] = domain_id

    def test_no_site_overlaps_the_9_0_2_domains(self):
        """Address-level, and deliberately symbolic-name blind."""

        core902 = _dylib("ffmpeg-core902")
        taken = {
            site
            for domain in core902.domains
            if domain.id in NINE_O2_DOMAINS
            for api in domain.apis
            for site in api.call_sites
        }
        self.assertEqual(EXPECTED_SITES & taken, set())

    def test_old_targets_match_the_ffmpeg_core_unit(self):
        core = _dylib("ffmpeg-core")
        core_targets = {api.symbol: api.old_target for dom in core.domains for api in dom.apis}
        for domain_id, face in FACES.items():
            apis = {api.symbol: api for api in self._domain(domain_id).apis}
            for symbol in face:
                local = prefixed(domain_id, symbol)
                core_symbol = "npa_" + symbol[len("npa_") :]
                self.assertEqual(apis[local].old_target, core_targets[core_symbol], symbol)

    def test_main_playback_audio_layer_is_untouched(self):
        start, end = AUDIO_TOOLBOX_LAYER
        swept = {site for site in EXPECTED_SITES if start <= site < end}
        self.assertEqual(swept, set())

    def test_codec_unit_and_probe_face_sites_stay_foreign(self):
        self.assertEqual(EXPECTED_SITES & CODEC_UNIT_SITES, set())
        self.assertEqual(EXPECTED_SITES & PROBE_FACE_SITES, set())

    def test_entry_points_are_declared_once_per_dylib(self):
        """One entry point per API; the export-set check compares a flat list."""

        declared = [api.symbol for domain in self.dylib.domains for api in domain.apis]
        self.assertEqual(len(declared), len(set(declared)))
        self.assertEqual(len(declared), 90)

    def test_conflicts_and_default_are_as_designed(self):
        self.assertEqual(set(self.dylib.conflicts), {"ffmpeg-core", "ffmpeg-full"})
        self.assertEqual(tuple(MANIFEST.default_dylibs), ("libass", "ffmpeg-full"))


if __name__ == "__main__":
    unittest.main()
