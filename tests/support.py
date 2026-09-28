"""Shared test inputs.

Tests that need a dev-only input skip when it is absent, so the public
suite stays runnable without an nPlayer IPA on the machine.
"""

import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# `patch.py` resolves its source, so the tests must do the same: inside a
# git worktree the shared IPA is a symlink at the worktree parent and the
# patched output lands next to the real file.
_override = os.environ.get("NPA_SOURCE_IPA")
_root_ipa = ROOT / "nPlayer_3.13.0.ipa"
SOURCE_IPA = Path(
    _override
    or (_root_ipa if _root_ipa.is_file() else ROOT.parent / "nPlayer_3.13.0.ipa")
).resolve()
