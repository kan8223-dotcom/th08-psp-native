#include "th_pch.h"

#include "Supervisor.hpp"
#include "TextHelper.hpp"
#include "dxutil.hpp"
#include "i18n.hpp"
#if defined(PSP)
#include "modern/linux/d3d8_internal.hpp"
#include "render_perf_telemetry.hpp"
#include "render_resource_arena.hpp"
#endif

namespace th08
{
DIFFABLE_STATIC(IDirect3DSurface8 *, g_TextBufferSurface)
#if defined(PSP)
// The portable GDI shim allocates one extra scanline for the top-down DIB, so
// this 1024x64 A1R5G5B5 buffer owns 133,120 bytes. Keep it for the complete
// TextHelper lifetime instead of repeatedly asking a fragmented stage heap for
// the same contiguous block.
static TextHelper g_PspTextWorkBuffer;

static i32 PspTextRasterHeight(i32 fontHeight)
{
    const i32 requested = fontHeight * 2 + 8;
    return requested < 64 ? requested : 64;
}
#endif

DIFFABLE_STATIC_ARRAY_ASSIGN(FormatInfo, 7, g_FormatInfoArray) = {
    {D3DFMT_X8R8G8B8, 32, 0x00000000, 0x00FF0000, 0x0000FF00, 0x000000FF},
    {D3DFMT_A8R8G8B8, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF},
    {D3DFMT_X1R5G5B5, 16, 0x00000000, 0x00007C00, 0x000003E0, 0x0000001F},
    {D3DFMT_R5G6B5, 16, 0x00000000, 0x0000F800, 0x000007E0, 0x0000001F},
    {D3DFMT_A1R5G5B5, 16, 0x00008000, 0x00007C00, 0x000003E0, 0x0000001F},
    {D3DFMT_A4R4G4B4, 16, 0x0000F000, 0x00000F00, 0x000000F0, 0x0000000F},
    {(D3DFORMAT)-1, 0, 0, 0, 0, 0},
};

TextHelper::TextHelper()
{
    this->format = (D3DFORMAT)-1;
    this->width = 0;
    this->height = 0;
    this->hdc = 0;
    this->gdiObj2 = 0;
    this->gdiObj = 0;
    this->buffer = NULL;
}

TextHelper::~TextHelper()
{
    ReleaseBuffer();
}

bool TextHelper::ReleaseBuffer()
{
    if (this->hdc)
    {
        SelectObject(this->hdc, this->gdiObj);
        DeleteDC(this->hdc);
        DeleteObject(this->gdiObj2);
        this->format = (D3DFORMAT)-1;
        this->width = 0;
        this->height = 0;
        this->hdc = 0;
        this->gdiObj2 = 0;
        this->gdiObj = 0;
        this->buffer = NULL;
        return true;
    }
    else
    {
        return false;
    }
}

bool TextHelper::AllocateBufferWithFallback(i32 width, i32 height, D3DFORMAT format)
{
#if defined(PSP)
    if (this->hdc != NULL && this->buffer != NULL && this->width == width &&
        this->height == height && this->format == format)
    {
        // CreateDIBSection receives -(height + 1), so the PSP shim owns one
        // physical guard row beyond biSizeImage. Reset that row as well to
        // reproduce a freshly value-initialized GdiBitmap on every use.
        memset(this->buffer, 0,
               static_cast<size_t>(this->imageWidthInBytes) *
                   static_cast<size_t>(this->height + 1));
        return true;
    }
#endif
    if (TryAllocateBuffer(width, height, format))
    {
        return true;
    }

    if (format == D3DFMT_A1R5G5B5 || format == D3DFMT_A4R4G4B4)
    {
        return TryAllocateBuffer(width, height, D3DFMT_A8R8G8B8);
    }
    if (format == D3DFMT_R5G6B5)
    {
        return TryAllocateBuffer(width, height, D3DFMT_X8R8G8B8);
    }
    return false;
}

struct THBITMAPINFO
{
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[17];
};

#pragma function(memset)
#pragma var_order(imageWidthInBytes, deviceContext, originalBitmapObj, padding, bitmapInfo, formatInfo, bitmapObj,     \
                  bitmapData)
