import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import keystone


class BuiltLibraryTests(unittest.TestCase):
    def test_accepts_cmake_unversioned_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            library_dir = build_dir / "llvm" / "lib"
            library_dir.mkdir(parents=True)
            versioned = library_dir / "libkeystone.0.dylib"
            versioned.write_bytes(b"keystone")
            unversioned = library_dir / "libkeystone.dylib"
            unversioned.symlink_to(versioned.name)

            self.assertEqual(keystone._built_library(build_dir, ".dylib"), unversioned)


class WindowsBootstrapTests(unittest.TestCase):
    def test_reuses_a_prebuilt_root_dll_without_building(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "keystone.dll"
            output.write_bytes(b"prebuilt")
            with (
                mock.patch.object(keystone, "OUTPUT", output),
                mock.patch.object(keystone.sys, "platform", "win32"),
                mock.patch.object(
                    keystone,
                    "extract_source",
                    side_effect=AssertionError("downloaded sources"),
                ),
            ):
                self.assertEqual(keystone.main([]), 0)

    def test_missing_root_dll_refuses_a_windows_source_build(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "keystone.dll"
            with (
                mock.patch.object(keystone, "OUTPUT", output),
                mock.patch.object(keystone.sys, "platform", "win32"),
                mock.patch.object(keystone, "extract_source") as extract_source,
                mock.patch.object(keystone, "build") as build,
            ):
                with self.assertRaisesRegex(
                    RuntimeError,
                    r"official Keystone 0\.9\.2.*repository root",
                ):
                    keystone.main([])
            extract_source.assert_not_called()
            build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
