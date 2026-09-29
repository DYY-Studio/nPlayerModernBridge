# Windows Patch Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `npa-patch` run on native 64-bit Windows with a root-level `keystone.dll` and `ldid.exe`, without external `zip` or `unzip` programs.

**Architecture:** Extend the explicit Keystone host mapping for Windows while retaining the shared `ctypes` ABI. Replace filesystem extraction plus external archive tools with one standard-library ZIP rewrite that signs scratch copies, preserves source entry metadata, verifies the temporary IPA, and publishes atomically on every host.

**Tech Stack:** Python 3.11–3.14, `zipfile`, `ctypes`, LIEF 1.0.0, Keystone 0.9.2, Procursus ldid, pytest.

**Spec:** `docs/superpowers/specs/2026-09-29-windows-patch-support-design.md`

## Global Constraints

- Windows support is for patching only; bridge dylib and iOS dependency builds remain macOS-only.
- Windows loads only `<repository-root>/keystone.dll`; do not scan subdirectories or `PATH`.
- `ldid.exe` on `PATH` is the only external executable required by IPA assembly.
- IPA assembly must not call or require `zip`, `unzip`, WSL, or MSYS2.
- Existing CLI arguments, JSON output, output naming, manifests, payload bytes, and dylib-selection behavior remain unchanged.
- macOS keeps `libkeystone.dylib`; Linux keeps `libkeystone.so`.
- Never stage or commit local IPAs, bridge dylibs, Keystone binaries, generated IPAs, or probe directories.
- Each task is one tested, independently revertible commit; amend a task commit when its own verification exposes a defect instead of stacking an unexplained repair commit.

## Review Focus

- A `Frameworks` file instead of a directory must fail before publication; Task 2 pins this rejection.
- Duplicate main or selected-bridge members must fail instead of being silently normalized; Task 2 exercises both.
- Archive comments and non-target member content/metadata must survive rewriting; Task 2 compares every relevant field.
- Signing or archive-write failures must remove temporary output and automatic scratch state while leaving supplied binaries unchanged; Task 2 injects both failures.
- A missing or unloadable Windows Keystone DLL must report its root-level path and likely architecture/runtime mismatch; Task 1 covers selection and real loading.

---

### Task 1: Add the Windows Keystone Host Path

**Files:**
- Modify: `npabridge/toolchain.py:14-34`
- Modify: `tools/keystone.py:26-111`
- Modify: `tests/test_toolchain.py:1-17`
- Modify: `tests/test_keystone.py:1-23`

**Interfaces:**
- Produces: `default_library("win32") -> ROOT / "keystone.dll"`.
- Preserves: `Toolchain(library_path: str | Path | None = None)` and Keystone 0.9 ABI/version enforcement.
- Preserves: `tools.keystone.main(argv: list[str] | None = None) -> int`; on Windows it reuses a present root DLL and otherwise raises an actionable prebuilt-DLL error instead of attempting a source build.

- [ ] **Step 1: Write failing Windows mapping and bootstrap tests**

Assert that `default_library("win32").name == "keystone.dll"`, unknown hosts still fail, a present Windows DLL is reused, and a missing Windows DLL tells users to copy the official 0.9.2 file to the repository root without calling `extract_source()` or `build()`.

- [ ] **Step 2: Run focused tests and verify the hard restriction fails**

Run: `uv run --frozen pytest tests/test_toolchain.py tests/test_keystone.py -q`

Expected: FAIL because `win32` is unsupported during collection or mapping and the Windows bootstrap behavior does not exist.

- [ ] **Step 3: Implement the minimal mapping and prebuilt-only bootstrap**

Add `"win32": "keystone.dll"` to `LIBRARY_NAMES`. Keep lookup at the repository root. In `tools.keystone.main`, retain reuse behavior but reject a missing or forced Windows source build with the documented copy instruction; do not add discovery or downloading.

- [ ] **Step 4: Run focused tests**

Run: `uv run --frozen pytest tests/test_toolchain.py tests/test_keystone.py -q`

Expected: PASS.

- [ ] **Step 5: Perform real Windows DLL acceptance**

Copy the supplied official DLL to ignored `keystone.dll`, then run:

```powershell
uv run --frozen python -c "from npabridge.toolchain import Toolchain; assert Toolchain().assemble('nop', 0x100000000).hex() == '1f2003d5'"
```

Expected: exit code 0. If loading fails, improve the exception context to name the selected path and likely architecture/runtime mismatch, add its unit assertion, and rerun.

- [ ] **Step 6: Commit Keystone support**

```powershell
git add npabridge/toolchain.py tools/keystone.py tests/test_toolchain.py tests/test_keystone.py
git commit -m "feat: support the Windows Keystone host library"
```

### Task 2: Replace External IPA Archive Tools with `zipfile`

**Files:**
- Modify: `npabridge/package.py:1-185`
- Modify: `tests/test_package.py:1-77`

**Interfaces:**
- Preserves: `sign(path: Path) -> None`, using only `_tool("ldid")`.
- Produces: `_write_ipa(source_ipa: Path, output: Path, main: Path, bridges: Mapping[str, Path]) -> dict[str, Any]`, the archive-only unit that writes and inspects one IPA without signing.
- Preserves: `package_ipa(source_ipa: Path, output: Path, main: Path, bridges: Mapping[str, Path], work: Path | None = None) -> dict[str, Any]`.
- Preserves: `publish(...)`, `inspect_ipa(...)`, and `extract_for_verification(...)`.

- [ ] **Step 1: Add archive-only fixtures independent of built bridge artifacts**

Create a minimal IPA in a pytest temporary directory with one main, an unrelated file carrying non-default `ZipInfo` metadata, an archive comment, and optional `Frameworks/` metadata. Create replacement-main and bridge fixtures as ordinary files.

