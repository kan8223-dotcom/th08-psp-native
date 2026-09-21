#!/usr/bin/env python3
"""Structural regression gates; replay/image/hardware validation is separate."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
GE = (ROOT / "psp/native_ge.cpp").read_text()
D3D = (ROOT / "src/modern/linux/d3d8_psp_native.cpp").read_text()


def body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if not depth:
                # Comments are not executable evidence.
                return re.sub(r"//[^\n]*", "", source[opening : i + 1])
    raise AssertionError(signature)


class NativeStreamContract(unittest.TestCase):
    def test_producer_submit_does_not_convert_or_copy_vertices(self):
        submit = body(D3D, "bool SubmitProducer(")
        for forbidden in ("converted", "memcpy", "for(", "for (", "Convert("):
            self.assertNotIn(forbidden, submit)
        self.assertIn("ng::DrawBorrowed(s,static_cast<const ng::Vertex *>(raw)", submit)
        self.assertIn("if(ok) CommitProducer()", submit)

    def test_borrowed_draw_preserves_addresses_and_publishes_indices(self):
        draw = body(GE, "bool DrawBorrowed(")
        for forbidden in ("memcpy", "sceGuGetMemory", "GU_TRANSFORM_2D"):
            self.assertNotIn(forbidden, draw)
        self.assertIn("GU_TRANSFORM_3D | GU_INDEX_16BIT, indexCount, indices, vertices", draw)
        self.assertIn("sceKernelDcacheWritebackRange(indices,", draw)
        self.assertRegex(draw, r"if \(!\(reinterpret_cast<unsigned>\(vertices\) & 0x40000000U\)\)\s+sceKernelDcacheWritebackRange\(vertices,")
        self.assertLess(draw.index("sceKernelDcacheWritebackRange"), draw.index("sceGuDrawArray"))
        self.assertLess(draw.index("sceGuDrawArray"), draw.index("Kick()"))

    def test_ortho_is_not_d3d_clip_conversion_or_viewport_relative(self):
        submit = body(D3D, "bool SubmitProducer(")
        for token in (
            "Prepare(s,true,false)", "s.through=false", "s.screenSpace=true", "s.fog=0",
            "s.projection[0]=2.0f/backbuffer->width",
            "s.projection[5]=-2.0f/backbuffer->height",
            "s.projection[10]=-1.0f", "s.projection[12]=-1.0f", "s.projection[13]=1.0f",
        ):
            self.assertIn(token, submit)
        apply = body(GE, "void Apply(")
        self.assertIn("s.screenSpace != applied.screenSpace", apply)
        self.assertIn("s.screenSpace ? 480 : s.width", apply)
        self.assertIn("s.screenSpace ? 272 : s.height", apply)
        self.assertIn("sceGuScissor(s.left, s.top, s.width, s.height)", apply)

    def test_reservation_fences_before_recycling_not_at_present(self):
        reserve = body(D3D, "void *ReserveProducer(")
        self.assertIn("if(producerPresent!=presentCount)", reserve)
        self.assertLess(reserve.index("ng::WaitPresentedFrame()"), reserve.index("reserveCursor=reserveStart=reserveEnd=0"))
        self.assertLess(reserve.index("ng::WaitPresentedFrame()"), reserve.index("producerVertices+reserveStart"))
        present = body(D3D, "HRESULT Present(")
        self.assertIn("++presentCount", present)
        self.assertNotIn("reserveCursor=", present)

    def test_presented_fence_never_waits_for_open_stream(self):
        wait = body(GE, "void WaitPresentedFrame(")
        self.assertIn("pendingFrame >= 0 && !pendingRequested", wait)
        self.assertIn("Wait(pendingFence)", wait)
        self.assertNotIn("openFence", wait)
        self.assertNotIn("Drain", wait)
        self.assertNotIn("WaitDisplay", wait)

    def test_each_draw_publishes_without_finishing_list(self):
        for signature in ("bool Draw(", "bool DrawBorrowed(", "void Clear("):
            draw = body(GE, signature)
            self.assertIn("Kick()", draw)
            self.assertNotIn("Submit()", draw)
        self.assertNotIn("drawsInList", GE)

    def test_finish_releases_existing_stream_using_returned_size(self):
        submit = body(GE, "void Submit(")
        self.assertLess(submit.index("sceGuFinish()"), submit.index("Publish(static_cast<unsigned>(bytes))"))
        self.assertNotIn("sceGuCheckList", submit)
        self.assertNotIn("sceGeListEnQueue", submit)
        self.assertLess(submit.index("Publish("), submit.index("listIndex ="))
        publish = body(GE, "void Publish(")
        self.assertIn("Uncached(lists[listIndex])", publish)
        self.assertIn("if (openFence < 0)", publish)
        self.assertIn("sceGeListUpdateStallAddr(openFence, base + bytes)", publish)

    def test_list_reuse_and_drain_keep_completion_fences(self):
        start = body(GE, "void Start(")
        self.assertLess(start.index("Wait(fences[listIndex])"), start.index("sceGuStart"))
        drain = body(GE, "void Drain(")
        self.assertLess(drain.index("Submit()"), drain.index("Wait(lastFence)"))

    def test_reissued_queue_id_retires_old_frame_and_ring_owners(self):
        publish = body(GE, "void Publish(")
        enqueue = publish.index("openFence = sceGeListEnQueue")
        retire = publish.index("if (f == openFence) f = -1")
        frame = publish.index("pendingFence == openFence")
        owner = publish.index("fences[listIndex] = openFence")
        self.assertLess(enqueue, retire)
        self.assertLess(retire, owner)
        self.assertLess(frame, owner)
        self.assertIn("pendingFence = -1", publish)

    def test_short_display_wait_is_bounded_and_not_cpu_load(self):
        wait = body(GE, "void WaitDisplay(")
        self.assertIn("i < 20 && pendingFrame >= 0", wait)
        self.assertIn("sceKernelDelayThread(100)", wait)
        self.assertIn("cadenceWait += Now() - delayStart", wait)
        self.assertIn("th08_psp_auto_cadence_note_wait(cadenceWait)", wait)


if __name__ == "__main__":
    unittest.main()
