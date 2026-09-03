# TH08 PSPGL GE4 fork

This directory freezes the PSPGL variants used by the TH08 PSP-2000/3000 GE4
renderer. The game links the v3 archive by default and selects the separately
fingerprinted mixed-v1 archive only when either
`TH08_PSP_BULLET_MIXED_QUADS_FASTPATH=1` or
`TH08_PSP_ITEM_MIXED_QUADS_FASTPATH=1`. The independent indexed-copy-v1
archive is selected only by `TH08_PSP_ITEM_NATURAL_NATIVE_COPY=1`; that gate
is mutually exclusive with both mixed products. The PSPSDK-installed `libGL.a`
is not an acceptable substitute, and selecting either candidate never
overwrites v3.

## Provenance

- Upstream: <https://github.com/pspdev/pspgl.git>
- Upstream commit: `de4260adf56d06516ec46018d404ca77e0b61748`
- License: BSD-3-Clause; see `../../licenses/PSPGL-LICENSE.txt`
- Installed unmodified archive observed during the audit:
  - size: `316370` bytes
  - SHA-256: `78679105d9943626ef048546bcc48e83b82b19d4c5f181289fdcfb78e44838ea`
- Base source delta: `pspgl-th08-ge4-v1.patch`
  - SHA-256: `b3f146b3c10c13424a386441726b11311a8b483627466aec0961de803184adb1`
- Native-submit delta, applied after v1:
  `pspgl-th08-native-submit-v3.patch`
  - SHA-256: `ae70bc1a492212989a87fa1656ff002c6b6f5d8f2ade2b2efa50983dc77aaf47`
- Source-only atomic mixed-submit delta, applied after v3:
  `pspgl-th08-native-mixed-submit-v1.patch`
  - SHA-256: `26e76be666d31e9adea5660edcf67290cf747e8104260484345c596cca4e828a`
  - This optional delta is not present in the frozen archive below.
- Item natural indexed-copy delta, applied independently after v3:
  `pspgl-th08-native-indexed-copy-v1.patch`
  - SHA-256: `a045436e26fafc7212ce5ec2699d841d1a19afcd1ff4f0156de78f66404c518f`
  - Post-patch `glDrawElements.c` blob:
    `23f2c5d63d23bbaedc64ae88bf9029e337a9940f`
- Frozen archive: `libGL_th08_ge4.a`
  - size: `1679664` bytes
  - SHA-256: `3711ea969b85c839e6d1b36e7faf3ea5922e339a5f1b800ac50f25a45aa226ce`
- Reproducible mixed-submit candidate archive:
  `libGL_th08_ge4_mixed_v1.a`
  - size: `1683644` bytes
  - SHA-256: `b401e0f924ffdaffca62ac62e16f58000dd7b0b7d862675124f5289252ead530`
  - `Makefile.psp` selects this archive only for the Bullet or Item mixed-quads
    product gate; audit and every non-mixed build retain the frozen v3 archive.
- Reproducible Item natural indexed-copy candidate archive:
  `libGL_th08_ge4_indexed_copy_v1.a`
  - size: `1685392` bytes
  - SHA-256: `3d63366aa076ee44627bcd5121cdb305c6816015b192943eaa22a9a818179607`
  - Exports the v3 symbols plus the owned-copy hook and its provenance marker.
  - Selected only by the default-off `TH08_PSP_ITEM_NATURAL_NATIVE_COPY` gate.

## Rebuild

Checkout the exact upstream commit, apply `pspgl-th08-ge4-v1.patch` followed by
`pspgl-th08-native-submit-v3.patch`, then run `make clean` followed by `make -j4`
in the PSPGL source directory with the same PSPSDK toolchain.  The pinned
upstream header contains mixed line endings, so `git apply
--ignore-space-change` is required for the v1 patch at this commit; v3 applies
normally.  Copy the resulting `libGL.a` only if its size, SHA-256, exported fork
symbols, and both build markers match this record.

The archive contains this exact marker:

`pspGL TH08 GE4 fork v1 upstream de4260adf56d06516ec46018d404ca77e0b61748`

The v1 marker remains unchanged for compatibility.  The archive additionally
exports `__pspgl_th08_native_submit_marker` with this exact v3 marker:

`pspGL TH08 native submit v3 indexed-no-copy sprite-copy upstream de4260adf56d06516ec46018d404ca77e0b61748 base ge4-v1`

The common marker is defined beside the sprite-copy entry point in
`glDrawArrays.o`. Both score-popup builds and indexed Bullet/Item builds already
link that object, so the final-ELF provenance gate can require the marker
without forcing an otherwise unused native implementation into the image.

### Mixed-v1 reproducible rebuild

