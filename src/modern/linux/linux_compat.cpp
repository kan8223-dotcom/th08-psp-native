#include "linux_compat.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <dinput.h>
#include <dsound.h>

#if defined(PSP)
#include <kubridge.h>
#include <pspctrl.h>

#include "Th08FontCoverage.hpp"
#include "audio_telemetry.hpp"
#include "audio_cursor_audit.hpp"
#include "boot_checkpoint.hpp"
#include "cp932_compact.generated.hpp"
#include "fileio.hpp"
#include "font_glyph_cache_policy.hpp"
#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
#include "io_activity_lamp.hpp"
#include "io_serialize.hpp"
#include "io_bounce_high.hpp"
#include "font_stream_cache.hpp"
#include "debug_start_stage.hpp"
#include "render_resource_arena.hpp"
#endif
#include "newlib_heap_geometry.hpp"
#else
#include <dlfcn.h>
#include <fontconfig/fontconfig.h>
#define TH08_PSP_BOOT_CHECKPOINT(phase, state, result) ((void)0)
#endif
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <iconv.h>
#include <limits.h>
#include <math.h>
#include <map>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace
{
enum HandleKind { HANDLE_FILE, HANDLE_THREAD, HANDLE_EVENT, HANDLE_MUTEX, HANDLE_FIND };

struct LinuxHandle
{
    explicit LinuxHandle(HandleKind value) : kind(value) {}
    virtual ~LinuxHandle() {}
    HandleKind kind;
};

#if TH08_PSP_IO_BOUNCE_HIGH_ENABLED
// Staging buffer inside the module image (always below the extended region).
constexpr uintptr_t kPspExtendedMemoryBase = 0x0A000000u;
constexpr DWORD kPspIoBounceBytes = 64u * 1024u;
static BYTE gPspIoBounce[kPspIoBounceBytes] __attribute__((aligned(64)));
#endif
struct FileHandle : LinuxHandle
{
    explicit FileHandle(int value) : LinuxHandle(HANDLE_FILE), fd(value)
    {
        pathTail[0] = '\0';
    }
    // Last bytes of the resolved path so slow-I/O records name the file.
    void TagPath(const char *path)
    {
        if (path == NULL) { pathTail[0] = '\0'; return; }
        const size_t length = strlen(path);
        const size_t keep = length < sizeof(pathTail) - 1 ? length : sizeof(pathTail) - 1;
        memcpy(pathTail, path + (length - keep), keep);
        pathTail[keep] = '\0';
    }
    ~FileHandle()
    {
        if (fd < 0)
            return;
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
        th08::psp::IoActivityScope ioActivity(
            th08::psp::IoActivityKind::Close, pathTail);
#endif
        close(fd);
    }
    int fd;
    char pathTail[40];
};

struct ThreadHandle : LinuxHandle
{
    ThreadHandle() : LinuxHandle(HANDLE_THREAD), finished(false), joined(false), result(0), id(0) {}
    pthread_t thread;
    volatile bool finished;
    bool joined;
    DWORD result;
    DWORD id;
    LPTHREAD_START_ROUTINE start;
    LPVOID parameter;
};

struct EventHandle : LinuxHandle
{
    EventHandle(bool manualReset, bool initial)
        : LinuxHandle(HANDLE_EVENT), manual(manualReset), signaled(initial)
    {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&condition, NULL);
    }
    ~EventHandle()
    {
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool manual;
    bool signaled;
};

struct MutexHandle : LinuxHandle
{
    MutexHandle() : LinuxHandle(HANDLE_MUTEX) { pthread_mutex_init(&mutex, NULL); }
    ~MutexHandle() { pthread_mutex_destroy(&mutex); }
    pthread_mutex_t mutex;
};

struct FindHandle : LinuxHandle
{
    FindHandle() : LinuxHandle(HANDLE_FIND), index(0) {}
    std::vector<std::string> paths;
    size_t index;
};

struct GdiObject
{
    enum Kind { BITMAP, FONT } kind;
    virtual ~GdiObject() {}
};

struct GdiBitmap : GdiObject
{
    GdiBitmap(int width_, int height_, int bits_)
        : width(width_), height(height_ < 0 ? -height_ : height_), bits(bits_)
    {
        kind = BITMAP;
        pitch = ((width * bits + 31) / 32) * 4;
        pixels.resize(pitch * height);
    }
    int width, height, bits, pitch;
    std::vector<BYTE> pixels;
};

struct GdiFont : GdiObject
{
#if defined(PSP)
    GdiFont()
        : font(NULL), pointSize(0), style(TTF_STYLE_NORMAL), sharedRuntime(false)
    {
        kind = FONT;
    }
    GdiFont(TTF_Font *font_, int pointSize_, int style_)
        : font(font_), pointSize(pointSize_), style(style_), sharedRuntime(font_ != NULL)
    {
        kind = FONT;
    }
    ~GdiFont() {}
    TTF_Font *font;
    int pointSize;
    int style;
    bool sharedRuntime;
#else
    GdiFont() : font(NULL) { kind = FONT; }
    explicit GdiFont(TTF_Font *font_) : font(font_) { kind = FONT; }
    ~GdiFont() { if (font != NULL) TTF_CloseFont(font); }
    TTF_Font *font;
#endif
};

struct GdiDc
{
    GdiDc() : bitmap(NULL), font(&stockFont), color(0xffffffff) {}
    GdiBitmap *bitmap;
    GdiFont stockFont;
    GdiFont *font;
    COLORREF color;
};

#if defined(PSP)
#if !defined(TH08_PSP_LOCAL_FONT_SUBSET)
#define TH08_PSP_LOCAL_FONT_SUBSET 0
#endif
#if !defined(TH08_PSP_FONT_GLYPH_CACHE_RETAIN)
#define TH08_PSP_FONT_GLYPH_CACHE_RETAIN 0
#endif

// PSP text rendering is serialized on the game thread. Keep one file-backed
// FreeType face alive from TextHelper startup through teardown so a late-stage
// text row never has to reopen and reparse the 4.3 MiB Noto file in a
// fragmented heap. GdiFont remains a lightweight Win32-compatible descriptor
// for the requested point size and weight; it never owns this shared face.
struct PspGdiTextRuntime
{
    TTF_Font *face;
    int currentPointSize;
    int currentStyle;
    int hardwareModel;
    unsigned int liveDescriptors;
    bool ownsTtfInitialization;
};

PspGdiTextRuntime g_pspGdiText = {};

constexpr size_t kExpectedStockFontCodepointCount = 1531u;
constexpr size_t kExpectedNameEntryCodepointCount = 94u;
static_assert(kTh08PspStockFontCodepointCount ==
                  kExpectedStockFontCodepointCount,
              "the local font profile must pin the audited 1531-codepoint stock union");
static_assert(kTh08PspNameEntryCodepointCount ==
                  kExpectedNameEntryCodepointCount,
              "the local font profile must include the complete name-entry charset");
static_assert(sizeof(kTh08PspStockFontCodepoints) /
                      sizeof(kTh08PspStockFontCodepoints[0]) ==
                  kExpectedStockFontCodepointCount,
              "the local font coverage array and count must agree");

struct PspFontCandidate
{
    const char *path;
    const char *source;
};

struct PspFontCoverageResult
{
    size_t provided;
    size_t missing;
    Uint32 firstMissing;
};

Uint32 PspCp932Codepoint(Uint32 encodedValue, size_t byteCount);

void LogPspFontHeap(const char *stage, const char *source)
{
    const th08::psp::NewlibHeapGeometrySnapshot heap =
        th08::psp::CaptureNewlibHeapGeometry();
    th08::psp::BootLog(
        "FONT_HEAP stage=%s source=%s committed_arena=%lu committed_used=%lu "
        "committed_free=%lu largest_free=%lu largest_nogrow=%lu top=%lu "
        "chunks=%lu scan_valid=%lu scan_errors=%lu scan_flags=0x%08lx\n",
        stage != NULL ? stage : "unknown", source != NULL ? source : "unknown",
        static_cast<unsigned long>(heap.arenaBytes),
        static_cast<unsigned long>(heap.usedBytes),
        static_cast<unsigned long>(heap.freeBytes),
        static_cast<unsigned long>(heap.largestFreeChunkBytes),
        static_cast<unsigned long>(heap.largestNoGrowRequestBytes),
        static_cast<unsigned long>(heap.topChunkBytes),
        static_cast<unsigned long>(heap.freeChunkCount),
        static_cast<unsigned long>(heap.scanValid),
        static_cast<unsigned long>(heap.scanErrorCount),
        static_cast<unsigned long>(heap.scanErrorFlags));
}

PspFontCoverageResult CheckPspFontCoverage(TTF_Font *font)
{
    PspFontCoverageResult result{};
    if (font == NULL)
    {
        result.missing = kExpectedStockFontCodepointCount;
        return result;
    }

    for (size_t index = 0; index < kExpectedStockFontCodepointCount; ++index)
    {
        const Uint32 codepoint = kTh08PspStockFontCodepoints[index];
        if (TTF_GlyphIsProvided32(font, codepoint) != 0)
        {
            ++result.provided;
        }
        else
        {
            if (result.firstMissing == 0)
                result.firstMissing = codepoint;
            ++result.missing;
        }
    }

    return result;
}

Uint32 PspCp932Codepoint(Uint32 encodedValue, size_t byteCount)
{
    const uint8_t input[2] = {
        static_cast<uint8_t>(byteCount == 2u ? encodedValue >> 8 : encodedValue),
        static_cast<uint8_t>(encodedValue & 0xffu),
    };
    const th08::cp932::DecodeResult result =
        th08::cp932::DecodeOne(input, byteCount);
    return result.status == th08::cp932::DecodeStatus::Valid &&
                   result.consumed == byteCount
               ? static_cast<Uint32>(result.codepoint)
               : 0u;
}

bool PspSubsetAuthorityMatchesRuntimeConverter()
{
    const Uint32 backslash = PspCp932Codepoint(0x5cu, 1u);
    const Uint32 tilde = PspCp932Codepoint(0x7eu, 1u);
    const Uint32 waveDash = PspCp932Codepoint(0x8160u, 2u);
    const Uint32 doubleVertical = PspCp932Codepoint(0x8161u, 2u);
    const Uint32 fullwidthMinus = PspCp932Codepoint(0x817cu, 2u);
    const Uint32 fullwidthCent = PspCp932Codepoint(0x8191u, 2u);
    const Uint32 fullwidthPound = PspCp932Codepoint(0x8192u, 2u);
    const Uint32 fullwidthNot = PspCp932Codepoint(0x81cau, 2u);
    const bool compatible =
        backslash == 0x005cu && tilde == 0x007eu &&
        waveDash == 0xff5eu && doubleVertical == 0x2225u &&
        fullwidthMinus == 0xff0du && fullwidthCent == 0xffe0u &&
        fullwidthPound == 0xffe1u && fullwidthNot == 0xffe2u;
    th08::psp::BootLog(
        "FONT_AUTHORITY codepoints=%lu profile_sha256=%s build_gate=%d "
        "decoder=python_cp932_v1 table_bytes=%lu "
        "runtime_5c=U+%04lX runtime_7e=U+%04lX runtime_8160=U+%04lX "
        "runtime_8161=U+%04lX runtime_817c=U+%04lX "
        "runtime_8191=U+%04lX runtime_8192=U+%04lX "
        "runtime_81ca=U+%04lX converter_match=%d subset_allowed=%d "
        "main_ram_copy=0\n",
        static_cast<unsigned long>(kExpectedStockFontCodepointCount),
        kTh08PspStockFontProfileSha256, TH08_PSP_LOCAL_FONT_SUBSET,
        static_cast<unsigned long>(sizeof(th08::cp932::kLeadRows) +
                                   sizeof(th08::cp932::kDoubleByteCodepoints)),
        static_cast<unsigned long>(backslash), static_cast<unsigned long>(tilde),
        static_cast<unsigned long>(waveDash),
        static_cast<unsigned long>(doubleVertical),
        static_cast<unsigned long>(fullwidthMinus),
        static_cast<unsigned long>(fullwidthCent),
        static_cast<unsigned long>(fullwidthPound),
        static_cast<unsigned long>(fullwidthNot),
        compatible ? 1 : 0,
        (TH08_PSP_LOCAL_FONT_SUBSET != 0 && compatible) ? 1 : 0);
    return compatible;
}

#if TH08_PSP_IO_SERIALIZE_ENABLED
// SDL_ttf/FreeType stream glyph data from the font file for the face's whole
// lifetime, i.e. during gameplay.  Route those reads through the Win32 bridge
// so they are bracketed, tagged and serialized like every other transaction.
namespace
{
#if TH08_PSP_FONT_STREAM_CACHE_ENABLED
constexpr Sint64 kPspFontBlockBytes = 4096;
constexpr int kPspFontBlocks = 8;
struct PspFontStream
{
    HANDLE handle;          // INVALID_HANDLE_VALUE once the file is resident
    const BYTE *resident;   // whole file in the renderer arena, or NULL
    Sint64 position;
    Sint64 size;
    Sint64 blockIndex[kPspFontBlocks];
    unsigned lastUse[kPspFontBlocks];
    unsigned useClock;
    BYTE blocks[kPspFontBlocks][kPspFontBlockBytes];
};
PspFontStream *PspFontStreamOf(SDL_RWops *context)
{
    return static_cast<PspFontStream *>(context->hidden.unknown.data1);
}
Sint64 SDLCALL PspFontStreamSize(SDL_RWops *context)
{
    return PspFontStreamOf(context)->size;
}
Sint64 SDLCALL PspFontStreamSeek(SDL_RWops *context, Sint64 offset, int whence)
{
    PspFontStream *stream = PspFontStreamOf(context);
    Sint64 target = whence == RW_SEEK_SET ? offset
                    : whence == RW_SEEK_CUR ? stream->position + offset
                                            : stream->size + offset;
    if (target < 0)
        return -1;
    stream->position = target;
    return target;
}
// Fetch one 4 KiB block through the bridge (one seek + one aligned read).
const BYTE *PspFontStreamBlock(PspFontStream *stream, Sint64 block)
{
    int slot = -1;
    for (int i = 0; i < kPspFontBlocks; ++i)
        if (stream->blockIndex[i] == block)
        {
            stream->lastUse[i] = ++stream->useClock;
            return stream->blocks[i];
        }
    unsigned oldest = 0xffffffffu;
    for (int i = 0; i < kPspFontBlocks; ++i)
        if (stream->blockIndex[i] < 0 || stream->lastUse[i] < oldest)
        {
            oldest = stream->blockIndex[i] < 0 ? 0 : stream->lastUse[i];
            slot = i;
            if (stream->blockIndex[i] < 0)
                break;
        }
    const DWORD offset = static_cast<DWORD>(block * kPspFontBlockBytes);
    if (SetFilePointer(stream->handle, static_cast<LONG>(offset), NULL, FILE_BEGIN) ==
        static_cast<DWORD>(-1))
        return NULL;
    DWORD got = 0;
    if (!ReadFile(stream->handle, stream->blocks[slot], static_cast<DWORD>(kPspFontBlockBytes),
                  &got, NULL))
        return NULL;
    if (got < kPspFontBlockBytes)
        memset(stream->blocks[slot] + got, 0, static_cast<size_t>(kPspFontBlockBytes - got));
    stream->blockIndex[slot] = block;
    stream->lastUse[slot] = ++stream->useClock;
    return stream->blocks[slot];
}
size_t SDLCALL PspFontStreamRead(SDL_RWops *context, void *ptr, size_t size, size_t maxnum)
{
    PspFontStream *stream = PspFontStreamOf(context);
    if (size == 0 || maxnum == 0)
        return 0;
    Sint64 remaining = static_cast<Sint64>(size * maxnum);
    if (stream->position >= stream->size)
        return 0;
    if (stream->position + remaining > stream->size)
        remaining = stream->size - stream->position;
    BYTE *out = static_cast<BYTE *>(ptr);
    Sint64 copied = 0;
    if (stream->resident != NULL)
    {
        memcpy(out, stream->resident + stream->position, static_cast<size_t>(remaining));
        stream->position += remaining;
        return static_cast<size_t>(remaining) / size;
    }
    while (copied < remaining)
    {
        const Sint64 at = stream->position + copied;
        const Sint64 block = at / kPspFontBlockBytes;
        const Sint64 within = at % kPspFontBlockBytes;
        const BYTE *data = PspFontStreamBlock(stream, block);
        if (data == NULL)
            break;
        Sint64 chunk = kPspFontBlockBytes - within;
        if (chunk > remaining - copied)
            chunk = remaining - copied;
        memcpy(out + copied, data + within, static_cast<size_t>(chunk));
        copied += chunk;
    }
    stream->position += copied;
    return static_cast<size_t>(copied) / size;
}
size_t SDLCALL PspFontStreamWrite(SDL_RWops *, const void *, size_t, size_t)
{
    return 0;
}
int SDLCALL PspFontStreamClose(SDL_RWops *context)
{
    PspFontStream *stream = PspFontStreamOf(context);
    if (stream != NULL)
    {
        if (stream->handle != INVALID_HANDLE_VALUE)
            CloseHandle(stream->handle);
        if (stream->resident != NULL)
            th08::psp::RenderResourceArenaFree(const_cast<BYTE *>(stream->resident));
        free(stream);
    }
    SDL_FreeRW(context);
    return 0;
}
SDL_RWops *OpenPspFontStream(const char *path)
{
    HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE || handle == NULL)
        return NULL;
    PspFontStream *stream = static_cast<PspFontStream *>(malloc(sizeof(PspFontStream)));
    SDL_RWops *rw = stream != NULL ? SDL_AllocRW() : NULL;
    if (rw == NULL)
    {
        free(stream);
        CloseHandle(handle);
        return NULL;
    }
    stream->handle = handle;
    stream->position = 0;
    DWORD high = 0;
    const DWORD low = GetFileSize(handle, &high);
    stream->size = low == static_cast<DWORD>(-1)
                       ? 0
                       : (static_cast<Sint64>(high) << 32) | static_cast<Sint64>(low);
    for (int i = 0; i < kPspFontBlocks; ++i)
    {
        stream->blockIndex[i] = -1;
        stream->lastUse[i] = 0;
    }
    stream->useClock = 0;
    stream->resident = NULL;
    // Read the whole file once with large sequential reads and close it, so
    // glyph loading never touches the storage driver again (PSP Go ef0 stalls
    // after any burst of seek+read pairs, whatever their size).
    // Go only (ef0): the Memory Stick driver streams fine and the 3000 keeps
    // its renderer arena headroom untouched.
    if (th08::psp::IoSerializeEnabled() && stream->size > 0 && stream->size < (8 << 20))
    {
        BYTE *resident = static_cast<BYTE *>(th08::psp::RenderResourceArenaAllocate(
            static_cast<size_t>(stream->size), 64, "font file resident"));
        if (resident != NULL)
        {
            bool complete = SetFilePointer(handle, 0, NULL, FILE_BEGIN) != static_cast<DWORD>(-1);
            Sint64 done = 0;
            while (complete && done < stream->size)
            {
                const DWORD want = static_cast<DWORD>(
                    stream->size - done < (64 << 10) ? stream->size - done : (64 << 10));
                DWORD got = 0;
                if (!ReadFile(handle, resident + done, want, &got, NULL) || got == 0)
                    complete = false;
                else
                    done += got;
            }
            if (complete)
            {
                stream->resident = resident;
                CloseHandle(handle);
                stream->handle = INVALID_HANDLE_VALUE;
            }
            else
                th08::psp::RenderResourceArenaFree(resident);
        }
    }
    th08::psp::BootLog("FONT_STREAM_CACHE blocks=%d block_bytes=%ld size=%ld resident=%d\n",
                       kPspFontBlocks, static_cast<long>(kPspFontBlockBytes),
                       static_cast<long>(stream->size), stream->resident != NULL ? 1 : 0);
    rw->size = PspFontStreamSize;
    rw->seek = PspFontStreamSeek;
    rw->read = PspFontStreamRead;
    rw->write = PspFontStreamWrite;
    rw->close = PspFontStreamClose;
    rw->type = SDL_RWOPS_UNKNOWN;
    rw->hidden.unknown.data1 = stream;
    return rw;
}
#else
HANDLE PspFontStreamHandle(SDL_RWops *context)
{
    return static_cast<HANDLE>(context->hidden.unknown.data1);
}
Sint64 SDLCALL PspFontStreamSize(SDL_RWops *context)
{
    DWORD high = 0;
    const DWORD low = GetFileSize(PspFontStreamHandle(context), &high);
    if (low == static_cast<DWORD>(-1))
        return -1;
    return (static_cast<Sint64>(high) << 32) | static_cast<Sint64>(low);
}
Sint64 SDLCALL PspFontStreamSeek(SDL_RWops *context, Sint64 offset, int whence)
{
    const DWORD origin = whence == RW_SEEK_SET ? FILE_BEGIN
                         : whence == RW_SEEK_CUR ? FILE_CURRENT : FILE_END;
    const DWORD result = SetFilePointer(PspFontStreamHandle(context),
                                        static_cast<LONG>(offset), NULL, origin);
    return result == static_cast<DWORD>(-1) ? -1 : static_cast<Sint64>(result);
}
size_t SDLCALL PspFontStreamRead(SDL_RWops *context, void *ptr, size_t size, size_t maxnum)
{
    if (size == 0 || maxnum == 0)
        return 0;
    DWORD got = 0;
    if (!ReadFile(PspFontStreamHandle(context), ptr,
                  static_cast<DWORD>(size * maxnum), &got, NULL))
        return 0;
    return static_cast<size_t>(got) / size;
}
size_t SDLCALL PspFontStreamWrite(SDL_RWops *, const void *, size_t, size_t)
{
    return 0;
}
int SDLCALL PspFontStreamClose(SDL_RWops *context)
{
    CloseHandle(PspFontStreamHandle(context));
    SDL_FreeRW(context);
    return 0;
}
SDL_RWops *OpenPspFontStream(const char *path)
{
    HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE || handle == NULL)
        return NULL;
    SDL_RWops *stream = SDL_AllocRW();
    if (stream == NULL)
    {
        CloseHandle(handle);
        return NULL;
    }
    stream->size = PspFontStreamSize;
    stream->seek = PspFontStreamSeek;
    stream->read = PspFontStreamRead;
    stream->write = PspFontStreamWrite;
    stream->close = PspFontStreamClose;
    stream->type = SDL_RWOPS_UNKNOWN;
    stream->hidden.unknown.data1 = handle;
    return stream;
}
#endif
} // namespace
#endif