bool TextHelper::TryAllocateBuffer(i32 width, i32 height, D3DFORMAT format)
{
    HGDIOBJ originalBitmapObj;
    u8 *bitmapData;
    HBITMAP bitmapObj;
    FormatInfo *formatInfo;
    THBITMAPINFO bitmapInfo;
    u32 padding;
    HDC deviceContext;
    i32 imageWidthInBytes;

    ReleaseBuffer();
    memset(&bitmapInfo, 0, sizeof(THBITMAPINFO));
    formatInfo = GetFormatInfo(format);
    if (formatInfo == NULL)
    {
        return false;
    }
    imageWidthInBytes = ((((width * formatInfo->bitCount) / 8) + 3) / 4) * 4;
    bitmapInfo.bmiHeader.biSize = sizeof(THBITMAPINFO);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -(height + 1);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = formatInfo->bitCount;
    bitmapInfo.bmiHeader.biSizeImage = height * imageWidthInBytes;
    if (format != D3DFMT_X1R5G5B5 && format != D3DFMT_X8R8G8B8)
    {
        bitmapInfo.bmiHeader.biCompression = 3;
        ((u32 *)bitmapInfo.bmiColors)[0] = formatInfo->redMask;
        ((u32 *)bitmapInfo.bmiColors)[1] = formatInfo->greenMask;
        ((u32 *)bitmapInfo.bmiColors)[2] = formatInfo->blueMask;
        ((u32 *)bitmapInfo.bmiColors)[3] = formatInfo->alphaMask;
    }
    bitmapObj = CreateDIBSection(NULL, (BITMAPINFO *)&bitmapInfo, 0, (void **)&bitmapData, NULL, 0);
    if (bitmapObj == NULL)
    {
        return false;
    }
    memset(bitmapData, 0, bitmapInfo.bmiHeader.biSizeImage);
    deviceContext = CreateCompatibleDC(NULL);
    originalBitmapObj = SelectObject(deviceContext, bitmapObj);
    this->hdc = deviceContext;
    this->gdiObj2 = bitmapObj;
    this->buffer = bitmapData;
    this->imageSizeInBytes = bitmapInfo.bmiHeader.biSizeImage;
    this->gdiObj = originalBitmapObj;
    this->width = width;
    this->height = height;
    this->format = format;
    this->imageWidthInBytes = imageWidthInBytes;
    return true;
}

FormatInfo *TextHelper::GetFormatInfo(D3DFORMAT format)
{
    i32 formatIndex;

    for (formatIndex = 0;
         g_FormatInfoArray[formatIndex].format != -1 && g_FormatInfoArray[formatIndex].format != format;
         formatIndex++)
    {
    }
    if (format == -1)
    {
        return NULL;
    }
    return &g_FormatInfoArray[formatIndex];
}

struct A1R5G5B5
{
    u16 blue : 5;
    u16 green : 5;
    u16 red : 5;
    u16 alpha : 1;
};

