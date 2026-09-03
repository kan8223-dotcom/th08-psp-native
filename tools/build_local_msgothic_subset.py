#!/usr/bin/env python3
"""Build a deterministic, user-local MS Gothic subset for TH08 PSP.

The input font is supplied by the user's Windows installation.  Neither that
font nor the generated subset is redistributable by this project.  To make an
accidental commit/release impossible through this tool, every generated path
must resolve outside the repository.

Typical WSL invocation::

    python3 tools/build_local_msgothic_subset.py \
        --archive /path/to/stock/th08.dat \
        --output /mnt/c/Users/NAME/AppData/Local/TH08PSP/msgothic-subset.ttf

Use repeatable ``--chars`` arguments only for additional local/mod text. The
stock 1,531-codepoint set is always derived from and checked against the local
archive plus the tracked numeric authority table.

``fontTools`` is intentionally a local developer dependency, not a project or
release dependency.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import importlib
import importlib.util
import json
import os
import re
import sys
import tempfile
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
RESULT_SCREEN_SOURCE = REPO_ROOT / "src" / "ResultScreen.cpp"
STOCK_PROFILE_TOOL = REPO_ROOT / "tools" / "th08_stock_font_profile.py"
NAME_ENTRY_SYMBOL = "g_AlphabetList"
POLICY_VERSION = "th08-psp-msgothic-subset-v1"
LOCAL_ONLY_NOTICE = (
    "MS Gothic and subsets derived from it are local-use-only Microsoft font "
    "artifacts. Do not commit them or place them in a release archive."
)

# Required independently of observed gameplay text. U+0020..U+007E covers the
# complete score/replay input table today, but the source table is also parsed
# and gated below so a future extension cannot silently lose a glyph. Japanese
# and fullwidth symbols come from the authoritative stock archive/source scan,
# not from an invented Unicode block range.
MANDATORY_RANGES: tuple[tuple[str, int, int], ...] = (
    ("ascii-printable", 0x0020, 0x007E),
)

RASTER_VALIDATION_SIZES = (16, 28, 30, 32)


class SubsetToolError(RuntimeError):
    """Expected user-facing build failure."""


class CoverageError(SubsetToolError):
    """The source or generated font does not cover the required set."""


@dataclass(frozen=True)
class CharacterInput:
    name: str
    sha256: str
    encoding: str
    codepoints: frozenset[int]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalized_codepoints(text: str) -> frozenset[int]:
    """Return rendered Unicode scalars, preserving width distinctions.

    NFC joins canonically equivalent combining sequences but, unlike NFKC,
    never folds fullwidth forms to ASCII. Line/control characters are input
    file structure rather than rendered glyphs and are intentionally ignored.
    """

    result: set[int] = set()
    for char in unicodedata.normalize("NFC", text):
        codepoint = ord(char)
        if 0xD800 <= codepoint <= 0xDFFF:
            raise SubsetToolError(f"character list contains surrogate U+{codepoint:04X}")
        if unicodedata.category(char).startswith("C"):
            continue
        result.add(codepoint)
    return frozenset(result)


def mandatory_codepoints() -> frozenset[int]:
    result: set[int] = set()
    for _name, first, last in MANDATORY_RANGES:
        result.update(range(first, last + 1))
    return frozenset(result)


def extract_c_string_assignment(source: str, symbol: str) -> str:
    """Extract one adjacent-literal C string assignment from trusted source."""

    pattern = re.compile(
        rf"const\s+char\s*\*\s*{re.escape(symbol)}\s*=\s*"
        rf"((?:\"(?:\\.|[^\"\\])*\"\s*)+);"
    )
    match = pattern.search(source)
    if not match:
        raise SubsetToolError(f"cannot extract {symbol} from ResultScreen.cpp")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', match.group(1))
    try:
        value = "".join(ast.literal_eval(literal) for literal in literals)
    except (SyntaxError, ValueError) as exc:
        raise SubsetToolError(f"cannot decode {symbol}: {exc}") from exc
    if not value:
        raise SubsetToolError(f"{symbol} is empty")
    return value


def load_name_entry_charset(path: Path = RESULT_SCREEN_SOURCE) -> tuple[str, str]:
    raw = path.read_bytes()
    source = raw.decode("utf-8")
    return extract_c_string_assignment(source, NAME_ENTRY_SYMBOL), sha256_bytes(raw)


def read_character_input(path: Path, encoding: str) -> CharacterInput:
    raw = path.read_bytes()
    try:
        text = raw.decode(encoding)
    except (LookupError, UnicodeDecodeError) as exc:
        raise SubsetToolError(f"cannot decode character list {path}: {exc}") from exc
    return CharacterInput(
        name=path.name,
        sha256=sha256_bytes(raw),
        encoding=encoding,
        codepoints=normalized_codepoints(text),
    )


def stable_codepoint_hash(codepoints: Iterable[int]) -> str:
    payload = "".join(f"U+{value:04X}\n" for value in sorted(set(codepoints)))
    return sha256_bytes(payload.encode("ascii"))


def format_codepoints(codepoints: Iterable[int]) -> list[str]:
    return [f"U+{value:04X}" if value <= 0xFFFF else f"U+{value:06X}"
            for value in sorted(set(codepoints))]


def _canonical_path_and_existing_anchor(path: Path) -> tuple[Path, Path]:
    """Resolve every existing ancestor and retain a non-existent tail.

    ``Path.resolve(strict=False)`` alone is not a sufficient write boundary on
    DrvFS: alternate case spellings and a symlinked existing ancestor can hide
    that an output is below the repository.  The deepest existing ancestor is
    resolved first and is later compared with ``samefile``.
    """

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
    canonical = anchor.joinpath(*reversed(tail))
    return canonical, anchor


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
        if re.match(r"^/mnt/[A-Za-z](?:/|$)", base_text) or re.match(
            r"^[A-Za-z]:/", base_text
        ):
            candidate_parts = _drvfs_casefold_parts(canonical)
            base_parts = _drvfs_casefold_parts(base)
            return candidate_parts[: len(base_parts)] == base_parts
        return False
    except OSError:
        # A broken/looping symlink is not a trustworthy external output path.
        return True


def _uses_windows_casefold(path: Path) -> bool:
    normalized = os.path.normpath(os.fspath(path)).replace("\\", "/").casefold()
    return bool(
        re.match(r"^/mnt/[a-z](?:/|$)", normalized)
        or re.match(r"^[a-z]:/", normalized)
    )


def paths_refer_to_same_location(first: Path, second: Path) -> bool:
    """Compare existing and future paths without trusting spelling alone."""

    try:
        if os.path.lexists(first) and os.path.lexists(second):
            if os.path.samefile(first, second):
                return True
        first_canonical, _first_anchor = _canonical_path_and_existing_anchor(first)
        second_canonical, _second_anchor = _canonical_path_and_existing_anchor(second)
        if first_canonical == second_canonical:
            return True
        if _uses_windows_casefold(first_canonical) or _uses_windows_casefold(
            second_canonical
        ):
            return _drvfs_casefold_parts(first_canonical) == _drvfs_casefold_parts(
                second_canonical
            )
        return False
    except OSError:
        # If a broken/looping symlink prevents proof of distinctness, do not
        # let --force turn that uncertainty into an overwrite.
        return True


def validate_generated_paths(
    paths: Sequence[Path],
    force: bool,
    protected_inputs: Sequence[Path] = (),
) -> None:
    outputs = tuple(paths)
    for index, first in enumerate(outputs):
        for second in outputs[index + 1 :]:
            if paths_refer_to_same_location(first, second):
                raise SubsetToolError(
                    "output, manifest and coverage paths must be distinct "
                    f"filesystem locations: {first} == {second}"
                )
    for path in outputs:
        if path_is_within(path, REPO_ROOT):
            raise SubsetToolError(
                f"refusing generated font/report path inside repository: {path}"
            )
        for input_path in protected_inputs:
            if paths_refer_to_same_location(path, input_path):
                raise SubsetToolError(
                    "refusing output path that aliases an input, even with --force: "
                    f"output={path} input={input_path}"
                )
        if path.exists() and not force:
            raise SubsetToolError(f"output already exists (use --force): {path}")


def discover_windows_font() -> Path | None:
    candidates: list[Path] = []
    windir = os.environ.get("WINDIR") or os.environ.get("SystemRoot")
    if windir:
        candidates.append(Path(windir) / "Fonts" / "msgothic.ttc")
    candidates.extend(
        (Path("/mnt/c/Windows/Fonts/msgothic.ttc"), Path("C:/Windows/Fonts/msgothic.ttc"))
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def require_fonttools() -> tuple[Any, Any, str]:
    try:
        subset_module = importlib.import_module("fontTools.subset")
        ttlib_module = importlib.import_module("fontTools.ttLib")
        root_module = importlib.import_module("fontTools")
    except ModuleNotFoundError as exc:
        raise SubsetToolError(
            "fontTools is required for this local-only tool. Install it with: "
            f"{sys.executable} -m pip install --user fonttools"
        ) from exc
    return subset_module, ttlib_module, str(getattr(root_module, "__version__", "unknown"))


def load_stock_profile_module() -> Any:
    spec = importlib.util.spec_from_file_location("th08_stock_font_profile", STOCK_PROFILE_TOOL)
    if spec is None or spec.loader is None:
        raise SubsetToolError(f"cannot load stock profile tool: {STOCK_PROFILE_TOOL}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def require_pillow() -> tuple[Any, str]:
    try:
        image_font = importlib.import_module("PIL.ImageFont")
        pillow = importlib.import_module("PIL")
    except ModuleNotFoundError as exc:
        raise SubsetToolError(
            "Pillow is required for local font raster validation. Install it with: "
            f"{sys.executable} -m pip install --user Pillow"
        ) from exc
    return image_font, str(getattr(pillow, "__version__", "unknown"))


def font_names(font: Any) -> set[str]:
    names: set[str] = set()
    if "name" not in font:
        return names
    for record in font["name"].names:
        if record.nameID not in (1, 4, 6):
            continue
        try:
            value = record.toUnicode().strip()
        except Exception:
            continue
        if value:
            names.add(value)
    return names


def open_font_face(source: Path, requested_face: str, ttlib: Any) -> tuple[Any, int, list[str]]:
    with source.open("rb") as stream:
        signature = stream.read(4)
    if signature == b"ttcf":
        collection = ttlib.TTCollection(str(source), lazy=False)
        available: list[str] = []
        requested = requested_face.casefold()
        selected: tuple[Any, int, list[str]] | None = None
        for index, font in enumerate(collection.fonts):
            names = sorted(font_names(font), key=str.casefold)
            available.append(f"{index}: " + ", ".join(names))
            if requested in {name.casefold() for name in names}:
                if selected is not None:
                    raise SubsetToolError(
                        f"font face {requested_face!r} is ambiguous in {source.name}"
                    )
                selected = (font, index, names)
        if selected is None:
            raise SubsetToolError(
                f"font face {requested_face!r} not found; available: {'; '.join(available)}"
            )
        # Keep the collection alive through the selected font object. TTFont
        # eagerly loaded above, so no file handle or lazy table is retained.
        return selected

    font = ttlib.TTFont(str(source), lazy=False, recalcTimestamp=False)
    names = sorted(font_names(font), key=str.casefold)
    requested = requested_face.casefold()
    if requested not in {name.casefold() for name in names}:
        raise SubsetToolError(
            f"font face {requested_face!r} not found; available: {', '.join(names)}"
        )
    return font, 0, names


def _glyph_program_bytes(glyph: Any) -> bytes:
    program = getattr(glyph, "program", None)
    if program is None:
        return b""
    return bytes(program.getBytecode())


def _glyph_outline_signature(font: Any, glyph_name: str) -> tuple[Any, ...]:
    glyf = font["glyf"]
    glyph = glyf[glyph_name]
    coordinates, end_points, flags = glyph.getCoordinates(glyf)
    return (
        int(glyph.numberOfContours),
        tuple((int(x), int(y)) for x, y in coordinates),
        tuple(int(value) for value in end_points),
        bytes(flags),
    )


def validate_structural_equivalence(
    source_font: Any, output_font: Any, codepoints: Iterable[int]
) -> dict[str, Any]:
    requested = tuple(sorted(set(codepoints)))
    source_cmap = source_font.getBestCmap() or {}
    output_cmap = output_font.getBestCmap() or {}
    if set(output_cmap) != set(requested):
        missing = set(requested).difference(output_cmap)
        unexpected = set(output_cmap).difference(requested)
        raise CoverageError(
            "subset cmap is not exact; missing="
            + " ".join(format_codepoints(missing))
            + " unexpected="
            + " ".join(format_codepoints(unexpected))
        )
    for required_table in ("hmtx", "glyf"):
        if required_table not in source_font or required_table not in output_font:
            raise CoverageError(f"cannot validate required TrueType table {required_table}")

    for codepoint in requested:
        source_name = source_cmap.get(codepoint)
        output_name = output_cmap.get(codepoint)
        # Subsetting may replace a synthetic glyph name such as uni000020 with
        # the canonical post-table name `space`. Cmap equivalence is the exact
        # codepoint set plus the metrics/program/outline checks below, not the
        # non-rendered internal glyph label.
        source_metric = source_font["hmtx"].metrics[source_name]
        output_metric = output_font["hmtx"].metrics[output_name]
        if source_metric != output_metric:
            raise CoverageError(f"subset hmtx changed at U+{codepoint:04X}")
        source_glyph = source_font["glyf"][source_name]
        output_glyph = output_font["glyf"][output_name]
        if _glyph_program_bytes(source_glyph) != _glyph_program_bytes(output_glyph):
            raise CoverageError(f"subset glyph instructions changed at U+{codepoint:04X}")
        if _glyph_outline_signature(source_font, source_name) != _glyph_outline_signature(
            output_font, output_name
        ):
            raise CoverageError(f"subset glyph outline changed at U+{codepoint:04X}")

    source_bitmap_tables = [name for name in ("EBDT", "EBLC") if name in source_font]
    output_bitmap_tables = [name for name in ("EBDT", "EBLC") if name in output_font]
    return {
        "status": "passed",
        "checked_codepoints": len(requested),
        "cmap_exact": True,
        "hmtx_exact": True,
        "glyph_instructions_exact": True,
        "glyph_outlines_exact": True,
        "source_embedded_bitmap_tables": source_bitmap_tables,
        "output_embedded_bitmap_tables": output_bitmap_tables,
        "embedded_bitmap_note": (
            "EBDT/EBLC are intentionally retained for TH08's 16px path; "
            "all PSP-used sizes are raster-gated below"
            if source_bitmap_tables == output_bitmap_tables
            else "embedded bitmap tables differ; PSP-used sizes remain raster-gated below"
        ),
    }


def _pillow_glyph_signature(font: Any, char: str) -> tuple[Any, ...]:
    mask, offset = font.getmask2(char, mode="L")
    bbox = font.getbbox(char)
    return (
        tuple(int(value) for value in mask.size),
        tuple(int(value) for value in offset),
        bytes(mask),
        None if bbox is None else tuple(int(value) for value in bbox),
        float(font.getlength(char)),
    )


def validate_raster_equivalence(
    source_path: Path,
    source_face_index: int,
    output_path: Path,
    codepoints: Iterable[int],
) -> dict[str, Any]:
    image_font, pillow_version = require_pillow()
    requested = tuple(sorted(set(codepoints)))
    layout = getattr(getattr(image_font, "Layout", None), "BASIC", None)
    checked = 0
    for size in RASTER_VALIDATION_SIZES:
        kwargs = {"index": source_face_index}
        if layout is not None:
            kwargs["layout_engine"] = layout
        source_font = image_font.truetype(str(source_path), size=size, **kwargs)
        output_kwargs: dict[str, Any] = {}
        if layout is not None:
            output_kwargs["layout_engine"] = layout
        output_font = image_font.truetype(str(output_path), size=size, **output_kwargs)
        for codepoint in requested:
            char = chr(codepoint)
            if _pillow_glyph_signature(source_font, char) != _pillow_glyph_signature(
                output_font, char
            ):
                raise CoverageError(
                    f"subset raster/metrics changed at U+{codepoint:04X}, size {size}"
                )
            checked += 1
    return {
        "status": "passed",
        "pillow_version": pillow_version,
        "sizes": list(RASTER_VALIDATION_SIZES),
        "checked_glyph_renders": checked,
        "pixel_and_metrics_exact": True,
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


def atomic_write_text(path: Path, text: str) -> None:
    atomic_write_bytes(path, text.encode("utf-8"))


def render_coverage_report(manifest: dict[str, Any]) -> str:
    coverage = manifest["coverage"]
    stock = manifest["stock_profile"]
    structural = manifest["validation"]["structural"]
    raster = manifest["validation"]["raster"]
    lines = [
        "TH08 PSP local MS Gothic subset coverage",
        f"policy: {manifest['policy_version']}",
        f"source_sha256: {manifest['source_font']['sha256']}",
        f"face: {manifest['source_font']['selected_face']}",
        f"requested: {coverage['requested_count']}",
        f"source_present: {coverage['source_present_count']}",
        f"source_missing: {coverage['source_missing_count']}",
        f"output_present: {coverage['output_present_count']}",
        f"output_missing: {coverage['output_missing_count']}",
        f"name_entry_charset_count: {coverage['name_entry_charset_count']}",
        f"stock_profile_count: {stock['codepoint_count'] if stock else 0}",
        f"stock_profile_sha256: {stock['codepoint_sha256'] if stock else '(none)'}",
        f"authority_header_match: {stock['authority_header_match'] if stock else False}",
        f"structural_validation: {structural['status']}",
        f"raster_validation: {raster['status']}",
        "raster_sizes: " + ",".join(str(value) for value in raster["sizes"]),
        f"character_set_sha256: {manifest['character_set_sha256']}",
        f"output_sha256: {manifest['output']['sha256']}",
        "",
        "stock_profile_groups:",
    ]
    if stock:
        lines.extend(
            f"{group['name']}: rows={group['row_count']} "
            f"unique_rows={group['unique_row_count']} "
            f"codepoints={group['unique_codepoints']} sha256={group['content_sha256']}"
            for group in stock["groups"]
        )
    else:
        lines.append("(none)")
    lines.extend(("", "source_missing_codepoints:"))
    lines.extend(coverage["source_missing"] or ["(none)"])
    lines.append("")
    lines.append("output_missing_codepoints:")
    lines.extend(coverage["output_missing"] or ["(none)"])
    lines.append("")
    lines.append("requested_codepoints:")
    lines.extend(coverage["requested"])
    lines.append("")
    return "\n".join(lines)


def build_subset(
    source_font: Path,
    character_files: Sequence[Path],
    output: Path,
    manifest_path: Path,
    coverage_path: Path,
    *,
    stock_archive: Path | None = None,
    encoding: str = "utf-8-sig",
    requested_face: str = "MS Gothic",
    force: bool = False,
    allow_missing: bool = False,
    validate_raster: bool = True,
) -> dict[str, Any]:
    protected_inputs = [source_font, *character_files]
    if stock_archive is not None:
        protected_inputs.append(stock_archive)
    validate_generated_paths(
        (output, manifest_path, coverage_path), force, protected_inputs
    )
    if not source_font.is_file():
        raise SubsetToolError(f"source font does not exist: {source_font}")
    if path_is_within(source_font, REPO_ROOT):
        raise SubsetToolError(
            f"refusing proprietary source font inside repository: {source_font}"
        )
    if not character_files and stock_archive is None:
        raise SubsetToolError("a stock --archive or at least one --chars file is required")

    name_entry, result_source_hash = load_name_entry_charset()
    name_entry_codepoints = normalized_codepoints(name_entry)
    stock_profile = None
    if stock_archive is not None:
        stock_module = load_stock_profile_module()
        try:
            stock_profile = stock_module.build_stock_profile(stock_archive)
            stock_module.verify_authority_header(stock_profile)
        except stock_module.StockProfileError as exc:
            raise SubsetToolError(str(exc)) from exc
    required_mandatory = set(mandatory_codepoints() | name_entry_codepoints)
    if stock_profile is not None:
        required_mandatory.update(stock_profile.codepoints)
    inputs = [read_character_input(path, encoding) for path in character_files]
    requested: set[int] = set(required_mandatory)
    for item in inputs:
        requested.update(item.codepoints)

    subset_module, ttlib, fonttools_version = require_fonttools()
    font, face_index, face_names = open_font_face(source_font, requested_face, ttlib)
    try:
        source_cmap = font.getBestCmap() or {}
        source_present = set(requested).intersection(source_cmap)
        source_missing = set(requested).difference(source_cmap)
        mandatory_missing = required_mandatory.difference(source_cmap)
        if mandatory_missing:
            raise CoverageError(
                "source font is missing mandatory/name-entry glyphs: "
                + " ".join(format_codepoints(mandatory_missing))
            )
        if source_missing and not allow_missing:
            raise CoverageError(
                "source font is missing requested glyphs; use a complete list/font or "
                "explicit --allow-missing: "
                + " ".join(format_codepoints(source_missing))
            )

        options = subset_module.Options()
        options.recalc_timestamp = False
        options.canonical_order = True
        # TH08 can request the 8px normal path (CreateFontA height 16).  MS
        # Gothic serves an embedded bitmap strike there; dropping EBDT/EBLC
        # changes pixels even though the scalable outline is identical.
        # Preserve and subset the strike tables so the local font remains
        # pixel-identical at every PSP-used size.
        options.drop_tables = [
            table
            for table in options.drop_tables
            if table not in ("EBDT", "EBLC", "EBSC")
        ]
        subsetter = subset_module.Subsetter(options=options)
        subsetter.populate(unicodes=sorted(source_present))
        subsetter.subset(font)
        font.recalcTimestamp = False

        output.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{output.name}.", dir=output.parent
        )
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            font.save(str(temporary), reorderTables=True)
            generated = ttlib.TTFont(
                str(temporary), lazy=False, recalcTimestamp=False
            )
            try:
                output_cmap = generated.getBestCmap() or {}
                output_present = set(requested).intersection(output_cmap)
                output_missing = set(source_present).difference(output_cmap)
                if output_missing:
                    raise CoverageError(
                        "subset output lost source-covered glyphs: "
                        + " ".join(format_codepoints(output_missing))
                    )
                validation_source, _validation_index, _validation_names = open_font_face(
                    source_font, requested_face, ttlib
                )
                try:
                    structural_validation = validate_structural_equivalence(
                        validation_source, generated, source_present
                    )
                finally:
                    validation_source.close()
            finally:
                generated.close()
            if validate_raster:
                raster_validation = validate_raster_equivalence(
                    source_font, face_index, temporary, source_present
                )
            else:
                raster_validation = {
                    "status": "skipped-explicitly",
                    "sizes": list(RASTER_VALIDATION_SIZES),
                    "checked_glyph_renders": 0,
                    "pixel_and_metrics_exact": False,
                }
            os.replace(temporary, output)
        finally:
            if temporary.exists():
                temporary.unlink()
    finally:
        font.close()

    sorted_inputs = sorted(
        (
            {
                "name": item.name,
                "sha256": item.sha256,
                "encoding": item.encoding,
                "unique_codepoints": len(item.codepoints),
            }
            for item in inputs
        ),
        key=lambda item: (item["name"].casefold(), item["sha256"]),
    )
    manifest: dict[str, Any] = {
        "schema_version": 1,
        "policy_version": POLICY_VERSION,
        "local_only_notice": LOCAL_ONLY_NOTICE,
        "fonttools_version": fonttools_version,
        "normalization": "NFC (format/control characters ignored)",
        "source_font": {
            "filename": source_font.name,
            "sha256": sha256_file(source_font),
            "collection_index": face_index,
            "selected_face": requested_face,
            "face_names": face_names,
        },
        "character_inputs": sorted_inputs,
        "name_entry_charset": {
            "source": "src/ResultScreen.cpp",
            "source_sha256": result_source_hash,
            "symbol": NAME_ENTRY_SYMBOL,
            "table_length": len(name_entry),
            "unique_codepoints": len(name_entry_codepoints),
            "codepoint_sha256": stable_codepoint_hash(name_entry_codepoints),
        },
        "stock_profile": (
            {
                "version": stock_module.PROFILE_VERSION,
                "archive_filename": stock_profile.archive_filename,
                "archive_sha256": stock_profile.archive_sha256,
                "codepoint_count": len(stock_profile.codepoints),
                "codepoint_sha256": stock_profile.codepoint_sha256,
                "authority_header": "src/Th08FontCoverage.hpp",
                "authority_header_match": True,
                "groups": [
                    {
                        "name": group.name,
                        "sources": list(group.sources),
                        "row_count": group.row_count,
                        "unique_row_count": group.unique_row_count,
                        "unique_codepoints": len(group.codepoints),
                        "content_sha256": group.content_sha256,
                    }
                    for group in stock_profile.groups
                ],
            }
            if stock_profile is not None
            else None
        ),
        "mandatory_ranges": [
            {"name": name, "first": f"U+{first:04X}", "last": f"U+{last:04X}"}
            for name, first, last in MANDATORY_RANGES
        ],
        "character_set_sha256": stable_codepoint_hash(requested),
        "coverage": {
            "requested_count": len(requested),
            "source_present_count": len(source_present),
            "source_missing_count": len(source_missing),
            "output_present_count": len(output_present),
            "output_missing_count": len(output_missing),
            "name_entry_charset_count": len(name_entry_codepoints),
            "requested": format_codepoints(requested),
            "source_missing": format_codepoints(source_missing),
            "output_missing": format_codepoints(output_missing),
        },
        "output": {
            "filename": output.name,
            "bytes": output.stat().st_size,
            "sha256": sha256_file(output),
        },
        "validation": {
            "structural": structural_validation,
            "raster": raster_validation,
        },
    }
    manifest_text = json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    atomic_write_text(manifest_path, manifest_text)
    atomic_write_text(coverage_path, render_coverage_report(manifest))
    return manifest


def default_report_paths(output: Path) -> tuple[Path, Path]:
    return (
        output.with_name(output.name + ".manifest.json"),
        output.with_name(output.name + ".coverage.txt"),
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a deterministic local-only MS Gothic TTF subset for TH08 PSP.",
        epilog=LOCAL_ONLY_NOTICE,
    )
    parser.add_argument(
        "--font",
        type=Path,
        help="Windows msgothic.ttc (auto-detected from WINDIR or /mnt/c when omitted)",
    )
    parser.add_argument(
        "--archive",
        type=Path,
        help=(
            "local stock TH08 PBGZ archive; defaults to the analysis artifact "
            "when present"
        ),
    )
    parser.add_argument(
        "--chars",
        type=Path,
        action="append",
        default=[],
        help="optional UTF text with additional local/mod characters; repeatable",
    )
    parser.add_argument("--encoding", default="utf-8-sig", help="--chars encoding")
    parser.add_argument("--face", default="MS Gothic", help="TTC face name")
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="repo-external output path named msgothic-subset.ttf",
    )
    parser.add_argument("--manifest", type=Path, help="deterministic JSON report outside repo")
    parser.add_argument("--coverage", type=Path, help="coverage text report outside repo")
    parser.add_argument("--allow-missing", action="store_true")
    parser.add_argument(
        "--skip-raster-validation",
        action="store_true",
        help="explicitly skip Pillow pixel/metric equality gate (recorded in manifest)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help=(
            "replace existing generated outputs only; input aliases remain "
            "forbidden (prefer a fresh candidate directory)"
        ),
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.output.name != "msgothic-subset.ttf":
        raise SubsetToolError("--output filename must be msgothic-subset.ttf")
    source_font = args.font or discover_windows_font()
    if source_font is None:
        raise SubsetToolError(
            "cannot locate Windows Fonts/msgothic.ttc; pass --font explicitly"
        )
    stock_module = load_stock_profile_module()
    stock_archive = args.archive
    if stock_archive is None and stock_module.DEFAULT_STOCK_ARCHIVE.is_file():
        stock_archive = stock_module.DEFAULT_STOCK_ARCHIVE
    if stock_archive is None:
        raise SubsetToolError(
            "stock TH08 archive is required for the 1531-codepoint authority gate; "
            "pass --archive"
        )
    default_manifest, default_coverage = default_report_paths(args.output)
    print(f"NOTICE: {LOCAL_ONLY_NOTICE}", file=sys.stderr)
    manifest = build_subset(
        source_font,
        args.chars,
        args.output,
        args.manifest or default_manifest,
        args.coverage or default_coverage,
        stock_archive=stock_archive,
        encoding=args.encoding,
        requested_face=args.face,
        force=args.force,
        allow_missing=args.allow_missing,
        validate_raster=not args.skip_raster_validation,
    )
    coverage = manifest["coverage"]
    print(
        f"wrote {args.output} ({manifest['output']['bytes']} bytes, "
        f"{coverage['output_present_count']}/{coverage['requested_count']} glyphs, "
        f"sha256={manifest['output']['sha256']})"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SubsetToolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