Start from the same upstream commit and apply the deltas strictly in this
order: v1 with `git apply --ignore-space-change`, v3 normally, then mixed-v1
normally. The expected `glDrawElements.c` Git blob is
`a0695fe85617bdb060d8251a45fe2dba109d6f28` after v3 and
`28ebed5834af2f1a435459a37654ec6723b91711` after mixed-v1.

The recorded archive was built with `psp-gcc 15.2.0`, GNU binutils 2.44, and
GNU Make 4.3. PSPGL embeds its build directory in DWARF and expands
`__DATE__`/`__TIME__`; its ordinary archive recipe also retains member
timestamps. Therefore a reproducible build fixes all three inputs:

```sh
make clean
make .deps
PSPGL_MIXED_SRC="$PWD"
SOURCE_DATE_EPOCH=1648032239 make -j4 libGL.a \
  CC="psp-gcc -std=gnu99 -ffile-prefix-map=${PSPGL_MIXED_SRC}=/usr/src/pspgl-th08-ge4-mixed-v1"
psp-ar t libGL.a > libGL.mixed-v1.members
MIXED_ARCHIVE_OUT=$(mktemp -d)
xargs psp-ar crD "$MIXED_ARCHIVE_OUT/libGL_th08_ge4_mixed_v1.a" \
  < libGL.mixed-v1.members
psp-ranlib -D "$MIXED_ARCHIVE_OUT/libGL_th08_ge4_mixed_v1.a"
```

Run the `xargs` command from the PSPGL source directory so its member names
resolve to the just-built objects. `SOURCE_DATE_EPOCH` is the upstream commit
time, the fixed prefix removes temporary checkout paths, and `crD`/`ranlib -D`
zero archive metadata. Two independently checked-out and patched source trees
produced byte-identical 1,683,644-byte archives with the SHA-256 above.

The candidate is ELF32 little-endian MIPS R3000, Allegrex/eabi32/mips2. Its
global defined-symbol set is exactly the v3 archive's 304 symbols plus one:
`__pspgl_th08_draw_native_mixed_quads` (`T`, size `0x2d8`). The v3 native
indexed/sprite symbols and both exact marker strings remain present. The build
reported 17 pre-existing GLU/GLUT prerequisite warnings caused by PSPGL's
generic archive rule; no warning originated in `glDrawElements.c` or another
patched GE4/native-submit file. The original v3 archive was re-hashed after
placement and remained exactly
`3711ea969b85c839a5f1b800ac50f25a45aa226ce`.

### Indexed-copy-v1 reproducible rebuild

Start from the same upstream commit and apply v1, v3, then
`pspgl-th08-native-indexed-copy-v1.patch`. The v3 input blob is
`a0695fe85617bdb060d8251a45fe2dba109d6f28`; the indexed-copy output blob is
`23f2c5d63d23bbaedc64ae88bf9029e337a9940f`. Use the mixed-v1 procedure above
with this fixed compiler prefix:

```sh
make clean
make .deps
PSPGL_INDEXED_COPY_SRC="$PWD"
SOURCE_DATE_EPOCH=1648032239 make -j4 libGL.a \
  CC="psp-gcc -std=gnu99 -ffile-prefix-map=${PSPGL_INDEXED_COPY_SRC}=/usr/src/pspgl-th08-ge4-indexed-copy-v1"
psp-ar t libGL.a > libGL.indexed-copy-v1.members
INDEXED_COPY_ARCHIVE_OUT=$(mktemp -d)
xargs psp-ar crD \
  "$INDEXED_COPY_ARCHIVE_OUT/libGL_th08_ge4_indexed_copy_v1.a" \
  < libGL.indexed-copy-v1.members
psp-ranlib -D \
  "$INDEXED_COPY_ARCHIVE_OUT/libGL_th08_ge4_indexed_copy_v1.a"
```

Two independent patched checkouts produced byte-identical 1,685,392-byte
archives with SHA-256
`3d63366aa076ee44627bcd5121cdb305c6816015b192943eaa22a9a818179607`.
The 17 warnings are the same pre-existing GLU/GLUT warnings documented for
mixed-v1; `glDrawElements.c` emitted none. The existing v3 and mixed archives
were re-hashed after placement and remained byte-for-byte unchanged. PSPGL is
BSD-3-Clause; the frozen notice is `../../licenses/PSPGL-LICENSE.txt` with
SHA-256 `707474f35d89b556e080aa21766fe5689fd55829b28b267bcbc0d13e1a418941`.

## TH08 native indexed-quad owned-copy contract

The independently gated post-v3 entry point is:

```c
int __pspgl_th08_draw_native_indexed_quads_copy(
    const void *vertices, unsigned vertex_bytes,
    const unsigned short *indices, unsigned index_bytes,
    unsigned quad_count);
```

