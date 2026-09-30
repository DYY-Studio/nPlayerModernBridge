# Directory font lifecycle and build modes

When LibASSBridge is selected, the nPlayer 3.13.0 Font Cache guards become
NOPs through `libass.extra_sites` so each media can load its ASS/SSA font
attachments. Other selections retain the original guards. Repeated `ass_set_fonts` with unchanged upstream
libass appends directory fonts to the library's embedded font history. A later
media with the same font family can therefore select the earlier font.

The default `libass-patch` fixes this in `ass_fontselect.c`: directory buffers
belong to the embedded font provider's snapshot, are released after its faces,
and are rebuilt from the current directory. Actual inline fonts remain in the
library and retain priority. This also supports multiple renderers sharing a
library without changing another renderer's live snapshot.

The alternative `bridge-isolation` leaves libass unchanged. The Bridge gives
each track a private library and renderer, imports its directory attachments
once, then disables further directory imports for that track. External
library/renderer handles are Bridge records; returned tracks remain real
`ASS_Track` objects. The owner survives external teardown until its last track
is freed. A track switch explicitly reports `detect_change=2`, including a
return to an already rendered track, so nPlayer redraws on the next playback
frame.

## Building and switching

From the repository root:

```sh
make bootstrap deps bridge                         # default libass-patch
make bridge ASS_FONT_MODE=bridge-isolation
make verify ASS_FONT_MODE=bridge-isolation
make bridge                                       # return to libass-patch
```

To build or verify only libass:

```sh
uv run python -m npabridge.build_bridge --dylib libass --font-mode bridge-isolation
uv run python -m npabridge.build_bridge --dylib libass --font-mode bridge-isolation --verify-only
```

Each invocation rebuilds the selected libass from the SHA-verified source
archive using the existing locked Meson options. Shared dependencies come
from `make deps`. The modes have separate source/build/archive directories
under `build/deps/ass-font-modes/` and separate Bridge objects under
`build/bridge/`. Both publish `build/LibASSBridge.dylib` with the same exports
and install name; the IPA patching interface is unchanged.

`build/bridge/libass-font-mode.json` records the mode, source archive, patch,
Bridge source, linked archive hashes and final output hash. Verification
requires the selected mode and matching output. A failed rebuild removes the
previous published libass output and report. A patch failure stops the build;
maintainers explicitly select isolation if an upstream update makes the patch
impractical to maintain.

## Scope and validation

Isolation targets the observed nPlayer 3.13.0 workflow: one external renderer
per library, a new track for new media, and per-track attachment snapshots.
The same live track does not re-import changed directory contents. Its opaque
library/renderer handles must be used through the declared Bridge API.
Additional active tracks cost roughly 3.8 MiB each in the measured warm-cache
sample with 95 bundled fonts. The default patch avoids those extra contexts.

Both builds passed manifest/ABI verification, same-family A→B→A font
replacement and track-switch checks. The uncorrected control retained font A.
Prior PlayCover playback also confirmed A→B→A and distinct TRACK-1→TRACK-2→TRACK-1
selection on the next rendered playback frame. Isolation qualification
included overlapping tracks, external teardown before track teardown, and
shared/independent-owner concurrency.

The full local experiments remain outside this repository at
`../font-cache-investigation/`; production-build evidence is in
`build-options/production-validation/`. `tests/test_ass_font_mode.py` retains
only the artifact mode/hash and failed-build safety regressions. Raw captures,
App backups and probe tools are not release inputs.
