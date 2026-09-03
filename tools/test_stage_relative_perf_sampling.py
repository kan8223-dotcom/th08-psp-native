#!/usr/bin/env python3
"""Host-only source and scheduler gates for stage-relative PSP perf sampling."""

from __future__ import annotations

import unittest
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "psp" / "memory_telemetry.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "psp" / "memory_telemetry.hpp").read_text(encoding="utf-8")
D3D_SOURCE = (ROOT / "src" / "modern" / "linux" / "d3d8_compat.cpp").read_text(
    encoding="utf-8"
)
PERIOD = 300
U32_MAX = (1 << 32) - 1


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


@dataclass
class SampleEvent:
    kind: str
    stage: int
    stage_frame: int
    baseline_stage_frame: int | None = None


class StageRelativeSamplerModel:
    """Executable policy model, independent of the target C++ implementation."""

    def __init__(self) -> None:
        self.stage: int | None = None
        self.last_stage_frame = 0
        self.baseline_stage_frame: int | None = None
        self.next_sample_stage_frame: int | None = None
        self.interval_discards = 0
        self.interval_presents = 0
        self.samples: list[SampleEvent] = []
        self.sample_render_frames: list[int] = []

    def _rearm(self, stage: int, stage_frame: int) -> None:
        self.stage = stage
        self.last_stage_frame = stage_frame
        self.baseline_stage_frame = None
        self.next_sample_stage_frame = None

    def _baseline(self, stage: int, stage_frame: int) -> SampleEvent:
        self.interval_discards += 1
        self.interval_presents = 0
        self.stage = stage
        self.last_stage_frame = stage_frame
        self.baseline_stage_frame = stage_frame
        self.next_sample_stage_frame = (
            stage_frame + PERIOD if stage_frame <= U32_MAX - PERIOD else None
        )
        return SampleEvent("baseline", stage, stage_frame, stage_frame)

    def observe(self, stage: int, stage_frame: int) -> SampleEvent | None:
        # The scheduler observes only after the real Present/EndFrame pair.
        self.interval_presents += 1
        stage_changed = self.stage is None or stage != self.stage
        rewound = (
            self.stage is not None
            and not stage_changed
            and stage_frame < self.last_stage_frame
        )
        if stage_changed or rewound or stage_frame == 0:
            self._rearm(stage, stage_frame)
        if stage_frame == 0:
            return None
        if self.baseline_stage_frame is None:
            return self._baseline(stage, stage_frame)
        if stage_frame == self.last_stage_frame:
            return None
        self.last_stage_frame = stage_frame
        target = self.next_sample_stage_frame
        if target is None or stage_frame < target:
            return None
        if stage_frame > target:
            return self._baseline(stage, stage_frame)

        baseline = self.baseline_stage_frame
        event = SampleEvent("sample", stage, stage_frame, baseline)
        self.samples.append(event)
        self.sample_render_frames.append(self.interval_presents)
        self.interval_presents = 0
        self.baseline_stage_frame = stage_frame
        self.next_sample_stage_frame = (
            stage_frame + PERIOD if stage_frame <= U32_MAX - PERIOD else None
        )
        return event

    def mark_phase(self) -> tuple[int | None, int | None, int, int]:
        """MARK must be a pure observation of scheduler state."""
        return (
            self.stage,
            self.baseline_stage_frame,
            self.last_stage_frame,
            self.interval_discards,
        )