It accepts 1 through `0x600` packed quads. Each quad must provide exactly four
24-byte vertices and six u16 indices; both caller ranges, all products, their
alignment, and the active PSPGL context/list are validated before allocation.
Index values are not rescanned because the Item frontend has already validated
the exact immutable `0,1,2,1,2,3` prefix at the canonical Flush boundary.

The hook allocates one `GL_STREAM_DRAW_ARB` PSPGL buffer, copies the complete
vertex range followed by the complete index range during the call, then emits
one indexed `GE_TRIANGLES` primitive and pins that one buffer to the current
display list. The caller may reuse both source ranges immediately. Allocation
or mapping failure frees any acquired buffer and returns zero before `PRIM`, so
TH08 executes its ordinary public client-array fallback exactly once. No
caller pointer survives the call, the Bullet no-copy arena is not referenced,
no raw GU list is started, and no eDRAM CPU mapping is attempted.

The archive marker is:

`pspGL TH08 native indexed-quad copy v1 upstream de4260adf56d06516ec46018d404ca77e0b61748 base native-submit-v3`

Status: **HOST/STATIC/ABI VERIFIED; PPSSPP NOT YET RUN; UNTESTED ON HARDWARE.**

## TH08 native indexed submit contract

The private exported entry point is:

```c
int __pspgl_th08_draw_native_indexed_triangles(
    const void *vertices, unsigned vertex_bytes,
    const unsigned short *indices, unsigned index_bytes,
    unsigned index_count);
```

It submits directly through PSPGL's current display list and
`__pspgl_context_render_prim`; it does not call `sceGuStart`, allocate a second
buffer, copy either range, or run the generic PSPGL vertex conversion and
min/max-index scan.  PSPGL still pins the active draw, depth, texture, and CLUT
buffers through its normal render path.  The function writes back both caller
ranges before emitting `GE_TRIANGLES` with this exact vertex format:

```c
GE_TEXTURE_32BITF | GE_COLOR_8888 | GE_VERTEX_32BITF |
GE_TRANSFORM_3D | GE_VINDEX_16BIT
```

The packed vertex stride is exactly 24 bytes: two 32-bit float texture
coordinates, one `GE_COLOR_8888` value, and three 32-bit float positions.  A
call is rejected with zero before any GE state or `PRIM` emission unless all of
these conditions hold:

- a current PSPGL context and draw surface exist, and no OpenGL display-list
  compilation is active;
- the vertex range is non-null, 4-byte aligned, non-empty, and an exact
  multiple of 24 bytes;
- the index range is non-null, 2-byte aligned, non-empty, and exactly
  `index_count * 2` bytes;
- `index_count` is a non-zero multiple of three and does not exceed 65535;
- the vertex count is in the range 1 through 65536.

Index values are deliberately not rescanned on each submit.  The caller must
validate the complete immutable u16 table once, then pass only a trusted prefix
whose every value is below `vertex_bytes / 24`.  Both caller-owned Main RAM
ranges must remain immutable and live until the swap/present fence has
completed.  A successful submit returns non-zero.

## TH08 native score-popup sprite submit contract

The second private exported entry point is:

```c
int __pspgl_th08_draw_native_sprite_pairs_copy(
    const void *vertices, unsigned vertex_bytes);
```

This is the M0 score-popup-only path. It consumes the same packed 24-byte
vertices as the indexed path, requires an even vertex count from 2 through
65534, and submits `GE_SPRITES` with:

```c
GE_TEXTURE_32BITF | GE_COLOR_8888 | GE_VERTEX_32BITF | GE_TRANSFORM_3D
```

Unlike the indexed entry point, it synchronously copies the caller range into
a `GL_STREAM_DRAW_ARB` PSPGL buffer, submits it through the current PSPGL list,
pins that buffer until list completion, and then returns. The caller may reuse
its staging memory immediately. A rejected validation or allocation returns
zero before `PRIM`; TH08 then executes the unchanged client-array fallback.
PSPGL retains draw, texture, render-state, transient-buffer, and display-list
ownership. The function never starts a raw GU list and does not add a delayed
frame arena.

The game gate is `TH08_PSP_SCORE_POPUP_NATIVE_GE`, which is default-off and
requires the independently gated score-only `TH08_PSP_ASCII_POPUP_BATCH`.
Neither `timePopups` nor Item's `ITEM_TIME` emitter enters this path.

## TH08 atomic mixed-submit contract

The optional post-v3 delta adds this private entry point:

```c
int __pspgl_th08_draw_native_mixed_quads(
    const void *pair_vertices, unsigned pair_vertex_bytes,
    const void *quad_vertices, unsigned quad_vertex_bytes,
    unsigned quad_count,
    const unsigned short *quad_indices, unsigned quad_index_bytes,
    unsigned quad_index_count);
```

