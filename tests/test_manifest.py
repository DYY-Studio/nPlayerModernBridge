import hashlib
import unittest
from pathlib import Path
from zipfile import ZipFile

from npabridge.manifest import branch_opcode, encode_branch, load_manifest

from support import SOURCE_IPA


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPOSITORY_ROOT / "manifests" / "nplayer-3.13.0.json"
MAIN_MEMBER = "Payload/nPlayer.app/nPlayer"
EXPECTED_API_SYMBOLS = frozenset(
    {
        "npa_ass_library_init",
        "npa_ass_set_extract_fonts",
        "npa_ass_set_message_cb",
        "npa_ass_renderer_init",
        "npa_ass_set_frame_size",
        "npa_ass_set_fonts_dir",
        "npa_ass_new_track",
        "npa_ass_process_codec_private",
        "npa_ass_process_data",
        "npa_ass_free_track",
        "npa_ass_flush_events",
        "npa_ass_render_frame",
        "npa_ass_renderer_done",
        "npa_ass_library_done",
        "npa_ass_set_fonts",
    }
)
EXPECTED_CALL_SITES = frozenset(
    {
        0x100A03F50,
        0x100A03FA0,
        0x100A03FB4,
        0x100A03FBC,
        0x100A03FC8,
        0x100A04680,
        0x100A047E8,
        0x100A04800,
        0x100A0482C,
        0x100A0529C,
        0x100A03B40,
        0x100A062F8,
        0x100A06408,
        0x100A035EC,
        0x100A035F4,
        0x100A0394C,
    }
)


def _apis(unit):
    return {api.symbol: api for api in unit.apis}