#pragma var_order(bufferRegion, byteOffset, regionByteCount, adjustedChannel, bufferCursor, bufferStart)
bool TextHelper::InvertAlpha(i32 unusedX, i32 y, i32 spriteWidth, i32 fontHeight, BOOL useGentleColorFalloff)
{
    i32 regionByteCount;
    u8 *bufferRegion;
    i32 byteOffset;
    A1R5G5B5 *bufferCursor;

    i32 adjustedChannel;

    regionByteCount = spriteWidth * fontHeight * 2;
    bufferRegion = &GetBuffer()[y * spriteWidth * 2];
    switch (this->format)
    {
    case D3DFMT_A8R8G8B8:
        for (byteOffset = 3; byteOffset < regionByteCount; byteOffset += 4)
        {
            bufferRegion[byteOffset] = bufferRegion[byteOffset] ^ 0xff;
        }
        break;
    case D3DFMT_A1R5G5B5:
        for (bufferCursor = (A1R5G5B5 *)bufferRegion, byteOffset = 0;
             byteOffset < regionByteCount;
             byteOffset += 2, bufferCursor += 1)
        {
            bufferCursor->alpha ^= 1;
            if (bufferCursor->alpha)
            {
                if (!useGentleColorFalloff)
                {
                    if (bufferCursor->red >= bufferCursor->blue)
                    {
                        adjustedChannel = bufferCursor->red -
                                          bufferCursor->red * byteOffset * 2 / regionByteCount / 3;
                        bufferCursor->red = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;

                        adjustedChannel = bufferCursor->green -
                                          bufferCursor->green * byteOffset * 2 / regionByteCount / 3;
                        bufferCursor->green = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;
                    }
                    else
                    {
                        adjustedChannel = bufferCursor->blue -
                                          bufferCursor->blue * byteOffset / regionByteCount / 2;
                        bufferCursor->blue = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;

                        adjustedChannel = bufferCursor->green -
                                          bufferCursor->green * byteOffset / regionByteCount / 2;
                        bufferCursor->green = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;
                    }
                }
                else
                {
                    if (bufferCursor->red >= bufferCursor->blue)
                    {
                        adjustedChannel = bufferCursor->red -
                                          bufferCursor->red * byteOffset / regionByteCount / 4;
                        bufferCursor->red = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;

                        adjustedChannel = bufferCursor->green -
                                          bufferCursor->green * byteOffset / regionByteCount / 4;
                        bufferCursor->green = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;
                    }
                    else
                    {
                        adjustedChannel = bufferCursor->blue -
                                          bufferCursor->blue * byteOffset / regionByteCount / 4;
                        bufferCursor->blue = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;

                        adjustedChannel = bufferCursor->green -
                                          bufferCursor->green * byteOffset / regionByteCount / 4;
                        bufferCursor->green = adjustedChannel >= 0x20 ? 0x1f : adjustedChannel;
                    }
                }
            }
            else
            {
                bufferCursor->red = 0;
                bufferCursor->green = 0;
                bufferCursor->blue = 0;
            }
        }
        break;
    case D3DFMT_A4R4G4B4:
        for (byteOffset = 1; byteOffset < regionByteCount; byteOffset = byteOffset + 2)
        {
            bufferRegion[byteOffset] = bufferRegion[byteOffset] ^ 0xf0;
        }
        break;
    default:
        return false;
    }
    return true;
}

u8 *TextHelper::GetBuffer()
{
    return this->buffer;
}

u32 TextHelper::GetImageWidthInBytes()
{
    return this->imageWidthInBytes;
}

i32 TextHelper::GetHeight()
{
    return this->height;
}

HDC TextHelper::GetHDC()
{
    return this->hdc;
}

i32 TextHelper::GetWidth()
{
    return this->width;
}

D3DFORMAT TextHelper::GetFormat()
{
    return this->format;
}

bool TextHelper::IsAllocated()
{
    return this->gdiObj2 != NULL;
}

#pragma function(memcpy)
#pragma var_order(dstBuf, dstWidthBytes, rectToLock, curHeight, srcWidthBytes, outSurfaceDesc, srcBuf, lockedRect,     \
                  width, height, thisFormat, thisHeight)
bool TextHelper::CopyTextToSurface(IDirect3DSurface8 *outSurface)
{
    D3DLOCKED_RECT lockedRect;
    u8 *srcBuf;
    D3DSURFACE_DESC outSurfaceDesc;
    size_t srcWidthBytes;
    int curHeight;
    RECT rectToLock;
    int dstWidthBytes;
    u8 *dstBuf;

    if (!IsAllocated())
    {
        return false;
    }
    outSurface->GetDesc(&outSurfaceDesc);
    rectToLock.left = 0;
    rectToLock.top = 0;
    rectToLock.right = GetWidth();
    rectToLock.bottom = GetHeight();
    if (outSurface->LockRect(&lockedRect, &rectToLock, 0))
    {
        return false;
    }
    dstWidthBytes = lockedRect.Pitch;
    srcWidthBytes = GetImageWidthInBytes();
    srcBuf = GetBuffer();
    dstBuf = (u8 *)lockedRect.pBits;
    if (outSurfaceDesc.Format == GetFormat())
    {
        for (curHeight = 0; curHeight < GetHeight(); curHeight++)
        {
            memcpy(dstBuf, srcBuf, srcWidthBytes);
            srcBuf += srcWidthBytes;
            dstBuf += dstWidthBytes;
        }
#if defined(PSP)
        th08::psp::RenderPerfNoteTextBytes(
            static_cast<std::uint32_t>(srcWidthBytes) *
            static_cast<std::uint32_t>(GetHeight()));
#endif
    }
    outSurface->UnlockRect();
    return true;
}

