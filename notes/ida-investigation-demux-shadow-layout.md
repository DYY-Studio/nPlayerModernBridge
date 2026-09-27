# Demux shadow: cross-major struct-layout drift

Found by the end-of-branch review and verified against both header trees.

## AVChapter - fixed

The legacy and modern `AVChapter` differ because the modern `id` is `int64_t`:

| field | 4.4.8 | 9.0.2 |
|---|---|---|
| `id` | `int` @0x0 | `int64_t` @0x0 |
| `time_base` | @0x4 | @0x8 |
| `start` / `end` / `metadata` | 0x10 / 0x18 / 0x20 | 0x10 / 0x18 / 0x20 |
| `sizeof` | 0x28 | 0x28 |

`modern_to_shadow` used to alias the modern `chapters` array, so the app read
the modern `id` low word as `id` and its high word as `time_base.num` - chapter
timings were garbage (start/end/metadata happened to line up). The shim now
builds a legacy-shaped `AVChapter[]` in `shadow_rebuild_chapters` and frees it
with the shadow.

## AVIOContext - drift confirmed, but unreachable

The shim returns the **modern** `AVIOContext` from `avio_alloc_context`, so only
the front fields sit at the legacy offsets. Fields past them moved:

| field | 4.4.8 | 9.0.2 |
|---|---|---|
| `buffer` / `buf_ptr` / `buf_end` / `opaque` / `pos` / `eof_reached` | same | same |
| `write_flag` | 0x54 | 0x58 |
| `error` | 0x78 | 0x54 |
| `max_packet_size` | 0x58 | 0x5c |
| `direct` | 0xa0 | 0x94 |
| `sizeof` | 0x108 | 0xd0 |

There is no way to mirror both layouts in one object. It turned out not to
matter: the app reads exactly one AVIOContext field on either class-A path -
`buffer` at `+8`, only to `free()` it - which is the same offset in both majors,
and it is NULL anyway because the app builds the context with a zero-size buffer
(`notes/ida-investigation-demux-custom-io.md`). `error`, `write_flag`, `direct`
and `eof_reached` are never read. The drift is therefore unreachable here; the
shim asserts the front fields only to keep it that way.

## Residual risks (not fixed)

- `ctx->metadata` / `st->metadata` are aliased to modern dictionaries. Layout is
  identical and no class-A site frees them, but a future site that does would
  double-free at the modern close.
- `side_data` / `side_data_elems` are dropped on the packet translation
  (plan-acknowledged); `coded_side_data` on the modern codecpar (HDR mastering,
  frame cropping) has no 4.4.x equivalent and is not surfaced.
- `initial_padding` / `trailing_padding` / `seek_preroll` are not mirrored
  (modern 0xa4/0xa8/0xac, legacy 0x80/0x84/0x88); minor audio priming fidelity.
- The registry mutex only guards list traversal; `shadow_lookup` returns the
  shadow after unlocking and `shadow_rebuild_streams` mutates `streams` without
  it. In practice each context is demuxed on one thread, but the lock does not
  buy what it looks like it buys.
