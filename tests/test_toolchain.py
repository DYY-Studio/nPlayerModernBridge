import unittest

from npabridge import toolchain


class DefaultLibraryTests(unittest.TestCase):
    def test_supported_hosts_use_their_own_shared_library_name(self):
        self.assertEqual(toolchain.default_library("darwin").name, "libkeystone.dylib")
        self.assertEqual(toolchain.default_library("linux").name, "libkeystone.so")
        self.assertEqual(toolchain.default_library("win32").name, "keystone.dll")

    def test_unsupported_host_fails_loudly(self):
        with self.assertRaises(RuntimeError):
            toolchain.default_library("freebsd")


if __name__ == "__main__":
    unittest.main()