class StageRelativePerfSourceTests(unittest.TestCase):
    def test_gate_defaults_off_and_legacy_scheduler_is_the_complete_else_path(self) -> None:
        self.assertIn("#define TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING 0", HEADER)
        sample = function_body(SOURCE, "void MemoryTelemetrySampleGameFrame()")
        after_present = function_body(SOURCE, "void MemoryTelemetryAfterPresent()")
        self.assertIn("#if !TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING", sample)
        self.assertIn(
            "if (gGameFrame - gLastSampleFrame < kSamplePeriodFrames)", sample
        )
        self.assertIn('LogSnapshot("SAMPLE", "periodic"', sample)
        self.assertIn("#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING", after_present)
        self.assertNotIn("STAGE_RELATIVE_PERF_BASELINE", sample)

    def test_first_positive_completed_present_discards_interval_and_logs_anchor(self) -> None:
        baseline = function_body(SOURCE, "void BeginStageRelativePerfBaseline(")
        self.assertIn(
            "(void)th08::psp::RenderPerfTelemetryTakeInterval();", baseline
        )
        self.assertIn("gLastSampleTimeUs = sceKernelGetSystemTimeWide();", baseline)
        self.assertIn("gLastSampleFrame = gGameFrame;", baseline)
        self.assertLess(
            baseline.index("STAGE_RELATIVE_PERF_BASELINE"),
            baseline.index("gLastSampleTimeUs = sceKernelGetSystemTimeWide();"),
        )
        self.assertIn("baseline_stage_frame=%lu", baseline)
        self.assertIn("next_sample_stage_frame=%lu", baseline)
        self.assertIn("stageFrame + kSamplePeriodFrames", baseline)
        self.assertIn("MemoryTelemetryAfterPresent runs only after EndFrame", baseline)
        self.assertNotIn("RenderPerfTelemetryReset", baseline)
        self.assertNotIn("gRenderPerfCurrentFrame", baseline)

    def test_exact_target_duplicate_and_missed_target_guards_are_explicit(self) -> None:
        sample = function_body(SOURCE, "void MemoryTelemetryAfterPresent()")
        self.assertIn(
            "stageFrame == gStageRelativePerfSampling.lastObservedStageFrame", sample
        )
        self.assertIn("if (target == 0U || stageFrame < target)", sample)
        self.assertIn("if (stageFrame > target)", sample)
        self.assertIn('BeginStageRelativePerfBaseline(stage, stageFrame, "missed_target")', sample)
        self.assertIn('LogSnapshot("SAMPLE", "stage_relative_periodic"', sample)
        self.assertIn("elapsed_stage_frames=%lu", sample)
        self.assertIn("render_frames=%lu", sample)

    def test_stage_change_rewind_and_zero_rearm_without_simulation_writes(self) -> None:
        sample = function_body(SOURCE, "void MemoryTelemetryAfterPresent()")
        self.assertIn("stageChanged || stageRewound || stageFrame == 0U", sample)
        self.assertIn("RearmStageRelativePerfSampling(stage, stageFrame);", sample)
        relative = sample[sample.index("#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING") :]
        self.assertNotIn("stageActiveFrames =", relative)
        self.assertNotIn("currentStage =", relative)
        for forbidden in ("Rng", "rand(", "score =", "playerState ="):
            self.assertNotIn(forbidden, relative)

    def test_present_hook_closes_render_frame_before_stage_scheduler(self) -> None:
        self.assertIn("void MemoryTelemetryAfterPresent();", HEADER)
        end_frame = D3D_SOURCE.index("th08::psp::RenderPerfTelemetryEndFrame();")
        after_present = D3D_SOURCE.index(
            "th08::psp::MemoryTelemetryAfterPresent();", end_frame
        )
        non_psp_branch = D3D_SOURCE.index("#else", end_frame)
        self.assertLess(end_frame, after_present)
        self.assertLess(after_present, non_psp_branch)
        self.assertIn("boundary=after_present", SOURCE)
        self.assertIn("baseline_present=discarded", SOURCE)
        self.assertIn("interval=next_300_complete_presents", SOURCE)

    def test_phase_marks_do_not_consume_or_move_interval(self) -> None:
        mark = function_body(SOURCE, "void MemoryTelemetryMarkPhase(")
        self.assertNotIn("RenderPerfTelemetryTakeInterval", mark)
        self.assertNotIn("gStageRelativePerfSampling", mark)
        self.assertNotIn("gLastSampleFrame =", mark)
        self.assertNotIn("gLastSampleTimeUs =", mark)


class StageRelativePerfModelTests(unittest.TestCase):
    def test_normal_stage_anchors_at_one_and_samples_301_601(self) -> None:
        model = StageRelativeSamplerModel()
        self.assertIsNone(model.observe(5, 0))
        baseline = model.observe(5, 1)
        self.assertEqual(baseline, SampleEvent("baseline", 5, 1, 1))
        for frame in range(2, 602):
            model.observe(5, frame)
        self.assertEqual(
            [(event.baseline_stage_frame, event.stage_frame) for event in model.samples],
            [(1, 301), (301, 601)],
        )
        self.assertEqual(model.sample_render_frames, [300, 300])

    def test_late_first_observation_is_explicit_and_still_exact(self) -> None:
        model = StageRelativeSamplerModel()
        baseline = model.observe(2, 2)
        self.assertEqual(baseline, SampleEvent("baseline", 2, 2, 2))
        for frame in range(3, 303):
            event = model.observe(2, frame)
        self.assertEqual(event, SampleEvent("sample", 2, 302, 2))
        self.assertEqual(model.sample_render_frames, [300])

    def test_terminal_stall_never_duplicates_a_sample(self) -> None:
        model = StageRelativeSamplerModel()
        model.observe(5, 1)
        for frame in range(2, 302):
            model.observe(5, frame)
        self.assertEqual([event.stage_frame for event in model.samples], [301])
        for _ in range(1000):
            self.assertIsNone(model.observe(5, 301))
        self.assertEqual([event.stage_frame for event in model.samples], [301])

    def test_zero_stage_change_and_rewind_each_rearm(self) -> None:
        model = StageRelativeSamplerModel()
        model.observe(1, 1)
        for frame in range(2, 75):
            model.observe(1, frame)
        self.assertIsNone(model.observe(1, 0))
        self.assertEqual(model.observe(1, 1), SampleEvent("baseline", 1, 1, 1))
        self.assertEqual(model.observe(2, 4), SampleEvent("baseline", 2, 4, 4))
        for frame in range(5, 80):
            model.observe(2, frame)
        self.assertEqual(model.observe(2, 20), SampleEvent("baseline", 2, 20, 20))
        self.assertEqual(model.interval_discards, 4)
        self.assertEqual(model.samples, [])

    def test_missed_target_is_discarded_not_mislabeled(self) -> None:
        model = StageRelativeSamplerModel()
        model.observe(3, 1)
        event = model.observe(3, 302)
        self.assertEqual(event, SampleEvent("baseline", 3, 302, 302))
        self.assertEqual(model.samples, [])
        for frame in range(303, 603):
            event = model.observe(3, frame)
        self.assertEqual(event, SampleEvent("sample", 3, 602, 302))

    def test_phase_marks_are_scheduler_noops(self) -> None:
        model = StageRelativeSamplerModel()
        model.observe(4, 7)
        before = model.mark_phase()
        for _ in range(10):
            self.assertEqual(model.mark_phase(), before)
        self.assertEqual(model.observe(4, 307), SampleEvent("sample", 4, 307, 7))


if __name__ == "__main__":
    unittest.main()
