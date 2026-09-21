#include "native_ge.hpp"
#include "fileio.hpp"
#include "ge4_bridge.hpp"
#include "perf_attribution.hpp"
#include "perf_env.hpp"
#include "render_resource_arena.hpp"
#include "portrait_lower_policy_20260909_170359.hpp"
#include "usage_meter.hpp"

#include <pspgu.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void th08_psp_auto_cadence_note_wait(unsigned long long);
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
// A nonblocking status query is CPU work, not a GE wait. Match the old
// triple-buffer path's bypass of the blocking-wait attribution wrapper.
extern "C" int __real_sceGeListSync(int, int);
#endif

namespace th08::psp::native
{
namespace
{
constexpr unsigned kFrameBytes = 512 * 272 * 2;
constexpr unsigned kListBytes = 256 * 1024;
constexpr unsigned kLists = 3;
alignas(64) unsigned char lists[kLists][kListBytes];
int fences[kLists] = {-1, -1, -1};
unsigned listIndex;
int openFence = -1;
unsigned publishedBytes;
int lastFence = -1;
bool active, listOpen, cached;
State applied;
void *frame[3];
int frameCount, drawFrame, displayedFrame, pendingFrame = -1;
int pendingFence = -1;
bool pendingRequested;
struct Block { unsigned offset, bytes; } blocks[256];
unsigned long presents, submits;
constexpr unsigned kVertexPages = 2;
constexpr unsigned kVerticesPerPage = 8192;
alignas(64) Vertex vertexPages[kVertexPages][kVerticesPerPage];
int vertexFences[kVertexPages] = {-1, -1};
unsigned vertexPage, vertexCursor, vertexReserved;
Vertex *vertexReservation;
unsigned long vertexDraws, vertexBytes, vertexPageTurns;

unsigned Physical(const void *p) { return reinterpret_cast<unsigned>(p) & 0x1fffffffU; }
void *Vram(unsigned offset) { return reinterpret_cast<void *>(0x04000000U + offset); }
void *Uncached(void *p) { return reinterpret_cast<void *>(reinterpret_cast<unsigned>(p) | 0x40000000U); }
unsigned PowerOfTwo(unsigned value)
{ unsigned result = 1; while (result < value) result <<= 1; return result; }

void Fatal(const char *reason)
{
    BootLog("NATIVE_GE FATAL reason=%s\n", reason);
    FlushBootLog();
    th08_psp_ge4_fail_closed(reason);
}
void Wait(int qid)
{
    if (qid >= 0 && sceGeListSync(qid, 0) < 0) Fatal("list_fence");
}
void Poll()
{
    if (pendingFrame < 0) return;
    bool completed = pendingFence < 0;
    if (!pendingRequested && !completed)
    {
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        completed = __real_sceGeListSync(pendingFence, 1) == PSP_GE_LIST_DONE;
#else
        completed = sceGeListSync(pendingFence, 1) == PSP_GE_LIST_DONE;
#endif
    }
    if (!pendingRequested && completed)
    {
        if (sceDisplaySetFrameBuf(frame[pendingFrame], 512, GU_PSM_5650,
                                 PSP_DISPLAY_SETBUF_NEXTFRAME) < 0) Fatal("display_request");
        pendingRequested = true;
    }
    if (pendingRequested)
    {
        void *top = nullptr; int stride, format;
        if (sceDisplayGetFrameBuf(&top, &stride, &format, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0)
            Fatal("display_query");
        if (Physical(top) == Physical(frame[pendingFrame]))
        {
            displayedFrame = pendingFrame;
            pendingFrame = -1;
            pendingFence = -1;
            pendingRequested = false;
        }
    }
}
void WaitDisplay()
{
    const auto start = Now();
    unsigned long long cadenceWait = 0;
    while (pendingFrame >= 0)
    {
        if (!pendingRequested)
        {
#if !TH08_PSP_PERF_ATTRIBUTION_ENABLED
            const auto geStart = Now();
#endif
            Wait(pendingFence);
#if !TH08_PSP_PERF_ATTRIBUTION_ENABLED
            cadenceWait += Now() - geStart;
#endif
        }
        Poll();
        if (pendingFrame >= 0)
        {
            // NEXTFRAME can latch shortly after the VBlank interrupt. Match
            // the accepted path's bounded short poll before losing a frame.
            for (unsigned i = 0; i < 20 && pendingFrame >= 0; ++i)
            {
                const auto delayStart = Now();
                sceKernelDelayThread(100);
                cadenceWait += Now() - delayStart;
                Poll();
            }
        }
        if (pendingFrame >= 0)
        {
            const auto vblankStart = Now();
            sceDisplayWaitVblankStart();
            cadenceWait += Now() - vblankStart;
        }
        if (Now() - start > 2000000ULL) Fatal("display_timeout");
    }
    const auto duration = Now() - start;
    UsageMeterNoteWait(duration);
    // Blocking GE waits already enter auto cadence through the attribution
    // wrapper. Add only display waits here; never subtract the GE time twice.
    th08_psp_auto_cadence_note_wait(cadenceWait);
#if TH08_PSP_PERF_ENV_ENABLED
    PerfEnvNoteFlipWait(duration);
#endif
}
void Start()
{
    if (listOpen) return;
    Wait(fences[listIndex]);
    fences[listIndex] = -1;
    // GU_SEND builds through the uncached alias without automatic stalls.
    // Publish only complete draws, never a half-written vertex allocation.
    if (sceGuStart(GU_SEND, lists[listIndex]) < 0) Fatal("list_start");
    listOpen = true;
    openFence = -1;
    publishedBytes = 0;
}
void Publish(unsigned bytes)
{
    if (bytes > kListBytes || bytes < publishedBytes) Fatal("list_publish_range");
    if (bytes == publishedBytes) return;
    auto *base = static_cast<unsigned char *>(Uncached(lists[listIndex]));
    if (openFence < 0)
    {
        openFence = sceGeListEnQueue(base, base + bytes, -1, nullptr);
        if (openFence < 0) Fatal("list_enqueue");
        // GE queue IDs are reusable, not generation-tagged. Reissuing an ID
        // proves its former list completed. Retire old owners before Poll or
        // ring reuse can mistake this new stalled stream for their fence.
        for (auto &f : fences) if (f == openFence) f = -1;
        for (auto &f : vertexFences) if (f == openFence) f = -1;
        if (pendingFrame >= 0 && !pendingRequested && pendingFence == openFence)
            pendingFence = -1;
        fences[listIndex] = openFence;
        ++submits;
    }
    else if (sceGeListUpdateStallAddr(openFence, base + bytes) < 0)
        Fatal("list_stall");
    publishedBytes = bytes;
}
void Kick()
{
    if (!listOpen) return;
    const int bytes = sceGuCheckList();
    if (bytes < 0) Fatal("list_size");
    Publish(static_cast<unsigned>(bytes));
    Poll();
}
void Submit()
{
    if (!listOpen) return;
    const int bytes = sceGuFinish();
    if (bytes < 0 || static_cast<unsigned>(bytes) > kListBytes) Fatal("list_overflow");
    // Finish appends FINISH/END and restores the GU context. Use its return
    // size, not CheckList(), and release the existing streaming list's tail.
    Publish(static_cast<unsigned>(bytes));
    lastFence = openFence;
    listOpen = false;
    openFence = -1;
    listIndex = (listIndex + 1) % kLists;
    Poll();
}
void Reserve(unsigned bytes)
{
    if (bytes + 4096 > kListBytes) Fatal("oversize_draw");
    Start();
    if (static_cast<unsigned>(sceGuCheckList()) + bytes + 4096 > kListBytes)
    { Submit(); Start(); }
}
void Enable(int cap, bool enabled) { if (enabled) sceGuEnable(cap); else sceGuDisable(cap); }
unsigned Rgba(unsigned argb)
{ return (argb & 0xff00ff00U) | ((argb & 255U) << 16) | ((argb >> 16) & 255U); }
int Blend(unsigned value)
{
    // D3DBLEND values, source/destination color share GE's other-color slot.
    switch (value) {
    case 3: case 9: return GU_SRC_COLOR;
    case 4: case 10: return GU_ONE_MINUS_SRC_COLOR;
    case 5: return GU_SRC_ALPHA; case 6: return GU_ONE_MINUS_SRC_ALPHA;
    case 7: return GU_DST_ALPHA; case 8: return GU_ONE_MINUS_DST_ALPHA;
    default: return GU_FIX;
    }
}
void Apply(const State &s)
{
    if (!cached || s.blend != applied.blend) Enable(GU_BLEND, s.blend);
    if (!cached || s.sourceBlend != applied.sourceBlend || s.destinationBlend != applied.destinationBlend)
        sceGuBlendFunc(GU_ADD, Blend(s.sourceBlend), Blend(s.destinationBlend),
                       s.sourceBlend == 2 ? 0xffffff : 0,
                       s.destinationBlend == 2 ? 0xffffff : 0);
    if (!cached || s.alphaTest != applied.alphaTest) Enable(GU_ALPHA_TEST, s.alphaTest);
    if (!cached || s.alphaFunction != applied.alphaFunction || s.alphaReference != applied.alphaReference)
    {
        static const int functions[] = {GU_ALWAYS, GU_NEVER, GU_LESS, GU_EQUAL, GU_LEQUAL,
                                        GU_GREATER, GU_NOTEQUAL, GU_GEQUAL, GU_ALWAYS};
        const unsigned reference = static_cast<unsigned char>(255.0f * ((s.alphaReference & 255U) / 255.0f));
        sceGuAlphaFunc(functions[s.alphaFunction <= 8 ? s.alphaFunction : 0], reference, 255);
    }
    if (!cached || s.depthTest != applied.depthTest) Enable(GU_DEPTH_TEST, s.depthTest);
    if (!cached || s.depthFunction != applied.depthFunction)
    {
        // Preserve the accepted TH08 PSPGL comparison (including its strict
        // LEQUAL -> GREATER mapping), not a newly chosen depth convention.
        static const int functions[] = {GU_ALWAYS, GU_NEVER, GU_GEQUAL, GU_EQUAL, GU_GREATER,
                                        GU_LEQUAL, GU_NOTEQUAL, GU_LESS, GU_ALWAYS};
        sceGuDepthFunc(functions[s.depthFunction <= 8 ? s.depthFunction : 0]);
    }
    if (!cached || s.depthWrite != applied.depthWrite) sceGuDepthMask(!s.depthWrite);
    const bool fog = s.fog && !s.through;
    if (!cached || fog != (applied.fog && !applied.through)) Enable(GU_FOG, fog);
    if (fog && (!cached || !applied.fog || applied.through || s.fogColor != applied.fogColor || s.fogStart != applied.fogStart || s.fogEnd != applied.fogEnd))
        sceGuFog(-s.fogStart, -s.fogEnd, Rgba(s.fogColor));
    if (!cached || s.screenSpace != applied.screenSpace ||
        s.left != applied.left || s.top != applied.top ||
        s.width != applied.width || s.height != applied.height)
    {
        const int left = s.screenSpace ? 0 : s.left;
        const int top = s.screenSpace ? 0 : s.top;
        const int width = s.screenSpace ? 480 : s.width;
        const int height = s.screenSpace ? 272 : s.height;
        sceGuOffset(2048 - 240, 2048 - 136);
        sceGuSendCommandf(0x42, width * 0.5f);
        sceGuSendCommandf(0x43, -height * 0.5f);
        sceGuSendCommandf(0x45, 2048 - 240 + left + width * 0.5f);
        sceGuSendCommandf(0x46, 2048 - 136 + top + height * 0.5f);
        sceGuScissor(s.left, s.top, s.width, s.height);
    }
    if (!cached || std::memcmp(s.model, applied.model, sizeof(s.model)))
        sceGuSetMatrix(GU_MODEL, reinterpret_cast<const ScePspFMatrix4 *>(s.model));
    if (!cached || std::memcmp(s.projection, applied.projection, sizeof(s.projection)))
        sceGuSetMatrix(GU_PROJECTION, reinterpret_cast<const ScePspFMatrix4 *>(s.projection));
    if (!cached || s.texture != applied.texture)
    {
        Enable(GU_TEXTURE_2D, s.texture != nullptr);
        if (s.texture)
        {
            Texture &t = *s.texture;
            sceGuTexMode(t.format, 0, 0, t.swizzled);
            sceGuTexImage(0, t.stride, t.storageHeight, t.stride, t.pixels);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexScale(static_cast<float>(t.width) / t.stride,
                          static_cast<float>(t.height) / t.storageHeight);
            sceGuTexOffset(0, 0);
        }
    }
    if (!cached || s.minLinear != applied.minLinear || s.magLinear != applied.magLinear)
        sceGuTexFilter(s.minLinear ? GU_LINEAR : GU_NEAREST, s.magLinear ? GU_LINEAR : GU_NEAREST);
    if (!cached || s.clampU != applied.clampU || s.clampV != applied.clampV)
        sceGuTexWrap(s.clampU ? GU_CLAMP : GU_REPEAT, s.clampV ? GU_CLAMP : GU_REPEAT);
    applied = s; cached = true;
}
unsigned AllocateVram(unsigned begin, unsigned end, unsigned bytes)
{
    unsigned candidate = begin;
    for (;;)
    {
        unsigned next = candidate;
        for (const auto &b : blocks)
            if (b.bytes && candidate < b.offset + b.bytes && candidate + bytes > b.offset)
                if (b.offset + b.bytes > next) next = b.offset + b.bytes;
        if (next == candidate) break;
        candidate = next;
    }
    if (candidate > end || bytes > end - candidate) return 0;
    for (auto &b : blocks)
        if (!b.bytes) { b.offset = candidate; b.bytes = bytes; return candidate; }
    return 0;
}
}

unsigned long long Now() { return sceKernelGetSystemTimeWide(); }
bool Initialize()
{
    if (active) return true;
    vertexPage = vertexCursor = vertexReserved = 0;
    vertexReservation = nullptr;
    vertexDraws = vertexBytes = vertexPageTurns = 0;
    for (auto &f : vertexFences) f = -1;
    const int initialized = sceGuInit();
    if (initialized < 0) { BootLog("NATIVE_GE init=FAIL gu=0x%08x\n", initialized); return false; }
    sceKernelDcacheWritebackInvalidateRange(lists, sizeof(lists));
    sceGuSync(0, 0);
    const bool upper = th08_psp_ge4_enable_after_gu_idle() != 0;
    frame[0] = Vram(0); frame[1] = Vram(kFrameBytes); frame[2] = Vram(0x200000);
    frameCount = 2; drawFrame = 1; displayedFrame = 0;
    const int mode = sceDisplaySetMode(0, 480, 272);
    if (mode < 0) { BootLog("NATIVE_GE init=FAIL mode=0x%08x\n", mode); sceGuTerm(); return false; }
    std::memset(Uncached(frame[0]), 0, kFrameBytes);
    std::memset(Uncached(frame[1]), 0, kFrameBytes);
    // A pixel-format/stride change is legal only on NEXTFRAME. Boot may still
    // have a 8888 display latch; IMMEDIATE would be rejected (0x80000107).
    const int display = sceDisplaySetFrameBuf(frame[0], 512, GU_PSM_5650, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (display < 0) { BootLog("NATIVE_GE init=FAIL display=0x%08x\n", display); sceGuTerm(); return false; }
    const auto displayStart = Now();
    for (;;)
    {
        void *shown = nullptr; int width, format;
        if (sceDisplayGetFrameBuf(&shown, &width, &format, PSP_DISPLAY_SETBUF_IMMEDIATE) >= 0 &&
            Physical(shown) == Physical(frame[0]) && width == 512 && format == GU_PSM_5650) break;
        if (Now() - displayStart > 2000000ULL) Fatal("initial_display_timeout");
        sceDisplayWaitVblankStart();
    }
    if (upper)
    {
        // Initialize the third image before even the short scanout probe.
        // The upper aperture is GE-only; the displayed lower image is a safe
        // read-only source and was zeroed above. Do not clear on later reuse:
        // the game deliberately preserves parts of the previous image.
        Reserve(128);
        sceGuCopyImage(GU_PSM_5650, 0, 0, 512, 272, 512, frame[0],
                       0, 0, 512, frame[2]);
        sceGuTexSync(); Drain();
        void *shown = nullptr; int width, format;
        const int set = sceDisplaySetFrameBuf(frame[2], 512, GU_PSM_5650, PSP_DISPLAY_SETBUF_IMMEDIATE);
        sceDisplayGetFrameBuf(&shown, &width, &format, PSP_DISPLAY_SETBUF_IMMEDIATE);
        if (set >= 0 && Physical(shown) == Physical(frame[2]))
        {
            th08_psp_ge4_note_upper_alloc(frame[2], kFrameBytes);
            frameCount = 3;
        }
        if (sceDisplaySetFrameBuf(frame[0], 512, GU_PSM_5650, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0)
            Fatal("restore_display");
    }
    active = true;
    Start();
    sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(kFrameBytes), 512);
    sceGuDepthBuffer(reinterpret_cast<void *>(2 * kFrameBytes), 512);
    sceGuDispBuffer(480, 272, nullptr, 512);
    sceGuSendCommandi(0x15, 0);
    sceGuSendCommandi(0x16, (271 << 10) | 479);
    sceGuSendCommandf(0x44, -32767.5f);
    sceGuSendCommandf(0x47, 32767.5f);
    sceGuDepthRange(65535, 0);
    // DepthRange rounds half-units; restore the accepted float viewport.
    sceGuSendCommandf(0x44, -32767.5f);
    sceGuSendCommandf(0x47, 32767.5f);
    sceGuDisable(GU_CULL_FACE); sceGuDisable(GU_LIGHTING);
    // Accepted RGB565 output uses PSPGL's default ordered dithering. Turning
    // it off changes every alpha-composited background, not just gradients.
    alignas(16) const int dither[16] = {-4,0,-3,1, 2,-2,3,-1, -3,1,-4,0, 3,-1,2,-2};
    sceGuSetDither(reinterpret_cast<const ScePspIMatrix4 *>(dither));
    sceGuEnable(GU_DITHER);
    sceGuDisable(GU_STENCIL_TEST); sceGuDisable(GU_COLOR_TEST);
    sceGuEnable(GU_SCISSOR_TEST); sceGuEnable(GU_CLIP_PLANES);
    // PSPGL initializes flat shading and this title never overrides it.
    sceGuScissor(0, 0, 480, 272); sceGuShadeModel(GU_FLAT);
    sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
    alignas(16) const float identity[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    sceGuSetMatrix(GU_VIEW, reinterpret_cast<const ScePspFMatrix4 *>(identity));
    sceGuClearColor(0); sceGuClearDepth(0); sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    Drain();
    BootLog("NATIVE_GE init=OK ge4=%d buffers=%d list_bytes=%u list_slots=%u no_pspgl=1 stream=1 borrowed=1 reserved_vertices=1\n",
            upper, frameCount, kListBytes, kLists);
    return true;
}
void Drain()
{
    Submit();
    Wait(lastFence);
    for (auto &f : fences) f = -1;
    for (auto &f : vertexFences) f = -1;
    Poll();
}
void Shutdown()
{
    if (!active) return;
    if (vertexReservation) Fatal("vertex_reservation_at_shutdown");
    Drain();
    BootLog("NATIVE_GE vertices draws=%lu bytes=%lu page_turns=%lu pages=%u page_bytes=%u copied=0\n",
            vertexDraws, vertexBytes, vertexPageTurns, kVertexPages,
            static_cast<unsigned>(sizeof(vertexPages[0])));
    if (pendingFrame >= 0) WaitDisplay();
    // Upper scanout must not survive aperture shutdown.
    sceDisplaySetFrameBuf(frame[0], 512, GU_PSM_5650, PSP_DISPLAY_SETBUF_IMMEDIATE);
    if (frameCount == 3) th08_psp_ge4_note_upper_free(frame[2], kFrameBytes);
    sceGuTerm();
    active = false;
    th08_psp_ge4_shutdown();
}
void PollDisplay() { if (active) Poll(); }
void BeginFrame() { Poll(); }
void WaitPresentedFrame()
{
    // A requested/displayed frame has already passed its GE completion poll.
    // If still rendering, wait for its closed final list, not the open list
    // that Present started for the next frame's draw-buffer commands.
    if (pendingFrame >= 0 && !pendingRequested) Wait(pendingFence);
    Poll();
}
void Present()
{
    Submit();
    if (pendingFrame >= 0) WaitDisplay();
    pendingFrame = drawFrame; pendingFence = lastFence; pendingRequested = false;
    Poll();
    int next = -1;
    for (int i = 0; i < frameCount; ++i)
        if (i != displayedFrame && i != pendingFrame) { next = i; break; }
    if (next < 0)
    {
        WaitDisplay();
        for (int i = 0; i < frameCount; ++i) if (i != displayedFrame) { next = i; break; }
    }
    drawFrame = next;
    Reserve(256);
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void *>(Physical(frame[drawFrame]) - 0x04000000), 512);
    ++presents;
#if TH08_PSP_PERF_ENV_ENABLED
    PerfEnvNoteGeFrameEnd(Now());
#endif
}
void Clear(int left, int top, int right, int bottom, unsigned flags,
           std::uint32_t rgba, float depth, unsigned stencil)
{
    Reserve(1024);
    sceGuScissor(left, top, right - left, bottom - top);
    sceGuDepthMask(0);
    if (depth < 0) depth = 0; else if (depth > 1) depth = 1;
    sceGuClearColor(rgba); sceGuClearDepth(65535U - static_cast<unsigned>(depth * 65535.0f));
    sceGuClearStencil(stencil);
    int mask = 0;
    if (flags & 1) mask |= GU_COLOR_BUFFER_BIT;
    if (flags & 2) mask |= GU_DEPTH_BUFFER_BIT;
    if (flags & 4) mask |= GU_STENCIL_BUFFER_BIT;
    sceGuClear(mask);
    cached = false;
    Kick();
}
bool Draw(const State &s, unsigned primitive, const Vertex *vertices, unsigned count,
          const unsigned short *indices, unsigned indexCount)
{
    if (!vertices || !count || primitive > 6 || count > 65535 || indexCount > 65535 ||
        (indices == nullptr) != (indexCount == 0)) return false;
    const unsigned bytes = count * sizeof(Vertex) + indexCount * sizeof(unsigned short);
    if (bytes + 4096 > kListBytes) return false;
    Reserve(bytes + 256);
    Apply(s);
    void *v = sceGuGetMemory(count * sizeof(Vertex));
    std::memcpy(v, vertices, count * sizeof(Vertex));
    void *ix = nullptr;
    if (indices)
    { ix = sceGuGetMemory(indexCount * sizeof(unsigned short)); std::memcpy(ix, indices, indexCount * sizeof(unsigned short)); }
    sceGuDrawArray(primitive, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF |
                   (s.through ? GU_TRANSFORM_2D : GU_TRANSFORM_3D) | (indices ? GU_INDEX_16BIT : 0),
                   indices ? indexCount : count, ix, v);
    Kick();
    return true;
}
Vertex *ReserveVertices(unsigned capacity)
{
    if (vertexReservation || !capacity || capacity > kVerticesPerPage) return nullptr;
    if (capacity > kVerticesPerPage - vertexCursor)
    {
        const unsigned next = (vertexPage + 1) % kVertexPages;
        // A stalled open list can already reference this page. Close it before
        // waiting; waiting on that unfinished stream would deadlock.
        if (listOpen && openFence >= 0 && vertexFences[next] == openFence) Submit();
        Wait(vertexFences[next]);
        vertexFences[next] = -1;
        vertexPage = next;
        vertexCursor = 0;
        ++vertexPageTurns;
    }
    vertexReserved = capacity;
    vertexReservation = vertexPages[vertexPage] + vertexCursor;
    return vertexReservation;
}
void CancelVertices(const Vertex *vertices)
{
    if (!vertexReservation || vertices != vertexReservation) Fatal("vertex_cancel_owner");
    vertexReservation = nullptr;
    vertexReserved = 0;
}
bool DrawReserved(const State &s, unsigned primitive, const Vertex *vertices, unsigned count,
                  const unsigned short *indices, unsigned indexCount, bool immutableIndices)
{
    if (!vertices || vertices != vertexReservation || !count || count > vertexReserved ||
        primitive > 6 || indexCount > 65535 || (indices == nullptr) != (indexCount == 0))
        return false;
#if !TH08_PSP_NATIVE_VERTEX_DIRECT
    // Reference copy path: same converted pixels, ordinary GU-owned storage.
    // Cancel only after Draw copied the reservation into its command list.
    const bool copied = Draw(s, primitive, vertices, count, indices, indexCount);
    if (copied) CancelVertices(vertices);
    return copied;
#endif
    // Unknown caller index lifetimes still require a command-list copy.
    Reserve((immutableIndices ? 0 : indexCount * sizeof(unsigned short)) + 256);
    Apply(s);
    const void *ix = immutableIndices ? indices : nullptr;
    if (indices && !immutableIndices)
    {
        void *copy = sceGuGetMemory(indexCount * sizeof(unsigned short));
        std::memcpy(copy, indices, indexCount * sizeof(unsigned short));
        ix = copy;
    }
    sceKernelDcacheWritebackRange(vertices, count * sizeof(Vertex));
    sceGuDrawArray(primitive, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF |
                   (s.through ? GU_TRANSFORM_2D : GU_TRANSFORM_3D) | (indices ? GU_INDEX_16BIT : 0),
                   indices ? indexCount : count, ix, vertices);
    Kick();
    // Bind to the actual consuming list, even if Reserve/texture work moved
    // to another command slot since the cached reservation was obtained.
    vertexFences[vertexPage] = openFence;
    vertexCursor = (vertexCursor + count + 7U) & ~7U; // 8 * 24 is cache-line aligned.
    vertexReservation = nullptr;
    vertexReserved = 0;
    ++vertexDraws;
    vertexBytes += count * sizeof(Vertex);
    return true;
}
bool DrawBorrowed(const State &s, const Vertex *vertices, unsigned count,
                  const unsigned short *indices, unsigned indexCount)
{
    // Producer output is written uncached (SC or ME). Never write back a
    // stale cached alias over it. GUI replay also uses this ABI, but owns
    // cached, double-buffered records that must be published before drawing.
    if (!vertices || !indices || !count || count > 65535 || !indexCount ||
        indexCount > 65535 || (indexCount % 3) != 0 ||
        (reinterpret_cast<unsigned>(vertices) & 3U) ||
        (reinterpret_cast<unsigned>(indices) & 1U))
    {
        BootLog("NATIVE_GE borrowed_invalid vertices=%p count=%u indices=%p count=%u\n",
                vertices, count, indices, indexCount);
        return false;
    }
    Reserve(256);
    Apply(s);
    if (!(reinterpret_cast<unsigned>(vertices) & 0x40000000U))
        sceKernelDcacheWritebackRange(vertices, count * sizeof(Vertex));
    sceKernelDcacheWritebackRange(indices, indexCount * sizeof(unsigned short));
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF |
                   GU_TRANSFORM_3D | GU_INDEX_16BIT, indexCount, indices, vertices);
    Kick();
    return true;
}
Texture *CreateTexture(unsigned width, unsigned height, unsigned format, bool swizzled, bool hot, const char *owner)
{
    if (!width || !height || width > 512 || height > 512 || format > 3) return nullptr;
    auto *t = new(std::nothrow) Texture{};
    if (!t) return nullptr;
    t->width = width; t->height = height; t->format = format;
    t->stride = PowerOfTwo(width); t->storageHeight = PowerOfTwo(height);
    const unsigned bpp = format == GU_PSM_8888 ? 4 : 2;
    t->swizzled = swizzled && t->stride * bpp >= 16 && t->storageHeight >= 8;
    t->bytes = (t->stride * t->storageHeight * bpp + 63U) & ~63U;
    unsigned offset = 0;
    if (hot && th08_psp_ge4_active())
        offset = AllocateVram(0x200000 + (frameCount == 3 ? kFrameBytes : 0), 0x400000, t->bytes);
    if (!offset && hot) offset = AllocateVram(3 * kFrameBytes, 0x200000, t->bytes);
#if TH08_PSP_PORTRAIT_LOWER_VRAM_ENABLED
    if (!offset && !hot && swizzled && format != GU_PSM_8888 && th08_psp_ge4_active())
    {
        unsigned available = 0x200000 - 3 * kFrameBytes;
        for (const auto &b : blocks) if (b.bytes && b.offset < 0x200000) available -= b.bytes;
        if (PortraitLowerBytes(owner, width, height, available))
            offset = AllocateVram(3 * kFrameBytes, 0x200000, t->bytes);
    }
#endif
    if (offset)
    {
        t->pixels = Vram(offset); t->upper = offset >= 0x200000;
        if (t->upper) th08_psp_ge4_note_upper_alloc(t->pixels, t->bytes);
    }
    else t->pixels = RenderResourceArenaAllocate(t->bytes, 64, owner);
    if (!t->pixels) { delete t; return nullptr; }
    if (t->upper)
    {
        t->staging = RenderResourceArenaAllocate(t->bytes, 64, "native upper upload");
        if (!t->staging) { DestroyTexture(t); return nullptr; }
    }
    void *cpu = t->upper ? t->staging :
        (Physical(t->pixels) < 0x04400000 ? Uncached(t->pixels) : t->pixels);
    std::memset(cpu, 0, t->bytes);
    return t;
}
void DestroyTexture(Texture *t)
{
    if (!t) return;
    Drain(); // queued GE reads own this backing until the fence completes
    ReleaseTextureRead(t);
    if (t->upper) th08_psp_ge4_note_upper_free(t->pixels, t->bytes);
    const unsigned offset = Physical(t->pixels) - 0x04000000;
    if (offset < 0x400000)
    { for (auto &b : blocks) if (b.bytes && b.offset == offset) { b.bytes = 0; break; } }
    else if (!RenderResourceArenaFree(t->pixels)) std::free(t->pixels);
    if (cached && applied.texture == t) cached = false;
    delete t;
}
void *TexturePixel(Texture *t, unsigned x, unsigned y)
{
    const unsigned bpp = t->format == GU_PSM_8888 ? 4 : 2;
    unsigned byte = y * t->stride * bpp + x * bpp;
    if (t->swizzled)
    {
        const unsigned rowBytes = t->stride * bpp, xb = x * bpp;
        byte = ((y / 8) * (rowBytes / 16) + xb / 16) * 128 + (y & 7) * 16 + (xb & 15);
    }
    auto *base = static_cast<unsigned char *>(t->upper ? t->staging : t->pixels);
    if (!base) Fatal("upper_cpu_mapping_without_staging");
    if (!t->upper && Physical(base) < 0x04400000) base = static_cast<unsigned char *>(Uncached(base));
    return base + byte;
}
static void CopyTextureBytes(void *destination, void *source, unsigned bytes)
{
    const unsigned width = bytes >= 2048 ? 512 : bytes / 4;
    const unsigned height = bytes / (width * 4);
    Reserve(128);
    sceGuCopyImage(GU_PSM_8888, 0, 0, width, height, width, source, 0, 0, width, destination);
    sceGuTexSync(); Drain();
}
bool PrepareTextureWrite(Texture *t)
{
    Drain();
    if (!t->upper || t->staging) return true;
    t->staging = RenderResourceArenaAllocate(t->bytes, 64, "native upper readback");
    if (!t->staging) return false;
    sceKernelDcacheWritebackInvalidateRange(t->staging, t->bytes);
    CopyTextureBytes(t->staging, t->pixels, t->bytes);
    sceKernelDcacheInvalidateRange(t->staging, t->bytes);
    return true;
}
void ReleaseTextureRead(Texture *t)
{
    if (t->staging)
    {
        if (!RenderResourceArenaFree(t->staging)) std::free(t->staging);
        t->staging = nullptr;
    }
}
void TextureChanged(Texture *t)
{
    if (t->upper && t->staging)
    {
        sceKernelDcacheWritebackRange(t->staging, t->bytes);
        CopyTextureBytes(t->pixels, t->staging, t->bytes);
        ReleaseTextureRead(t);
    }
    if (Physical(t->pixels) >= 0x04400000) sceKernelDcacheWritebackRange(t->pixels, t->bytes);
    Reserve(32); sceGuTexFlush(); cached = false;
}
void TextureRowsChanged(Texture *t, unsigned firstRow, unsigned endRow)
{
    if (!t || firstRow >= endRow || endRow > t->height) Fatal("texture_row_range");
    if (t->upper) { TextureChanged(t); return; }
    if (t->swizzled) { firstRow &= ~7U; endRow = (endRow + 7U) & ~7U; }
    const unsigned rowBytes = t->stride * (t->format == GU_PSM_8888 ? 4 : 2);
    if (Physical(t->pixels) >= 0x04400000)
        sceKernelDcacheWritebackRange(static_cast<unsigned char *>(t->pixels) + firstRow * rowBytes,
                                     (endRow - firstRow) * rowBytes);
    Reserve(32); sceGuTexFlush(); cached = false;
}
bool ReadFramebuffer(unsigned short *pixels, unsigned stride, bool displayed)
{
    if (!pixels || stride < 480 || (reinterpret_cast<unsigned>(pixels) & 15U)) return false;
    Drain();
    void *source = frame[drawFrame];
    if (displayed)
    {
        int displayStride, format;
        if (sceDisplayGetFrameBuf(&source, &displayStride, &format, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 ||
            displayStride != 512 || format != GU_PSM_5650) return false;
    }
    sceKernelDcacheWritebackInvalidateRange(pixels, stride * 272 * 2);
    Reserve(128);
    sceGuCopyImage(GU_PSM_5650, 0, 0, 480, 272, 512, source, 0, 0, stride, pixels);
    sceGuTexSync(); Drain();
    sceKernelDcacheInvalidateRange(pixels, stride * 272 * 2);
    return true;
}
} // namespace th08::psp::native