#define TEXT_BUFFER_HEIGHT 64

void TextHelper::CreateTextBuffer()
{
#if defined(PSP)
    if (!th08_psp_gdi_text_initialize())
    {
        g_GameErrorContext.Fatal("PSP shared Japanese font initialization failed.\r\n");
        return;
    }
    // The GDI DIB is already the complete 1024x64 A1R5G5B5 filter source.
    // Keep only that owner and let the PSP area-average path read it directly;
    // a second 131,072-byte IDirect3DSurface8 copy carried identical pixels.
    // The DIB remains Main RAM in the render arena (not eDRAM/ME memory), and
    // destination atlas geometry, filtering and glyph rasterization stay exact.
    th08::psp::RenderResourceAllocationScope textWorkScope(
        "PSP text work buffer");
    if (!g_PspTextWorkBuffer.TryAllocateBuffer(
            1024, TEXT_BUFFER_HEIGHT, D3DFMT_A1R5G5B5))
    {
        g_GameErrorContext.Fatal("PSP persistent text work buffer allocation failed.\r\n");
        th08_psp_gdi_text_shutdown();
    }
#else
    g_Supervisor.d3dDevice->CreateImageSurface(1024, TEXT_BUFFER_HEIGHT, D3DFMT_A1R5G5B5, &g_TextBufferSurface);
#endif
}

#if defined(PSP)
bool TextHelper::IsTextBufferReady()
{
    return g_PspTextWorkBuffer.IsAllocated();
}
#endif

void TextHelper::ReleaseTextBuffer()
{
#if defined(PSP)
    // No dynamic GdiFont may remain selected here. Release the DC/bitmap
    // consumer first, then close the shared face and finally its SDL_ttf
    // owner. Shutdown refuses to close a still-borrowed face.
    g_PspTextWorkBuffer.ReleaseBuffer();
    th08_psp_gdi_text_shutdown();
#endif
    SAFE_RELEASE(g_TextBufferSurface);
}

