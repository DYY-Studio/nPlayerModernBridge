import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_highbit_copy_preserves_plane_rows_and_padding(tmp_path):
    compiler = shutil.which("clang")
    if compiler is None:
        pytest.skip("Apple clang is required for the P010 copy test")
    executable = tmp_path / "highbit-copy-test"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(ROOT / "tests/highbit_copy_test.c"),
            str(ROOT / "bridge/npa_highbit_copy.c"),
            "-o",
            str(executable),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(executable)], check=True)