TTF_Font *OpenCoverageCheckedPspFont(const PspFontCandidate &candidate)
{
    struct stat fileStatus{};
    const bool statOk = candidate.path != NULL && stat(candidate.path, &fileStatus) == 0;
    const unsigned long long fileBytes =
        statOk && fileStatus.st_size >= 0
            ? static_cast<unsigned long long>(fileStatus.st_size)
            : 0ULL;

    if (candidate.path == NULL || access(candidate.path, R_OK) != 0)
    {
        th08::psp::BootLog(
            "FONT candidate source=%s face=0 path=%s bytes=%llu stat_ok=%d "
            "result=absent coverage=0/%lu missing=%lu first_missing=n/a "
            "main_ram_copy=0\n",
            candidate.source != NULL ? candidate.source : "unknown",
            candidate.path != NULL ? candidate.path : "(null)", fileBytes,
            statOk ? 1 : 0,
            static_cast<unsigned long>(kExpectedStockFontCodepointCount),
            static_cast<unsigned long>(kExpectedStockFontCodepointCount));
        return NULL;
    }

    LogPspFontHeap("before_open", candidate.source);
#if TH08_PSP_IO_SERIALIZE_ENABLED
    SDL_RWops *fontStream = OpenPspFontStream(candidate.path);
    TTF_Font *font = fontStream != NULL ? TTF_OpenFontRW(fontStream, 1, 10) : NULL;
#else
    TTF_Font *font = TTF_OpenFont(candidate.path, 10);
#endif
    LogPspFontHeap("after_open", candidate.source);
    if (font == NULL)
    {
        th08::psp::BootLog(
            "FONT candidate source=%s face=0 path=%s bytes=%llu stat_ok=%d "
            "result=open_fail coverage=0/%lu missing=%lu first_missing=n/a "
            "main_ram_copy=0 error=%s\n",
            candidate.source, candidate.path, fileBytes, statOk ? 1 : 0,
            static_cast<unsigned long>(kExpectedStockFontCodepointCount),
            static_cast<unsigned long>(kExpectedStockFontCodepointCount),
            TTF_GetError());
        return NULL;
    }

    const PspFontCoverageResult coverage = CheckPspFontCoverage(font);
    LogPspFontHeap("after_coverage", candidate.source);
    if (coverage.missing != 0)
    {
        th08::psp::BootLog(
            "FONT candidate source=%s face=0 path=%s bytes=%llu stat_ok=%d "
            "result=reject coverage=%lu/%lu missing=%lu first_missing=U+%04lX "
            "main_ram_copy=0\n",
            candidate.source, candidate.path, fileBytes, statOk ? 1 : 0,
            static_cast<unsigned long>(coverage.provided),
            static_cast<unsigned long>(kExpectedStockFontCodepointCount),
            static_cast<unsigned long>(coverage.missing),
            static_cast<unsigned long>(coverage.firstMissing));
        TTF_CloseFont(font);
        LogPspFontHeap("after_reject_close", candidate.source);
        return NULL;
    }

    th08::psp::BootLog(
        "FONT selected source=%s face=0 path=%s bytes=%llu stat_ok=%d "
        "coverage=%lu/%lu missing=0 first_missing=none main_ram_copy=0\n",
        candidate.source, candidate.path, fileBytes, statOk ? 1 : 0,
        static_cast<unsigned long>(coverage.provided),
        static_cast<unsigned long>(kExpectedStockFontCodepointCount));
    return font;
}
#endif