#pragma function(strlen)
#pragma var_order(hdc, font, textSurfaceDesc, h, textHelper, hdc, srcRect, destRect, destSurface)
void TextHelper::RenderTextToTextureBold(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                         i32 fontWidth, COLORREF textColor, COLORREF outlineType, const char *string,
                                         IDirect3DTexture8 *outTexture)
{
    HGDIOBJ h;
    LPDIRECT3DSURFACE8 destSurface;
    RECT destRect;
    RECT srcRect;
    D3DSURFACE_DESC textSurfaceDesc;
    HFONT font;
    HDC hdc;

    font =
        CreateFontA(fontHeight * 2 - 2, 0, 0, 0, FW_SEMIBOLD, false, false, false, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FF_ROMAN | FIXED_PITCH, TH_FONT_NAME);
#if defined(PSP)
    TextHelper &textHelper = g_PspTextWorkBuffer;
    if (font == NULL || !textHelper.IsAllocated())
    {
        if (font != NULL)
            DeleteObject(font);
        return;
    }
#else
    TextHelper textHelper;
#endif
#if defined(PSP)
    memset(&textSurfaceDesc, 0, sizeof(textSurfaceDesc));
    textSurfaceDesc.Width = 1024;
    textSurfaceDesc.Height = TEXT_BUFFER_HEIGHT;
    textSurfaceDesc.Format = D3DFMT_A1R5G5B5;
    const i32 rasterHeight = PspTextRasterHeight(fontHeight);
    if (!textHelper.AllocateBufferWithFallback(
            textSurfaceDesc.Width, textSurfaceDesc.Height, textSurfaceDesc.Format))
    {
        DeleteObject(font);
        return;
    }
#else
    g_TextBufferSurface->GetDesc(&textSurfaceDesc);
    textHelper.AllocateBufferWithFallback(textSurfaceDesc.Width, textSurfaceDesc.Height, textSurfaceDesc.Format);
#endif
    hdc = textHelper.GetHDC();
    h = SelectObject(hdc, font);
#if defined(PSP)
    textHelper.InvertAlpha(0, 0, spriteWidth * 2, rasterHeight, FALSE);
#else
    textHelper.InvertAlpha(0, 0, spriteWidth * 2, fontHeight * 2 + 6, FALSE);
#endif
    SetBkMode(hdc, TRANSPARENT);

    // Render outline.
    if (outlineType != 0xffffffff)
    {
        SetTextColor(hdc, 0);
        TextOutA(hdc, xPos * 2 + 4, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 0, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 4, string, strlen(string));
    }
    else
    {
        SetTextColor(hdc, 0);
        TextOutA(hdc, xPos * 2 + 3, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 1, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 1, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 3, string, strlen(string));
    }
    // Render main text.
    SetTextColor(hdc, textColor);
    TextOutA(hdc, xPos * 2 + 2, 2, string, strlen(string));

    SelectObject(hdc, h);
#if defined(PSP)
    textHelper.InvertAlpha(0, 0, spriteWidth * 2, rasterHeight,
                           outlineType == 0xffffffff);
#else
    textHelper.InvertAlpha(0, 0, spriteWidth * 2, fontHeight * 2 + 6, outlineType == 0xffffffff);
#endif
#if !defined(PSP)
    textHelper.CopyTextToSurface(g_TextBufferSurface);
#endif
    SelectObject(hdc, h);
    DeleteObject(font);
    destRect.left = 0;
    destRect.top = yPos;
    destRect.right = spriteWidth;
    destRect.bottom = yPos + fontWidth;
    srcRect.left = 0;
    srcRect.top = 0;
    srcRect.right = spriteWidth * 2;
#if defined(PSP)
    srcRect.bottom = rasterHeight;
#else
    srcRect.bottom = fontHeight * 2;
#endif
    if (srcRect.right > 1024)
    {
        srcRect.right = 1024;
    }
    outTexture->GetSurfaceLevel(0, &destSurface);
#if defined(PSP)
    th08::psp::RenderPerfNoteTextBytes(
        static_cast<std::uint32_t>(srcRect.right - srcRect.left) *
        static_cast<std::uint32_t>(srcRect.bottom - srcRect.top) * sizeof(WORD));
    th08_linux_surface_area_average_from_memory(
        destSurface, &destRect, textHelper.GetBuffer(),
        static_cast<UINT>(textHelper.GetWidth()),
        static_cast<UINT>(textHelper.GetHeight()),
        textHelper.GetImageWidthInBytes(), textHelper.GetFormat(), &srcRect, 0);
#else
    D3DXLoadSurfaceFromSurface(destSurface, NULL, &destRect, g_TextBufferSurface, NULL, &srcRect, 4, 0);
#endif
    SAFE_RELEASE(destSurface);
    return;
}

