#include "ge2d_direct.hpp"
#include "ge_draw_direct.hpp"

#if TH08_PSP_GE_2D_DIRECT_ENABLED
#include <GLES/egl.h>
#include <pspkernel.h>
#include <cstdint>
#include <cstring>
extern "C"
{
#include "pspgl_internal.h"
#include "pspgl_buffers.h"
#include "pspgl_texobj.h"
#include "pspgl_dlist.h"
}

namespace
{
unsigned long gSubmits = 0UL, gFallbacks = 0UL, gVertices = 0UL;

inline float RegFloat(std::uint32_t word)
{
    const std::uint32_t bits = (word & 0x00ffffffu) << 8;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace

extern "C" int th08_ge2d_direct_submit(const Th08Ge2dVertex *src, unsigned vertexCount, Th08Ge2dVertex *dst,
                                       const unsigned short *indices, unsigned indexCount, float fitLeft,
                                       float fitScaleX, float scaleY)
{
    struct pspgl_context *c = pspgl_curctx;
    if (c == NULL || src == NULL || dst == NULL || indices == NULL || vertexCount == 0U ||
        vertexCount > 0x10000u || indexCount == 0U || indexCount > 0xffffu || (indexCount % 3u) != 0u ||
        __pspgl_actuallist != NULL)
    {
        ++gFallbacks;
        return 0;
    }
    float texW = 1.0f, texH = 1.0f;
    bool flipped = false;
    if ((c->hw.ge_reg[CMD_ENA_TEXTURE] & 1u) != 0u)
    {
        struct pspgl_texobj *tobj = c->texture.bound;
        if (tobj == NULL || tobj->images[0] == NULL ||
            (!TH08_PSP_GE_DRAW_DIRECT_ENABLED && __pspgl_texobj_cmap(tobj) != NULL))
        {
            ++gFallbacks;
            return 0;
        }
        texW = static_cast<float>(tobj->images[0]->width);
        texH = static_cast<float>(tobj->images[0]->height);
        flipped = (tobj->flags & TOF_FLIPPED) != 0u;
    }
    // GE viewport depth mapping as PSPGL programmed it (depth = sz*ndc + tz,
    // ndc = -z for the GL ortho near=-1/far=1 the 3D path used).
    const float sz = RegFloat(c->hw.ge_reg[CMD_VIEWPORT_SZ]);
    const float tz = RegFloat(c->hw.ge_reg[CMD_VIEWPORT_TZ]);
    for (unsigned i = 0u; i < vertexCount; ++i)
    {
        const Th08Ge2dVertex s = src[i];
        Th08Ge2dVertex &d = dst[i];
        d.u = s.u * texW;
        d.v = (flipped ? 1.0f - s.v : s.v) * texH;
        d.r = s.r; d.g = s.g; d.b = s.b; d.a = s.a;
        d.x = fitLeft + s.x * fitScaleX;
        d.y = s.y * scaleY;
        d.z = sz * (-s.z) + tz;
    }
    sceKernelDcacheWritebackRange(dst, vertexCount * sizeof(Th08Ge2dVertex));
    sceKernelDcacheWritebackRange(indices, indexCount * sizeof(unsigned short));
    // Everything the compat layer changed through gl* (blend, alpha, depth,
    // scissor, texture regs) is still pending in PSPGL's shadow: emit it.
#if TH08_PSP_GE_DRAW_DIRECT_ENABLED
    th08_ge_draw_direct_flush_state();
#else
    __pspgl_context_flush_pending_state_changes(c, 0, 255);
#endif
    const std::uint32_t vtype = GE_TEXTURE_32BITF | GE_COLOR_8888 | GE_VERTEX_32BITF | GE_TRANSFORM_2D | GE_VINDEX_16BIT;
    __pspgl_context_writereg_uncached(c, CMD_VERTEXTYPE, vtype);
    __pspgl_context_writereg_uncached(c, CMD_BASE, (reinterpret_cast<unsigned>(dst) >> 8) & 0x000f0000u);
    __pspgl_context_writereg_uncached(c, CMD_VERTEXPTR, reinterpret_cast<unsigned>(dst) & 0x00ffffffu);
    __pspgl_context_writereg_uncached(c, CMD_BASE, (reinterpret_cast<unsigned>(indices) >> 8) & 0x000f0000u);
    __pspgl_context_writereg_uncached(c, CMD_INDEXPTR, reinterpret_cast<unsigned>(indices) & 0x00ffffffu);
    __pspgl_context_writereg_uncached(c, CMD_PRIM, (static_cast<std::uint32_t>(GE_TRIANGLES) << 16) | indexCount);
    __pspgl_context_pin_buffers(c);
    ++gSubmits;
    gVertices += vertexCount;
    return 1;
}

extern "C" void th08_ge2d_direct_stats(unsigned long *submits, unsigned long *fallbacks, unsigned long *vertices)
{
    *submits = gSubmits;
    *fallbacks = gFallbacks;
    *vertices = gVertices;
}

extern "C" int th08_ge2d_direct_params(Th08Ge2dParams *params)
{
    struct pspgl_context *c = pspgl_curctx;
    if (c == NULL || params == NULL || __pspgl_actuallist != NULL)
    {
        ++gFallbacks;
        return 0;
    }
    params->texW = 1.0f;
    params->texH = 1.0f;
    params->flipped = 0;
    if ((c->hw.ge_reg[CMD_ENA_TEXTURE] & 1u) != 0u)
    {
        struct pspgl_texobj *tobj = c->texture.bound;
        if (tobj == NULL || tobj->images[0] == NULL ||
            (!TH08_PSP_GE_DRAW_DIRECT_ENABLED && __pspgl_texobj_cmap(tobj) != NULL))
        {
            ++gFallbacks;
            return 0;
        }
        params->texW = static_cast<float>(tobj->images[0]->width);
        params->texH = static_cast<float>(tobj->images[0]->height);
        params->flipped = (tobj->flags & TOF_FLIPPED) != 0u ? 1 : 0;
    }
    params->sz = RegFloat(c->hw.ge_reg[CMD_VIEWPORT_SZ]);
    params->tz = RegFloat(c->hw.ge_reg[CMD_VIEWPORT_TZ]);
    return 1;
}

extern "C" int th08_ge2d_direct_submit_prepared(const Th08Ge2dVertex *dst, unsigned vertexCount,
                                                const unsigned short *indices, unsigned indexCount)
{
    struct pspgl_context *c = pspgl_curctx;
    if (c == NULL || dst == NULL || indices == NULL || vertexCount == 0U || vertexCount > 0x10000u ||
        indexCount == 0U || indexCount > 0xffffu || (indexCount % 3u) != 0u)
    {
        ++gFallbacks;
        return 0;
    }
    sceKernelDcacheWritebackRange(dst, vertexCount * sizeof(Th08Ge2dVertex));
    sceKernelDcacheWritebackRange(indices, indexCount * sizeof(unsigned short));
#if TH08_PSP_GE_DRAW_DIRECT_ENABLED
    th08_ge_draw_direct_flush_state();
#else
    __pspgl_context_flush_pending_state_changes(c, 0, 255);
#endif
    const std::uint32_t vtype = GE_TEXTURE_32BITF | GE_COLOR_8888 | GE_VERTEX_32BITF | GE_TRANSFORM_2D | GE_VINDEX_16BIT;
    __pspgl_context_writereg_uncached(c, CMD_VERTEXTYPE, vtype);
    __pspgl_context_writereg_uncached(c, CMD_BASE, (reinterpret_cast<unsigned>(dst) >> 8) & 0x000f0000u);
    __pspgl_context_writereg_uncached(c, CMD_VERTEXPTR, reinterpret_cast<unsigned>(dst) & 0x00ffffffu);
    __pspgl_context_writereg_uncached(c, CMD_BASE, (reinterpret_cast<unsigned>(indices) >> 8) & 0x000f0000u);
    __pspgl_context_writereg_uncached(c, CMD_INDEXPTR, reinterpret_cast<unsigned>(indices) & 0x00ffffffu);
    __pspgl_context_writereg_uncached(c, CMD_PRIM, (static_cast<std::uint32_t>(GE_TRIANGLES) << 16) | indexCount);
    __pspgl_context_pin_buffers(c);
    ++gSubmits;
    gVertices += vertexCount;
    return 1;
}
#else
extern "C" int th08_ge2d_direct_submit(const Th08Ge2dVertex *, unsigned, Th08Ge2dVertex *, const unsigned short *,
                                       unsigned, float, float, float)
{
    return 0;
}
extern "C" void th08_ge2d_direct_stats(unsigned long *submits, unsigned long *fallbacks, unsigned long *vertices)
{
    *submits = *fallbacks = *vertices = 0UL;
}
extern "C" int th08_ge2d_direct_params(Th08Ge2dParams *) { return 0; }
extern "C" int th08_ge2d_direct_submit_prepared(const Th08Ge2dVertex *, unsigned, const unsigned short *, unsigned)
{
    return 0;
}
#endif