DWORD g_lastError;
WNDPROC g_windowProcedure;
SDL_Window *g_window;
std::map<DWORD, std::vector<MSG> > g_threadMessages;
pthread_mutex_t g_messageMutex = PTHREAD_MUTEX_INITIALIZER;

std::string ExecutableSiblingPath(const char *filename)
{
#if defined(PSP)
    const char *directory = th08::psp::GameDirectory();
    if (directory == NULL || filename == NULL)
        return std::string();
    return std::string(directory) + "/" + filename;
#else
    char path[PATH_MAX + 1];
    ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
    if (count <= 0 || count > PATH_MAX)
        return std::string();
    path[count] = '\0';
    char *separator = strrchr(path, '/');
    if (separator == NULL)
        return std::string();
    separator[1] = '\0';
    return std::string(path) + filename;
#endif
}

#if defined(PSP)
bool EqualAsciiIgnoreCase(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
        return false;
    while (*left != '\0' && *right != '\0')
    {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return *left == *right;
}

// Writable state deliberately remains relative to the EBOOT directory.  Only
// the two immutable retail archives may live in the separately discovered
// data directory, so resolve exactly those simple relative names.
std::string ResolvePspArchivePath(const char *path)
{
    if (path == NULL)
        return std::string();

    const char *name = path;
    while (name[0] == '.' && (name[1] == '/' || name[1] == '\\'))
        name += 2;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strchr(name, ':') != NULL ||
        (!EqualAsciiIgnoreCase(name, "th08.dat") &&
         !EqualAsciiIgnoreCase(name, "thbgm.dat")))
        return std::string(path);

    const char *dataDirectory = th08::psp::DataDirectory();
    if (dataDirectory == NULL || dataDirectory[0] == '\0')
        return std::string(path);
    return std::string(dataDirectory) + "/" + name;
}
#endif

void SetApplicationIcon(SDL_Window *window)
{
    if (window == NULL)
        return;
    const std::string iconPath = ExecutableSiblingPath("th08-modern.png");
    if (iconPath.empty())
        return;
    SDL_Surface *icon = IMG_Load(iconPath.c_str());
    if (icon == NULL)
        return;
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
}

DWORD CurrentThreadIdImpl()
{
#if defined(PSP)
    return static_cast<DWORD>(reinterpret_cast<uintptr_t>(pthread_self()));
#else
    return static_cast<DWORD>(pthread_self());
#endif
}

std::string ConvertCp932ToUtf8(const char *text, size_t length,
                               bool *conversionValid)
{
    if (conversionValid != NULL)
        *conversionValid = true;
    if (text == NULL || length == 0)
        return std::string();
#if defined(PSP)
    // newlib's PSP iconv build only links its basic Unicode/Latin tables;
    // CP932 requires external NLS files that do not exist on a Memory Stick.
    // Decode through the generated Windows-31J table used by the stock-font
    // authority. Keep the explicit length because TextOutA strings are not
    // required to be NUL terminated. Invalid/truncated input fails visibly;
    // silently skipping a byte could select the wrong glyph from a subset.
    std::string output;
    output.reserve(length * 3);
    size_t inputIndex = 0;
    while (inputIndex < length)
    {
        const uint8_t *input =
            reinterpret_cast<const uint8_t *>(text) + inputIndex;
        const th08::cp932::DecodeResult decoded =
            th08::cp932::DecodeOne(input, length - inputIndex);
        if (decoded.status != th08::cp932::DecodeStatus::Valid ||
            decoded.consumed == 0u)
        {
            if (conversionValid != NULL)
                *conversionValid = false;
            static bool loggedDecodeFailure = false;
            if (!loggedDecodeFailure)
            {
                const unsigned long byte1 =
                    inputIndex + 1u < length
                        ? static_cast<unsigned long>(input[1])
                        : 0x100ul;
                th08::psp::BootLog(
                    "FONT CP932 decode_error offset=%lu remaining=%lu "
                    "status=%u byte0=0x%02lX byte1=%s0x%02lX\n",
                    static_cast<unsigned long>(inputIndex),
                    static_cast<unsigned long>(length - inputIndex),
                    static_cast<unsigned int>(decoded.status),
                    static_cast<unsigned long>(input[0]),
                    byte1 == 0x100ul ? "absent/" : "",
                    byte1 == 0x100ul ? 0ul : byte1);
                loggedDecodeFailure = true;
            }
            return std::string();
        }
        inputIndex += decoded.consumed;

        const uint32_t unicode = decoded.codepoint;
        if (unicode < 0x80)
        {
            output.push_back(static_cast<char>(unicode));
        }
        else if (unicode < 0x800)
        {
            output.push_back(static_cast<char>(0xc0 | (unicode >> 6)));
            output.push_back(static_cast<char>(0x80 | (unicode & 0x3f)));
        }
        else
        {
            output.push_back(static_cast<char>(0xe0 | (unicode >> 12)));
            output.push_back(static_cast<char>(0x80 | ((unicode & 0x0fff) >> 6)));
            output.push_back(static_cast<char>(0x80 | (unicode & 0x3f)));
        }
    }
    return output;
#else
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == reinterpret_cast<iconv_t>(-1))
    {
        if (conversionValid != NULL)
            *conversionValid = false;
        return std::string(text, length);
    }

    std::vector<char> output(length * 4 + 8, 0);
    char *input = const_cast<char *>(text);
    char *destination = &output[0];
    size_t inputLeft = length;
    size_t outputLeft = output.size() - 1;
    if (iconv(converter, &input, &inputLeft, &destination, &outputLeft) == static_cast<size_t>(-1))
    {
        iconv_close(converter);
        if (conversionValid != NULL)
            *conversionValid = false;
        return std::string(text, length);
    }
    iconv_close(converter);
    return std::string(&output[0], destination - &output[0]);
#endif
}

#if !defined(PSP)
const char *ResolveJapaneseFont()
{
    static std::string path;
    static bool resolved;
    if (resolved) return path.empty() ? NULL : path.c_str();
    resolved = true;

    const char *overridePath = getenv("TH08_FONT");
    if (overridePath != NULL && access(overridePath, R_OK) == 0)
    {
        path = overridePath;
        return path.c_str();
    }

    if (!FcInit()) return NULL;
    FcPattern *pattern = FcPatternCreate();
    if (pattern == NULL) return NULL;
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>("VL Gothic"));
    FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8 *>("ja"));
    FcPatternAddInteger(pattern, FC_SPACING, FC_MONO);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (match == NULL) return NULL;
    FcChar8 *file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != NULL)
        path = reinterpret_cast<const char *>(file);
    FcPatternDestroy(match);
    return path.empty() ? NULL : path.c_str();
}
#endif

