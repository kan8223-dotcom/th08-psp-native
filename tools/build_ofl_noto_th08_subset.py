#!/usr/bin/env python3
"""Build and prove a lossless OFL Noto-derived subset for stock TH08 PSP.

The checked-in numeric authority is derived from the user's stock ``th08.dat``.
No game text is written by this tool.  The generated font and reports are kept
outside the repository until the matching OFL notice is corrected and the PSP
runtime integration is reviewed separately.

The input is the project's attested OpenType/CFF Noto Sans CJK JP 2.004 asset.
Subsetting is an OFL Modified Version, so the primary names are changed to
``TH08 PSP Subset Sans JP``.  Original copyright, authorship, trademark,
project URLs, and license metadata remain intact.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.util
import io
import json
import os
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_FONT = REPO_ROOT / "psp" / "assets" / "NotoSansJP-Regular.ttf"
REPO_OFL = REPO_ROOT / "licenses" / "NotoSansJP" / "OFL.txt"
STOCK_PROFILE_TOOL = REPO_ROOT / "tools" / "th08_stock_font_profile.py"

POLICY_VERSION = "th08-psp-ofl-font-subset-v1"
EXPECTED_FONTTOOLS_VERSION = "4.62.1"
EXPECTED_SOURCE_BYTES = 4_491_696
EXPECTED_SOURCE_SHA256 = (
    "6ab1664d8adc20b19237ddc451c94e31f493cb851a1917242debf66f9af6da05"
)
EXPECTED_AUTHORITY_COUNT = 1_531
EXPECTED_AUTHORITY_SHA256 = (
    "9b3d0d5fa1abbc82086d18788c775d939997235b6d11c2072bdb48254bd58ade"
)

# Filled after the first independently verified build.  Keeping these gates in
# the tool turns a later FontTools/options drift into an explicit review event.
EXPECTED_OUTPUT_BYTES = 854_368
EXPECTED_OUTPUT_SHA256 = (
    "b81aec6511d36205ca5db12e3c2d7f90045dc7394b53556c358dd8b34854f5f3"
)

DERIVATIVE_FAMILY = "TH08 PSP Subset Sans JP"
DERIVATIVE_SUBFAMILY = "Regular"
DERIVATIVE_FULL_NAME = f"{DERIVATIVE_FAMILY} {DERIVATIVE_SUBFAMILY}"
DERIVATIVE_POSTSCRIPT_NAME = "TH08PspSubsetSansJP-Regular"
DERIVATIVE_VERSION = "1.000"
OUTPUT_FILENAME = f"{DERIVATIVE_POSTSCRIPT_NAME}.otf"

SOURCE_COPYRIGHT = "© 2014-2021 Adobe (http://www.adobe.com/)."
SOURCE_TRADEMARK = "Noto is a trademark of Google Inc."
SOURCE_FAMILY = "Noto Sans CJK JP"
SOURCE_POSTSCRIPT_NAME = "NotoSansCJKjp-Regular"
SOURCE_UNIQUE_ID = "2.004;GOOG;NotoSansCJKjp-Regular;ADOBE"
SOURCE_VERSION = "Version 2.004;hotconv 1.0.118;makeotfexe 2.5.65603"
SOURCE_CFF_NOTICE = (
    "Copyright 2014-2021 Adobe (http://www.adobe.com/). "
    "Noto is a trademark of Google Inc."
)
OFL_LICENSE_MARKER = "SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007"
APPLICABLE_OFL_HEADER = (
    "Copyright 2014-2021 Adobe (http://www.adobe.com/), "
    "with Reserved Font Name 'Source'"
)
OFL_REFERENCE_URL = (
    "https://github.com/google/fonts/blob/main/ofl/notosansjp/OFL.txt"
)
OFFICIAL_OFL_BYTES = 4_388
OFFICIAL_OFL_SHA256 = (
    "1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9"
)
UPSTREAM_REFERENCE_URL = "https://github.com/notofonts/noto-cjk"

PRESERVED_LEGAL_NAME_IDS = (0, 7, 8, 9, 10, 11, 12, 13, 14)
PRIMARY_NAME_IDS = (1, 2, 3, 4, 5, 6, 16, 17, 18, 21, 22, 25)
REQUIRED_TABLES = (
    "head", "hhea", "maxp", "OS/2", "name", "cmap", "post", "CFF ",
    "BASE", "GPOS", "GSUB", "VORG", "hmtx", "vhea", "vmtx",
)
RASTER_SIZES = (16, 28, 30, 32)
RASTER_STYLES = (("normal", 0), ("synthetic-bold", 1))

# Hint-zone/private values that can change CFF rasterization even when the
# decoded outline is identical.  Local/global subroutines are compared after
# full expansion instead of by unstable subroutine indices.
CFF_PRIVATE_RENDER_FIELDS = (
    "BlueValues", "OtherBlues", "FamilyBlues", "FamilyOtherBlues",
    "BlueScale", "BlueShift", "BlueFuzz", "StdHW", "StdVW",
    "StemSnapH", "StemSnapV", "ForceBold", "LanguageGroup",
    "ExpansionFactor", "initialRandomSeed", "defaultWidthX", "nominalWidthX",
)
CFF_TOP_RENDER_FIELDS = (
    "FontBBox", "FontMatrix", "PaintType", "CharstringType", "StrokeWidth",
)


class NotoSubsetError(RuntimeError):
    """Expected, user-facing subset or verification failure."""


class CoverageError(NotoSubsetError):
    """A source/output glyph or rendering equivalence gate failed."""


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def format_codepoints(values: Iterable[int]) -> list[str]:
    return [
        f"U+{value:04X}" if value <= 0xFFFF else f"U+{value:06X}"
        for value in sorted(set(values))
    ]


def _canonical_path_and_existing_anchor(path: Path) -> tuple[Path, Path]:
    absolute = Path(os.path.abspath(os.fspath(path.expanduser())))
    current = absolute
    tail: list[str] = []
    while not os.path.lexists(current):
        parent = current.parent
        if parent == current:
            break
        tail.append(current.name)
        current = parent
    anchor = current.resolve(strict=True)
    return anchor.joinpath(*reversed(tail)), anchor


def _drvfs_casefold_parts(path: Path) -> tuple[str, ...]:
    normalized = os.path.normpath(os.fspath(path)).replace("\\", "/")
    return tuple(part.casefold() for part in normalized.split("/") if part)


def path_is_within(path: Path, directory: Path) -> bool:
    try:
        canonical, existing_anchor = _canonical_path_and_existing_anchor(path)
        base = directory.resolve(strict=True)
        cursor = existing_anchor
        while True:
            if os.path.samefile(cursor, base):
                return True
            parent = cursor.parent
            if parent == cursor:
                break
            cursor = parent
        try:
            canonical.relative_to(base)
            return True
        except ValueError:
            pass
        base_text = str(base).replace("\\", "/")
        if base_text.startswith(("/mnt/", "/MNT/")) or (
            len(base_text) >= 3 and base_text[1:3] == ":/"
        ):
            candidate_parts = _drvfs_casefold_parts(canonical)
            base_parts = _drvfs_casefold_parts(base)
            return candidate_parts[: len(base_parts)] == base_parts
        return False
    except OSError:
        return True


def validate_output_paths(paths: Sequence[Path], force: bool) -> None:
    resolved = [path.resolve(strict=False) for path in paths]
    if len(set(resolved)) != len(resolved):
        raise NotoSubsetError("font, manifest, coverage and FONTLOG paths must differ")
    for path in resolved:
        if path_is_within(path, REPO_ROOT):
            raise NotoSubsetError(
                "refusing generated derivative inside the repository before the "
                f"OFL packaging gate is fixed: {path}"
            )
        if path.exists() and not force:
            raise NotoSubsetError(f"output already exists (use --force): {path}")


def require_fonttools() -> tuple[Any, Any, Any, str]:
    try:
        subset_module = importlib.import_module("fontTools.subset")
        ttlib_module = importlib.import_module("fontTools.ttLib")
        recording_pen_module = importlib.import_module("fontTools.pens.recordingPen")
        root_module = importlib.import_module("fontTools")
    except ModuleNotFoundError as exc:
        raise NotoSubsetError(
            "fontTools is required; install the audited version with: "
            f"{sys.executable} -m pip install --user fonttools=={EXPECTED_FONTTOOLS_VERSION}"
        ) from exc
    version = str(getattr(root_module, "__version__", "unknown"))
    if version != EXPECTED_FONTTOOLS_VERSION:
        raise NotoSubsetError(
            f"fontTools {version} is not the reproducibility authority; "
            f"expected {EXPECTED_FONTTOOLS_VERSION}"
        )
    return subset_module, ttlib_module, recording_pen_module, version


def require_pillow() -> tuple[Any, Any, str]:
    try:
        image_font = importlib.import_module("PIL.ImageFont")
        features = importlib.import_module("PIL.features")
        pillow = importlib.import_module("PIL")
    except ModuleNotFoundError as exc:
        raise NotoSubsetError(
            "Pillow with FreeType/RAQM is required for pixel equivalence checks"
        ) from exc
    if not features.check("freetype2") or not features.check("raqm"):
        raise NotoSubsetError("Pillow must provide both FreeType and RAQM/HarfBuzz")
    return image_font, features, str(getattr(pillow, "__version__", "unknown"))


def load_stock_profile_module() -> Any:
    spec = importlib.util.spec_from_file_location(
        "th08_stock_font_profile_for_ofl_subset", STOCK_PROFILE_TOOL
    )
    if spec is None or spec.loader is None:
        raise NotoSubsetError(f"cannot load stock profile tool: {STOCK_PROFILE_TOOL}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def name_values(font: Any, name_id: int) -> tuple[str, ...]:
    values: list[str] = []
    if "name" not in font:
        return ()
    for record in font["name"].names:
        if record.nameID != name_id:
            continue
        try:
            value = record.toUnicode()
        except Exception as exc:
            raise NotoSubsetError(f"cannot decode name ID {name_id}: {exc}") from exc
        values.append(value)
    return tuple(values)


def legal_name_snapshot(font: Any) -> dict[int, tuple[str, ...]]:
    return {name_id: name_values(font, name_id) for name_id in PRESERVED_LEGAL_NAME_IDS}


def _require_exact_value(values: tuple[str, ...], expected: str, label: str) -> None:
    if not values or set(values) != {expected}:
        raise NotoSubsetError(f"unexpected {label}: {values!r}; expected {expected!r}")


def inspect_and_gate_source(font: Any, source_path: Path) -> dict[str, Any]:
    source_size = source_path.stat().st_size
    source_hash = sha256_file(source_path)
    if source_size != EXPECTED_SOURCE_BYTES or source_hash != EXPECTED_SOURCE_SHA256:
        raise NotoSubsetError(
            "source font attestation failed: "
            f"bytes={source_size}, sha256={source_hash}"
        )
    if font.sfntVersion != "OTTO":
        raise NotoSubsetError(f"source is not OpenType/CFF: sfnt={font.sfntVersion!r}")
    missing_tables = [tag for tag in REQUIRED_TABLES if tag not in font]
    if missing_tables:
        raise NotoSubsetError(f"source font lacks tables: {', '.join(missing_tables)}")

    _require_exact_value(name_values(font, 0), SOURCE_COPYRIGHT, "copyright")
    _require_exact_value(name_values(font, 1), SOURCE_FAMILY, "family")
    _require_exact_value(name_values(font, 3), SOURCE_UNIQUE_ID, "unique ID")
    _require_exact_value(name_values(font, 4), SOURCE_FAMILY, "full name")
    _require_exact_value(name_values(font, 5), SOURCE_VERSION, "version")
    _require_exact_value(name_values(font, 6), SOURCE_POSTSCRIPT_NAME, "PostScript name")
    _require_exact_value(name_values(font, 7), SOURCE_TRADEMARK, "trademark")
    if not any("SIL Open Font License" in value for value in name_values(font, 13)):
        raise NotoSubsetError("source name ID 13 does not declare SIL OFL 1.1")
    if not any("OFL" in value for value in name_values(font, 14)):
        raise NotoSubsetError("source name ID 14 lacks an OFL URL")

    cff = font["CFF "].cff
    if cff.fontNames != [SOURCE_POSTSCRIPT_NAME]:
        raise NotoSubsetError(f"unexpected CFF font names: {cff.fontNames!r}")
    top = cff.topDictIndex[0]
    if top.Notice != SOURCE_CFF_NOTICE:
        raise NotoSubsetError(f"unexpected CFF Notice: {top.Notice!r}")
    if top.FullName != f"{SOURCE_FAMILY} Regular" or top.FamilyName != SOURCE_FAMILY:
        raise NotoSubsetError("unexpected CFF family/full-name metadata")

    return {
        "filename": source_path.name,
        "bytes": source_size,
        "sha256": source_hash,
        "sfnt": "OpenType/CFF1",
        "glyph_count": len(font.getGlyphOrder()),
        "cmap_codepoints": len(font.getBestCmap() or {}),
        "tables": list(font.keys())[1:],
        "font_revision": float(font["head"].fontRevision),
        "name_metadata": {
            "copyright": list(name_values(font, 0)),
            "family": list(name_values(font, 1)),
            "subfamily": list(name_values(font, 2)),
            "unique_id": list(name_values(font, 3)),
            "full_name": list(name_values(font, 4)),
            "version": list(name_values(font, 5)),
            "postscript_name": list(name_values(font, 6)),
            "trademark": list(name_values(font, 7)),
            "manufacturer": list(name_values(font, 8)),
            "designer": list(name_values(font, 9)),
            "description": list(name_values(font, 10)),
            "vendor_url": list(name_values(font, 11)),
            "designer_url": list(name_values(font, 12)),
            "license": list(name_values(font, 13)),
            "license_url": list(name_values(font, 14)),
        },
        "cff": {
            "font_names": list(cff.fontNames),
            "notice": top.Notice,
            "full_name": top.FullName,
            "family_name": top.FamilyName,
            "weight": top.Weight,
        },
    }


def audit_repo_license(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {
            "path": str(path.relative_to(REPO_ROOT)),
            "present": False,
            "distribution_ready": False,
            "issue": "matching OFL file is absent",
        }
    raw = path.read_bytes()
    text = raw.decode("utf-8-sig")
    lines = text.splitlines()
    declared_header = lines[0].strip() if lines else ""
    full_text_present = all(
        marker in text
        for marker in (
            OFL_LICENSE_MARKER,
            "PREAMBLE",
            "DEFINITIONS",
            "PERMISSION & CONDITIONS",
            "TERMINATION",
            "DISCLAIMER",
            "5) The Font Software, modified or unmodified",
        )
    )
    header_exact = declared_header == APPLICABLE_OFL_HEADER
    official_file_exact = (
        len(raw) == OFFICIAL_OFL_BYTES and sha256_bytes(raw) == OFFICIAL_OFL_SHA256
    )
    issue = ""
    if not official_file_exact:
        issue = (
            "repository OFL is not the exact official Noto Sans JP/CJK license "
            "with the Adobe 2014-2021 copyright and RFN 'Source'; replace it "
            "before redistribution"
        )
    return {
        "path": str(path.relative_to(REPO_ROOT)),
        "present": True,
        "bytes": len(raw),
        "sha256": sha256_bytes(raw),
        "declared_header": declared_header,
        "expected_header": APPLICABLE_OFL_HEADER,
        "header_exact": header_exact,
        "full_ofl_text_present": full_text_present,
        "official_file_bytes": OFFICIAL_OFL_BYTES,
        "official_file_sha256": OFFICIAL_OFL_SHA256,
        "official_file_exact": official_file_exact,
        "reserved_font_names": ["Source"],
        "reference": OFL_REFERENCE_URL,
        "distribution_ready": official_file_exact,
        "issue": issue,
    }


def configure_subset_options(subset_module: Any) -> Any:
    options = subset_module.Options()
    options.recalc_timestamp = False
    options.canonical_order = True
    options.hinting = True
    options.desubroutinize = False
    options.glyph_names = True
    options.notdef_glyph = True
    options.notdef_outline = True
    options.recommended_glyphs = True
    options.layout_closure = True
    options.layout_scripts = ["*"]
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.name_legacy = True
    options.prune_unicode_ranges = False
    options.prune_codepage_ranges = False
    options.recalc_average_width = False
    options.recalc_bounds = False
    options.recalc_max_context = False
    # None of these is normally dropped by FontTools, but make that policy
    # explicit so a future default change cannot silently discard them.
    options.drop_tables = [
        tag for tag in options.drop_tables
        if tag not in ("BASE", "GPOS", "GSUB", "VORG")
    ]
    return options


def rename_derivative(font: Any) -> None:
    name_table = font["name"]
    for name_id in PRIMARY_NAME_IDS:
        name_table.removeNames(nameID=name_id)

    for language in (0x0409, 0x0411):
        name_table.setName(DERIVATIVE_FAMILY, 1, 3, 1, language)
        name_table.setName(DERIVATIVE_SUBFAMILY, 2, 3, 1, language)
        name_table.setName(DERIVATIVE_FULL_NAME, 4, 3, 1, language)
        name_table.setName(DERIVATIVE_FAMILY, 16, 3, 1, language)
        name_table.setName(DERIVATIVE_SUBFAMILY, 17, 3, 1, language)
    name_table.setName(
        f"{DERIVATIVE_VERSION};TH08PSP;{DERIVATIVE_POSTSCRIPT_NAME};"
        "source-NotoSansCJKjp-Regular-2.004",
        3, 3, 1, 0x0409,
    )
    name_table.setName(
        f"Version {DERIVATIVE_VERSION}; stock-profile-v1 subset of "
        "Noto Sans CJK JP 2.004",
        5, 3, 1, 0x0409,
    )
    name_table.setName(DERIVATIVE_POSTSCRIPT_NAME, 6, 3, 1, 0x0409)

    cff = font["CFF "].cff
    cff.fontNames = [DERIVATIVE_POSTSCRIPT_NAME]
    top = cff.topDictIndex[0]
    top.FullName = DERIVATIVE_FULL_NAME
    top.FamilyName = DERIVATIVE_FAMILY
    top.version = DERIVATIVE_VERSION
    font["head"].fontRevision = float(DERIVATIVE_VERSION)
    font.recalcTimestamp = False


def generate_subset_bytes(
    source_path: Path, codepoints: Sequence[int], subset_module: Any, ttlib: Any
) -> bytes:
    font = ttlib.TTFont(
        str(source_path), lazy=False, recalcBBoxes=False, recalcTimestamp=False
    )
    try:
        # FontTools correctly tightens these aggregate bounds after removing
        # glyphs.  Keeping the original supersets is also valid and makes the
        # retained font-wide rendering geometry exactly source-equivalent.
        original_head_bounds = tuple(
            getattr(font["head"], field)
            for field in ("xMin", "yMin", "xMax", "yMax")
        )
        original_cff_bounds = list(font["CFF "].cff.topDictIndex[0].FontBBox)
        options = configure_subset_options(subset_module)
        subsetter = subset_module.Subsetter(options=options)
        subsetter.populate(unicodes=list(codepoints))
        subsetter.subset(font)
        for field, value in zip(
            ("xMin", "yMin", "xMax", "yMax"), original_head_bounds
        ):
            setattr(font["head"], field, value)
        font["CFF "].cff.topDictIndex[0].FontBBox = original_cff_bounds
        rename_derivative(font)
        output = io.BytesIO()
        font.save(output, reorderTables=True)
        return output.getvalue()
    finally:
        font.close()


def _normalize_value(value: Any) -> Any:
    if isinstance(value, (list, tuple)):
        return tuple(_normalize_value(item) for item in value)
    if isinstance(value, float):
        return float(value)
    if isinstance(value, (str, int, bool, bytes, type(None))):
        return value
    return repr(value)


def _dict_signature(obj: Any, fields: Sequence[str]) -> tuple[tuple[str, Any], ...]:
    return tuple(
        (field, _normalize_value(getattr(obj, field)))
        for field in fields
        if hasattr(obj, field)
    )


def _layout_signature(font: Any, table_tag: str) -> dict[str, Any]:
    table = font[table_tag].table
    scripts = tuple(record.ScriptTag for record in table.ScriptList.ScriptRecord)
    features = tuple(record.FeatureTag for record in table.FeatureList.FeatureRecord)
    return {
        "scripts": scripts,
        "features": features,
        "lookup_count": len(table.LookupList.Lookup),
    }


def _glyph_outline_signature(glyph_set: Any, glyph_name: str, recording_pen: Any) -> Any:
    pen = recording_pen.RecordingPen()
    glyph_set[glyph_name].draw(pen)
    return tuple(pen.value)


def validate_structural_equivalence(
    source_font: Any,
    output_font: Any,
    codepoints: Sequence[int],
    recording_pen: Any,
) -> dict[str, Any]:
    requested = tuple(sorted(set(codepoints)))
    source_cmap = source_font.getBestCmap() or {}
    output_cmap = output_font.getBestCmap() or {}
    missing_source = set(requested).difference(source_cmap)
    if missing_source:
        raise CoverageError(
            "source font lacks authority glyphs: "
            + " ".join(format_codepoints(missing_source))
        )
    if set(output_cmap) != set(requested):
        missing = set(requested).difference(output_cmap)
        unexpected = set(output_cmap).difference(requested)
        raise CoverageError(
            "output cmap is not exact; missing="
            + " ".join(format_codepoints(missing))
            + " unexpected="
            + " ".join(format_codepoints(unexpected))
        )
    if source_font.sfntVersion != output_font.sfntVersion or output_font.sfntVersion != "OTTO":
        raise CoverageError("OpenType/CFF format changed")
    if set(source_font.keys()) != set(output_font.keys()):
        raise CoverageError(
            f"font table set changed: source={source_font.keys()} output={output_font.keys()}"
        )

    for table_tag in ("GSUB", "GPOS"):
        source_layout = _layout_signature(source_font, table_tag)
        output_layout = _layout_signature(output_font, table_tag)
        if source_layout["scripts"] != output_layout["scripts"]:
            raise CoverageError(f"{table_tag} script inventory changed")
        if source_layout["features"] != output_layout["features"]:
            raise CoverageError(f"{table_tag} feature inventory changed")
    if source_font.getTableData("BASE") != output_font.getTableData("BASE"):
        raise CoverageError("BASE table changed")

    global_metric_fields = {
        "head": ("unitsPerEm", "xMin", "yMin", "xMax", "yMax"),
        "hhea": ("ascent", "descent", "lineGap", "caretSlopeRise", "caretSlopeRun"),
        "vhea": ("ascent", "descent", "lineGap", "caretSlopeRise", "caretSlopeRun"),
        "OS/2": (
            "sTypoAscender", "sTypoDescender", "sTypoLineGap",
            "usWinAscent", "usWinDescent", "sxHeight", "sCapHeight",
        ),
    }
    for table_tag, fields in global_metric_fields.items():
        for field in fields:
            if getattr(source_font[table_tag], field) != getattr(output_font[table_tag], field):
                raise CoverageError(f"global metric changed: {table_tag}.{field}")

    source_glyph_set = source_font.getGlyphSet()
    output_glyph_set = output_font.getGlyphSet()
    source_glyph_names = set(source_font.getGlyphOrder())
    output_glyph_order = tuple(output_font.getGlyphOrder())
    unexpected_closure_glyphs = set(output_glyph_order).difference(source_glyph_names)
    if unexpected_closure_glyphs:
        raise CoverageError(
            "layout closure created glyphs absent from the source: "
            + " ".join(sorted(unexpected_closure_glyphs))
        )
    source_vorg = source_font["VORG"]
    output_vorg = output_font["VORG"]
    if source_vorg.defaultVertOriginY != output_vorg.defaultVertOriginY:
        raise CoverageError("VORG default vertical origin changed")
    for codepoint in requested:
        source_name = source_cmap[codepoint]
        output_name = output_cmap[codepoint]
        if source_name != output_name:
            raise CoverageError(
                f"internal glyph identity changed at U+{codepoint:04X}: "
                f"{source_name} != {output_name}"
            )

    # Layout closure retains unencoded substitution targets.  Prove those
    # glyphs too, not only the 1,531 directly encoded authority characters.
    for output_name in output_glyph_order:
        source_name = output_name
        if source_font["hmtx"].metrics[source_name] != output_font["hmtx"].metrics[output_name]:
            raise CoverageError(
                f"horizontal advance/bearing changed for glyph {output_name}"
            )
        if source_font["vmtx"].metrics[source_name] != output_font["vmtx"].metrics[output_name]:
            raise CoverageError(
                f"vertical advance/bearing changed for glyph {output_name}"
            )
        source_vertical_origin = source_vorg.VOriginRecords.get(
            source_name, source_vorg.defaultVertOriginY
        )
        output_vertical_origin = output_vorg.VOriginRecords.get(
            output_name, output_vorg.defaultVertOriginY
        )
        if source_vertical_origin != output_vertical_origin:
            raise CoverageError(f"VORG changed for glyph {output_name}")
        source_outline = _glyph_outline_signature(
            source_glyph_set, source_name, recording_pen
        )
        output_outline = _glyph_outline_signature(
            output_glyph_set, output_name, recording_pen
        )
        if source_outline != output_outline:
            raise CoverageError(f"CFF outline changed for glyph {output_name}")

    source_cff = source_font["CFF "].cff
    output_cff = output_font["CFF "].cff
    source_top = source_cff.topDictIndex[0]
    output_top = output_cff.topDictIndex[0]
    if _dict_signature(source_top, CFF_TOP_RENDER_FIELDS) != _dict_signature(
        output_top, CFF_TOP_RENDER_FIELDS
    ):
        raise CoverageError("CFF top-level rendering parameters changed")

    # Compare fully expanded Type 2 programs.  Subroutine indices are expected
    # to change when unused subroutines are removed; expanded operators,
    # operands, stems and hint masks must remain byte-for-byte equivalent.
    transforms = importlib.import_module("fontTools.cffLib.transforms")
    checked_source_programs: dict[str, tuple[Any, ...]] = {}
    checked_output_programs: dict[str, tuple[Any, ...]] = {}
    for output_name in output_glyph_order:
        source_name = output_name
        if source_name not in checked_source_programs:
            char_string = source_top.CharStrings[source_name]
            private_before = _dict_signature(char_string.private, CFF_PRIVATE_RENDER_FIELDS)
            transforms.desubroutinizeCharString(char_string)
            checked_source_programs[source_name] = (
                private_before,
                tuple(_normalize_value(item) for item in char_string.program),
            )
        if output_name not in checked_output_programs:
            char_string = output_top.CharStrings[output_name]
            private_before = _dict_signature(char_string.private, CFF_PRIVATE_RENDER_FIELDS)
            transforms.desubroutinizeCharString(char_string)
            checked_output_programs[output_name] = (
                private_before,
                tuple(_normalize_value(item) for item in char_string.program),
            )
        if checked_source_programs[source_name] != checked_output_programs[output_name]:
            raise CoverageError(
                f"expanded CFF/hint program changed for glyph {output_name}"
            )

    return {
        "status": "passed",
        "checked_codepoints": len(requested),
        "checked_encoded_glyphs": len(requested),
        "checked_glyphs_including_layout_closure": len(output_glyph_order),
        "cmap_exact": True,
        "horizontal_advance_and_bearing_exact": True,
        "vertical_advance_and_bearing_exact": True,
        "cff_outlines_exact": True,
        "cff_expanded_programs_and_hint_zones_exact": True,
        "base_table_exact": True,
        "vorg_for_retained_glyphs_exact": True,
        "table_inventory_exact": True,
        "layout": {
            table_tag: {
                "scripts": list(_layout_signature(output_font, table_tag)["scripts"]),
                "features": list(_layout_signature(output_font, table_tag)["features"]),
                "source_lookup_count": _layout_signature(source_font, table_tag)["lookup_count"],
                "subset_lookup_count": _layout_signature(output_font, table_tag)["lookup_count"],
                "script_and_feature_inventory_exact": True,
            }
            for table_tag in ("GSUB", "GPOS")
        },
    }


def validate_derivative_metadata(
    source_legal_names: dict[int, tuple[str, ...]], source_font: Any, output_font: Any
) -> dict[str, Any]:
    for name_id, expected in source_legal_names.items():
        actual = name_values(output_font, name_id)
        if actual != expected:
            raise CoverageError(
                f"copyright/authorship/license name ID {name_id} changed: {actual!r}"
            )

    expected_primary = {
        1: {DERIVATIVE_FAMILY},
        2: {DERIVATIVE_SUBFAMILY},
        4: {DERIVATIVE_FULL_NAME},
        6: {DERIVATIVE_POSTSCRIPT_NAME},
        16: {DERIVATIVE_FAMILY},
        17: {DERIVATIVE_SUBFAMILY},
    }
    for name_id, expected in expected_primary.items():
        actual = set(name_values(output_font, name_id))
        if actual != expected:
            raise CoverageError(f"derivative name ID {name_id} is wrong: {actual!r}")
    primary_values = {
        value
        for name_id in (1, 4, 6, 16, 18, 21, 25)
        for value in name_values(output_font, name_id)
    }
    forbidden = [
        value for value in primary_values
        if "source" in value.casefold() or "noto" in value.casefold()
    ]
    if forbidden:
        raise CoverageError(f"primary derivative names retain RFN/trademark: {forbidden!r}")

    source_cff = source_font["CFF "].cff
    output_cff = output_font["CFF "].cff
    source_top = source_cff.topDictIndex[0]
    output_top = output_cff.topDictIndex[0]
    if output_cff.fontNames != [DERIVATIVE_POSTSCRIPT_NAME]:
        raise CoverageError(f"CFF PostScript name is wrong: {output_cff.fontNames!r}")
    if output_top.FamilyName != DERIVATIVE_FAMILY or output_top.FullName != DERIVATIVE_FULL_NAME:
        raise CoverageError("CFF family/full name was not renamed")
    if output_top.Notice != source_top.Notice:
        raise CoverageError("CFF copyright/trademark Notice changed")

    return {
        "status": "passed",
        "primary_family": DERIVATIVE_FAMILY,
        "postscript_name": DERIVATIVE_POSTSCRIPT_NAME,
        "reserved_font_names": ["Source"],
        "reserved_name_absent_from_primary_names": True,
        "noto_trademark_absent_from_primary_names": True,
        "original_legal_name_ids_preserved": list(PRESERVED_LEGAL_NAME_IDS),
        "cff_notice_preserved": True,
    }


def _raster_signature(font: Any, text: str, stroke_width: int, **kwargs: Any) -> Any:
    mask, offset = font.getmask2(text, mode="L", stroke_width=stroke_width, **kwargs)
    bbox = font.getbbox(text, stroke_width=stroke_width, **kwargs)
    length_kwargs = {
        key: value for key, value in kwargs.items()
        if key in ("direction", "features", "language")
    }
    return (
        tuple(int(value) for value in mask.size),
        tuple(int(value) for value in offset),
        bytes(mask),
        None if bbox is None else tuple(float(value) for value in bbox),
        float(font.getlength(text, **length_kwargs)),
    )


def validate_raster_equivalence(
    source_path: Path, output_path: Path, codepoints: Sequence[int]
) -> dict[str, Any]:
    image_font, _features, pillow_version = require_pillow()
    requested = tuple(sorted(set(codepoints)))
    engine_specs = (
        ("basic-freetype", image_font.Layout.BASIC),
        ("raqm-harfbuzz", image_font.Layout.RAQM),
    )
    checked = 0
    for size in RASTER_SIZES:
        for engine_name, engine in engine_specs:
            source_font = image_font.truetype(
                str(source_path), size=size, index=0, layout_engine=engine
            )
            output_font = image_font.truetype(
                str(output_path), size=size, index=0, layout_engine=engine
            )
            if source_font.getmetrics() != output_font.getmetrics():
                raise CoverageError(f"FreeType global metrics changed at {size}px/{engine_name}")
            for style_name, stroke_width in RASTER_STYLES:
                for codepoint in requested:
                    char = chr(codepoint)
                    if _raster_signature(
                        source_font, char, stroke_width
                    ) != _raster_signature(output_font, char, stroke_width):
                        raise CoverageError(
                            "raster changed at "
                            f"U+{codepoint:04X}, {size}px/{engine_name}/{style_name}"
                        )
                    checked += 1

    # Exercise GSUB/GPOS/RAQM on deterministic multi-glyph runs as well as on
    # every isolated character above.  No decoded TH08 text is serialized.
    shaped_runs = 0
    chars = "".join(chr(codepoint) for codepoint in requested)
    chunks = tuple(chars[index:index + 48] for index in range(0, len(chars), 48))
    for size in RASTER_SIZES:
        source_font = image_font.truetype(
            str(source_path), size=size, index=0, layout_engine=image_font.Layout.RAQM
        )
        output_font = image_font.truetype(
            str(output_path), size=size, index=0, layout_engine=image_font.Layout.RAQM
        )
        for style_name, stroke_width in RASTER_STYLES:
            for chunk in chunks:
                kwargs = {"direction": "ltr", "language": "ja"}
                if _raster_signature(
                    source_font, chunk, stroke_width, **kwargs
                ) != _raster_signature(output_font, chunk, stroke_width, **kwargs):
                    raise CoverageError(
                        f"RAQM shaped run changed at {size}px/{style_name}"
                    )
                shaped_runs += 1

    return {
        "status": "passed",
        "pillow_version": pillow_version,
        "engines": [name for name, _engine in engine_specs],
        "sizes": list(RASTER_SIZES),
        "styles": [name for name, _stroke in RASTER_STYLES],
        "checked_isolated_renders": checked,
        "checked_raqm_shaped_runs": shaped_runs,
        "pixels_bbox_offsets_and_advance_exact": True,
    }


def atomic_write_bytes(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def atomic_write_text(path: Path, content: str) -> None:
    atomic_write_bytes(path, content.encode("utf-8"))


def render_fontlog(manifest: dict[str, Any]) -> str:
    authority = manifest["stock_profile"]
    output = manifest["output"]
    return "\n".join(
        (
            f"FONTLOG for {DERIVATIVE_FAMILY}",
            "",
            f"Version {DERIVATIVE_VERSION}",
            "",
            "This is an OFL Modified Version of Noto Sans CJK JP Regular 2.004.",
            f"Original project: {UPSTREAM_REFERENCE_URL}",
            f"License reference: {OFL_REFERENCE_URL}",
            f"Reserved Font Name declared upstream: Source",
            "",
            "Changes:",
            f"- Retained the {authority['codepoint_count']} Unicode scalars required by the",
            "  attested stock TH08 profile; removed unneeded encoded glyphs.",
            "- Kept OpenType/CFF1 outlines, metrics, hint programs, BASE, GSUB, GPOS,",
            "  VORG and their required closure glyphs for the retained character set.",
            f"- Changed primary names to {DERIVATIVE_FAMILY} to distinguish the Modified Version.",
            "- Did not alter the original copyright, authorship, trademark, project URL,",
            "  or OFL metadata fields.",
            "",
            f"Authority SHA-256: {authority['codepoint_sha256']}",
            f"Source font SHA-256: {manifest['source_font']['sha256']}",
            f"Output font SHA-256: {output['sha256']}",
            "",
            "This FONTLOG records modifications; it does not replace the OFL license text.",
            "",
        )
    )


def render_coverage(manifest: dict[str, Any]) -> str:
    validation = manifest["validation"]
    license_audit = manifest["license_audit"]
    lines = [
        "TH08 PSP OFL font subset verification",
        f"policy: {manifest['policy_version']}",
        f"authority_count: {manifest['stock_profile']['codepoint_count']}",
        f"authority_sha256: {manifest['stock_profile']['codepoint_sha256']}",
        f"source_bytes: {manifest['source_font']['bytes']}",
        f"source_sha256: {manifest['source_font']['sha256']}",
        f"output_bytes: {manifest['output']['bytes']}",
        f"output_sha256: {manifest['output']['sha256']}",
        f"missing_codepoints: {manifest['coverage']['missing_count']}",
        f"structural: {validation['structural']['status']}",
        f"metadata: {validation['metadata']['status']}",
        f"raster: {validation['raster']['status']}",
        f"reproducible: {validation['reproducibility']['byte_identical_two_builds']}",
        f"repo_ofl_distribution_ready: {license_audit['repository_ofl']['distribution_ready']}",
        f"repo_ofl_issue: {license_audit['repository_ofl']['issue']}",
        "",
        "Authority codepoints (numeric only):",
        *manifest["coverage"]["codepoints"],
        "",
    ]
    return "\n".join(lines)


def default_sidecar_paths(output: Path) -> tuple[Path, Path, Path]:
    return (
        output.with_name(output.name + ".manifest.json"),
        output.with_name(output.name + ".coverage.txt"),
        output.with_name("FONTLOG-TH08PspSubsetSansJP.txt"),
    )


def build_and_validate(
    source_path: Path,
    archive_path: Path,
    output_path: Path,
    manifest_path: Path,
    coverage_path: Path,
    fontlog_path: Path,
    *,
    force: bool,
) -> dict[str, Any]:
    validate_output_paths(
        (output_path, manifest_path, coverage_path, fontlog_path), force
    )
    if output_path.name != OUTPUT_FILENAME:
        raise NotoSubsetError(f"--output filename must be {OUTPUT_FILENAME}")
    if not source_path.is_file():
        raise NotoSubsetError(f"source font is missing: {source_path}")
    if source_path.resolve() != SOURCE_FONT.resolve():
        raise NotoSubsetError(
            "this reproducibility profile accepts only psp/assets/NotoSansJP-Regular.ttf"
        )

    subset_module, ttlib, recording_pen, fonttools_version = require_fonttools()
    profile_module = load_stock_profile_module()
    try:
        profile = profile_module.build_stock_profile(archive_path)
        profile_module.verify_authority_header(profile)
    except profile_module.StockProfileError as exc:
        raise NotoSubsetError(str(exc)) from exc
    if (
        len(profile.codepoints) != EXPECTED_AUTHORITY_COUNT
        or profile.codepoint_sha256 != EXPECTED_AUTHORITY_SHA256
    ):
        raise NotoSubsetError("stock authority count/hash changed")
    codepoints = tuple(sorted(profile.codepoints))

    source_font = ttlib.TTFont(str(source_path), lazy=False, recalcTimestamp=False)
    try:
        source_audit = inspect_and_gate_source(source_font, source_path)
        source_legal_names = legal_name_snapshot(source_font)
        source_cmap = source_font.getBestCmap() or {}
        missing = set(codepoints).difference(source_cmap)
        if missing:
            raise CoverageError(
                "source is missing authority codepoints: "
                + " ".join(format_codepoints(missing))
            )
    finally:
        source_font.close()

    first_payload = generate_subset_bytes(
        source_path, codepoints, subset_module, ttlib
    )
    second_payload = generate_subset_bytes(
        source_path, codepoints, subset_module, ttlib
    )
    if first_payload != second_payload:
        raise CoverageError("two clean subset builds are not byte-identical")
    output_hash = sha256_bytes(first_payload)
    if EXPECTED_OUTPUT_BYTES and len(first_payload) != EXPECTED_OUTPUT_BYTES:
        raise CoverageError(
            f"output byte count drifted: {len(first_payload)} != {EXPECTED_OUTPUT_BYTES}"
        )
    if EXPECTED_OUTPUT_SHA256 and output_hash != EXPECTED_OUTPUT_SHA256:
        raise CoverageError(
            f"output SHA-256 drifted: {output_hash} != {EXPECTED_OUTPUT_SHA256}"
        )

    source_font = ttlib.TTFont(str(source_path), lazy=False, recalcTimestamp=False)
    output_font = ttlib.TTFont(
        io.BytesIO(first_payload), lazy=False, recalcTimestamp=False
    )
    try:
        structural = validate_structural_equivalence(
            source_font, output_font, codepoints, recording_pen
        )
        metadata = validate_derivative_metadata(
            source_legal_names, source_font, output_font
        )
        output_glyph_count = len(output_font.getGlyphOrder())
        output_tables = list(output_font.keys())[1:]
    finally:
        output_font.close()
        source_font.close()

    with tempfile.TemporaryDirectory(prefix="th08-ofl-font-validation-") as temporary_dir:
        temporary_font = Path(temporary_dir) / OUTPUT_FILENAME
        temporary_font.write_bytes(first_payload)
        raster = validate_raster_equivalence(source_path, temporary_font, codepoints)

    repo_license = audit_repo_license(REPO_OFL)
    manifest: dict[str, Any] = {
        "schema_version": 1,
        "policy_version": POLICY_VERSION,
        "fonttools_version": fonttools_version,
        "source_font": source_audit,
        "stock_profile": {
            "version": profile_module.PROFILE_VERSION,
            "archive_filename": profile.archive_filename,
            "archive_sha256": profile.archive_sha256,
            "codepoint_count": len(codepoints),
            "codepoint_sha256": profile.codepoint_sha256,
            "numeric_authority_header": "src/Th08FontCoverage.hpp",
            "authority_header_match": True,
        },
        "derivative": {
            "family": DERIVATIVE_FAMILY,
            "full_name": DERIVATIVE_FULL_NAME,
            "postscript_name": DERIVATIVE_POSTSCRIPT_NAME,
            "version": DERIVATIVE_VERSION,
            "ofl_status": "Modified Version",
        },
        "license_audit": {
            "font_embedded_ofl_metadata": True,
            "font_embedded_copyright": SOURCE_COPYRIGHT,
            "font_embedded_trademark": SOURCE_TRADEMARK,
            "applicable_copyright_and_rfn_header": APPLICABLE_OFL_HEADER,
            "reserved_font_names": ["Source"],
            "official_license_reference": OFL_REFERENCE_URL,
            "upstream_reference": UPSTREAM_REFERENCE_URL,
            "repository_ofl": repo_license,
            "notice_file_required_by_name": False,
            "notice_note": (
                "OFL condition 2 requires copyright and license, not a file literally "
                "named NOTICE; a third-party notice remains recommended packaging."
            ),
            "fontlog_legally_required": False,
            "fontlog_recommended_for_modified_version": True,
            "distribution_ready": repo_license["distribution_ready"],
        },
        "subset_policy": {
            "layout_scripts": ["*"],
            "layout_features": ["*"],
            "hinting": True,
            "desubroutinize_output": False,
            "layout_closure": True,
            "notdef_outline": True,
            "preserved_tables": ["CFF ", "BASE", "GSUB", "GPOS", "VORG"],
        },
        "coverage": {
            "requested_count": len(codepoints),
            "source_present_count": len(codepoints),
            "output_present_count": len(codepoints),
            "missing_count": 0,
            "codepoints": format_codepoints(codepoints),
        },
        "output": {
            "filename": output_path.name,
            "bytes": len(first_payload),
            "sha256": output_hash,
            "glyph_count_including_layout_closure": output_glyph_count,
            "tables": output_tables,
            "size_reduction_bytes": EXPECTED_SOURCE_BYTES - len(first_payload),
            "size_reduction_percent": round(
                (EXPECTED_SOURCE_BYTES - len(first_payload)) * 100.0
                / EXPECTED_SOURCE_BYTES,
                4,
            ),
        },
        "validation": {
            "structural": structural,
            "metadata": metadata,
            "raster": raster,
            "reproducibility": {
                "byte_identical_two_builds": True,
                "first_sha256": output_hash,
                "second_sha256": sha256_bytes(second_payload),
                "expected_output_gate_enabled": bool(EXPECTED_OUTPUT_SHA256),
            },
        },
    }

    atomic_write_bytes(output_path, first_payload)
    atomic_write_text(
        manifest_path,
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
    )
    atomic_write_text(coverage_path, render_coverage(manifest))
    atomic_write_text(fontlog_path, render_fontlog(manifest))
    return manifest


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build an externally-staged, lossless OFL Noto-derived subset for "
            "the attested stock TH08 character profile."
        )
    )
    parser.add_argument(
        "--archive",
        type=Path,
        help="stock th08.dat (uses the profile tool's attested default when omitted)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help=f"repo-external output path ending in {OUTPUT_FILENAME}",
    )
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--coverage", type=Path)
    parser.add_argument("--fontlog", type=Path)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    profile_module = load_stock_profile_module()
    archive = args.archive or profile_module.DEFAULT_STOCK_ARCHIVE
    default_manifest, default_coverage, default_fontlog = default_sidecar_paths(args.output)
    manifest = build_and_validate(
        SOURCE_FONT,
        archive,
        args.output,
        args.manifest or default_manifest,
        args.coverage or default_coverage,
        args.fontlog or default_fontlog,
        force=args.force,
    )
    output = manifest["output"]
    repo_license = manifest["license_audit"]["repository_ofl"]
    print(
        f"PASS: {output['filename']} {output['bytes']} bytes "
        f"sha256={output['sha256']}"
    )
    print(
        f"coverage={manifest['coverage']['requested_count']} missing=0; "
        "CFF/metrics/hints/GSUB/GPOS/raster exact"
    )
    if not repo_license["distribution_ready"]:
        print(
            "DISTRIBUTION BLOCKED: " + repo_license["issue"],
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except NotoSubsetError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
