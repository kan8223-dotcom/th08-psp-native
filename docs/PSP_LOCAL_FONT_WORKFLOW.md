# PSP personal MS Gothic subset workflow

TH08 now uses the same local-font method already proven in the TH07 PSP port.
The Microsoft font is supplied by the owner's Windows installation, a subset is
generated outside the repository, and only that owner may stage it into a
personal PPSSPP or Memory Stick runtime. Neither `msgothic.ttc` nor a derived
font belongs in a source or public release archive.

## Directly reused TH07 contract

| Contract | TH07 operational result | TH08 implementation |
| --- | --- | --- |
| local output | `%LOCALAPPDATA%/TH07PSP/msgothic-subset.ttf` | `%LOCALAPPDATA%/TH08PSP/msgothic-subset.ttf` |
| source | owner's Windows `msgothic.ttc` | same |
| required set | stock archive/static UI plus full name-entry table | TH08 1.00d archive, dialogue, spells, comments, Music Room, endings, UI, score/replay entry |
| coverage gate | every pinned scalar must exist | all 1,531 pinned scalars must exist |
| runtime order | local subset, full MS Gothic, Noto | override, local subset, full MS Gothic, OFL subset, full Noto |
| allocation | file-backed face | file-backed shared face; `main_ram_copy=0` |

The current audited personal TH08 subset is 753,976 bytes with SHA-256
`82c5a071d25da573ce74d86f9350930145a1d611f7d5924c0427a5d3f33109ae`.
It covers 1,531/1,531 codepoints and passed structural plus pixel/metric checks
at 16, 28, 30, and 32 pixels. This is a completed local asset, not a proposal.

## Generate or refresh the personal subset

Run from this repository. Generate a refresh into a new, repo-external
candidate directory so an existing known-good local asset is not overwritten:

```sh
python3 tools/build_local_msgothic_subset.py \
  --archive ../original-data/th08-pre100d-20260901/th08.dat \
  --output /mnt/c/Users/kan82/AppData/Local/TH08PSP/candidates/20260902/msgothic-subset.ttf
```

The builder emits the font, a deterministic JSON manifest, and a coverage
report. It refuses repo-local output and rejects missing glyphs, a stale TH08
archive/profile, the wrong TTC face, structural damage, and raster/metric
differences. The font, manifest, and coverage output must each be distinct from
every input and from one another, including DrvFS case aliases, symlinks, and
hardlinks. This remains enforced even with `--force`.

`--force` is not part of the routine refresh command. Use it only when
deliberately replacing a previously generated three-file candidate after
checking its directory. It never authorizes overwriting `msgothic.ttc`,
`th08.dat`, a `--chars` file, or any filesystem alias of those inputs.

## Stage without racing PPSSPP

Validation and dry-run are the default and do not modify the runtime:

```sh
python3 tools/stage_local_msgothic_subset.py \
  --source /mnt/c/Users/kan82/AppData/Local/TH08PSP/msgothic-subset.ttf \
  --destination /path/to/ppsspp/memstick/PSP/GAME/TH08PSP \
  --runtime-kind ppsspp
```

After every PPSSPP process has exited, repeat with `--apply`. The tool checks
the manifest, coverage report, tracked numeric authority, actual font cmap,
byte count, and SHA-256. It also parses the PBP and requires a valid PARAM.SFO
whose title is exactly `Touhou 8 PSP SC Engine Bring-up`, the title emitted by
the audited PSP build. Every apply mode, including `hardware` and custom `auto`
paths, checks Windows and native Qt/SDL PPSSPP processes both before staging and
immediately before target replacement. An obvious PPSSPP or `memstick` path
cannot be relabeled as hardware.

The final copy uses a same-directory temporary file plus atomic replace and
verifies readback. Any replaced font is saved only under
`%LOCALAPPDATA%/TH08PSP/backups/`. A custom backup directory must be that exact
private root or one of its real descendants. The complete PPSSPP/memstick tree,
the current game, sibling games, and symlink escapes are rejected. Repository
boundary checks resolve an existing ancestor and compare filesystem identity,
so a DrvFS case variation or symlink cannot turn a local font into a repo
output.

For a mounted personal Memory Stick, use its exact game directory and
`--runtime-kind hardware --apply`. The directory must already contain
`EBOOT.PBP`; this prevents accidentally staging into a broad or wrong path.

## Distribution boundary

Before making any public/review package, first audit the repository, then run
the mandatory release gate against the exact final archive that would be
published:

```sh
python3 tools/audit_private_font_boundaries.py
python3 tools/audit_private_font_boundaries.py \
  --release /absolute/path/to/final/TH08PSP-package.zip
```

The bare repository audit is not package qualification. Do not publish or hand
off an artifact unless the second command succeeds on that exact artifact
after packaging. Rebuilding or modifying the archive invalidates the result
and requires rerunning the same `--release` gate.

The audit checks every tracked/release font by content, not only by filename.
It rejects MS Gothic family names, known source/subset SHA-256 values, renamed
font binaries, backup suffixes, and suspicious names. It recursively audits
ZIP, TAR, TAR.GZ/TGZ, GZIP, and nested archive members under bounded member,
expanded-byte, archive-depth, and total-byte limits; oversized input fails
closed before unbounded allocation. Invalid font content also fails closed.
The OFL fallback may be packaged separately under its renamed family and
license; it is not a Microsoft-derived font.

Runtime success is recorded only when `TH08PSP_BOOT.LOG` reports the selected
source and `coverage=1531/1531`. PPSSPP and PSP hardware validation remain
separate claims.