#if defined(PSP)
bool InitializePspGdiText()
{
    if (g_pspGdiText.face != NULL)
        return true;
    if (g_pspGdiText.liveDescriptors != 0)
    {
        fprintf(stderr, "TH08PSP FONT init rejected live_descriptors=%u\n",
                g_pspGdiText.liveDescriptors);
        return false;
    }

    const bool ownsTtfInitialization = TTF_WasInit() == 0;
    if (ownsTtfInitialization && TTF_Init() != 0)
    {
        fprintf(stderr, "TH08PSP FONT SDL_ttf initialization failed: %s\n",
                TTF_GetError());
        return false;
    }

    const bool converterMatchesAuthority =
        PspSubsetAuthorityMatchesRuntimeConverter();
    TTF_Font *face = NULL;

    // An explicit developer override retains highest priority, but it now
    // obeys the same complete stock-coverage contract as every default face.
    // A rejected override safely falls through instead of reaching gameplay
    // with a latent missing-glyph failure.
    const char *overridePath = getenv("TH08_FONT");
    if (overridePath != NULL && overridePath[0] != '\0' &&
        access(overridePath, R_OK) == 0)
    {
        const PspFontCandidate overrideCandidate = {overridePath, "override"};
        face = OpenCoverageCheckedPspFont(overrideCandidate);
    }

#if TH08_PSP_LOCAL_FONT_SUBSET
    if (face == NULL && converterMatchesAuthority)
    {
        const std::string path = ExecutableSiblingPath("msgothic-subset.ttf");
        const PspFontCandidate candidate = {path.c_str(), "subset"};
        face = OpenCoverageCheckedPspFont(candidate);
    }
#else
    (void)converterMatchesAuthority;
#endif

    if (face == NULL)
    {
        const std::string path = ExecutableSiblingPath("msgothic.ttc");
        const PspFontCandidate candidate = {path.c_str(), "msgothic"};
        face = OpenCoverageCheckedPspFont(candidate);
    }
    if (face == NULL && converterMatchesAuthority)
    {
        // Redistributable OFL fallback generated from the same 1,531-scalar
        // authority. Its primary names deliberately avoid the upstream
        // Reserved Font Name; coverage is still verified at runtime before
        // the face can become the shared text owner.
        const std::string path =
            ExecutableSiblingPath("TH08PspSubsetSansJP-Regular.otf");
        const PspFontCandidate candidate = {path.c_str(), "ofl_subset"};
        face = OpenCoverageCheckedPspFont(candidate);
    }
    if (face == NULL)
    {
        const std::string path = ExecutableSiblingPath("NotoSansJP-Regular.ttf");
        const PspFontCandidate candidate = {path.c_str(), "noto"};
        face = OpenCoverageCheckedPspFont(candidate);
    }

    if (face == NULL)
    {
        fprintf(stderr, "TH08PSP FONT no complete coverage candidate: %s\n",
                TTF_GetError());
        th08::psp::BootLog(
            "FONT selected source=none result=failed coverage=0/%lu "
            "main_ram_copy=0\n",
            static_cast<unsigned long>(kExpectedStockFontCodepointCount));
        if (ownsTtfInitialization)
            TTF_Quit();
        return false;
    }

    TTF_SetFontStyle(face, TTF_STYLE_NORMAL);
    TTF_SetFontHinting(face, TTF_HINTING_LIGHT);
#if TH08_PSP_FONT_GLYPH_CACHE_RETAIN
    const int hardwareModel = kuKernelGetModel();
#else
    // Do not add a model-query dependency to the established OFF path.
    const int hardwareModel = -1;
#endif
    g_pspGdiText.face = face;
    g_pspGdiText.currentPointSize = 10;
    g_pspGdiText.currentStyle = TTF_STYLE_NORMAL;
    g_pspGdiText.hardwareModel = hardwareModel;
    g_pspGdiText.liveDescriptors = 0;
    g_pspGdiText.ownsTtfInitialization = ownsTtfInitialization;
    th08::psp::BootLog(
        "FONT shared_face=file ready=1 coverage_required=%lu main_ram_copy=0\n",
        static_cast<unsigned long>(kExpectedStockFontCodepointCount));
    th08::psp::BootLog(
        "FONT glyph_cache_retain feature=%d model=%d eligible=%d\n",
        TH08_PSP_FONT_GLYPH_CACHE_RETAIN != 0 ? 1 : 0, hardwareModel,
        TH08_PSP_FONT_GLYPH_CACHE_RETAIN != 0 &&
                th08::psp::IsPspFontGlyphCacheRetainModel(hardwareModel)
            ? 1
            : 0);
    return true;
}

bool ConfigurePspGdiFont(GdiFont *font)
{
    if (font == NULL || !font->sharedRuntime || font->font == NULL ||
        font->font != g_pspGdiText.face || font->pointSize <= 0)
        return false;

    if (g_pspGdiText.currentPointSize != font->pointSize)
    {
        if (TTF_SetFontSize(g_pspGdiText.face, font->pointSize) != 0)
        {
            fprintf(stderr, "TH08PSP FONT size failed point=%d: %s\n",
                    font->pointSize, TTF_GetError());
            return false;
        }
        g_pspGdiText.currentPointSize = font->pointSize;
    }
    if (g_pspGdiText.currentStyle != font->style)
    {
        // Always restore NORMAL explicitly after a bold descriptor. Otherwise
        // the shared face would leak the prior row's weight into later text.
        TTF_SetFontStyle(g_pspGdiText.face, font->style);
        g_pspGdiText.currentStyle = font->style;
    }
    return true;
}

void ReleasePspGdiFontDescriptor(GdiFont *font)
{
    if (font == NULL || !font->sharedRuntime)
        return;

    if (font->font != g_pspGdiText.face || g_pspGdiText.liveDescriptors == 0)
    {
        fprintf(stderr, "TH08PSP FONT descriptor ownership mismatch\n");
        font->font = NULL;
        font->sharedRuntime = false;
        return;
    }

    const bool retainGlyphCache = th08::psp::ShouldRetainPspFontGlyphCache(
        TH08_PSP_FONT_GLYPH_CACHE_RETAIN != 0,
        g_pspGdiText.hardwareModel, g_pspGdiText.face,
        g_pspGdiText.currentPointSize, font->font, font->pointSize);
    --g_pspGdiText.liveDescriptors;
    font->font = NULL;
    font->sharedRuntime = false;

    if (g_pspGdiText.liveDescriptors == 0 && g_pspGdiText.face != NULL &&
        g_pspGdiText.currentPointSize > 0 && !retainGlyphCache)
    {
        // TH07 proved this same-size call as a deterministic glyph-cache
        // flush. The default-OFF path and PSP-1000 retain it once after the
        // complete outline/main-text group, never between its five TextOutA
        // passes. Slim+/Go may skip it only for the exact same face and size.
        if (TTF_SetFontSize(g_pspGdiText.face, g_pspGdiText.currentPointSize) != 0)
        {
            fprintf(stderr, "TH08PSP FONT glyph flush failed point=%d: %s\n",
                    g_pspGdiText.currentPointSize, TTF_GetError());
        }
    }
}

void ShutdownPspGdiText()
{
    if (g_pspGdiText.face == NULL)
        return;
    if (g_pspGdiText.liveDescriptors != 0)
    {
        // A leak is preferable to closing a face still borrowed by a GDI
        // descriptor. Normal TextHelper teardown reaches this with zero.
        fprintf(stderr, "TH08PSP FONT shutdown deferred live_descriptors=%u\n",
                g_pspGdiText.liveDescriptors);
        return;
    }

    TTF_CloseFont(g_pspGdiText.face);
    g_pspGdiText.face = NULL;
    if (g_pspGdiText.ownsTtfInitialization)
        TTF_Quit();
    g_pspGdiText = {};
    th08::psp::BootLog("FONT shared_face=file released=1\n");
}
#endif

void PutGdiTextPixel(GdiBitmap *bitmap, int x, int y, COLORREF color, BYTE coverage)
{
    if (bitmap == NULL || x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height || coverage == 0) return;
    const int red = color & 0xff;
    const int green = (color >> 8) & 0xff;
    const int blue = (color >> 16) & 0xff;
    BYTE *pixel = &bitmap->pixels[y * bitmap->pitch + x * bitmap->bits / 8];
    if (bitmap->bits == 16)
    {
        uint16_t packed;
        memcpy(&packed, pixel, sizeof(packed));
        int oldRed = ((packed >> 10) & 0x1f) * 255 / 31;
        int oldGreen = ((packed >> 5) & 0x1f) * 255 / 31;
        int oldBlue = (packed & 0x1f) * 255 / 31;
        oldRed = (oldRed * (255 - coverage) + red * coverage) / 255;
        oldGreen = (oldGreen * (255 - coverage) + green * coverage) / 255;
        oldBlue = (oldBlue * (255 - coverage) + blue * coverage) / 255;
        packed = static_cast<uint16_t>(((oldRed >> 3) << 10) | ((oldGreen >> 3) << 5) | (oldBlue >> 3));
        memcpy(pixel, &packed, sizeof(packed));
    }
    else if (bitmap->bits == 32)
    {
        pixel[0] = static_cast<BYTE>((pixel[0] * (255 - coverage) + blue * coverage) / 255);
        pixel[1] = static_cast<BYTE>((pixel[1] * (255 - coverage) + green * coverage) / 255);
        pixel[2] = static_cast<BYTE>((pixel[2] * (255 - coverage) + red * coverage) / 255);
        pixel[3] = 0;
    }
}

void *ThreadTrampoline(void *opaque)
{
    ThreadHandle *handle = static_cast<ThreadHandle *>(opaque);
    handle->id = CurrentThreadIdImpl();
    handle->result = handle->start(handle->parameter);
    handle->finished = true;
    return NULL;
}

bool FillFindData(FindHandle *handle, WIN32_FIND_DATAA *data)
{
    if (handle->index >= handle->paths.size())
        return false;
    memset(data, 0, sizeof(*data));
    const std::string &path = handle->paths[handle->index++];
    const char *name = strrchr(path.c_str(), '/');
    strncpy(data->cFileName, name != NULL ? name + 1 : path.c_str(), MAX_PATH - 1);
    struct stat info;
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(
        th08::psp::IoActivityKind::Metadata, path.c_str());
#endif
    if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
        data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    return true;
}

void PumpSdlEvents(MSG *message, bool *hasMessage)
{
    *hasMessage = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
        return;
    SDL_Event event;
    if (!SDL_PollEvent(&event))
        return;
    memset(message, 0, sizeof(*message));
    message->hwnd = reinterpret_cast<HWND>(g_window);
    if (event.type == SDL_QUIT)
        message->message = WM_CLOSE;
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = TRUE;
    }
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = FALSE;
    }
    else
        message->message = 0;
    *hasMessage = true;
}

void FillKeyboard(BYTE *state, bool directInput)
{
    memset(state, 0, 256);
#if defined(PSP)
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
#if TH08_PSP_DEBUG_START_STAGE_ENABLED
    {
        static bool first = true;
        if (first)
        {
            first = false;
            const int peek = sceCtrlPeekBufferPositive(&pad, 1);
            th08::psp::BootLog("FILLKEYBOARD_FIRST peek=%d buttons=0x%08x\n", peek, static_cast<unsigned int>(pad.Buttons));
        }
    }
#endif
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0)
        return;
#if TH08_PSP_DEBUG_START_STAGE_ENABLED
    pad.Buttons |= th08::psp::DebugAutoStartButtons();
#endif

    const bool up = (pad.Buttons & PSP_CTRL_UP) != 0 || pad.Ly < 64;
    const bool down = (pad.Buttons & PSP_CTRL_DOWN) != 0 || pad.Ly > 192;
    const bool left = (pad.Buttons & PSP_CTRL_LEFT) != 0 || pad.Lx < 64;
    const bool right = (pad.Buttons & PSP_CTRL_RIGHT) != 0 || pad.Lx > 192;
    const bool shoot = (pad.Buttons & PSP_CTRL_CROSS) != 0;
    const bool bomb = (pad.Buttons & PSP_CTRL_CIRCLE) != 0;
    const bool focus = (pad.Buttons & (PSP_CTRL_SQUARE | PSP_CTRL_LTRIGGER |
                                      PSP_CTRL_RTRIGGER)) != 0;
    const bool skip = (pad.Buttons & PSP_CTRL_TRIANGLE) != 0;
    const bool menu = (pad.Buttons & PSP_CTRL_START) != 0;