class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE_IPA.is_file():
            raise unittest.SkipTest("source IPA is not present")
        cls.manifest = load_manifest(MANIFEST_PATH)
        cls.units = cls.manifest.units()
        cls.libass = cls.manifest.dylib("libass")
        cls.unit = cls.manifest.units(("libass",))[0]
        with ZipFile(SOURCE_IPA) as archive:
            cls.main_bytes = archive.read(MAIN_MEMBER)

    def test_every_unit_is_reachable_from_the_dylib(self):
        self.assertEqual(
            [unit.id for unit in self.units],
            [
                "libass/libass",
                "ffmpeg-full/ffmpeg-core",
                "ffmpeg-full/libswscale",
                "ffmpeg-full/libswresample",
            ],
        )
        self.assertEqual(self.unit.dylib_id, "libass")
        self.assertEqual(self.unit.domain_id, "libass")
        for dylib_id in dict.fromkeys(unit.dylib_id for unit in self.units):
            dylib = self.manifest.dylib(dylib_id)
            selected = [unit for unit in self.units if unit.dylib_id == dylib_id]
            with self.subTest(dylib=dylib_id):
                self.assertEqual(
                    [domain.id for domain in dylib.domains],
                    [unit.domain_id for unit in selected],
                )
                for unit in selected:
                    self.assertEqual(unit.basename, dylib.basename)

    def test_ffmpeg_units_split_the_legacy_scaler_and_resampler(self):
        swscale, swresample = (
            self.manifest.units(("ffmpeg",))[0],
            self.manifest.units(("ffmpeg",))[1],
        )
        self.assertEqual((swscale.symbol_count, swscale.call_site_count), (5, 13))
        self.assertEqual((swresample.symbol_count, swresample.call_site_count), (6, 7))
        self.assertEqual(
            {api.symbol for api in swscale.apis},
            {
                "npa_sws_alloc_context",
                "npa_sws_getContext",
                "npa_sws_getCachedContext",
                "npa_sws_scale",
                "npa_sws_freeContext",
            },
        )
        self.assertEqual(
            {api.symbol for api in swresample.apis},
            {
                "npa_swr_alloc",
                "npa_swr_alloc_set_opts",
                "npa_swr_set_matrix",
                "npa_swr_init",
                "npa_swr_convert",
                "npa_swr_free",
            },
        )

    def test_ffmpeg_alternatives_conflict(self):
        for combination in (
            ("ffmpeg-full", "ffmpeg-core"),
            ("libass", "ffmpeg-full", "ffmpeg"),
        ):
            with self.subTest(combination=combination):
                with self.assertRaises(ValueError) as caught:
                    self.manifest.units(combination)
                self.assertIn("conflicting dylib selection", str(caught.exception))

    def test_the_split_alternative_is_still_selectable(self):
        self.assertEqual(
            [
                unit.id
                for unit in self.manifest.units(("libass", "ffmpeg-core", "ffmpeg"))
            ],
            [
                "libass/libass",
                "ffmpeg/libswscale",
                "ffmpeg/libswresample",
                "ffmpeg-core/ffmpeg-core",
            ],
        )

    def test_shared_domains_are_declared_once(self):
        """A registry lookup must hand every dylib the same domain object."""

        full = self.manifest.dylib("ffmpeg-full")
        self.assertEqual(
            [domain.id for domain in full.domains],
            ["ffmpeg-core", "libswscale", "libswresample"],
        )
        self.assertIs(
            full.domains[0], self.manifest.dylib("ffmpeg-core").domains[0]
        )
        for shared, util in zip(full.domains[1:], self.manifest.dylib("ffmpeg").domains):
            self.assertIs(shared, util)

    def test_ffmpeg_full_covers_the_whole_4_4_8_surface(self):
        """The one dylib carries the core, the scaler and the resampler.

        The domain identity is pinned separately; what matters here is that the
        full dylib's three units are the core, the scaler and the resampler at
        their frozen sizes, in that order.
        """

        self.assertEqual(
            [
                (unit.domain_id, unit.symbol_count, unit.call_site_count)
                for unit in self.manifest.units(("ffmpeg-full",))
            ],
            [
                ("ffmpeg-core", 99, 449),
                ("libswscale", 5, 13),
                ("libswresample", 6, 7),
            ],
        )

    def test_unit_ids_are_globally_unique(self):
        ids = [unit.id for unit in self.units]
        self.assertEqual(len(ids), len(set(ids)))

    def test_no_call_site_is_shared_between_units(self):
        owners = {}
        for unit in self.units:
            for api in unit.apis:
                for site in api.call_sites:
                    with self.subTest(site=site):
                        self.assertNotIn(site, owners)
                        owners[site] = unit.id

    def test_domain_has_fifteen_unique_apis(self):
        apis = _apis(self.unit)
        self.assertEqual(len(apis), 15)
        self.assertEqual(len({api.symbol for api in self.unit.apis}), 15)

    def test_domain_contains_exact_api_symbol_set(self):
        symbols = {api.symbol for api in self.unit.apis}
        self.assertEqual(symbols, EXPECTED_API_SYMBOLS)

    def test_domain_has_sixteen_callsites(self):
        sites = [site for api in self.unit.apis for site in api.call_sites]
        self.assertEqual(len(sites), 16)

    def test_domain_contains_exact_unique_callsite_set(self):
        sites = [site for api in self.unit.apis for site in api.call_sites]
        self.assertEqual(len(sites), len(set(sites)))
        self.assertEqual(set(sites), EXPECTED_CALL_SITES)

    def test_process_data_has_two_callsites(self):
        api = _apis(self.unit)["npa_ass_process_data"]
        self.assertEqual(api.call_sites, (0x100A0482C, 0x100A0529C))

    def test_free_track_has_expected_callsite(self):
        api = _apis(self.unit)["npa_ass_free_track"]
        self.assertEqual(api.call_sites, (0x100A03B40,))
        self.assertEqual(api.old_target, 0x100C0A2F4)

    def test_symbol_spellings_are_distinct(self):
        api = _apis(self.unit)["npa_ass_library_init"]
        self.assertEqual(api.dlsym_name, "npa_ass_library_init")
        self.assertEqual(api.macho_name, "_npa_ass_library_init")

    def test_unit_reports_its_counts(self):
        self.assertEqual(self.unit.symbol_count, 15)
        self.assertEqual(self.unit.call_site_count, 16)

    def test_binary_baseline_metadata(self):
        self.assertEqual(self.manifest.imagebase, 0x100000000)
        self.assertEqual(
            self.manifest.main_sha256,
            "28e4a62ca87642338deeedbaf144bb8e4b3a801963abcdb59434aae88369b2b8",
        )
        self.assertEqual(self.manifest.dlsym_stub, 0x1011362CC)
        self.assertEqual(self.manifest.dladdr_stub, 0x10113629C)

    def test_libass_dylib_metadata(self):
        self.assertEqual(self.libass.library_version, "0.17.5")
        self.assertEqual(self.libass.basename, "LibASSBridge.dylib")
        self.assertEqual(
            self.libass.path,
            "@executable_path/Frameworks/LibASSBridge.dylib",
        )
        self.assertEqual(self.libass.install_name, "@rpath/LibASSBridge.dylib")
        self.assertEqual(
            [(item.site, item.expected, item.replacement) for item in self.libass.extra_sites],
            [(0x100A0392C, 0x35000148, 0xD503201F), (0x100ACBC14, 0x37000080, 0xD503201F)],
        )

    def test_extra_sites_are_selected_by_dylib(self):
        self.assertEqual(
            self.manifest.extra_sites(("libass",)), self.libass.extra_sites
        )
        self.assertEqual(self.manifest.extra_sites(("ffmpeg",)), ())
        self.assertEqual(self.manifest.extra_sites(()), ())

    def test_callback_metadata(self):
        callback = self.libass.callback
        self.assertEqual(callback.app_callback, 0x100A033C4)
        self.assertEqual(callback.prototype, "void(int, const char *, va_list, void *)")
        self.assertEqual(callback.va_list_size, 8)
        self.assertEqual(callback.ignored_argument_register, "x3")

    def test_build_spec_points_at_the_libass_closure(self):
        build = self.libass.build
        self.assertEqual(build.source, "bridge/npa_ass_bridge.c")
        self.assertEqual(build.exports, "bridge/bridge.exports")
        self.assertEqual(build.closure, "build/deps/libass-closure.txt")
        self.assertEqual(build.include_root, "build/deps/include")
        self.assertEqual(build.lib_root, "build/deps/lib")

    def test_build_spec_points_at_the_ffmpeg_closure(self):
        build = self.manifest.dylib("ffmpeg").build
        self.assertEqual(build.source, "bridge/npa_ffmpeg_util_bridge.c")
        self.assertEqual(build.exports, "bridge/ffmpeg-util.exports")
        self.assertEqual(build.closure, "build/deps/ffmpeg-closure.txt")
        self.assertEqual(build.include_root, "build/deps/ffmpeg/include")
        self.assertEqual(build.lib_root, "build/deps/ffmpeg/lib")
        self.assertEqual(self.manifest.dylib("ffmpeg").library_version, "9.0.2")
        self.assertEqual(
            self.manifest.dylib("ffmpeg").install_name, "@rpath/LibFFmpegBridge.dylib"
        )
        self.assertIsNone(self.manifest.dylib("ffmpeg").callback)
        self.assertEqual(self.manifest.dylib("ffmpeg").extra_sites, ())

    def test_manifest_declares_the_frozen_abi(self):
        self.assertEqual(self.manifest.app_version, "3.13.0")
        self.assertEqual(self.manifest.target_abi.platform.upper(), "IOS")
        self.assertEqual(self.manifest.target_abi.dl_info_size, 32)
        self.assertEqual(
            (
                self.manifest.target_abi.dl_info_fname_offset,
                self.manifest.target_abi.dl_info_fbase_offset,
                self.manifest.target_abi.dl_info_sname_offset,
                self.manifest.target_abi.dl_info_saddr_offset,
            ),
            (0, 8, 16, 24),
        )
        self.assertEqual(self.manifest.target_abi.rtld_default_masked, (1 << 64) - 2)

    def test_ipa_member_matches_manifest_hash(self):
        digest = hashlib.sha256(self.main_bytes).hexdigest()
        self.assertEqual(digest, self.manifest.main_sha256)

    def test_every_original_bl_word_matches_ipa(self):
        for unit in self.units:
            for api in unit.apis:
                for call_site in api.call_sites:
                    with self.subTest(unit=unit.id, symbol=api.symbol, call_site=call_site):
                        file_offset = call_site - self.manifest.imagebase
                        actual = int.from_bytes(
                            self.main_bytes[file_offset : file_offset + 4], "little"
                        )
                        self.assertEqual(
                            actual,
                            encode_branch(branch_opcode(actual), call_site, api.old_target),
                        )


if __name__ == "__main__":
    unittest.main()
