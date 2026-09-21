#include "th_pch.h"
#include "dialogue_text_cache.hpp"
#include "text_row_codec.hpp"
#include "Gui.hpp"
#include "GameManager.hpp"
#include "Supervisor.hpp"
#include "TextHelper.hpp"
#include "modern/linux/d3d8_internal.hpp"
#include "fileio.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" void th08_psp_gdi_text_trim_cache();
// Avoid the SDK's global u32 typedef colliding with the game's shared types.
extern "C" long long sceKernelGetSystemTimeWide();
extern "C" int kuKernelGetModel();

namespace th08::psp
{
namespace
{
constexpr unsigned kScratchBytes = 64 * 1024;
struct Entry
{
    TextRowKey key;
    IDirect3DTexture8 *texture;
    unsigned text, textBytes, payload, encodedBytes, rawBytes, pitch;
    D3DFORMAT format;
};
const GuiMessageFile *source;
unsigned sourceBytes, colors[4];
unsigned char *arena;
unsigned capacity, entries, dataBegin, requests, covered, hits, misses, failed;
unsigned long long hitUs, maxHitUs;
bool warming, ready, attempted;
IDirect3DTexture8 *dialogueTextures[2];
Entry *Rows() { return reinterpret_cast<Entry *>(arena + kScratchBytes); }
bool Same(const Entry &e, const TextRowKey &key, const char *text, unsigned bytes,
          IDirect3DTexture8 *texture)
{
    return e.texture == texture && e.textBytes == bytes &&
        !std::memcmp(&e.key, &key, sizeof(key)) && !std::memcmp(arena + e.text, text, bytes);
}
Entry *Find(const TextRowKey &key, const char *text, IDirect3DTexture8 *texture)
{
    if (!arena || !text) return nullptr;
    const unsigned bytes = std::strlen(text) + 1;
    for (unsigned i = 0; i < entries; ++i) if (Same(Rows()[i], key, text, bytes, texture)) return Rows() + i;
    return nullptr;
}
bool RowShape(const TextRowKey &key, IDirect3DTexture8 *texture,
              D3DSURFACE_DESC &desc, RECT &rect, unsigned &pitch, unsigned &bytes)
{
    if (!texture || FAILED(texture->GetLevelDesc(0, &desc)) || key.x < 0 || key.y < 0 ||
        key.width <= 0 || key.fontWidth <= 0 || key.fontHeight <= 0 ||
        static_cast<unsigned>(key.y) >= desc.Height) return false;
    const unsigned bpp = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8 ? 4 :
        desc.Format == D3DFMT_A1R5G5B5 || desc.Format == D3DFMT_A4R4G4B4 ||
        desc.Format == D3DFMT_R5G6B5 || desc.Format == D3DFMT_X1R5G5B5 ? 2 : 0;
    rect = {0, key.y, static_cast<LONG>(std::min<unsigned>(key.width, desc.Width)),
            static_cast<LONG>(std::min<unsigned>(key.y + key.fontWidth, desc.Height))};
    pitch = rect.right * bpp;
    bytes = pitch * (rect.bottom - rect.top);
    return bpp && pitch && bytes && bytes <= kScratchBytes;
}
bool DecodeMessage(const unsigned char *p, unsigned bytes, char (&out)[64])
{
    for (unsigned i = 0; i < bytes && i < sizeof(out); ++i)
    {
        out[i] = p[i] ^ 0x77;
        if (!out[i]) return true;
        // Retail passes these strings as printf formats. Refuse unknown
        // format directives instead of inventing missing variadic arguments.
        if (out[i] == '%') return false;
    }
    return false;
}
bool DialogueGeometry(AnmVm (&lines)[2])
{
    auto *anm = g_Supervisor.textAnm;
    if (!anm || !anm->rawData || !anm->sprites || !anm->scripts ||
        anm->rawData->numSprites < 2 || anm->rawData->numScripts < 2 ||
        anm->numberEntriesToBeLoaded) return false;
    // Stock TH08 text.anm, independently checked against the local asset.
    // Read a fixed 76-byte prefix, never execute a probe ANM VM: even a
    // restored RNG would not undo a variable-write instruction's side effects.
    static const int opcodes[] = {3, 22, 6, 8, 15, 2};
    static const unsigned sizes[] = {12, 8, 20, 12, 16, 8};
    for (unsigned line = 0; line < 2; ++line)
    {
        if (!anm->scripts[line]) return false;
        auto *p = reinterpret_cast<const unsigned char *>(anm->scripts[line]);
        for (unsigned i = 0; i < 6; ++i)
        {
            const auto *instruction = reinterpret_cast<const AnmRawInstr *>(p);
            if (instruction->opcode != opcodes[i] || instruction->instructionSize != sizes[i] ||
                instruction->time != (i == 5 ? 12 : 0) || instruction->varMask != 0) return false;
            if (!i && instruction->intArgs[0] != static_cast<int>(line)) return false;
            p += sizes[i];
        }
        lines[line].loadedSprite = &anm->sprites[line];
        lines[line].fontWidth = lines[line].fontHeight = 15;
        if (!lines[line].loadedSprite->texture) return false;
        dialogueTextures[line] = lines[line].loadedSprite->texture;
    }
    return true;
}
bool Scan(AnmVm (&lines)[2])
{
    if (!source || sourceBytes < 4 || source->messageCount <= 0 ||
        static_cast<unsigned>(source->messageCount) > (sourceBytes - 4) / 4) return false;
    const auto *file = reinterpret_cast<const unsigned char *>(source);
    const unsigned header = 4 + source->messageCount * 4;
    const auto offset = [&](unsigned n) -> unsigned {
        const auto address = reinterpret_cast<std::uintptr_t>(source->messages[n]);
        const auto base = reinterpret_cast<std::uintptr_t>(source);
        return address < base || address - base > sourceBytes ? sourceBytes + 1 : address - base;
    };
    for (int n = 0; n < source->messageCount; ++n)
        if (offset(n) < header || offset(n) >= sourceBytes) return false;
    auto draw = [&](unsigned line, unsigned color, const char *text) {
        g_AnmManager->DrawTextLeft(&lines[line], colors[color], 0, "%s", text);
    };
    for (unsigned color = 0; color < 4; ++color) draw(1, color, " ");
    for (int n = 0; n < source->messageCount; ++n)
    {
        unsigned pos = offset(n), end = sourceBytes, color = 0;
        for (int j = 0; j < source->messageCount; ++j)
            if (offset(j) > pos && offset(j) < end) end = offset(j);
        bool terminated = false;
        while (pos < end)
        {
            if (g_Supervisor.subthreadCloseRequestActive || end - pos < 4) return false;
            const unsigned opcode = file[pos + 2], bytes = file[pos + 3];
            if (bytes > end - pos - 4) return false;
            const auto *args = file + pos + 4;
            if (opcode == GUI_MSG_DELETE) { terminated = true; break; }
            if (opcode == GUI_MSG_CONFIGURE_ALL_PORTRAITS || opcode == GUI_MSG_CONFIGURE_PORTRAIT)
            {
                if (bytes < 4) return false;
                unsigned nextColor; std::memcpy(&nextColor, args, 4);
                if (nextColor >= 4) return false;
                color = nextColor;
            }
            char text[64];
            if (opcode == GUI_MSG_SHOW_DIALOGUE_TEXT)
            {
                if (bytes < 5) return false;
                unsigned short c, line; std::memcpy(&c, args, 2); std::memcpy(&line, args + 2, 2);
                if (c >= 4 || line >= 2 || !DecodeMessage(args + 4, bytes - 4, text)) return false;
                draw(line, c, text);
            }
            else if (opcode == GUI_MSG_SHOW_SPEAKER_TEXT || opcode == GUI_MSG_SHOW_TOP_TEXT ||
                     opcode == GUI_MSG_SHOW_BOTTOM_TEXT)
            {
                if (!DecodeMessage(args, bytes, text)) return false;
                if (opcode == GUI_MSG_SHOW_SPEAKER_TEXT)
                { draw(0, color, text); draw(1, color, text); }
                else draw(opcode == GUI_MSG_SHOW_BOTTOM_TEXT ? 1 : 0, 0, text);
            }
            pos += 4 + bytes;
            if (failed) return false;
        }
        if (!terminated) return false;
    }
    return true;
}
}

void ReleaseDialogueTextCache()
{
    if (arena)
        BootLog("TEXT_CACHE release ready=%u entries=%u hit=%u miss=%u hit_us=%llu hit_max_us=%llu bytes=%u\n",
                ready, entries, hits, misses, hitUs, maxHitUs, capacity - dataBegin);
    ready = warming = false;
    std::free(arena); arena = nullptr;
    source = nullptr; sourceBytes = capacity = entries = dataBegin = 0;
    requests = covered = hits = misses = failed = 0; hitUs = maxHitUs = 0;
    attempted = false; dialogueTextures[0] = dialogueTextures[1] = nullptr;
}
void DialogueTextSource(const void *file, unsigned bytes, const unsigned *textColors)
{
    ReleaseDialogueTextCache();
    source = static_cast<const GuiMessageFile *>(file); sourceBytes = bytes;
    std::memcpy(colors, textColors, sizeof(colors));
}
void PrewarmDialogueText()
{
#if defined(TH08_PSP_DIALOGUE_TEXT_CACHE) && !TH08_PSP_DIALOGUE_TEXT_CACHE
    return; // Ordinary live rasterization remains the complete fallback.
#endif
    if (attempted || !source) return;
    attempted = true;
    if (kuKernelGetModel() <= 0 || !g_Supervisor.textAnm) return;
    const auto start = sceKernelGetSystemTimeWide();
    // TH07's loading-only headroom guard. It remains allocated through
    // rasterization and is released before the stage starts its BGM.
    void *guard = std::malloc(2 * 1024 * 1024);
    if (!guard) { BootLog("TEXT_CACHE prewarm=OFF reason=headroom\n"); return; }
    for (unsigned bytes : {1536U * 1024, 768U * 1024, 384U * 1024, 256U * 1024})
        if ((arena = static_cast<unsigned char *>(std::malloc(bytes)))) { capacity = bytes; break; }
    if (!arena) { std::free(guard); BootLog("TEXT_CACHE prewarm=OFF reason=arena\n"); return; }
    dataBegin = capacity; warming = true;
    AnmVm lines[2];
    const bool geometry = DialogueGeometry(lines);
    const bool complete = geometry && Scan(lines);
    warming = false;
    ready = complete && !failed && requests && requests == covered;
    th08_psp_gdi_text_trim_cache();
    BootLog("TEXT_CACHE prewarm=%s stage=%d entries=%u covered=%u/%u failed=%u bytes=%u capacity=%u us=%llu geometry=%u\n",
            ready ? "READY" : "OFF", g_GameManager.currentStage, entries, covered, requests, failed,
            capacity - dataBegin + kScratchBytes + entries * static_cast<unsigned>(sizeof(Entry)),
            capacity, sceKernelGetSystemTimeWide() - start, geometry);
    if (!ready) { std::free(arena); arena = nullptr; }
    std::free(guard);
}
bool DrawCachedText(const TextRowKey &key, const char *text, IDirect3DTexture8 *texture)
{
    if (!warming && !ready) return false;
    if (warming) ++requests;
    Entry *e = Find(key, text, texture);
    if (!e)
    {
        // Spell/bomb/enemy labels share this atlas, but are not dialogue rows.
        if (!warming && (texture == dialogueTextures[0] || texture == dialogueTextures[1]))
            for (unsigned i = 0; i < entries; ++i)
                if (Rows()[i].texture == texture && !std::memcmp(&Rows()[i].key, &key, sizeof(key)))
                { ++misses; break; }
        return false;
    }
    if (warming) { ++covered; return true; }
    const auto start = sceKernelGetSystemTimeWide();
    D3DSURFACE_DESC desc; RECT rect; unsigned pitch, bytes;
    if (!RowShape(key, texture, desc, rect, pitch, bytes) || e->rawBytes != bytes ||
        e->pitch != pitch || e->format != desc.Format ||
        !DecodeTextRow(arena + e->payload, e->encodedBytes, arena, bytes) ||
        !th08_native_upload_text_row(texture, rect, arena, pitch, desc.Format))
    { ready = false; ++misses; return false; }
    ++hits;
    const auto elapsed = sceKernelGetSystemTimeWide() - start;
    hitUs += elapsed; maxHitUs = std::max<unsigned long long>(maxHitUs, elapsed);
    return true;
}
bool StorePrewarmedText(const TextRowKey &key, const char *text, IDirect3DTexture8 *texture,
                        const void *pixels, unsigned width, unsigned height, unsigned sourcePitch,
                        D3DFORMAT sourceFormat, const RECT &sourceRect)
{
    if (!warming) return false;
    D3DSURFACE_DESC desc; RECT row; unsigned pitch, bytes;
    if (!RowShape(key, texture, desc, row, pitch, bytes)) { ++failed; return true; }
    IDirect3DSurface8 *scratch = nullptr;
    if (FAILED(g_Supervisor.d3dDevice->CreateImageSurface(row.right, row.bottom - row.top, desc.Format, &scratch)))
    { ++failed; return true; }
    const RECT destination = {0, 0, row.right, row.bottom - row.top};
    const HRESULT result = th08_linux_surface_area_average_from_memory(scratch, &destination,
        pixels, width, height, sourcePitch, sourceFormat, &sourceRect, 0);
    LinuxSurfaceAccess view;
    bool ok = SUCCEEDED(result) && th08_linux_surface_access(scratch, &view, false) && view.pitch == pitch;
    const unsigned textBytes = std::strlen(text) + 1;
    const unsigned metadataEnd = kScratchBytes + (entries + 1) * sizeof(Entry);
    unsigned encoded = 0;
    if (ok)
    {
        // Worst-case expansion is <= 2x. Encode into the fixed scratch area;
        // actual dialogue rows are sparse, but capacity failure is atomic.
        encoded = EncodeTextRow(view.pixels, bytes, arena, kScratchBytes);
        ok = encoded && dataBegin >= metadataEnd && encoded + textBytes <= dataBegin - metadataEnd;
    }
    if (ok)
    {
        Entry e{}; e.key = key; e.texture = texture; e.rawBytes = bytes; e.pitch = pitch; e.format = desc.Format;
        dataBegin -= encoded; e.payload = dataBegin; e.encodedBytes = encoded;
        std::memcpy(arena + dataBegin, arena, encoded);
        dataBegin -= textBytes; e.text = dataBegin; e.textBytes = textBytes;
        std::memcpy(arena + dataBegin, text, textBytes);
        Rows()[entries++] = e; ++covered;
    }
    else ++failed;
    scratch->Release();
    return true;
}
}