#define MAP_PSP_KEY(key, pressed) state[(key)] = (pressed) ? 0x80 : 0
    if (directInput)
    {
        MAP_PSP_KEY(DIK_UP, up); MAP_PSP_KEY(DIK_DOWN, down);
        MAP_PSP_KEY(DIK_LEFT, left); MAP_PSP_KEY(DIK_RIGHT, right);
        MAP_PSP_KEY(DIK_Z, shoot); MAP_PSP_KEY(DIK_RETURN, shoot);
        MAP_PSP_KEY(DIK_X, bomb); MAP_PSP_KEY(DIK_LSHIFT, focus);
        MAP_PSP_KEY(DIK_RSHIFT, focus); MAP_PSP_KEY(DIK_LCONTROL, skip);
        MAP_PSP_KEY(DIK_ESCAPE, menu);
    }
    else
    {
        MAP_PSP_KEY(VK_UP, up); MAP_PSP_KEY(VK_DOWN, down);
        MAP_PSP_KEY(VK_LEFT, left); MAP_PSP_KEY(VK_RIGHT, right);
        MAP_PSP_KEY('Z', shoot); MAP_PSP_KEY(VK_RETURN, shoot);
        MAP_PSP_KEY('X', bomb); MAP_PSP_KEY(VK_SHIFT, focus);
        MAP_PSP_KEY(VK_CONTROL, skip); MAP_PSP_KEY(VK_ESCAPE, menu);
    }
#undef MAP_PSP_KEY
#else
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
#define MAP_KEY(win, sdl) state[(win)] = keys[(sdl)] ? 0x80 : 0
    if (directInput)
    {
        MAP_KEY(DIK_UP, SDL_SCANCODE_UP); MAP_KEY(DIK_DOWN, SDL_SCANCODE_DOWN);
        MAP_KEY(DIK_LEFT, SDL_SCANCODE_LEFT); MAP_KEY(DIK_RIGHT, SDL_SCANCODE_RIGHT);
        MAP_KEY(DIK_NUMPAD1, SDL_SCANCODE_KP_1); MAP_KEY(DIK_NUMPAD2, SDL_SCANCODE_KP_2);
        MAP_KEY(DIK_NUMPAD3, SDL_SCANCODE_KP_3); MAP_KEY(DIK_NUMPAD4, SDL_SCANCODE_KP_4);
        MAP_KEY(DIK_NUMPAD6, SDL_SCANCODE_KP_6); MAP_KEY(DIK_NUMPAD7, SDL_SCANCODE_KP_7);
        MAP_KEY(DIK_NUMPAD8, SDL_SCANCODE_KP_8); MAP_KEY(DIK_NUMPAD9, SDL_SCANCODE_KP_9);
        MAP_KEY(DIK_HOME, SDL_SCANCODE_HOME); MAP_KEY(DIK_P, SDL_SCANCODE_P);
        MAP_KEY(DIK_D, SDL_SCANCODE_D); MAP_KEY(DIK_Z, SDL_SCANCODE_Z); MAP_KEY(DIK_X, SDL_SCANCODE_X);
        MAP_KEY(DIK_LSHIFT, SDL_SCANCODE_LSHIFT); MAP_KEY(DIK_RSHIFT, SDL_SCANCODE_RSHIFT);
        MAP_KEY(DIK_ESCAPE, SDL_SCANCODE_ESCAPE); MAP_KEY(DIK_LCONTROL, SDL_SCANCODE_LCTRL);
        MAP_KEY(DIK_RCONTROL, SDL_SCANCODE_RCTRL); MAP_KEY(DIK_Q, SDL_SCANCODE_Q);
        MAP_KEY(DIK_S, SDL_SCANCODE_S); MAP_KEY(DIK_R, SDL_SCANCODE_R); MAP_KEY(DIK_RETURN, SDL_SCANCODE_RETURN);
    }
    else
    {
        MAP_KEY(VK_UP, SDL_SCANCODE_UP); MAP_KEY(VK_DOWN, SDL_SCANCODE_DOWN);
        MAP_KEY(VK_LEFT, SDL_SCANCODE_LEFT); MAP_KEY(VK_RIGHT, SDL_SCANCODE_RIGHT);
        MAP_KEY(VK_NUMPAD1, SDL_SCANCODE_KP_1); MAP_KEY(VK_NUMPAD2, SDL_SCANCODE_KP_2);
        MAP_KEY(VK_NUMPAD3, SDL_SCANCODE_KP_3); MAP_KEY(VK_NUMPAD4, SDL_SCANCODE_KP_4);
        MAP_KEY(VK_NUMPAD6, SDL_SCANCODE_KP_6); MAP_KEY(VK_NUMPAD7, SDL_SCANCODE_KP_7);
        MAP_KEY(VK_NUMPAD8, SDL_SCANCODE_KP_8); MAP_KEY(VK_NUMPAD9, SDL_SCANCODE_KP_9);
        MAP_KEY(VK_HOME, SDL_SCANCODE_HOME); MAP_KEY('P', SDL_SCANCODE_P); MAP_KEY('D', SDL_SCANCODE_D);
        MAP_KEY('Z', SDL_SCANCODE_Z); MAP_KEY('X', SDL_SCANCODE_X); MAP_KEY(VK_SHIFT, SDL_SCANCODE_LSHIFT);
        MAP_KEY(VK_ESCAPE, SDL_SCANCODE_ESCAPE); MAP_KEY(VK_CONTROL, SDL_SCANCODE_LCTRL);
        MAP_KEY('Q', SDL_SCANCODE_Q); MAP_KEY('S', SDL_SCANCODE_S); MAP_KEY('R', SDL_SCANCODE_R);
        MAP_KEY(VK_RETURN, SDL_SCANCODE_RETURN);
    }
#undef MAP_KEY
#endif
}
} // namespace

extern "C" SDL_Window *th08_linux_get_window() { return g_window; }

#if defined(PSP)
extern "C" BOOL th08_psp_gdi_text_initialize()
{
    return InitializePspGdiText() ? TRUE : FALSE;
}

extern "C" void th08_psp_gdi_text_shutdown()
{
    ShutdownPspGdiText();
}
#endif

extern "C" {
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, LPVOID, DWORD disposition, DWORD, HANDLE)
{
    int flags = (access & (GENERIC_WRITE | FILE_APPEND_DATA)) ? O_WRONLY : O_RDONLY;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) flags = O_RDWR;
    if (access & FILE_APPEND_DATA) flags |= O_APPEND;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    if (disposition == OPEN_ALWAYS) flags |= O_CREAT;
#if defined(PSP)
    const std::string resolvedPath = ResolvePspArchivePath(path);
#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    int fd;
    {
        th08::psp::IoActivityScope ioActivity(
            th08::psp::IoActivityKind::Open, resolvedPath.c_str());
        fd = open(resolvedPath.c_str(), flags, 0666);
    }
#else
    int fd = open(resolvedPath.c_str(), flags, 0666);
#endif
#else
    int fd = open(path, flags, 0666);
#endif
    if (fd < 0) { g_lastError = errno; return INVALID_HANDLE_VALUE; }
    FileHandle *handle = new FileHandle(fd);
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    handle->TagPath(resolvedPath.c_str());
#endif
    return handle;
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPVOID security, DWORD disposition, DWORD attrs, HANDLE templ)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1))
        return INVALID_HANDLE_VALUE;
    converted[sizeof(converted) - 1] = 0;
    return CreateFileA(converted, access, share, security, disposition, attrs, templ);
}

BOOL ReadFile(HANDLE raw, LPVOID data, DWORD size, LPDWORD readSize, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(th08::psp::IoActivityKind::Read,
                                          static_cast<FileHandle *>(raw)->pathTail, true, data);
#endif
    BYTE *cursor = static_cast<BYTE *>(data);
    DWORD total = 0;
#if TH08_PSP_IO_BOUNCE_HIGH_ENABLED
    const bool bounce = th08::psp::IoSerializeEnabled() &&
                        reinterpret_cast<uintptr_t>(data) >= kPspExtendedMemoryBase;
#endif
    while (total < size)
    {
#if TH08_PSP_IO_BOUNCE_HIGH_ENABLED
        ssize_t result;
        if (bounce)
        {
            const DWORD want = size - total < kPspIoBounceBytes ? size - total : kPspIoBounceBytes;
            result = read(static_cast<FileHandle *>(raw)->fd, gPspIoBounce, want);
            if (result > 0)
                memcpy(cursor + total, gPspIoBounce, static_cast<size_t>(result));
        }
        else
            result = read(static_cast<FileHandle *>(raw)->fd, cursor + total, size - total);
#else
        const ssize_t result = read(static_cast<FileHandle *>(raw)->fd,
                                    cursor + total, size - total);
#endif
        if (result > 0)
        {
            total += static_cast<DWORD>(result);
            continue;
        }
        if (result == 0) break;
        if (errno == EINTR) continue;
        if (readSize != NULL) *readSize = total;
        g_lastError = errno;
        return FALSE;
    }
    if (readSize != NULL) *readSize = total;
    return TRUE;
}

BOOL WriteFile(HANDLE raw, LPCVOID data, DWORD size, LPDWORD written, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(th08::psp::IoActivityKind::Write,
                                          static_cast<FileHandle *>(raw)->pathTail, true, data);
#endif
    const BYTE *cursor = static_cast<const BYTE *>(data);
    DWORD total = 0;
    while (total < size)
    {
        const ssize_t result = write(static_cast<FileHandle *>(raw)->fd,
                                     cursor + total, size - total);
        if (result > 0)
        {
            total += static_cast<DWORD>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        if (written != NULL) *written = total;
        if (result < 0) g_lastError = errno;
        return FALSE;
    }
    if (written != NULL) *written = total;
    return TRUE;
}

DWORD SetFilePointer(HANDLE raw, LONG offset, LONG *, DWORD origin)
{
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(th08::psp::IoActivityKind::Seek,
                                          static_cast<FileHandle *>(raw)->pathTail);
#endif
    int whence = origin == FILE_BEGIN ? SEEK_SET : origin == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t result = lseek(static_cast<FileHandle *>(raw)->fd, offset, whence);
    return result < 0 ? static_cast<DWORD>(-1) : static_cast<DWORD>(result);
}

DWORD GetFileSize(HANDLE raw, LPDWORD high)
{
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(
        th08::psp::IoActivityKind::Metadata, static_cast<FileHandle *>(raw)->pathTail);
#endif
    struct stat info;
    if (fstat(static_cast<FileHandle *>(raw)->fd, &info) != 0) return static_cast<DWORD>(-1);
    if (high != NULL) *high = static_cast<DWORD>(static_cast<unsigned long long>(info.st_size) >> 32);
    return static_cast<DWORD>(info.st_size);
}

BOOL CloseHandle(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE) return FALSE;
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind == HANDLE_THREAD)
    {
        ThreadHandle *thread = static_cast<ThreadHandle *>(handle);
        if (!thread->finished) return TRUE;
        if (!thread->joined) pthread_join(thread->thread, NULL);
    }
    delete handle;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE raw)
{
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(th08::psp::IoActivityKind::Sync,
                                          static_cast<FileHandle *>(raw)->pathTail);
#endif
    return fsync(static_cast<FileHandle *>(raw)->fd) == 0;
}

BOOL DeleteFileA(LPCSTR path)
{
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(
        th08::psp::IoActivityKind::Write, path);
#endif
    return unlink(path) == 0;
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1)) return INVALID_FILE_ATTRIBUTES;
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(
        th08::psp::IoActivityKind::Metadata, converted);
