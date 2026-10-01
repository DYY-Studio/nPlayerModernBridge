# nPlayer Plus manifest design

Date: 2026-10-01
Status: approved for implementation

## Goal

Support the nPlayer **Plus** build (bundle `com.newin.nplayer`, same version
3.13.0) with the existing bridge toolchain by adding a second manifest, so
`npa-patch` selects the right site table automatically.

The manifest is the only new product. The bridge dylibs, the patch flow and the
verification flow stay unchanged.

## Background

Plus and the standard build (`com.newin.nplayer.basic`) are the same app
lineage: identical ObjC class set (1054/1054), identical exported API set
(218/218), and the standard string table is a strict subset of Plus
(32,261 present, 52 extra in Plus).

But Plus is a different compilation: only ~23% of functions are byte-identical
and ~28% match after masking `B/BL/ADRP/ADR` displacements (~57% have no
structural counterpart). Function placement is reordered. Located evidence:

- Plus main pin (normalized): `9f3f873ffc95e43b5b79fa54d1adf00ec2ffbd032a35ec88f5d21fa38d66395f`
- `Payload/nPlayer.app/nPlayer`, imagebase `0x100000000`, `__TEXT,__text` at
  file offset `0x8000`.

Therefore the standard manifest's addresses cannot be transplanted verbatim;
they must be re-derived against the Plus executable and verified.

## Inputs and outputs

Inputs:

- `manifests/nplayer-3.13.0.json` (standard site table, the source of truth).
- the standard main executable (the one that matches the standard pin).
- the Plus main executable.

Output:

- candidate `manifests/nplayer-plus-3.13.0.json`.
- a report of addresses that could not be resolved automatically.

## Tool

New dev-only tool `dev/tools/map_plus.py` (LIEF + raw bytes; no disassembler
dependency). It is reproducible and testable, and never runs during patching.

### Address mapping

1. Segment both `__text` by `LC_FUNCTION_STARTS`.
2. Function fingerprint: hash of 4-byte instruction words with `B/BL`
   displacement, `ADRP/ADR` and `LDR`-literal immediates masked to zero.
3. Pair functions: exact content, then normalized fingerprint, then a unique
   32–64 byte window anywhere inside the function plus a monotonic
   address-delta check.
4. `old_target` → start of the paired Plus function.
5. `call_site` → search Plus for the site whose surrounding bytes match and
   whose `BL` targets the mapped `old_target`; require a unique match.
6. `extra_sites` / `main_sites` → map the site and require the mapped word to
   equal `expected`.
7. `dlsym_stub` / `dladdr_stub` → match the `__stubs` thunk bytes.

Anything still unresolved is listed for IDA follow-up.

### Manifest deltas

Same `dylibs`, `domains`, `target_abi` and `imagebase`. Changed:

- `main_pin_sha256` = Plus pin above.
- all `call_sites`, `old_target`, `extra_sites`, `main_sites`.
- `callback.app_callback`, `dlsym_stub`, `dladdr_stub`.

## Verification

- `npabridge.macho.preflight(plus_main, plus_manifest, units)` passes for every
  dylib selection, and each asserted site/branch matches in the Plus binary.
- Optional end-to-end: `npa-patch` on a Plus IPA passes `verify_artifact`.
- `target_abi` is re-checked against the Plus binary (same OS ABI expected).

## Testing

- Unit tests for the mapper on synthetic fixtures (masking, matching,
  monotonicity, branch checks).
- One integration test gated on the Plus binary being present, skipped
  otherwise, mirroring the existing `SOURCE_IPA` gate.

## Risks and non-goals

- `target_abi` parity must be confirmed; if an OS ABI constant differs, the
  frozen dylibs are not safe and the work stops.
- Passing preflight proves the static contract, not runtime behaviour; a device
  test is still recommended.
- Non-goal: rebuilding the bridge dylibs. Non-goal: supporting any other Plus
  build than the one whose pin matches.