It represents one contiguous original-order compatibility run as an optional
packed 2-vertex `GE_SPRITES` prefix followed by an optional packed 4-vertex
indexed `GE_TRIANGLES` suffix. Every argument, range, product, count, and the
current PSPGL context/list is preflighted before any cache writeback or first
`PRIM`. A valid call writes back its live caller ranges and emits the prefix,
then the suffix, through `__pspgl_context_render_prim`. Thus PSPGL continues to
own the active display list and draw/texture/render state. The hook starts no
raw GU list, allocates no storage, and copies no vertex or index data.

Both halves use the same packed 24-byte vertex format as the v3 hooks. A
present sprite half must contain an even 2 through 65534 vertices. A present
indexed half must contain exactly four vertices and six u16 indices per
`quad_count`; its vertex count must fit 65536, its index count must fit 65535,
and the index count is checked as both a triangle and whole-quad multiple. An
absent half must use the canonical all-null/all-zero representation, and at
least one half must be present.

The hook deliberately does not rescan index values. The application is the
immutable index authority: it must validate the complete canonical table once
(`0,1,2,1,2,3`, offset by four for each following quad), pass only its exact
trusted `quad_count * 6` prefix, and keep every referenced range immutable and
live until the present fence. The caller must also preserve the original draw
order: it may submit only a naturally representable `[sprite prefix][indexed
suffix]` run and must flush at a state-key change or before any later sprite
would otherwise cross the indexed suffix. Reordering quads to manufacture a
larger prefix is forbidden.

The separately fingerprinted `libGL_th08_ge4_mixed_v1.a` contains this hook,
but the application experiment remains separately gated and default-off.
Applying the delta changes the archive hash; the v3 archive remains the default
and its marker remains authoritative and unchanged. Status: **BUILT AND ABI/NM
VERIFIED; SELECTED BY TH08 ONLY WHEN either
`TH08_PSP_BULLET_MIXED_QUADS_FASTPATH=1` or
`TH08_PSP_ITEM_MIXED_QUADS_FASTPATH=1`; the Bullet product is
STAGE 5 PPSSPP REPLAY/SURFACE VALIDATED, while the new Item product remains
unvalidated;
UNTESTED ON HARDWARE.**

The upper 2 MiB allocation path is usable only after the separately frozen
TH07 `ge4wrap_texv1.prx` bridge has admitted known PSP-3000 hardware and
confirmed a 4 MiB aperture.  PSP-1000, PPSSPP, unknown models, missing wrapper,
and every failed probe remain on the ordinary lower-2-MiB path.

Status: **UNTESTED ON HARDWARE in TH08.**

## Stream-arena v1 (frame-parity GL_STREAM_DRAW storage)

`pspgl-th08-stream-arena-v1.patch` applies after v3 (source delta on
`pspgl_buffers.c` and `pspgl_internal.h`).  Every client-array or native-copy
draw allocates a `GL_STREAM_DRAW_ARB` buffer; upstream takes it from
`memalign()` and frees it when its display list completes.  With the
application-installed arena (`__pspgl_th08_stream_arena_install(base,
half_bytes)`), those copies are bump-allocated from one of two halves and the
application selects the half per frame
(`__pspgl_th08_stream_arena_begin_frame(parity)`) after fencing the frame that
used it, so no per-draw heap traffic remains and the GE may run one frame
behind the CPU without the heap live set doubling.  Overflow falls back to
`memalign()`; `__pspgl_th08_stream_arena_stats()` reports allocations,
overflows and the peak bytes.  Buffers from the arena carry `BF_UNMANAGED`
and are never `free()`d.  Selected only by the default-off
`TH08_PSP_PSPGL_STREAM_ARENA` gate; every other build keeps the frozen v3
archive.

- Patch SHA-256: see `Makefile.psp` (`PSPGL_STREAM_ARENA_PATCH_SHA256`).
- Archive: `libGL_th08_ge4_streamarena_v1.a` (size/SHA-256 in `Makefile.psp`).
- Marker: `pspGL TH08 stream arena v1 frame-parity GL_STREAM_DRAW upstream de4260adf56d06516ec46018d404ca77e0b61748 base native-submit-v3`
  (exported as `__pspgl_th08_stream_arena_marker`).
- Rebuild: upstream commit, v1 (`--ignore-space-change`), v3, then this patch;
  `mkdir -p .deps && make -j8 libGL.a`.  Byte identity with the frozen v3
  archive is not reproducible on this host (debug paths differ; stripped
  size and exported symbols match), so the record pins this archive's own
  size and SHA-256.