#endif
    struct stat info;
    if (stat(converted, &info) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL SetCurrentDirectoryW(LPCWSTR path)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1)) return FALSE;
    return chdir(converted) == 0;
}

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data)
{
    glob_t result;
#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    int globResult;
    {
        th08::psp::IoActivityScope ioActivity(
            th08::psp::IoActivityKind::Directory, pattern);
        globResult = glob(pattern, 0, NULL, &result);
    }
#else
    const int globResult = glob(pattern, 0, NULL, &result);
#endif
    if (globResult != 0) return INVALID_HANDLE_VALUE;
    FindHandle *handle = new FindHandle();
    for (size_t i = 0; i < result.gl_pathc; ++i) handle->paths.push_back(result.gl_pathv[i]);
    globfree(&result);
    if (!FillFindData(handle, data)) { delete handle; return INVALID_HANDLE_VALUE; }
    return handle;
}

BOOL FindNextFileA(HANDLE raw, WIN32_FIND_DATAA *data) { return FillFindData(static_cast<FindHandle *>(raw), data); }
BOOL FindClose(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE)
        return FALSE;
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind != HANDLE_FIND)
        return FALSE;
    delete static_cast<FindHandle *>(handle);
    return TRUE;
}
void Sleep(DWORD milliseconds) { usleep(static_cast<useconds_t>(milliseconds) * 1000); }

DWORD timeGetTime(void)
{
    struct timeval value; gettimeofday(&value, NULL);
    return static_cast<DWORD>(value.tv_sec * 1000ULL + value.tv_usec / 1000);
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *value) { value->QuadPart = 1000000; return TRUE; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *value)
{
    struct timeval time; gettimeofday(&time, NULL);
    value->QuadPart = time.tv_sec * 1000000LL + time.tv_usec; return TRUE;
}
DWORD GetCurrentThreadId(void) { return CurrentThreadIdImpl(); }

HANDLE CreateThread(LPVOID, size_t, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD, LPDWORD id)
{
    ThreadHandle *handle = new ThreadHandle(); handle->start = start; handle->parameter = parameter;
    if (pthread_create(&handle->thread, NULL, ThreadTrampoline, handle) != 0) { delete handle; return NULL; }
    while (handle->id == 0) sched_yield();
    if (id != NULL) *id = handle->id;
    return handle;
}

BOOL PostThreadMessageA(DWORD id, UINT message, WPARAM wparam, LPARAM lparam)
{
    MSG value; memset(&value, 0, sizeof(value)); value.message = message; value.wParam = wparam; value.lParam = lparam;
    pthread_mutex_lock(&g_messageMutex); g_threadMessages[id].push_back(value); pthread_mutex_unlock(&g_messageMutex);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE raw, DWORD timeout)
{
    LinuxHandle *base = static_cast<LinuxHandle *>(raw);
    DWORD start = timeGetTime();
    for (;;)
    {
        if (base->kind == HANDLE_THREAD && static_cast<ThreadHandle *>(base)->finished) return WAIT_OBJECT_0;
        if (base->kind == HANDLE_EVENT)
        {
            EventHandle *event = static_cast<EventHandle *>(base);
            pthread_mutex_lock(&event->mutex);
            if (event->signaled)
            {
                if (!event->manual) event->signaled = false;
                pthread_mutex_unlock(&event->mutex); return WAIT_OBJECT_0;
            }
            pthread_mutex_unlock(&event->mutex);
        }
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL, DWORD timeout, DWORD)
{
    DWORD start = timeGetTime();
    for (;;)
    {
        for (DWORD i = 0; i < count; ++i) if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) return i;
        pthread_mutex_lock(&g_messageMutex);
        bool hasMessages = !g_threadMessages[CurrentThreadIdImpl()].empty();
        pthread_mutex_unlock(&g_messageMutex);
        if (hasMessages) return count;
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

HANDLE CreateEventA(LPVOID, BOOL manual, BOOL initial, LPCSTR) { return new EventHandle(manual != FALSE, initial != FALSE); }
BOOL SetEvent(HANDLE raw)
{
    EventHandle *event = static_cast<EventHandle *>(raw); pthread_mutex_lock(&event->mutex);
    event->signaled = true; pthread_cond_broadcast(&event->condition); pthread_mutex_unlock(&event->mutex); return TRUE;
}
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void *) { return id != 0 ? id : 1; }
BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }
HANDLE CreateMutexA(LPVOID, BOOL, LPCSTR) { g_lastError = 0; return new MutexHandle(); }
DWORD GetLastError(void) { return g_lastError; }
void InitializeCriticalSection(CRITICAL_SECTION *value)
{
#if defined(PSP)
    memset(value, 0, sizeof(*value));
    value->native = static_cast<pthread_mutex_t *>(malloc(sizeof(*value->native)));
    if (value->native != NULL)
    {
        pthread_mutexattr_t attributes;
        pthread_mutexattr_init(&attributes);
        pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(value->native, &attributes);
        pthread_mutexattr_destroy(&attributes);
    }
#else
    pthread_mutex_init(value, NULL);
#endif
}
void DeleteCriticalSection(CRITICAL_SECTION *value)
{
#if defined(PSP)
    if (value->native != NULL)
    {
        pthread_mutex_destroy(value->native);
        free(value->native);
        value->native = NULL;
    }
#else
    pthread_mutex_destroy(value);
#endif
}
void EnterCriticalSection(CRITICAL_SECTION *value)
{
#if defined(PSP)
    if (value->native != NULL)
        pthread_mutex_lock(value->native);
#else
    pthread_mutex_lock(value);
#endif
}
void LeaveCriticalSection(CRITICAL_SECTION *value)
{
#if defined(PSP)
    if (value->native != NULL)
        pthread_mutex_unlock(value->native);
#else
    pthread_mutex_unlock(value);
#endif
}

BOOL PeekMessageA(MSG *message, HWND, UINT, UINT, UINT)
{
    pthread_mutex_lock(&g_messageMutex);
    std::vector<MSG> &queue = g_threadMessages[CurrentThreadIdImpl()];
    if (!queue.empty()) { *message = queue.front(); queue.erase(queue.begin()); pthread_mutex_unlock(&g_messageMutex); return TRUE; }
    pthread_mutex_unlock(&g_messageMutex);
    bool hasMessage; PumpSdlEvents(message, &hasMessage); return hasMessage;
}
BOOL TranslateMessage(const MSG *) { return TRUE; }
LRESULT DispatchMessageA(const MSG *message) { return g_windowProcedure != NULL ? g_windowProcedure(message->hwnd, message->message, message->wParam, message->lParam) : 0; }
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
BOOL RegisterClassA(const WNDCLASSA *value) { g_windowProcedure = value->lpfnWndProc; return TRUE; }

HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR title, DWORD style, int, int, int width, int height, HWND, HANDLE, HINSTANCE, LPVOID)
{
    TH08_PSP_BOOT_CHECKPOINT("sdl_init", "before", 0);
    const int sdlInitResult =
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    TH08_PSP_BOOT_CHECKPOINT("sdl_init", "after", sdlInitResult);
    if (sdlInitResult != 0)
    {
        fprintf(stderr, "th08-modern: SDL_Init failed: %s\n", SDL_GetError());
#if defined(PSP)
        SDL_Quit();
#endif
        return NULL;
    }
    TH08_PSP_BOOT_CHECKPOINT("sdl_gl_attributes", "before", 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
#if !defined(PSP)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
    TH08_PSP_BOOT_CHECKPOINT("sdl_gl_attributes", "after", 0);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
#if defined(PSP)
    (void)style;
    width = 480;
    height = 272;
#else
    if (style == WS_OVERLAPPEDWINDOW) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    else { width = 640; height = 480; }
#endif
    TH08_PSP_BOOT_CHECKPOINT("sdl_window", "before_create", 0);
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, width, height, flags);
    TH08_PSP_BOOT_CHECKPOINT("sdl_window", "after_create",
                             g_window != NULL ? 1 : 0);
    if (g_window == NULL)
    {
        fprintf(stderr, "th08-modern: SDL_CreateWindow failed: %s\n", SDL_GetError());
#if defined(PSP)
        // Leave GU/eDRAM ownership clean for the outer fail-visible screen.
        SDL_Quit();
#endif
    }
    else SetApplicationIcon(g_window);
    return reinterpret_cast<HWND>(g_window);
}

BOOL DestroyWindow(HWND) { if (g_window != NULL) SDL_DestroyWindow(g_window); g_window = NULL; SDL_Quit(); return TRUE; }
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL MoveWindow(HWND, int, int, int, int, BOOL) { return TRUE; }
int ShowCursor(BOOL show) { return SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }
HCURSOR SetCursor(HCURSOR value) { return value; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HCURSOR>(1); }
HGDIOBJ GetStockObject(int) { return NULL; }
int GetSystemMetrics(int metric) { return metric == SM_CYCAPTION ? 24 : 4; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID value, UINT) { if (value != NULL) *static_cast<BOOL *>(value) = FALSE; return TRUE; }
HWND GetForegroundWindow(void) { return reinterpret_cast<HWND>(g_window); }
DWORD GetWindowThreadProcessId(HWND, LPDWORD process) { if (process != NULL) *process = getpid(); return GetCurrentThreadId(); }
BOOL AttachThreadInput(DWORD, DWORD, BOOL) { return TRUE; }
HWND SetActiveWindow(HWND window) { if (g_window != NULL) SDL_RaiseWindow(g_window); return window; }
LONG GetWindowLongA(HWND, int) { return 0; }
BOOL WINNLSEnableIME(HWND, BOOL) { return TRUE; }
BOOL GetKeyboardState(BYTE *state) { FillKeyboard(state, false); return TRUE; }
BOOL SetKeyboardState(const BYTE *) { return TRUE; }
int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) { fprintf(stderr, "%s: %s\n", title ? title : "TH08", text ? text : ""); return 0; }
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT) { fwprintf(stderr, L"%ls: %ls\n", title ? title : L"TH08", text ? text : L""); return 0; }

