# Windows Patch Support Design

## Goal

Make the existing `npa-patch` flow run on native 64-bit Windows without WSL,
MSYS2 `zip`/`unzip`, or an Apple SDK. Preserve the already verified macOS and
Linux behavior and continue to consume the same decrypted nPlayer 3.13.0 IPA,
manifest, and prebuilt iOS bridge dylibs.

Windows bridge builds are out of scope. Windows is a patch host only.

## Supported Host Inputs

The Windows patch flow requires:

- Python 3.11 through 3.14 and `uv`;
- the project dependencies, including LIEF 1.0.0;
- the official Keystone 0.9.2 64-bit Windows `keystone.dll`, copied directly to
  the repository root;
- the Procursus `ldid_w64_x86_64.exe`, renamed to `ldid.exe` and available on
  `PATH`;
- the selected prebuilt iOS bridge dylibs in `--dylibs-dir`, as on the existing
  macOS/Linux path;
- a decrypted, supported IPA owned by the user.

The Keystone lookup is intentionally narrow. On Windows it resolves only
`<repository-root>/keystone.dll`; it does not recursively scan an extracted
release directory, inspect `PATH`, or guess versioned directory names. Missing
files fail with an actionable message that points to the documented copy step.

## Architecture

### Keystone host library

`npabridge.toolchain` adds `win32` to the explicit host-library mapping with
the filename `keystone.dll`. The existing `ctypes.CDLL` binding, version check,
and assembler API remain shared by all hosts.

The Windows support test must load the real official DLL and assemble a known
arm64 instruction. Unit tests also pin the platform-to-filename mapping and
retain the loud failure for unknown hosts.

`tools/keystone.py` remains the source-build helper used by the established
macOS/Linux development routes. Windows patch users consume the official
prebuilt DLL instead of building Keystone from source. Its import and status
paths must no longer reject Windows merely because `sys.platform == "win32"`.

### IPA assembly

`npabridge.package` replaces the external `unzip` and `zip` subprocesses with
Python's `zipfile` module. `ldid` remains the only external program used by IPA
assembly.

The package operation will:

1. copy the patched main executable and each selected bridge dylib into a
   scratch directory;
2. pseudo-sign only those scratch copies, leaving the supplied build artifacts
   unchanged;
3. create a temporary output archive beside the requested output;
4. copy source entries in source order while preserving their filename,
   timestamp, compression type, comment, extra data, creator system, version
   fields, internal attributes, and external attributes (ZIP writer-managed
   flags such as data-descriptor state may be normalized);
5. replace the single main-executable entry with the signed main while keeping
   that entry's metadata;
6. add each selected bridge at its manifest path with a regular-file Unix mode
   of `0755` and forward-slash ZIP member names;
7. preserve the source archive comment;
8. inspect and fully verify the temporary IPA before atomically publishing it.

The writer rejects an input with zero or multiple main entries, an existing
selected bridge member, or a non-directory `Payload/nPlayer.app/Frameworks`
entry. Failures remove the temporary archive and the automatically created
scratch directory without publishing a partial output.

No platform-specific archive backend is introduced. macOS, Linux, and Windows
all use this one standard-library path.

### Test input discovery

Tests continue to honor `NPA_SOURCE_IPA` first. Without that variable, the test
fixture checks the repository root for `nPlayer_3.13.0.ipa` before retaining the
existing worktree-parent fallback. The IPA and locally supplied host/release
artifacts remain untracked test inputs and must not be committed.

## Windows Documentation

README requirements and quick-start sections will include a PowerShell flow
that:

1. installs Python and `uv`;
2. runs `uv sync --frozen`;
3. downloads and extracts the official Keystone 0.9.2 Windows release, then
   copies `keystone.dll` to the repository root;
4. downloads the Procursus 64-bit Windows ldid release, renames the executable
   to `ldid.exe`, places it on `PATH`, and verifies it with `ldid`;
5. places the bridge dylibs in a chosen directory;
6. invokes `uv run npa-patch` with Windows paths;
7. explains that `zip`, `unzip`, WSL, MSYS2, Xcode, and an iOS SDK are not
   required for patching.

Troubleshooting will distinguish a missing root-level `keystone.dll`, a DLL
that cannot be loaded (wrong architecture or missing runtime dependency), and
a missing `ldid.exe`.

## Verification

Automated verification covers:

- the Windows Keystone filename and unknown-host rejection;
- loading the real official Windows DLL and assembling arm64 `nop` to
  `1f2003d5` in the local acceptance run;
- packaging with `zip` and `unzip` hidden or absent from `PATH`;
- preservation of source entry contents and relevant `ZipInfo` metadata for
  every non-replaced member;
- preservation of the archive comment;
- executable permissions on the replacement main and added dylibs;
- rejection of duplicate managed members and a file occupying `Frameworks`;
- cleanup after both success and failure;
- the complete existing test suite on supported hosts;
- Windows end-to-end patches for the default `libass + ffmpeg-full` selection
  and the split `libass + ffmpeg-core902 + ffmpeg-out448` selection;
- existing post-package Mach-O, signature, manifest, and payload verification.

The final Windows artifact must also be installed through the project's normal
sideloading route on a real device or equivalent user environment. Automated
archive and Mach-O checks do not replace that installation acceptance test.

## Compatibility and Non-goals

- Existing CLI arguments, output naming, JSON output, manifest format, payload
  bytes, and dylib selection rules do not change.
- macOS continues to use `libkeystone.dylib`; Linux continues to use
  `libkeystone.so`.
- Building bridge dylibs, dependency closures, or iOS smoke applications on
  Windows is not supported by this work.
- Supporting 32-bit Windows, ARM64 Windows, Keystone versions other than 0.9,
  alternative signers, or automatic dependency downloads is out of scope.