- [ ] **Step 2: Write failing metadata-preservation and validation tests**

Test these exact behaviors:

- preserve source order, archive comment, unrelated bytes, timestamp, compression, entry comment, extra data, creator system, version fields, and attributes;
- replace exactly one main while retaining that member's metadata;
- add bridge members with forward-slash names and Unix regular-file mode `0755`;
- reject zero/duplicate mains, an existing selected bridge, and a `Frameworks` file;
- remove partially written output after a copy/write exception.

- [ ] **Step 3: Run archive tests and verify failure**

Run: `uv run --frozen pytest tests/test_package.py -q`

Expected: FAIL because `_write_ipa` does not exist and packaging still shells out.

- [ ] **Step 4: Implement `_write_ipa` with `ZipFile`**

Validate managed-member counts before writing. Iterate source `ZipInfo` records in order, writing each original record and bytes while substituting the signed main. Add bridge records with `create_system = 3`, regular-file `0755` mode, and deflate compression. Copy the archive comment, inspect the output, and delete it on exceptions. Permit writer-managed flags such as data-descriptor state to normalize.

- [ ] **Step 5: Write failing orchestration and cleanup tests**

Mock `sign` and `_write_ipa` to prove `package_ipa` copies before signing, signs every scratch copy exactly once, never resolves `zip` or `unzip`, cleans success and failure scratch state, removes temporary IPA after failure, leaves supplied binaries unchanged, and handles paths containing spaces.

- [ ] **Step 6: Refactor `package_ipa` around signed scratch copies**

Remove `zip` and `unzip` from `TOOL_HINTS` and delete external extraction. Copy inputs into scratch, sign copies, call `_write_ipa` for the temporary output, then `os.replace` it. Preserve report keys and SHA-256 semantics based on supplied unsigned inputs.

- [ ] **Step 7: Run package and patch-flow tests**

Run: `uv run --frozen pytest tests/test_package.py tests/test_patch.py -q`

Expected: PASS, with integration skips only for genuinely absent declared artifacts.

- [ ] **Step 8: Run the full platform-independent suite**

Run: `uv run --frozen pytest -q`

Expected: no Windows, Keystone, `zip`, `unzip`, or patch-path failures. Apple-SDK/build-closure-only tests may retain established skips.

- [ ] **Step 9: Commit the archive unit**

```powershell
git add npabridge/package.py tests/test_package.py
git commit -m "refactor: package IPAs without external zip tools"
```

### Task 3: Document and Exercise the Native Windows Workflow

**Files:**
- Modify: `tests/support.py:1-18`
- Modify: `.gitignore:1-24`
- Modify: `README.md:1-188`

**Interfaces:**
- Preserves: explicit `NPA_SOURCE_IPA` as the highest-priority test input.
- Produces: default test input order `repository root -> worktree parent`.
- Produces: a PowerShell quick start requiring Python, uv, LIEF, root `keystone.dll`, ldid on `PATH`, bridge dylibs, and a decrypted IPA.

- [ ] **Step 1: Let tests discover the repository-root IPA**

Update `tests/support.py` so an environment value wins, followed by an existing root IPA and then the worktree-parent fallback. Run `uv run --frozen python -c "from support import SOURCE_IPA; print(SOURCE_IPA)"` with `PYTHONPATH=tests`; confirm it resolves to the root IPA without an environment override.

- [ ] **Step 2: Ignore local Windows acceptance assets**

Add `/keystone.dll`, `/keystone-*-win64/`, and `/Lib*Bridge.dylib` to `.gitignore`. Confirm local release inputs disappear from `git status --short` while intended edits remain visible.

- [ ] **Step 3: Update requirements, quick start, and troubleshooting**

Document exact PowerShell copy/rename/PATH steps. Remove `zip` and `unzip` from all Patch requirements. State that native Windows patching needs neither WSL nor MSYS2, while local bridge rebuilding remains macOS-only. Preserve accurate macOS/Linux instructions and CLI examples.

- [ ] **Step 4: Run default Windows acceptance with archive tools absent**

Start a process with a restricted `PATH` containing Python/uv and `ldid.exe` but no MSYS2 directory. Verify `zip` and `unzip` do not resolve, then run:

```powershell
uv run --frozen npa-patch --dylibs-dir . nPlayer_3.13.0.ipa -o build/windows-accept/default.ipa
```

Expected: success for `libass + ffmpeg-full`; app version `3.13.0`, source hash `28e4a62ca87642338deeedbaf144bb8e4b3a801963abcdb59434aae88369b2b8`, state `0`, and a nonzero check count.

- [ ] **Step 5: Run split-selection Windows acceptance**

Under the same restricted `PATH`, run:

```powershell
uv run --frozen npa-patch --dylib libass --dylib ffmpeg-core902 --dylib ffmpeg-out448 --dylibs-dir . nPlayer_3.13.0.ipa -o build/windows-accept/split.ipa
```

Expected: success; the dylib list is exactly `libass`, `ffmpeg-core902`, `ffmpeg-out448`, and post-package verification passes.

- [ ] **Step 6: Run final regression and hygiene checks**

Run:

```powershell
uv run --frozen pytest -q
git diff --check
git status --short
```

Expected: no Windows/patch-path failure, a clean diff check, and only intended tracked edits plus ignored local acceptance assets.

- [ ] **Step 7: Commit documentation and fixture support**

```powershell
git add README.md tests/support.py .gitignore
git commit -m "docs: add the native Windows patch workflow"
```

- [ ] **Step 8: Hand off manual device acceptance**

Report both generated IPA paths and hashes. Request installation of at least the default artifact through the normal sideloading route; expected behavior is successful installation, launch, and ordinary playback/subtitle behavior. Do not commit either IPA.

