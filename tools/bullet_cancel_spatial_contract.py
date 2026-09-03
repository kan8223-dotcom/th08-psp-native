#!/usr/bin/env python3
"""Fixed telemetry/route contract for the PSP Bullet cancel spatial gate."""

FEATURE_MACRO = "TH08_PSP_BULLET_CANCEL_SPATIAL"
FEATURE_FIELD = "BULLET_CANCEL_SPATIAL"
TELEMETRY_PREFIX = "BULLET_CANCEL_SPATIAL_TELEMETRY"
EXPECTED_KEYS = tuple((5, 301 + 300 * index) for index in range(20))
REQUIRED_BOOTSTRAP_ITERATIONS = 1_000_000

INTERVAL_FIELDS = (
    "calls", "indexed_queries", "rejected_queries", "fallbacks",
    "rebuilds", "circles", "rects", "full_candidates",
    "indexed_candidates", "fallback_candidates", "exact_tests",
    "false_positives", "occupancy_owner_fallbacks",
    "unsupported_region_fallbacks", "nonfinite_fallbacks",
    "duplicate_pairs", "duplicate_replays",
    "duplicate_exact_tests_saved", "duplicate_fallbacks",
)
