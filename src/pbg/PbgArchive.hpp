#pragma once

#include "diffbuild.hpp"
#include "inttypes.hpp"
#include "pbg/PbgFile.hpp"
#include "pbg/PbgMemory.hpp"
#if defined(TH08_PSP_PORT)
#include "pbg/Lzss.hpp"
#endif

#include <stddef.h>

namespace th08
{
struct PbgArchiveHeader
{
    i32 encodedEntryCount;
    i32 encodedFileTableOffset;
    i32 encodedFileTableDecompressedSize;
};
C_ASSERT(sizeof(PbgArchiveHeader) == 0xc);
C_ASSERT(offsetof(PbgArchiveHeader, encodedFileTableDecompressedSize) == 0x8);

struct PbgArchiveEntry
{
    // FUNCTION: th08 0x4716e0 FOLDED
    PbgArchiveEntry()
    {
        filename = NULL;
    }

    ~PbgArchiveEntry();

    char *filename;
    u32 dataOffset;
    u32 decompressedSize;
    // Copied from each table record, but never read by the retail loader.
    u32 unconsumedMetadata;
};
C_ASSERT(sizeof(PbgArchiveEntry) == 0x10);
C_ASSERT(offsetof(PbgArchiveEntry, unconsumedMetadata) == 0xc);

class PbgArchive
{
  public:
    PbgArchive();
    ~PbgArchive();

    bool Load(LPCSTR filename);
    void Release();
    LPBYTE ReadDecompressEntry(LPCSTR filename, LPBYTE outBuffer);
#if defined(TH08_PSP_PORT)
    // This layer applies only the PBG LZSS transform.  Resource encryption is
    // deliberately not detected or removed here: an encrypted entry still
    // carries its resource signature and must pass through the upper-layer
    // decrypt filter before an ANM/resource parser consumes it.
    enum StreamPayloadState
    {
        STREAM_PAYLOAD_NONE = 0,
        STREAM_PAYLOAD_PBG_DECOMPRESSED_RESOURCE_TRANSFORM_PENDING
    };

    struct StreamEntryInfo
    {
        u32 dataOffset;
        u32 compressedSize;
        u32 decompressedSize;
        StreamPayloadState payloadState;
    };

    enum StreamResult
    {
        STREAM_SUCCESS = 0,
        STREAM_INVALID_ARGUMENT,
        STREAM_ARCHIVE_NOT_LOADED,
        STREAM_ENTRY_NOT_FOUND,
        STREAM_ARCHIVE_OPEN_FAILED,
        STREAM_ENTRY_RANGE_INVALID,
        STREAM_ARCHIVE_SEEK_FAILED,
        STREAM_ARCHIVE_CURSOR_MISMATCH,
        STREAM_DECODE_INVALID_ARGUMENT,
        STREAM_DECODE_INPUT_READ_FAILED,
        STREAM_DECODE_OUTPUT_OVERFLOW,
        STREAM_DECODE_OUTPUT_SIZE_MISMATCH,
        STREAM_DECODE_SINK_WRITE_FAILED
    };

    // The call itself is the archive transaction.  m_FileAbstraction remains
    // open and workspace remains exclusively borrowed until this synchronous
    // call returns; PbgArchive is not internally thread-safe, so callers must
    // serialize transactions which share one archive.  The archive is opened
    // read-only and the source DAT is never modified.
    //
    // Success means the named entry range was validated against the current
    // archive size, exactly the declared compressed range was consumed, and
    // exactly the declared decompressed size was accepted by sink.  On failure
    // sink may already hold a prefix.  That prefix must remain private and be
    // discarded by the caller; this method never publishes or commits it.
    StreamResult ReadDecompressEntryToSink(LPCSTR filename,
                                           Lzss::StreamSinkWrite sink,
                                           void *sinkContext,
                                           Lzss::StreamWorkspace *workspace,
                                           StreamEntryInfo *outInfo = NULL);
#endif
    DWORD GetEntryDecompressedSize(LPCSTR filename);
    PbgArchiveEntry *FindEntry(LPCSTR filename);
    bool ParseHeader(LPCSTR filename);
    PbgArchiveEntry *AllocEntries(LPVOID entryBuffer, i32 count, u32 offset);
    char *CopyFileName(LPCSTR filename);

    static i32 SeekPastInt(LPVOID *ptr);
    static LPVOID SeekPastString(LPVOID *ptr);

  private:
    PbgArchiveEntry *m_Entries;
    i32 m_NumOfEntries;
    char *m_Filename;
    CPbgFile *m_FileAbstraction;
#if defined(TH08_PSP_PORT)
    // ParseHeader leaves the read-only archive handle open.  PSP resource
    // reads reuse it so Memory Stick/internal-storage drivers are not asked to
    // reopen the same large DAT for every entry.  Every consumer still seeks
    // to the entry's absolute offset before reading.
    bool m_PspArchiveHandleRetained;
    u32 m_PspArchiveEntryTransactions;
    u32 m_PspArchiveRecoveryOpens;
#endif
};

}; // namespace th08
