#!/usr/bin/env python3
"""Naming and fixed-route contract for the ordinary Effect 6V-to-4V gate."""

from __future__ import annotations


FEATURE_MACRO = "TH08_PSP_EFFECT_INDEXED_QUADS"
TELEMETRY_PREFIX = "EFFECT_INDEXED_QUADS_TELEMETRY"
EXPECTED_KEYS = tuple((5, 301 + 300 * index) for index in range(20))
REQUIRED_BOOTSTRAP_ITERATIONS = 1_000_000
MAX_BATCH_QUADS = 0x600

INTERVAL_FIELDS = (
    "passes",
    "flushes",
    "batches",
    "successful_ordinary_quads",
    "canonical_input_vertices",
    "indexed_output_vertices",
    "vertices_saved",
    "bytes_saved",
    "fallbacks",
    "fallback_quads",
    "owner_conflicts",
    "abandoned_passes",
    "abandoned_quads",
    "max_batch_quads",
)

FAILURE_FIELDS = (
    "fallbacks",
    "fallback_quads",
    "owner_conflicts",
    "abandoned_passes",
    "abandoned_quads",
)