DWORD GetModuleFileNameA(HMODULE, LPSTR buffer, DWORD size)
{
#if defined(PSP)
    const std::string path = ExecutableSiblingPath("EBOOT.PBP");
    if (size == 0 || path.empty()) return 0;
    strncpy(buffer, path.c_str(), size - 1);
    buffer[size - 1] = 0;
    return static_cast<DWORD>(strlen(buffer));
#else
    ssize_t count = readlink("/proc/self/exe", buffer, size - 1); if (count < 0) return 0;
    buffer[count] = 0; return static_cast<DWORD>(count);
#endif
}
DWORD GetConsoleTitleA(LPSTR buffer, DWORD size) { if (size) buffer[0] = 0; return 0; }
void GetStartupInfoA(STARTUPINFOA *value) { DWORD size = value->cb; memset(value, 0, size); value->cb = size; }
int MultiByteToWideChar(UINT, DWORD, LPCSTR source, int sourceSize, LPWSTR dest, int destSize)
{
    size_t result = mbstowcs(dest, source, destSize); return result == static_cast<size_t>(-1) ? 0 : static_cast<int>(result + (sourceSize < 0));
}
DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD error, DWORD, LPSTR buffer, DWORD size, va_list *)
{
    const char *message = strerror(error); if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) *reinterpret_cast<char **>(buffer) = strdup(message);
    else if (size) { strncpy(buffer, message, size - 1); buffer[size - 1] = 0; } return strlen(message);
}
LPVOID LocalFree(LPVOID value) { free(value); return NULL; }
HGLOBAL GlobalAlloc(UINT, size_t size) { return calloc(1, size); }
HGLOBAL GlobalFree(HGLOBAL value) { free(value); return NULL; }
#if defined(PSP)
HMODULE LoadLibraryA(LPCSTR) { return NULL; }
void *GetProcAddress(HMODULE, LPCSTR) { return NULL; }
#else
HMODULE LoadLibraryA(LPCSTR path) { return dlopen(path, RTLD_NOW); }
void *GetProcAddress(HMODULE module, LPCSTR name) { return dlsym(module, name); }
#endif
HDC CreateCompatibleDC(HDC) { return new GdiDc(); }
BOOL DeleteDC(HDC value) { delete static_cast<GdiDc *>(value); return TRUE; }
HGDIOBJ SelectObject(HDC dcRaw, HGDIOBJ objectRaw)
{
    if (dcRaw == NULL || objectRaw == NULL) return NULL;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    GdiObject *object = static_cast<GdiObject *>(objectRaw);
    if (object->kind == GdiObject::BITMAP)
    {
        GdiBitmap *old = dc->bitmap;
        dc->bitmap = static_cast<GdiBitmap *>(object);
        return old;
    }
    GdiFont *old = dc->font;
    dc->font = static_cast<GdiFont *>(object);
    return old;
}
BOOL DeleteObject(HGDIOBJ value)
{
    GdiObject *object = static_cast<GdiObject *>(value);
#if defined(PSP)
    if (object != NULL && object->kind == GdiObject::FONT)
        ReleasePspGdiFontDescriptor(static_cast<GdiFont *>(object));
#endif
    delete object;
    return TRUE;
}
int SetBkMode(HDC, int mode) { return mode; }
COLORREF SetTextColor(HDC raw, COLORREF color) { GdiDc *dc = static_cast<GdiDc *>(raw); COLORREF old = dc->color; dc->color = color; return old; }
HFONT CreateFontA(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR)
{
#if defined(PSP)
    if (g_pspGdiText.face == NULL)
    {
        fprintf(stderr, "TH08PSP FONT CreateFontA rejected before startup\n");
        return NULL;
    }
    const int pointSize = height < 0 ? -height : height;
    const int style = weight >= FW_SEMIBOLD ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL;
    GdiFont *font = new (std::nothrow) GdiFont(g_pspGdiText.face, pointSize, style);
    if (font == NULL)
    {
        fprintf(stderr, "TH08PSP FONT descriptor allocation failed\n");
        return NULL;
    }
    ++g_pspGdiText.liveDescriptors;
    return font;
#else
    const char *path = ResolveJapaneseFont();
    if (path == NULL) return new GdiFont();
    if (!TTF_WasInit() && TTF_Init() != 0)
    {
        fprintf(stderr, "th08-modern: SDL_ttf initialization failed: %s\n", TTF_GetError());
        return new GdiFont();
    }
    TTF_Font *font = TTF_OpenFont(path, height < 0 ? -height : height);
    if (font == NULL)
    {
        fprintf(stderr, "th08-modern: unable to load Japanese font %s: %s\n", path, TTF_GetError());
        return new GdiFont();
    }
    if (weight >= FW_SEMIBOLD) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    return new GdiFont(font);
#endif
}
HBITMAP CreateDIBSection(HDC, const void *infoRaw, UINT, VOID **pixels, HANDLE, DWORD)
{
    const BITMAPINFO *info = static_cast<const BITMAPINFO *>(infoRaw); GdiBitmap *bitmap = new GdiBitmap(info->bmiHeader.biWidth, info->bmiHeader.biHeight, info->bmiHeader.biBitCount);
    *pixels = bitmap->pixels.empty() ? NULL : &bitmap->pixels[0]; return bitmap;
}
BOOL TextOutA(HDC dcRaw, int x, int y, LPCSTR text, int length)
{
    if (dcRaw == NULL || text == NULL || length <= 0) return FALSE;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    if (dc->bitmap == NULL || dc->font == NULL || dc->font->font == NULL) return FALSE;
#if defined(PSP)
    if (!ConfigurePspGdiFont(dc->font)) return FALSE;
#endif
    bool conversionValid = true;
    std::string utf8 = ConvertCp932ToUtf8(
        text, static_cast<size_t>(length), &conversionValid);
    if (!conversionValid)
        return FALSE;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *rendered = TTF_RenderUTF8_Blended(dc->font->font, utf8.c_str(), white);
    if (rendered == NULL) return FALSE;
    SDL_Surface *glyph = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(rendered);
    if (glyph == NULL) return FALSE;
    if (SDL_MUSTLOCK(glyph)) SDL_LockSurface(glyph);
    for (int row = 0; row < glyph->h; ++row)
    {
        const BYTE *source = static_cast<const BYTE *>(glyph->pixels) + row * glyph->pitch;
        for (int column = 0; column < glyph->w; ++column)
            PutGdiTextPixel(dc->bitmap, x + column, y + row, dc->color, source[column * 4 + 3]);
    }
    if (SDL_MUSTLOCK(glyph)) SDL_UnlockSurface(glyph);
    SDL_FreeSurface(glyph);
    return TRUE;
}
HRESULT CoInitialize(LPVOID) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFGUID, LPVOID, DWORD, REFIID, LPVOID *) { return E_NOTIMPL; }
} // extern C

extern const GUID CLSID_ShellLink = {0};
extern const GUID IID_IShellLink = {1};
extern const GUID IID_IPersistFile = {2};
const GUID GUID_NULL = {0};
const GUID IID_IDirectSoundNotify = {3};
const GUID IID_IDirectInput8A = {4};
const GUID GUID_SysKeyboard = {5};
const GUID DIPROP_RANGE = {6};
const DIDATAFORMAT c_dfDIKeyboard = {sizeof(DIDATAFORMAT)};
const DIDATAFORMAT c_dfDIJoystick = {sizeof(DIDATAFORMAT)};

MMRESULT timeGetDevCaps(TIMECAPS *caps, UINT) { caps->wPeriodMin = 1; caps->wPeriodMax = 1000; return 0; }
MMRESULT timeBeginPeriod(UINT) { return 0; }
MMRESULT timeEndPeriod(UINT) { return 0; }
UINT timeSetEvent(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT) { static UINT id = 1; return id++; }
MMRESULT timeKillEvent(UINT) { return 0; }
MMRESULT midiOutOpen(HMIDIOUT *handle, UINT, DWORD_PTR, DWORD_PTR, DWORD) { *handle = reinterpret_cast<HMIDIOUT>(1); return 0; }
MMRESULT midiOutClose(HMIDIOUT) { return 0; }
MMRESULT midiOutReset(HMIDIOUT) { return 0; }
MMRESULT midiOutPrepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutUnprepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutLongMsg(HMIDIOUT, LPMIDIHDR header, UINT) { header->dwFlags |= 1; return 0; }
MMRESULT midiOutShortMsg(HMIDIOUT, DWORD) { return 0; }
MMRESULT joyGetPosEx(UINT, JOYINFOEX *) { return 1; }
MMRESULT joyGetDevCapsA(UINT_PTR, JOYCAPSA *caps, UINT) { memset(caps, 0, sizeof(*caps)); caps->wXmax = caps->wYmax = 65535; return 1; }

class LinuxInputDevice : public IDirectInputDevice8A
{
  public:
    explicit LinuxInputDevice(bool keyboard_) : refs(1), keyboard(keyboard_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT GetCapabilities(DIDEVCAPS *caps) { caps->dwAxes = keyboard ? 0 : 2; caps->dwButtons = keyboard ? 0 : 32; return S_OK; }
    HRESULT EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) { return S_OK; }
    HRESULT GetDeviceState(DWORD size, LPVOID data)
    {
        if (keyboard && size >= 256) FillKeyboard(static_cast<BYTE *>(data), true);
        else memset(data, 0, size); return S_OK;
    }
    HRESULT SetDataFormat(const DIDATAFORMAT *) { return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
    HRESULT SetProperty(REFGUID, const DIPROPHEADER *) { return S_OK; }
    HRESULT Acquire() { return S_OK; }
    HRESULT Unacquire() { return S_OK; }
    HRESULT Poll() { return S_OK; }
  private:
    ULONG refs; bool keyboard;
};

class LinuxDirectInput : public IDirectInput8A
{
  public:
    LinuxDirectInput() : refs(1) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateDevice(REFGUID guid, IDirectInputDevice8A **device, LPVOID)
    { *device = new LinuxInputDevice(guid.Data1 == GUID_SysKeyboard.Data1); return S_OK; }
    HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *out, LPVOID) { *out = new LinuxDirectInput(); return S_OK; }

class LinuxSoundBuffer;

SDL_AudioDeviceID g_audioDevice;
std::vector<LinuxSoundBuffer *> g_soundBuffers;

void LockAudio()
{
    if (g_audioDevice != 0) SDL_LockAudioDevice(g_audioDevice);
}

void UnlockAudio()
{
    if (g_audioDevice != 0) SDL_UnlockAudioDevice(g_audioDevice);
}

class LinuxSoundNotify : public IDirectSoundNotify
{
  public:
    explicit LinuxSoundNotify(LinuxSoundBuffer *buffer_) : refs(1), buffer(buffer_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions);
  private: ULONG refs; LinuxSoundBuffer *buffer;
};

class LinuxSoundBuffer : public IDirectSoundBuffer
{
  public:
    explicit LinuxSoundBuffer(const DSBUFFERDESC *desc)
        : refs(1), playing(false), looping(false), position(0), cursorFrame(0.0), volume(0), pan(0),
          hasFormat(false), locked(false)
    {
        memset(&format, 0, sizeof(format));
        if (desc != NULL)
        {
            bytes.resize(desc->dwBufferBytes);
            if (desc->lpwfxFormat != NULL) { format = *desc->lpwfxFormat; hasFormat = true; }
        }
        LockAudio(); g_soundBuffers.push_back(this); UnlockAudio();
    }
    LinuxSoundBuffer(const LinuxSoundBuffer &other)
        : refs(1), bytes(other.bytes), playing(false), looping(false), position(0), cursorFrame(0.0),
          volume(other.volume), pan(other.pan), format(other.format), hasFormat(other.hasFormat), locked(false)
    { LockAudio(); g_soundBuffers.push_back(this); UnlockAudio(); }
    ~LinuxSoundBuffer()
    {
        LockAudio();
        for (std::vector<LinuxSoundBuffer *>::iterator it = g_soundBuffers.begin(); it != g_soundBuffers.end(); ++it)
            if (*it == this) { g_soundBuffers.erase(it); break; }
        UnlockAudio();
    }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT QueryInterface(REFIID, void **out) { *out = new LinuxSoundNotify(this); return S_OK; }
    HRESULT GetCurrentPosition(LPDWORD play, LPDWORD write)
    {
        LockAudio();
        if (play) *play = position;
        if (write) *write = position;
        UnlockAudio();
        return S_OK;
    }
    HRESULT GetStatus(LPDWORD status)
    { LockAudio(); *status = playing ? DSBSTATUS_PLAYING : 0; UnlockAudio(); return S_OK; }
    HRESULT Initialize(void *, const DSBUFFERDESC *) { return S_OK; }
    HRESULT Lock(DWORD offset, DWORD length, LPVOID *first, LPDWORD firstSize, LPVOID *second, LPDWORD secondSize, DWORD)
    {
        LockAudio(); locked = true;
        if (bytes.empty()) bytes.resize(length ? length : 1); offset %= bytes.size(); if (length == 0 || length > bytes.size()) length = bytes.size();
        DWORD contiguous = static_cast<DWORD>(bytes.size() - offset); if (contiguous > length) contiguous = length;
        *first = &bytes[offset]; *firstSize = contiguous; if (second) *second = length > contiguous ? &bytes[0] : NULL; if (secondSize) *secondSize = length - contiguous; return S_OK;
    }
    HRESULT Play(DWORD, DWORD, DWORD flags)
    {
        LockAudio();
        playing = true;
        looping = (flags & DSBPLAY_LOOPING) != 0;
        UnlockAudio();
#if defined(PSP)
        th08::psp::AudioTelemetryRecordPlaySubmit();
#endif
        return S_OK;
    }
    HRESULT SetCurrentPosition(DWORD value)
    {
        LockAudio();
        position = bytes.empty() ? 0 : value % bytes.size();
        cursorFrame = FrameBytes() != 0 ? static_cast<double>(position / FrameBytes()) : 0.0;
        UnlockAudio();
        return S_OK;
    }
    HRESULT SetFormat(const WAVEFORMATEX *value)
    { if (value != NULL) { LockAudio(); format = *value; hasFormat = true; UnlockAudio(); } return S_OK; }
    HRESULT SetVolume(LONG value) { LockAudio(); volume = value; UnlockAudio(); return S_OK; }
    HRESULT SetPan(LONG value) { LockAudio(); pan = value; UnlockAudio(); return S_OK; }
    HRESULT Stop() { LockAudio(); playing = false; UnlockAudio(); return S_OK; }
    HRESULT Unlock(LPVOID, DWORD, LPVOID, DWORD)
    { if (locked) { locked = false; UnlockAudio(); } return S_OK; }
    HRESULT Restore() { return S_OK; }
    void SetNotifications(DWORD count, const DSBPOSITIONNOTIFY *positions)
    {
        LockAudio();
        if (count == 0)
            notifications.clear();
        else
            notifications.assign(positions, positions + count);
        UnlockAudio();
    }
#if defined(PSP)
    // The SDL callback owns the audio-device lock while it walks this list, so
    // this read does not introduce a lock or change mixer ordering.
    bool IsPlayingForTelemetry() const { return playing; }
#endif
    void Mix(Sint16 *output, int outputFrames)
    {
        const DWORD frameBytes = FrameBytes();
        if (!playing || !hasFormat || bytes.empty() || frameBytes == 0 || format.wFormatTag != WAVE_FORMAT_PCM)
            return;
        const DWORD sourceFrames = static_cast<DWORD>(bytes.size() / frameBytes);
        if (sourceFrames == 0) return;
        const double step = static_cast<double>(format.nSamplesPerSec) / 44100.0;
        const float gain = volume <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, static_cast<float>(volume) / 2000.0f);
        const float panValue = pan < -10000 ? -1.0f : pan > 10000 ? 1.0f : static_cast<float>(pan) / 10000.0f;
        const float leftGain = gain * (panValue > 0.0f ? 1.0f - panValue : 1.0f);
        const float rightGain = gain * (panValue < 0.0f ? 1.0f + panValue : 1.0f);
        const DWORD oldPosition = position;
        bool wrapped = false;
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED || TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
        // Exact fixed-point cursor when 44100 / rate is a power of two and the
        // binary64 cursor is a multiple of that step (see audio_fixed_cursor_math.hpp).
        unsigned int fixedShift = 0U;
        std::uint64_t fixedCursor = 0U;
        const bool fixedEligible =
            th08::psp::AudioFixedCursorEligible(format.nSamplesPerSec, &fixedShift) &&
            th08::psp::AudioFixedCursorFromDouble(cursorFrame, fixedShift, &fixedCursor);
#endif
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
        th08::psp::AudioCursorAuditBeginMix(format.nSamplesPerSec, fixedEligible, outputFrames);
        bool fixedWrapped = false;
#endif
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
        th08::psp::AudioCursorProductNoteMix(format.nSamplesPerSec, fixedEligible);
#endif

