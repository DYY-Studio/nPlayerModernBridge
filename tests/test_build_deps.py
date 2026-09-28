import tempfile
import unittest
from pathlib import Path

from deps import build_deps


class FFmpegConfigurationTests(unittest.TestCase):
    def test_removes_checkout_path_from_embedded_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "checkout"
            config = Path(directory) / "config.h"
            config.write_text(
                '#define FFMPEG_CONFIGURATION "--prefix='
                f'{root}/build -ffile-prefix-map={root}=."\n'
                f'#define FFMPEG_DATADIR "{root}/share/ffmpeg"\n',
                encoding="utf-8",
            )

            build_deps.sanitize_ffmpeg_configuration(config, root)

            self.assertEqual(
                config.read_text(encoding="utf-8"),
                '#define FFMPEG_CONFIGURATION '
                '"--prefix=./build -ffile-prefix-map=.=."\n'
                f'#define FFMPEG_DATADIR "{root}/share/ffmpeg"\n',
            )


if __name__ == "__main__":
    unittest.main()
