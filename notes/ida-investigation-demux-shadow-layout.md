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

## AVIOContext - confirmed drift, unverified app impact

The shim returns the **modern** `AVIOContext` from `avio_alloc_context`. The
front fields the app is expected to read are identical, but later fields moved:

| field | 4.4.8 | 9.0.2 |
|---|---|---|
| `buffer` / `buf_ptr` / `buf_end` / `opaque` / `pos` / `eof_reached` | same | same |
| `write_flag` | 0x54 | 0x58 |
| `error` | 0x78 | 0x54 |
| `max_packet_size` | 0x58 | 0x5c |
| `direct` | 0xa0 | 0x94 |
| `sizeof` | 0x108 | 0xd0 |

A legacy read of `pb->error` returns the modern `update_checksum` low bits and
`pb->write_flag` returns the modern `error`; there is no way to mirror both
layouts in one object, so the shim only asserts the front fields. The custom-IO
path (`[Reference]`/mmsh) is the one that exercises it and is still unverified;
if it misbehaves, this is the first suspect.

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
