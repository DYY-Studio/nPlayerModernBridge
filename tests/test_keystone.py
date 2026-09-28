import tempfile
import unittest
from pathlib import Path

from tools.keystone import _built_library


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

            self.assertEqual(_built_library(build_dir, ".dylib"), unversioned)


if __name__ == "__main__":
    unittest.main()