#pragma function(strlen)
#pragma var_order(hdc, font, textSurfaceDesc, h, textHelper, hdc, srcRect, destRect, destSurface)
void TextHelper::RenderTextToTexture(i32 xPos, i32 yPos, i32 spriteWidth, i32 spriteHeight, i32 fontHeight,
                                     i32 fontWidth, COLORREF textColor, COLORREF outlineType, const char *string,
                                     IDirect3DTexture8 *outTexture)
{
    HGDIOBJ h;
    LPDIRECT3DSURFACE8 destSurface;
    RECT destRect;
    RECT srcRect;
    D3DSURFACE_DESC textSurfaceDesc;
    HFONT font;
    HDC hdc;

    font = CreateFontA(fontHeight * 2, 0, 0, 0, FW_NORMAL, false, false, false, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FF_ROMAN | FIXED_PITCH, TH_FONT_NAME);
#if defined(PSP)
    TextHelper &textHelper = g_PspTextWorkBuffer;
    if (font == NULL || !textHelper.IsAllocated())
    {
        if (font != NULL)
            DeleteObject(font);
        return;
    }
#else
    TextHelper textHelper;
#endif
#if defined(PSP)
    memset(&textSurfaceDesc, 0, sizeof(textSurfaceDesc));
    textSurfaceDesc.Width = 1024;
    textSurfaceDesc.Height = TEXT_BUFFER_HEIGHT;
    textSurfaceDesc.Format = D3DFMT_A1R5G5B5;
    const i32 rasterHeight = PspTextRasterHeight(fontHeight);
    if (!textHelper.AllocateBufferWithFallback(
            textSurfaceDesc.Width, textSurfaceDesc.Height, textSurfaceDesc.Format))
    {
        DeleteObject(font);
        return;
    }
#else
    g_TextBufferSurface->GetDesc(&textSurfaceDesc);
    textHelper.AllocateBufferWithFallback(textSurfaceDesc.Width, textSurfaceDesc.Height, textSurfaceDesc.Format);
#endif
    hdc = textHelper.GetHDC();
    h = SelectObject(hdc, font);
#if defined(PSP)
    textHelper.InvertAlpha(0, 0, textSurfaceDesc.Width, rasterHeight, FALSE);
#else
    textHelper.InvertAlpha(0, 0, textSurfaceDesc.Width, fontHeight * 2 + 6, FALSE);
#endif
    SetBkMode(hdc, TRANSPARENT);

    // Render outline.
    if (outlineType != 0xffffffff)
    {
        SetTextColor(hdc, 0);
        TextOutA(hdc, xPos * 2 + 4, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 0, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 4, string, strlen(string));
    }
    else
    {
        SetTextColor(hdc, 0);
        TextOutA(hdc, xPos * 2 + 3, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 1, 2, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 1, string, strlen(string));
        TextOutA(hdc, xPos * 2 + 2, 3, string, strlen(string));
    }
    // Render main text.
    SetTextColor(hdc, textColor);
    TextOutA(hdc, xPos * 2 + 2, 2, string, strlen(string));

    SelectObject(hdc, h);
#if defined(PSP)
    textHelper.InvertAlpha(0, 0, textSurfaceDesc.Width, rasterHeight,
                           outlineType == 0xffffffff);
#else
    textHelper.InvertAlpha(0, 0, textSurfaceDesc.Width, fontHeight * 2 + 6, outlineType == 0xffffffff);
#endif
#if !defined(PSP)
    textHelper.CopyTextToSurface(g_TextBufferSurface);
#endif
    SelectObject(hdc, h);
    DeleteObject(font);
    destRect.left = 0;
    destRect.top = yPos;
    destRect.right = spriteWidth;
    destRect.bottom = yPos + fontWidth;
    srcRect.left = 0;
    srcRect.top = 0;
    srcRect.right = spriteWidth * 2;
#if defined(PSP)
    srcRect.bottom = rasterHeight;
#else
    srcRect.bottom = fontHeight * 2;
#endif
    if (srcRect.right > 1024)
    {
        srcRect.right = 1024;
    }
    outTexture->GetSurfaceLevel(0, &destSurface);
#if defined(PSP)
    th08::psp::RenderPerfNoteTextBytes(
        static_cast<std::uint32_t>(srcRect.right - srcRect.left) *
        static_cast<std::uint32_t>(srcRect.bottom - srcRect.top) * sizeof(WORD));
    th08_linux_surface_area_average_from_memory(
        destSurface, &destRect, textHelper.GetBuffer(),
        static_cast<UINT>(textHelper.GetWidth()),
        static_cast<UINT>(textHelper.GetHeight()),
        textHelper.GetImageWidthInBytes(), textHelper.GetFormat(), &srcRect, 0);
#else
    D3DXLoadSurfaceFromSurface(destSurface, NULL, &destRect, g_TextBufferSurface, NULL, &srcRect, 4, 0);
#endif
    SAFE_RELEASE(destSurface);
    return;
}
}; // namespace th08
