#include "d3d8_internal.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <d3dx8.h>

#if defined(PSP)
#include "fileio.hpp"
#include "render_resource_arena.hpp"
#endif

#include <math.h>
#include <string.h>
#include <vector>

namespace
{
#if defined(PSP)
struct SurfaceDecodeBreadcrumb
{
    explicit SurfaceDecodeBreadcrumb(UINT inputBytes_)
        : inputBytes(inputBytes_), before(th08::psp::CaptureRenderResourceArenaSnapshot())
    {
    }

    ~SurfaceDecodeBreadcrumb()
    {
        const th08::psp::RenderResourceArenaSnapshot after =
            th08::psp::CaptureRenderResourceArenaSnapshot();
        th08::psp::BootLog(
            "SURFACE_DECODE result=%s stage=%s input=%lu scope=%d size=%lux%lu "
            "arena_before_free=%lu arena_before_largest=%lu "
            "arena_after_free=%lu arena_after_largest=%lu failures=%lu\n",
            success ? "READY" : "FAILED", stage, static_cast<unsigned long>(inputBytes),
            scopeActive ? 1 : 0, static_cast<unsigned long>(width),
            static_cast<unsigned long>(height),
            static_cast<unsigned long>(before.freeBytes),
            static_cast<unsigned long>(before.largestFreeBytes),
            static_cast<unsigned long>(after.freeBytes),
            static_cast<unsigned long>(after.largestFreeBytes),
            static_cast<unsigned long>(after.failureCount));
    }

    UINT inputBytes;
    th08::psp::RenderResourceArenaSnapshot before;
    const char *stage = "begin";
    UINT width = 0;
    UINT height = 0;
    bool scopeActive = false;
    bool success = false;
};
#endif

UINT BytesPerPixel(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: return 2;
    default: return 4;
    }
}

void DecodePixel(const BYTE *source, D3DFORMAT format, BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0]; rgba[3] = 255; break;
    case D3DFMT_R5G6B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 11) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 63) * 255 / 63);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31); rgba[3] = 255; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 10) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 31) * 255 / 31);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31);
        rgba[3] = format == D3DFMT_A1R5G5B5 && !(pixel & 0x8000) ? 0 : 255; break;
    case D3DFMT_A4R4G4B4:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 8) & 15) * 17);
        rgba[1] = static_cast<BYTE>(((pixel >> 4) & 15) * 17);
        rgba[2] = static_cast<BYTE>((pixel & 15) * 17);
        rgba[3] = static_cast<BYTE>(((pixel >> 12) & 15) * 17); break;
    default:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0];
        rgba[3] = format == D3DFMT_X8R8G8B8 ? 255 : source[3]; break;
    }
}

