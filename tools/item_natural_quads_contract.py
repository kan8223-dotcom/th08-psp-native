#!/usr/bin/env python3
"""Single naming/schema authority for the proposed ITEM_TIME natural-4V path."""

from __future__ import annotations


# Rename the implementation here only; the static audit and perf gate import it.
FEATURE_MACRO = "TH08_PSP_ITEM_NATURAL_QUADS"
TELEMETRY_PREFIX = "ITEM_NATURAL_QUADS_TELEMETRY"
MODE = "product"

EXPECTED_KEYS = tuple((5, 301 + 300 * index) for index in range(20))
EXPECTED_TEARDOWN_STAGE_FRAME = 6119
REQUIRED_BOOTSTRAP_ITERATIONS = 1_000_000

# Interval counters reset on each stage-relative SAMPLE, matching the existing
# Item frontend diagnostics.  An eligible batch is one existing canonical 6V
# batch containing at least one visible ITEM_TIME; every quad in that same
# batch is packed to unique 4V vertices at its already-existing flush.
INTERVAL_FIELDS = (
    "passes",
    "canonical_batches",
    "item_time_candidates",
    "visible_item_time",
    "culled_item_time",
    "trigger_batches",
    "trigger_quads",
    "coalesced_quads",
    "eligible_quads",
    "submitted_batches",
    "submitted_quads",
    "native_submits",
    "native_submitted_quads",
    "client_fallback_submits",
    "client_fallback_quads",
    "canonical_input_vertices",
    "packed_output_vertices",
    "duplicate_vertices_avoided",
    "fallback_batches",
    "pointer_fallbacks",
    "span_fallbacks",
    "capacity_fallbacks",
    "topology_fallbacks",
    "state_fallbacks",
    "extra_topology_batches",
    "index_fallbacks",
    "native_fallbacks",
    "topology_checks",
    "topology_checked_quads",
    "extra_split_batches",
    "extra_flushes",
    "abandoned_batches",
    "abandoned_quads",
    "max_batch_quads",
)

FAILURE_FIELDS = (
    "fallback_batches",
    "pointer_fallbacks",
    "span_fallbacks",
    "capacity_fallbacks",
    "topology_fallbacks",
    "state_fallbacks",
    "extra_topology_batches",
    "index_fallbacks",
    "extra_split_batches",
    "extra_flushes",
    "abandoned_batches",
    "abandoned_quads",
)