        for (int frame = 0; frame < outputFrames && playing; ++frame)
        {
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
            DWORD sourceFrame;
            if (fixedEligible)
            {
                std::uint32_t fixedFrame = 0U;
                if (!th08::psp::AudioFixedCursorStep(&fixedCursor, fixedShift, sourceFrames,
                                                     looping, &fixedFrame, &wrapped))
                {
                    playing = false;
                    break;
                }
                sourceFrame = fixedFrame;
            }
            else
            {
                sourceFrame = static_cast<DWORD>(cursorFrame);
                if (sourceFrame >= sourceFrames)
                {
                    if (!looping) { playing = false; break; }
                    cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
                }
            }
#else
            DWORD sourceFrame = static_cast<DWORD>(cursorFrame);
            if (sourceFrame >= sourceFrames)
            {
                if (!looping) { playing = false; break; }
                cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
            }
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
            if (fixedEligible)
            {
                std::uint32_t fixedFrame = 0U;
                const bool fixedContinues = th08::psp::AudioFixedCursorStep(
                    &fixedCursor, fixedShift, sourceFrames, looping, &fixedFrame, &fixedWrapped);
                th08::psp::AudioCursorAuditCompareFrame(fixedContinues, fixedFrame, sourceFrame);
            }
#endif
#endif
            const BYTE *source = &bytes[sourceFrame * frameBytes];
            int left, right;
            if (format.wBitsPerSample == 8)
            {
                left = (static_cast<int>(source[0]) - 128) << 8;
                right = format.nChannels > 1 ? (static_cast<int>(source[1]) - 128) << 8 : left;
            }
            else if (format.wBitsPerSample == 16)
            {
                INT16 leftSample, rightSample;
                memcpy(&leftSample, source, sizeof(leftSample));
                if (format.nChannels > 1) memcpy(&rightSample, source + sizeof(INT16), sizeof(rightSample));
                else rightSample = leftSample;
                left = leftSample; right = rightSample;
            }
            else
                break;

            int mixedLeft = output[frame * 2] + static_cast<int>(left * leftGain);
            int mixedRight = output[frame * 2 + 1] + static_cast<int>(right * rightGain);
            if (mixedLeft < -32768) mixedLeft = -32768; else if (mixedLeft > 32767) mixedLeft = 32767;
            if (mixedRight < -32768) mixedRight = -32768; else if (mixedRight > 32767) mixedRight = 32767;
            output[frame * 2] = static_cast<Sint16>(mixedLeft);
            output[frame * 2 + 1] = static_cast<Sint16>(mixedRight);
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
            if (!fixedEligible)
                cursorFrame += step;
#else
            cursorFrame += step;
#endif
        }
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
        if (fixedEligible)
            cursorFrame = th08::psp::AudioFixedCursorToDouble(fixedCursor, fixedShift);
#endif
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
        if (fixedEligible)
            th08::psp::AudioCursorAuditEndMix(
                wrapped, fixedWrapped, cursorFrame,
                th08::psp::AudioFixedCursorToDouble(fixedCursor, fixedShift), playing);
#endif

        if (cursorFrame >= sourceFrames)
        {
            if (looping) { cursorFrame = fmod(cursorFrame, static_cast<double>(sourceFrames)); wrapped = true; }
            else { cursorFrame = sourceFrames; playing = false; }
        }
        position = static_cast<DWORD>(cursorFrame) * frameBytes;
        for (size_t index = 0; index < notifications.size(); ++index)
        {
            const DWORD offset = notifications[index].dwOffset;
            if ((!wrapped && oldPosition <= offset && position > offset) ||
                (wrapped && (offset >= oldPosition || offset < position)))
            {
#if defined(PSP)
                th08::psp::AudioTelemetryRecordBgmNotify();
#endif
                SetEvent(notifications[index].hEventNotify);
            }
        }
    }
  private:
    DWORD FrameBytes() const
    { return hasFormat && format.nBlockAlign != 0 ? format.nBlockAlign : 0; }
    ULONG refs;
    std::vector<BYTE> bytes;
    bool playing, looping;
    DWORD position;
    double cursorFrame;
    LONG volume, pan;
    WAVEFORMATEX format;
    bool hasFormat, locked;
    std::vector<DSBPOSITIONNOTIFY> notifications;
};

HRESULT LinuxSoundNotify::SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions)
{ if (buffer == NULL || (count != 0 && positions == NULL)) return E_INVALIDARG; buffer->SetNotifications(count, positions); return S_OK; }

void AudioCallback(void *, Uint8 *stream, int length)
{
    memset(stream, 0, length);
    Sint16 *output = reinterpret_cast<Sint16 *>(stream);
    const int frames = length / (sizeof(Sint16) * 2);
#if defined(PSP) && TH08_PSP_RUNTIME_TELEMETRY
    std::uint32_t activeVoices = 0;
#endif
    for (size_t index = 0; index < g_soundBuffers.size(); ++index)
    {
#if defined(PSP) && TH08_PSP_RUNTIME_TELEMETRY
        if (g_soundBuffers[index]->IsPlayingForTelemetry())
            ++activeVoices;
#endif
        g_soundBuffers[index]->Mix(output, frames);
    }

#if defined(PSP) && TH08_PSP_RUNTIME_TELEMETRY
    std::uint32_t nonzeroSamples = 0;
    std::uint32_t peakAmplitude = 0;
    const std::size_t sampleCount =
        static_cast<std::size_t>(frames > 0 ? frames : 0) * 2U;
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        const int sample = static_cast<int>(output[index]);
        const std::uint32_t amplitude = static_cast<std::uint32_t>(sample < 0 ? -sample : sample);
        if (amplitude != 0)
            ++nonzeroSamples;
        if (amplitude > peakAmplitude)
            peakAmplitude = amplitude;
    }
    th08::psp::AudioTelemetryRecordCallback(
        static_cast<std::uint32_t>(frames > 0 ? frames : 0),
        nonzeroSamples, peakAmplitude, activeVoices);
#endif
}

void EnsureAudio()
{
    if (g_audioDevice != 0) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    { fprintf(stderr, "th08-modern: SDL audio initialization failed: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec requested, obtained;
    memset(&requested, 0, sizeof(requested));
    requested.freq = 44100; requested.format = AUDIO_S16SYS; requested.channels = 2;
    requested.samples = 1024; requested.callback = AudioCallback;
    g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &requested, &obtained, 0);
    if (g_audioDevice == 0)
    { fprintf(stderr, "th08-modern: SDL audio device unavailable: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_audioDevice, 0);
}

void ShutdownAudio()
{
    if (g_audioDevice == 0) return;
    SDL_CloseAudioDevice(g_audioDevice); g_audioDevice = 0;
}

class LinuxDirectSound : public IDirectSound8
{
  public:
    LinuxDirectSound() : refs(1) { EnsureAudio(); }
    ~LinuxDirectSound() { ShutdownAudio(); }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, IDirectSoundBuffer **out, LPVOID) { *out = new LinuxSoundBuffer(desc); return S_OK; }
    HRESULT DuplicateSoundBuffer(IDirectSoundBuffer *source, IDirectSoundBuffer **out) { *out = new LinuxSoundBuffer(*static_cast<LinuxSoundBuffer *>(source)); return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectSoundCreate8(const GUID *, LPDIRECTSOUND8 *out, LPVOID) { *out = new LinuxDirectSound(); return S_OK; }