void EncodePixel(BYTE *destination, D3DFORMAT format, const BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0]; break;
    case D3DFMT_R5G6B5:
        pixel = static_cast<WORD>(((rgba[0] * 31 / 255) << 11) |
                                  ((rgba[1] * 63 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        pixel = static_cast<WORD>(((format == D3DFMT_X1R5G5B5 || rgba[3] >= 128) ? 0x8000 : 0) |
                                  ((rgba[0] * 31 / 255) << 10) |
                                  ((rgba[1] * 31 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_A4R4G4B4:
        pixel = static_cast<WORD>(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
                                  ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    default:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0];
        destination[3] = format == D3DFMT_X8R8G8B8 ? 255 : rgba[3]; break;
    }
}

RECT FullRect(UINT width, UINT height)
{
    RECT rect; rect.left = 0; rect.top = 0; rect.right = width; rect.bottom = height; return rect;
}

bool ValidRect(const RECT &rect)
{ return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left && rect.bottom > rect.top; }

HRESULT CopyRgbaToSurface(IDirect3DSurface8 *destinationRaw, const RECT *destinationRectRaw,
                          const BYTE *source, UINT sourceWidth, UINT sourceHeight, UINT sourcePitch,
                          const RECT *sourceRectRaw, D3DCOLOR colorKey)
{
    LinuxSurfaceAccess destination;
    if (!th08_linux_surface_access(destinationRaw, &destination, false) || destination.pixels == NULL)
        return E_INVALIDARG;
    RECT destinationRect = destinationRectRaw != NULL ? *destinationRectRaw : FullRect(destination.width, destination.height);
    RECT sourceRect = sourceRectRaw != NULL ? *sourceRectRaw : FullRect(sourceWidth, sourceHeight);
    if (!ValidRect(destinationRect) || !ValidRect(sourceRect)) return E_INVALIDARG;
    if (destinationRect.right > static_cast<LONG>(destination.width)) destinationRect.right = destination.width;
    if (destinationRect.bottom > static_cast<LONG>(destination.height)) destinationRect.bottom = destination.height;
    if (sourceRect.right > static_cast<LONG>(sourceWidth)) sourceRect.right = sourceWidth;
    if (sourceRect.bottom > static_cast<LONG>(sourceHeight)) sourceRect.bottom = sourceHeight;
    UINT destinationWidth = destinationRect.right - destinationRect.left;
    UINT destinationHeight = destinationRect.bottom - destinationRect.top;
    UINT sourceRectWidth = sourceRect.right - sourceRect.left;
    UINT sourceRectHeight = sourceRect.bottom - sourceRect.top;
    UINT destinationBytes = BytesPerPixel(destination.format);
    for (UINT y = 0; y < destinationHeight; ++y)
    {
        UINT sourceY = sourceRect.top + static_cast<UINT>((static_cast<unsigned long long>(y) * sourceRectHeight) / destinationHeight);
        for (UINT x = 0; x < destinationWidth; ++x)
        {
            UINT sourceX = sourceRect.left + static_cast<UINT>((static_cast<unsigned long long>(x) * sourceRectWidth) / destinationWidth);
            BYTE rgba[4]; memcpy(rgba, source + sourceY * sourcePitch + sourceX * 4, 4);
            if (colorKey != 0 && (colorKey & 0x00ffffffu) ==
                                 ((static_cast<DWORD>(rgba[0]) << 16) | (static_cast<DWORD>(rgba[1]) << 8) | rgba[2]))
                rgba[3] = 0;
            EncodePixel(destination.pixels + (destinationRect.top + y) * destination.pitch +
                        (destinationRect.left + x) * destinationBytes, destination.format, rgba);
        }
    }
    th08_linux_surface_changed(destinationRaw); return S_OK;
}

HRESULT CopySurface(IDirect3DSurface8 *destinationRaw, const RECT *destinationRectRaw,
                    IDirect3DSurface8 *sourceRaw, const RECT *sourceRectRaw, D3DCOLOR colorKey)
{
    LinuxSurfaceAccess source;
    if (!th08_linux_surface_access(sourceRaw, &source, true) || source.pixels == NULL) return E_INVALIDARG;

    // Embedded ANM images and PSP framebuffer captures are copied between
    // equal-format, equal-sized rectangles. Preserve those packed pixels
    // directly instead of expanding the whole source to temporary 32-bit RGBA.
    LinuxSurfaceAccess destination;
    if (!th08_linux_surface_access(destinationRaw, &destination, false) ||
        destination.pixels == NULL)
        return E_INVALIDARG;
    if (colorKey == 0 && destination.format == source.format)
    {
        const RECT sourceRect = sourceRectRaw != NULL
                                  ? *sourceRectRaw
                                  : FullRect(source.width, source.height);
        const RECT destinationRect = destinationRectRaw != NULL
                                       ? *destinationRectRaw
                                       : FullRect(destination.width, destination.height);
        if (ValidRect(sourceRect) && ValidRect(destinationRect) &&
            sourceRect.right <= static_cast<LONG>(source.width) &&
            sourceRect.bottom <= static_cast<LONG>(source.height) &&
            destinationRect.right <= static_cast<LONG>(destination.width) &&
            destinationRect.bottom <= static_cast<LONG>(destination.height) &&
            sourceRect.right - sourceRect.left == destinationRect.right - destinationRect.left &&
            sourceRect.bottom - sourceRect.top == destinationRect.bottom - destinationRect.top)
        {
            const UINT copyWidth = static_cast<UINT>(sourceRect.right - sourceRect.left);
            const UINT copyHeight = static_cast<UINT>(sourceRect.bottom - sourceRect.top);
            const UINT bytes = BytesPerPixel(source.format);
            const UINT rowBytes = copyWidth * bytes;
            for (UINT y = 0; y < copyHeight; ++y)
            {
                memcpy(destination.pixels +
                           (static_cast<UINT>(destinationRect.top) + y) * destination.pitch +
                           static_cast<UINT>(destinationRect.left) * bytes,
                       source.pixels +
                           (static_cast<UINT>(sourceRect.top) + y) * source.pitch +
                           static_cast<UINT>(sourceRect.left) * bytes,
                       rowBytes);
            }
            th08_linux_surface_changed(destinationRaw);
            return S_OK;
        }
    }

    // The generic desktop route used to expand the entire source to RGBA
    // first.  A 1024x64 text surface therefore requested a contiguous 256 KiB
    // temporary on every copy and failed when the PSP heap's largest block was
    // 64 bytes smaller, despite ample total free RAM.  Convert each sampled
    // pixel directly into the destination.  The sampling, color-key and packed
    // output operations are identical; only the temporary lifetime disappears.
    const RECT sourceRect = sourceRectRaw != NULL
                              ? *sourceRectRaw
                              : FullRect(source.width, source.height);
    RECT destinationRect = destinationRectRaw != NULL
                              ? *destinationRectRaw
                              : FullRect(destination.width, destination.height);
    if (!ValidRect(sourceRect) || !ValidRect(destinationRect) ||
        sourceRect.right > static_cast<LONG>(source.width) ||
        sourceRect.bottom > static_cast<LONG>(source.height))
        return E_INVALIDARG;
    if (destinationRect.right > static_cast<LONG>(destination.width))
        destinationRect.right = static_cast<LONG>(destination.width);
    if (destinationRect.bottom > static_cast<LONG>(destination.height))
        destinationRect.bottom = static_cast<LONG>(destination.height);
    if (!ValidRect(destinationRect))
        return E_INVALIDARG;

    const UINT destinationWidth = static_cast<UINT>(destinationRect.right - destinationRect.left);
    const UINT destinationHeight = static_cast<UINT>(destinationRect.bottom - destinationRect.top);
    const UINT sourceWidth = static_cast<UINT>(sourceRect.right - sourceRect.left);
    const UINT sourceHeight = static_cast<UINT>(sourceRect.bottom - sourceRect.top);
    const UINT sourceBytes = BytesPerPixel(source.format);
    const UINT destinationBytes = BytesPerPixel(destination.format);
    for (UINT y = 0; y < destinationHeight; ++y)
    {
        const UINT sourceY = static_cast<UINT>(sourceRect.top) +
            static_cast<UINT>((static_cast<unsigned long long>(y) * sourceHeight) /
                              destinationHeight);
        BYTE *destinationRow = destination.pixels +
            (static_cast<UINT>(destinationRect.top) + y) * destination.pitch +
            static_cast<UINT>(destinationRect.left) * destinationBytes;
        for (UINT x = 0; x < destinationWidth; ++x)
        {
            const UINT sourceX = static_cast<UINT>(sourceRect.left) +
                static_cast<UINT>((static_cast<unsigned long long>(x) * sourceWidth) /
                                  destinationWidth);
            BYTE rgba[4];
            DecodePixel(source.pixels + sourceY * source.pitch + sourceX * sourceBytes,
                        source.format, rgba);
            if (colorKey != 0 && (colorKey & 0x00ffffffu) ==
                ((static_cast<DWORD>(rgba[0]) << 16) |
                 (static_cast<DWORD>(rgba[1]) << 8) | rgba[2]))
                rgba[3] = 0;
            EncodePixel(destinationRow + x * destinationBytes, destination.format, rgba);
        }
    }
    th08_linux_surface_changed(destinationRaw);
    return S_OK;
}

#if defined(PSP)
HRESULT CopyMemoryAreaAverage(IDirect3DSurface8 *destinationRaw,
                              const RECT *destinationRectRaw,
                              const LinuxSurfaceAccess &source,
                              const RECT *sourceRectRaw, D3DCOLOR colorKey)
{
    LinuxSurfaceAccess destination;
    if (source.pixels == NULL ||
        !th08_linux_surface_access(destinationRaw, &destination, false) ||
        destination.pixels == NULL)
        return E_INVALIDARG;

    const RECT sourceRect = sourceRectRaw != NULL
                              ? *sourceRectRaw
                              : FullRect(source.width, source.height);
    RECT destinationRect = destinationRectRaw != NULL
                              ? *destinationRectRaw
                              : FullRect(destination.width, destination.height);
    if (!ValidRect(sourceRect) || !ValidRect(destinationRect) ||
        sourceRect.right > static_cast<LONG>(source.width) ||
        sourceRect.bottom > static_cast<LONG>(source.height))
        return E_INVALIDARG;
    if (destinationRect.right > static_cast<LONG>(destination.width))
        destinationRect.right = static_cast<LONG>(destination.width);
    if (destinationRect.bottom > static_cast<LONG>(destination.height))
        destinationRect.bottom = static_cast<LONG>(destination.height);
    if (!ValidRect(destinationRect))
        return E_INVALIDARG;

    const UINT destinationWidth = static_cast<UINT>(destinationRect.right - destinationRect.left);
    const UINT destinationHeight = static_cast<UINT>(destinationRect.bottom - destinationRect.top);
    const UINT sourceWidth = static_cast<UINT>(sourceRect.right - sourceRect.left);
    const UINT sourceHeight = static_cast<UINT>(sourceRect.bottom - sourceRect.top);
    const UINT sourceBytes = BytesPerPixel(source.format);
    const UINT destinationBytes = BytesPerPixel(destination.format);

    // TH07's PSP text path preserves thin glyph strokes by averaging every
    // source texel covered by one destination texel.  Do the same directly in
    // the existing packed surfaces: no temporary row/surface or heap traffic.
    for (UINT y = 0; y < destinationHeight; ++y)
    {
        const UINT sourceY0 = static_cast<UINT>(sourceRect.top) +
            static_cast<UINT>((static_cast<unsigned long long>(y) * sourceHeight) /
                              destinationHeight);
        UINT sourceY1 = static_cast<UINT>(sourceRect.top) +
            static_cast<UINT>((static_cast<unsigned long long>(y + 1) * sourceHeight +
                               destinationHeight - 1) /
                              destinationHeight);
        if (sourceY1 <= sourceY0)
            sourceY1 = sourceY0 + 1;
        if (sourceY1 > static_cast<UINT>(sourceRect.bottom))
            sourceY1 = static_cast<UINT>(sourceRect.bottom);

        BYTE *destinationRow = destination.pixels +
            (static_cast<UINT>(destinationRect.top) + y) * destination.pitch +
            static_cast<UINT>(destinationRect.left) * destinationBytes;
        for (UINT x = 0; x < destinationWidth; ++x)
        {
            const UINT sourceX0 = static_cast<UINT>(sourceRect.left) +
                static_cast<UINT>((static_cast<unsigned long long>(x) * sourceWidth) /
                                  destinationWidth);
            UINT sourceX1 = static_cast<UINT>(sourceRect.left) +
                static_cast<UINT>((static_cast<unsigned long long>(x + 1) * sourceWidth +
                                   destinationWidth - 1) /
                                  destinationWidth);
            if (sourceX1 <= sourceX0)
                sourceX1 = sourceX0 + 1;
            if (sourceX1 > static_cast<UINT>(sourceRect.right))
                sourceX1 = static_cast<UINT>(sourceRect.right);

            UINT sums[4] = {0, 0, 0, 0};
            UINT sampleCount = 0;
            for (UINT sourceY = sourceY0; sourceY < sourceY1; ++sourceY)
            {
                const BYTE *sourceRow = source.pixels + sourceY * source.pitch;
                for (UINT sourceX = sourceX0; sourceX < sourceX1; ++sourceX)
                {
                    BYTE rgba[4];
                    DecodePixel(sourceRow + sourceX * sourceBytes, source.format, rgba);
                    if (colorKey != 0 && (colorKey & 0x00ffffffu) ==
                        ((static_cast<DWORD>(rgba[0]) << 16) |
                         (static_cast<DWORD>(rgba[1]) << 8) | rgba[2]))
                        rgba[3] = 0;
                    sums[0] += rgba[0];
                    sums[1] += rgba[1];
                    sums[2] += rgba[2];
                    sums[3] += rgba[3];
                    ++sampleCount;
                }
            }

            BYTE averaged[4];
            averaged[0] = static_cast<BYTE>(sums[0] / sampleCount);
            averaged[1] = static_cast<BYTE>(sums[1] / sampleCount);
            averaged[2] = static_cast<BYTE>(sums[2] / sampleCount);
            averaged[3] = static_cast<BYTE>(sums[3] / sampleCount);
            EncodePixel(destinationRow + x * destinationBytes,
                        destination.format, averaged);
        }
    }
    th08_linux_surface_changed(destinationRaw);
    return S_OK;
}

HRESULT CopySurfaceAreaAverage(IDirect3DSurface8 *destinationRaw,
                               const RECT *destinationRectRaw,
                               IDirect3DSurface8 *sourceRaw,
                               const RECT *sourceRectRaw, D3DCOLOR colorKey)
{
    LinuxSurfaceAccess source;
    if (!th08_linux_surface_access(sourceRaw, &source, true) ||
        source.pixels == NULL)
        return E_INVALIDARG;
    return CopyMemoryAreaAverage(destinationRaw, destinationRectRaw, source,
                                 sourceRectRaw, colorKey);
}
#endif

SDL_Surface *LoadImage(LPCVOID data, UINT size)
{
    if (data == NULL || size == 0) return NULL;
    SDL_RWops *stream = SDL_RWFromConstMem(data, size);
    if (stream == NULL) return NULL;
    SDL_Surface *loaded = IMG_Load_RW(stream, 1);
    if (loaded == NULL) return NULL;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded); return rgba;
}

void SetImageInfo(D3DXIMAGE_INFO *info, UINT width, UINT height, D3DFORMAT format)
{
    if (info == NULL) return;
    info->Width = width; info->Height = height; info->Depth = 1; info->MipLevels = 1; info->Format = format;
}
} // namespace

D3DXMATRIX *D3DXMatrixIdentity(D3DXMATRIX *out)
{
    if (out == NULL) return NULL;
    memset(out, 0, sizeof(*out)); out->_11 = out->_22 = out->_33 = out->_44 = 1.0f; return out;
}

D3DXMATRIX *D3DXMatrixMultiply(D3DXMATRIX *out, const D3DXMATRIX *left, const D3DXMATRIX *right)
{
    if (out == NULL || left == NULL || right == NULL) return NULL;
    D3DXMATRIX result;
    const FLOAT *a = *left, *b = *right;
    FLOAT *r = result;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            r[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column] +
                                  a[row * 4 + 2] * b[8 + column] + a[row * 4 + 3] * b[12 + column];
    *out = result; return out;
}

D3DXMATRIX *D3DXMatrixRotationX(D3DXMATRIX *out, FLOAT angle)
{
    D3DXMatrixIdentity(out); if (out == NULL) return NULL;
    FLOAT cosine = cosf(angle), sine = sinf(angle);
    out->_22 = cosine; out->_23 = sine; out->_32 = -sine; out->_33 = cosine; return out;
}

D3DXMATRIX *D3DXMatrixRotationY(D3DXMATRIX *out, FLOAT angle)
{
    D3DXMatrixIdentity(out); if (out == NULL) return NULL;
    FLOAT cosine = cosf(angle), sine = sinf(angle);
    out->_11 = cosine; out->_13 = -sine; out->_31 = sine; out->_33 = cosine; return out;
}

D3DXMATRIX *D3DXMatrixRotationZ(D3DXMATRIX *out, FLOAT angle)
{
    D3DXMatrixIdentity(out); if (out == NULL) return NULL;
    FLOAT cosine = cosf(angle), sine = sinf(angle);
    out->_11 = cosine; out->_12 = sine; out->_21 = -sine; out->_22 = cosine; return out;
}

D3DXMATRIX *D3DXMatrixRotationQuaternion(D3DXMATRIX *out, const D3DXQUATERNION *q)
{
    if (out == NULL || q == NULL) return NULL;
    out->_11 = 1 - 2 * (q->y * q->y + q->z * q->z);
    out->_12 = 2 * (q->x * q->y + q->z * q->w); out->_13 = 2 * (q->x * q->z - q->y * q->w); out->_14 = 0;
    out->_21 = 2 * (q->x * q->y - q->z * q->w);
    out->_22 = 1 - 2 * (q->x * q->x + q->z * q->z); out->_23 = 2 * (q->y * q->z + q->x * q->w); out->_24 = 0;
    out->_31 = 2 * (q->x * q->z + q->y * q->w); out->_32 = 2 * (q->y * q->z - q->x * q->w);
    out->_33 = 1 - 2 * (q->x * q->x + q->y * q->y); out->_34 = 0;
    out->_41 = out->_42 = out->_43 = 0; out->_44 = 1; return out;
}

D3DXVECTOR3 *D3DXVec3Cross(D3DXVECTOR3 *out, const D3DXVECTOR3 *left, const D3DXVECTOR3 *right)
{
    if (out == NULL || left == NULL || right == NULL) return NULL;
    D3DXVECTOR3 result(left->y * right->z - left->z * right->y,
                       left->z * right->x - left->x * right->z,
                       left->x * right->y - left->y * right->x);
    *out = result; return out;
}

FLOAT D3DXVec3Dot(const D3DXVECTOR3 *left, const D3DXVECTOR3 *right)
{ return left != NULL && right != NULL ? left->x * right->x + left->y * right->y + left->z * right->z : 0.0f; }

FLOAT D3DXVec3LengthSq(const D3DXVECTOR3 *value) { return D3DXVec3Dot(value, value); }
FLOAT D3DXVec3Length(const D3DXVECTOR3 *value) { return sqrtf(D3DXVec3LengthSq(value)); }

D3DXVECTOR3 *D3DXVec3Normalize(D3DXVECTOR3 *out, const D3DXVECTOR3 *value)
{
    if (out == NULL || value == NULL) return NULL;
    FLOAT length = D3DXVec3Length(value);
    if (length > 1.0e-8f) { D3DXVECTOR3 copy = *value; *out = copy / length; }
    else out->x = out->y = out->z = 0.0f;
    return out;
}

D3DXMATRIX *D3DXMatrixLookAtLH(D3DXMATRIX *out, const D3DXVECTOR3 *eye,
                               const D3DXVECTOR3 *at, const D3DXVECTOR3 *up)
{
    if (out == NULL || eye == NULL || at == NULL || up == NULL) return NULL;
    D3DXVECTOR3 zaxis, xaxis, yaxis, direction = *at - *eye;
    D3DXVec3Normalize(&zaxis, &direction); D3DXVec3Cross(&xaxis, up, &zaxis);
    D3DXVec3Normalize(&xaxis, &xaxis); D3DXVec3Cross(&yaxis, &zaxis, &xaxis);
    out->_11 = xaxis.x; out->_12 = yaxis.x; out->_13 = zaxis.x; out->_14 = 0;
    out->_21 = xaxis.y; out->_22 = yaxis.y; out->_23 = zaxis.y; out->_24 = 0;
    out->_31 = xaxis.z; out->_32 = yaxis.z; out->_33 = zaxis.z; out->_34 = 0;
    out->_41 = -D3DXVec3Dot(&xaxis, eye); out->_42 = -D3DXVec3Dot(&yaxis, eye);
    out->_43 = -D3DXVec3Dot(&zaxis, eye); out->_44 = 1; return out;
}

D3DXMATRIX *D3DXMatrixPerspectiveFovLH(D3DXMATRIX *out, FLOAT fov, FLOAT aspect, FLOAT nearZ, FLOAT farZ)
{
    if (out == NULL || aspect == 0 || farZ == nearZ) return NULL;
    memset(out, 0, sizeof(*out)); FLOAT yScale = 1.0f / tanf(fov * 0.5f);
    out->_11 = yScale / aspect; out->_22 = yScale; out->_33 = farZ / (farZ - nearZ);
    out->_34 = 1.0f; out->_43 = -nearZ * farZ / (farZ - nearZ); return out;
}

D3DXVECTOR3 *D3DXVec3TransformCoord(D3DXVECTOR3 *out, const D3DXVECTOR3 *value, const D3DXMATRIX *matrix)
{
    if (out == NULL || value == NULL || matrix == NULL) return NULL;
    FLOAT x = value->x * matrix->_11 + value->y * matrix->_21 + value->z * matrix->_31 + matrix->_41;
    FLOAT y = value->x * matrix->_12 + value->y * matrix->_22 + value->z * matrix->_32 + matrix->_42;
    FLOAT z = value->x * matrix->_13 + value->y * matrix->_23 + value->z * matrix->_33 + matrix->_43;
    FLOAT w = value->x * matrix->_14 + value->y * matrix->_24 + value->z * matrix->_34 + matrix->_44;
    if (fabsf(w) > 1.0e-8f) { x /= w; y /= w; z /= w; }
    out->x = x; out->y = y; out->z = z; return out;
}

D3DXVECTOR3 *D3DXVec3Project(D3DXVECTOR3 *out, const D3DXVECTOR3 *value, const D3DVIEWPORT8 *viewport,
                             const D3DXMATRIX *projection, const D3DXMATRIX *view, const D3DXMATRIX *world)
{
    if (out == NULL || value == NULL || viewport == NULL || projection == NULL || view == NULL || world == NULL) return NULL;
    D3DXMATRIX worldView, transform; D3DXVECTOR3 projected;
    D3DXMatrixMultiply(&worldView, world, view); D3DXMatrixMultiply(&transform, &worldView, projection);
    D3DXVec3TransformCoord(&projected, value, &transform);
    out->x = viewport->X + (projected.x + 1.0f) * viewport->Width * 0.5f;
    out->y = viewport->Y + (1.0f - projected.y) * viewport->Height * 0.5f;
    out->z = viewport->MinZ + projected.z * (viewport->MaxZ - viewport->MinZ); return out;
}

HRESULT D3DXCreateTexture(IDirect3DDevice8 *device, UINT width, UINT height, UINT levels, DWORD usage,
                          D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8 **result)
{
    if (device == NULL) return E_INVALIDARG;
    return device->CreateTexture(width, height, levels, usage, format, pool, result);
}

HRESULT D3DXCreateTextureFromFileInMemoryEx(IDirect3DDevice8 *device, LPCVOID data, UINT size,
                                            UINT width, UINT height, UINT levels, DWORD usage,
                                            D3DFORMAT format, D3DPOOL pool, DWORD, DWORD, D3DCOLOR colorKey,
                                            D3DXIMAGE_INFO *info, void *, IDirect3DTexture8 **result)
{
    if (device == NULL || result == NULL) return E_INVALIDARG;
    SDL_Surface *image = LoadImage(data, size); if (image == NULL) return E_FAIL;
    UINT textureWidth = width == 0 || width == D3DX_DEFAULT ? image->w : width;
    UINT textureHeight = height == 0 || height == D3DX_DEFAULT ? image->h : height;
    if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
    SetImageInfo(info, image->w, image->h, format);
    HRESULT status = device->CreateTexture(textureWidth, textureHeight, levels, usage, format, pool, result);
    if (SUCCEEDED(status))
    {
        IDirect3DSurface8 *surface = NULL; status = (*result)->GetSurfaceLevel(0, &surface);
        if (SUCCEEDED(status))
        {
            status = CopyRgbaToSurface(surface, NULL, static_cast<const BYTE *>(image->pixels), image->w, image->h,
                                       image->pitch, NULL, colorKey);
            surface->Release();
        }
        if (FAILED(status)) { (*result)->Release(); *result = NULL; }
    }
    SDL_FreeSurface(image); return status;
}

HRESULT D3DXLoadSurfaceFromFileInMemory(IDirect3DSurface8 *destination, const void *, const RECT *destinationRect,
                                        LPCVOID data, UINT size, const RECT *sourceRect, DWORD, D3DCOLOR colorKey,
                                        D3DXIMAGE_INFO *info)
{
    SDL_Surface *image = LoadImage(data, size); if (image == NULL) return E_FAIL;
    SetImageInfo(info, image->w, image->h, D3DFMT_A8R8G8B8);
    HRESULT status = CopyRgbaToSurface(destination, destinationRect, static_cast<const BYTE *>(image->pixels),
                                       image->w, image->h, image->pitch, sourceRect, colorKey);
    SDL_FreeSurface(image); return status;
}

HRESULT D3DXLoadSurfaceFromSurface(IDirect3DSurface8 *destination, const void *, const RECT *destinationRect,
                                   IDirect3DSurface8 *source, const void *, const RECT *sourceRect,
                                   DWORD filter, D3DCOLOR colorKey)
{
#if defined(PSP)
    // D3DX_FILTER_TRIANGLE is 4. Preserve the surface-backed compatibility
    // route with the same TH07 area-average kernel used by the PSP TextHelper's
    // direct DIB source; neither route drops rows with nearest sampling.
    if (filter == 4)
        return CopySurfaceAreaAverage(destination, destinationRect, source,
                                      sourceRect, colorKey);
#else
    (void)filter;
#endif
    return CopySurface(destination, destinationRect, source, sourceRect, colorKey);
}

#if defined(PSP)
HRESULT th08_linux_surface_area_average_from_memory(
    IDirect3DSurface8 *destination, const RECT *destinationRect,
    const void *sourcePixels, UINT sourceWidth, UINT sourceHeight,
    UINT sourcePitch, D3DFORMAT sourceFormat, const RECT *sourceRect,
    D3DCOLOR colorKey)
{
    if (sourcePixels == NULL || sourceWidth == 0 || sourceHeight == 0 ||
        sourcePitch < sourceWidth * BytesPerPixel(sourceFormat))
        return E_INVALIDARG;

    LinuxSurfaceAccess source;
    source.pixels = const_cast<BYTE *>(static_cast<const BYTE *>(sourcePixels));
    source.width = sourceWidth;
    source.height = sourceHeight;
    source.pitch = sourcePitch;
    source.format = sourceFormat;
    return CopyMemoryAreaAverage(destination, destinationRect, source,
                                 sourceRect, colorKey);
}

bool th08_linux_surface_load_image_memory(IDirect3DDevice8 *device, const void *data,
                                          UINT size, IDirect3DSurface8 **surface,
                                          UINT *width, UINT *height)
{
    if (device == NULL || data == NULL || size == 0 || surface == NULL)
        return false;

    *surface = NULL;
#if defined(PSP)
    // r079 hardware returned to the title with only a 94 KiB largest newlib
    // block. title00.png itself was read successfully, but SDL_image then
    // needed a ~1.2 MiB decoded surface plus libpng row work. Keep that
    // bounded, temporary C allocation lifetime in the retained renderer arena
    // instead of asking the fragmented gameplay heap.
    SurfaceDecodeBreadcrumb breadcrumb(size);
    SDL_Surface *decoded = NULL;
    {
        // Keep the C-allocation opt-in strictly around SDL_image/libpng. The
        // decoded surface can safely leave this lexical block because the
        // global free wrapper recognizes arena ownership, while stdio/GL and
        // other libraries below continue using their normal allocators.
        th08::psp::SurfaceDecodeAllocationScope decodeScope("surface image decode");
        breadcrumb.scopeActive = th08::psp::SurfaceDecodeAllocationScopeActive();
        if (!breadcrumb.scopeActive)
        {
            breadcrumb.stage = "arena_scope";
            return false;
        }
        SDL_RWops *stream = SDL_RWFromConstMem(data, static_cast<int>(size));
        if (stream == NULL)
        {
            breadcrumb.stage = "rw_stream";
            return false;
        }
        decoded = IMG_Load_RW(stream, 1);
    }
#else
    SDL_RWops *stream = SDL_RWFromConstMem(data, static_cast<int>(size));
    if (stream == NULL)
        return false;
    SDL_Surface *decoded = IMG_Load_RW(stream, 1);
#endif
    if (decoded == NULL || decoded->w <= 0 || decoded->h <= 0)
    {
        if (decoded != NULL) SDL_FreeSurface(decoded);
#if defined(PSP)
        breadcrumb.stage = "image_decode";
#endif
        return false;
    }
#if defined(PSP)
    breadcrumb.width = static_cast<UINT>(decoded->w);
    breadcrumb.height = static_cast<UINT>(decoded->h);
#endif

    IDirect3DSurface8 *destination = NULL;
    if (device->CreateImageSurface(static_cast<UINT>(decoded->w),
                                   static_cast<UINT>(decoded->h),
                                   D3DFMT_R5G6B5, &destination) != D3D_OK)
    {
        SDL_FreeSurface(decoded);
#if defined(PSP)
        breadcrumb.stage = "destination_surface";
#endif
        return false;
    }

    LinuxSurfaceAccess access;
    const bool accessible = th08_linux_surface_access(destination, &access, false) &&
                            access.pixels != NULL;
    const bool mustUnlock = SDL_MUSTLOCK(decoded) != 0;
    const bool sourceLocked = !mustUnlock || SDL_LockSurface(decoded) == 0;
    const int converted = accessible && sourceLocked
                              ? SDL_ConvertPixels(decoded->w, decoded->h,
                                                  decoded->format->format,
                                                  decoded->pixels, decoded->pitch,
                                                  SDL_PIXELFORMAT_RGB565,
                                                  access.pixels,
                                                  static_cast<int>(access.pitch))
                              : -1;
    if (mustUnlock && sourceLocked) SDL_UnlockSurface(decoded);
    if (converted == 0)
    {
#if defined(PSP) && defined(TH08_PSP_SURFACE_PIXEL_AUDIT) && \
    TH08_PSP_SURFACE_PIXEL_AUDIT
        unsigned long nonBlackPixels = 0;
        unsigned long checksum = 2166136261u;
        for (int y = 0; y < decoded->h; ++y)
        {
            const WORD *row = reinterpret_cast<const WORD *>(
                access.pixels + static_cast<UINT>(y) * access.pitch);
            for (int x = 0; x < decoded->w; ++x)
            {
                const WORD pixel = row[x];
                if (pixel != 0) ++nonBlackPixels;
                checksum ^= pixel;
                checksum *= 16777619u;
            }
        }
        fprintf(stderr,
                "TH08PSP SURFACE_DECODE size=%dx%d nonblack=%lu checksum=%08lx "
                "first=%04x center=%04x\n",
                decoded->w, decoded->h, nonBlackPixels, checksum,
                static_cast<unsigned int>(*reinterpret_cast<const WORD *>(access.pixels)),
                static_cast<unsigned int>(*reinterpret_cast<const WORD *>(
                    access.pixels + static_cast<UINT>(decoded->h / 2) * access.pitch +
                    static_cast<UINT>(decoded->w / 2) * sizeof(WORD))));
#endif
        th08_linux_surface_changed(destination);
        if (width != NULL) *width = static_cast<UINT>(decoded->w);
        if (height != NULL) *height = static_cast<UINT>(decoded->h);
        *surface = destination;
    }
    else
    {
        destination->Release();
#if defined(PSP)
        breadcrumb.stage = accessible ? (sourceLocked ? "pixel_convert" : "source_lock")
                                      : "destination_access";
#endif
    }
    SDL_FreeSurface(decoded);
#if defined(PSP)
    if (converted == 0)
    {
        breadcrumb.stage = "complete";
        breadcrumb.success = true;
    }
#endif
    return converted == 0;
}
#endif
