#pragma once

#include <cstdint>
#ifndef TH08_PSP_NATIVE_VERTEX_DIRECT
#define TH08_PSP_NATIVE_VERTEX_DIRECT 1
#endif

// PSP-only rendering contract. No GL/EGL types, contexts, caches or allocators.
// The D3D edge folds texture/color operations and converts D3D matrices once.
namespace th08::psp::native
{
struct Vertex { float u, v; std::uint32_t color; float x, y, z; };
static_assert(sizeof(Vertex) == 24, "GE vertex ABI");
struct Texture
{
    void *pixels;
    void *staging;
    unsigned width, height, stride, storageHeight, format, bytes;
    bool swizzled, upper;
};
struct State
{
    Texture *texture;
    unsigned blend, sourceBlend, destinationBlend;
    unsigned alphaTest, alphaFunction, alphaReference;
    unsigned depthTest, depthFunction, depthWrite;
    unsigned fog, fogColor;
    float fogStart, fogEnd;
    unsigned minLinear, magLinear, clampU, clampV;
    int left, top, width, height;
    float model[16], projection[16];
    bool through;
    // Pretransformed producer vertices use GE ortho over the full screen;
    // left/top/width/height remain the independent D3D scissor rectangle.
    bool screenSpace;
};
bool Initialize();
void Shutdown();
void Drain();
void PollDisplay();
void Present();
void BeginFrame();
// Fence the last presented frame before recycling caller-owned vertices.
// Does not finish or wait for the new frame's still-open command list.
void WaitPresentedFrame();
void Clear(int left, int top, int right, int bottom,
           unsigned flags, std::uint32_t rgba, float depth, unsigned stencil);
bool Draw(const State &, unsigned primitive, const Vertex *, unsigned vertices,
          const unsigned short *indices = nullptr, unsigned indexCount = 0);
// One outstanding cached reservation. Convert directly into this final GE
// storage; DrawReserved publishes it and keeps the page until its last fence.
// Texture uploads/Drain may run between reserve and draw without recycling it.
Vertex *ReserveVertices(unsigned capacity);
void CancelVertices(const Vertex *);
bool DrawReserved(const State &, unsigned primitive, const Vertex *, unsigned vertices,
                  const unsigned short *indices = nullptr, unsigned indexCount = 0,
                  bool immutableIndices = false);
// immutableIndices is only for an aligned, already-published table whose
// address/content survive every consuming GE list (the fixed quad table).
// The caller owns these vertices and immutable cached indices until their
// frame completes. Uncached producer writes are already published; cached
// double-buffered GUI records are written back. Neither path copies data.
bool DrawBorrowed(const State &, const Vertex *, unsigned vertices,
                  const unsigned short *indices, unsigned indexCount);
Texture *CreateTexture(unsigned width, unsigned height, unsigned format,
                       bool swizzled, bool hot, const char *owner);
void DestroyTexture(Texture *);
void TextureChanged(Texture *);
void TextureRowsChanged(Texture *, unsigned firstRow, unsigned endRow);
bool PrepareTextureWrite(Texture *);
void ReleaseTextureRead(Texture *);
// CPU address for the pixel; handles GE's 16-byte x 8-row swizzle layout.
void *TexturePixel(Texture *, unsigned x, unsigned y);
bool ReadFramebuffer(unsigned short *pixels, unsigned stride, bool displayed);
unsigned long long Now();
} // namespace th08::psp::native
