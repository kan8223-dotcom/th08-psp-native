#include "th_pch.h"

#include "Global.hpp"
#include "i18n.hpp"
#include "pbg/Lzss.hpp"
#include "pbg/PbgArchive.hpp"
#include "utils.hpp"
#if defined(TH08_PSP_PORT)
#include "psp/fileio.hpp"
#endif

namespace th08
{
namespace
{
// PBG table records are packed immediately after a variable-length filename.
// Consequently, the three 32-bit fields are usually not word aligned.  x86
// accepts the original pointer casts, but Allegrex raises an address-error
// exception for the resulting plain `lw` instruction.  Decode the little-
// endian wire values bytewise so the archive contents and record order stay
// identical on both targets without relying on unaligned CPU loads.
u32 ReadPbgU32LE(const void *source)
{
    const u8 *bytes = static_cast<const u8 *>(source);
    return static_cast<u32>(bytes[0]) |
           (static_cast<u32>(bytes[1]) << 8) |
           (static_cast<u32>(bytes[2]) << 16) |
           (static_cast<u32>(bytes[3]) << 24);
}
} // namespace

PbgArchive::PbgArchive()
{
    m_Entries = NULL;
    m_NumOfEntries = 0;
    m_Filename = NULL;
    m_FileAbstraction = NULL;
#if defined(TH08_PSP_PORT)
    m_PspArchiveHandleRetained = false;
    m_PspArchiveEntryTransactions = 0;
    m_PspArchiveRecoveryOpens = 0;
#endif
}

PbgArchive::~PbgArchive()
{
    Release();
}

bool PbgArchive::Load(LPCSTR filename)
{
    Release();
    utils::DebugPrint("info : %s open arcfile\r\n", filename);

    m_FileAbstraction = NewEx(CPbgFile());
    if (m_FileAbstraction == NULL)
    {
        return false;
    }

    if (ParseHeader(filename))
    {
        m_Filename = CopyFileName(filename);
        if (m_Filename != NULL)
        {
            return true;
        }
    }

    utils::DebugPrint("info : %s not found\r\n", filename);
    Release();
    return false;
}

void PbgArchive::Release()
{
#if defined(TH08_PSP_PORT)
    if (m_PspArchiveEntryTransactions != 0 || m_PspArchiveRecoveryOpens != 0)
    {
        psp::BootLog("PBG_HANDLE entry_transactions=%lu recovery_opens=%lu retained=%d\n",
                     static_cast<unsigned long>(m_PspArchiveEntryTransactions),
                     static_cast<unsigned long>(m_PspArchiveRecoveryOpens),
                     m_PspArchiveHandleRetained ? 1 : 0);
    }
#endif
    if (m_Filename != NULL)
    {
        utils::DebugPrint("info : %s close arcfile\r\n", m_Filename);
    }
    MemFree(m_Filename);
    DeleteArray(m_Entries);
    DeleteEx(m_FileAbstraction);
    m_NumOfEntries = 0;
#if defined(TH08_PSP_PORT)
    m_PspArchiveHandleRetained = false;
    m_PspArchiveEntryTransactions = 0;
    m_PspArchiveRecoveryOpens = 0;
#endif
}

#pragma var_order(entry, decompressedSize, decompressedData, compressedData, compressedSize)
LPBYTE PbgArchive::ReadDecompressEntry(LPCSTR filename, LPBYTE outBuffer)
{
    LPBYTE decompressedData;
    LPBYTE compressedData = NULL;
    u32 compressedSize;
    u32 decompressedSize;

    if (m_FileAbstraction == NULL)
    {
        return NULL;
    }

    PbgArchiveEntry *entry = FindEntry(filename);
    if (entry == NULL)
    {
        goto entry_read_error;
    }

#if defined(TH08_PSP_PORT)
    // ParseHeader already opened the archive and deliberately leaves that
    // handle alive until Release.  Reopening here used to close it first and
    // exposed every resource read to storage-driver reopen latency.  Retain a
    // one-time recovery open only for fixtures or an unexpectedly lost setup.
    if (!m_PspArchiveHandleRetained)
    {
        if (!m_FileAbstraction->Open(m_Filename, g_PbgFileOpenModes[0]))
        {
            goto entry_read_error;
        }
        m_PspArchiveHandleRetained = true;
        m_PspArchiveRecoveryOpens++;
    }
#else
    if (m_FileAbstraction->Open(m_Filename, g_PbgFileOpenModes[0]) == false)
    {
        goto entry_read_error;
    }
#endif

    compressedSize = entry[1].dataOffset - entry->dataOffset;
    decompressedSize = entry->decompressedSize;

#if defined(TH08_PSP_PORT)
    m_PspArchiveEntryTransactions++;
    // PSP must not keep the complete compressed entry alive beside the final
    // ANM/resource allocation.  Validate the table range, then decode through
    // a fixed 4 KiB input window directly into the caller's destination.
    if (entry[1].dataOffset < entry->dataOffset || entry[1].dataOffset > m_FileAbstraction->GetSize())
    {
        goto entry_read_error;
    }
    if (m_FileAbstraction->Seek(entry->dataOffset, g_PbgFileSeekModes[0]) == false)
    {
        goto entry_read_error;
    }
    decompressedData = Lzss::DecodeFile(m_FileAbstraction, compressedSize, outBuffer, decompressedSize);
    if (decompressedData == NULL)
    {
        goto entry_read_error;
    }
    return decompressedData;
#else
    compressedData = (LPBYTE)MemAlloc(compressedSize);
    if (compressedData == NULL)
    {
        goto entry_read_error;
    }
    if (m_FileAbstraction->Seek(entry->dataOffset, g_PbgFileSeekModes[0]) == false)
    {
        goto entry_read_error;
    }
    if (m_FileAbstraction->Read(compressedData, compressedSize) == 0)
    {
        goto entry_read_error;
    }

    decompressedData = Lzss::Decode(compressedData, compressedSize, outBuffer, decompressedSize);
    MemFree(compressedData);
    return decompressedData;
#endif

entry_read_error:
    utils::DebugPrint("info : %s error\r\n", m_Filename);
    MemFree(compressedData);
    return NULL;
}

#if defined(TH08_PSP_PORT)
namespace
{
PbgArchive::StreamResult MapLzssStreamResult(Lzss::StreamResult result)
{
    switch (result)
    {
    case Lzss::STREAM_SUCCESS:
        return PbgArchive::STREAM_SUCCESS;
    case Lzss::STREAM_INVALID_ARGUMENT:
        return PbgArchive::STREAM_DECODE_INVALID_ARGUMENT;
    case Lzss::STREAM_INPUT_READ_FAILED:
        return PbgArchive::STREAM_DECODE_INPUT_READ_FAILED;
    case Lzss::STREAM_OUTPUT_OVERFLOW:
        return PbgArchive::STREAM_DECODE_OUTPUT_OVERFLOW;
    case Lzss::STREAM_OUTPUT_SIZE_MISMATCH:
        return PbgArchive::STREAM_DECODE_OUTPUT_SIZE_MISMATCH;
    case Lzss::STREAM_SINK_WRITE_FAILED:
        return PbgArchive::STREAM_DECODE_SINK_WRITE_FAILED;
    }
    return PbgArchive::STREAM_DECODE_INVALID_ARGUMENT;
}
} // namespace

PbgArchive::StreamResult PbgArchive::ReadDecompressEntryToSink(
    LPCSTR filename, Lzss::StreamSinkWrite sink, void *sinkContext,
    Lzss::StreamWorkspace *workspace, StreamEntryInfo *outInfo)
{
    if (outInfo != NULL)
    {
        outInfo->dataOffset = 0;
        outInfo->compressedSize = 0;
        outInfo->decompressedSize = 0;
        outInfo->payloadState = STREAM_PAYLOAD_NONE;
    }
    if (filename == NULL || sink == NULL || workspace == NULL)
    {
        return STREAM_INVALID_ARGUMENT;
    }
    if (m_FileAbstraction == NULL || m_Filename == NULL || m_Entries == NULL || m_NumOfEntries <= 0)
    {
        return STREAM_ARCHIVE_NOT_LOADED;
    }

    PbgArchiveEntry *entry = NULL;
    for (i32 i = 0; i < m_NumOfEntries; i++)
    {
        // AllocEntries can leave an individual filename NULL if its allocation
        // fails.  Do not let a damaged/partially built table reach _stricmp.
        if (m_Entries[i].filename != NULL && _stricmp(filename, m_Entries[i].filename) == 0)
        {
            entry = &m_Entries[i];
            break;
        }
    }
    if (entry == NULL)
    {
        return STREAM_ENTRY_NOT_FOUND;
    }
    if (entry < m_Entries || entry >= m_Entries + m_NumOfEntries)
    {
        return STREAM_ENTRY_RANGE_INVALID;
    }

    // ParseHeader retains one read-only PSP handle for the archive lifetime.
    // A one-time fallback keeps manually constructed/test archives valid, but
    // successful entry transactions never close/reopen the DAT.
    if (!m_PspArchiveHandleRetained)
    {
        if (!m_FileAbstraction->Open(m_Filename, g_PbgFileOpenModes[0]))
        {
            return STREAM_ARCHIVE_OPEN_FAILED;
        }
        m_PspArchiveHandleRetained = true;
        m_PspArchiveRecoveryOpens++;
    }

    m_PspArchiveEntryTransactions++;

    const u32 archiveSize = m_FileAbstraction->GetSize();
    const u32 dataOffset = entry->dataOffset;
    const u32 nextDataOffset = entry[1].dataOffset;
    if (dataOffset > nextDataOffset || nextDataOffset > archiveSize)
    {
        return STREAM_ENTRY_RANGE_INVALID;
    }

    const u32 compressedSize = nextDataOffset - dataOffset;
    const u32 decompressedSize = entry->decompressedSize;
    if (outInfo != NULL)
    {
        outInfo->dataOffset = dataOffset;
        outInfo->compressedSize = compressedSize;
        outInfo->decompressedSize = decompressedSize;
        outInfo->payloadState = STREAM_PAYLOAD_PBG_DECOMPRESSED_RESOURCE_TRANSFORM_PENDING;
    }

    if (!m_FileAbstraction->Seek(dataOffset, g_PbgFileSeekModes[0]) ||
        m_FileAbstraction->Tell() != dataOffset)
    {
        return STREAM_ARCHIVE_SEEK_FAILED;
    }

    const Lzss::StreamResult decodeResult =
        Lzss::DecodeFileToSink(m_FileAbstraction, compressedSize, decompressedSize,
                               sink, sinkContext, workspace);
    const StreamResult result = MapLzssStreamResult(decodeResult);
    if (result != STREAM_SUCCESS)
    {
        return result;
    }
    if (m_FileAbstraction->Tell() != nextDataOffset)
    {
        return STREAM_ARCHIVE_CURSOR_MISMATCH;
    }
    return STREAM_SUCCESS;
}
#endif

DWORD PbgArchive::GetEntryDecompressedSize(LPCSTR filename)
{
    PbgArchiveEntry *entry = FindEntry(filename);
    if (entry != NULL)
        return entry->decompressedSize;
    return 0;
}

PbgArchiveEntry *PbgArchive::FindEntry(LPCSTR filename)
{
    if (m_Entries == NULL)
    {
        return NULL;
    }

    PbgArchiveEntry *entry = m_Entries;
    for (i32 i = m_NumOfEntries; i > 0; i--, entry++ /* PBG why */)
    {
        if (_stricmp(filename, entry->filename) == 0)
            return entry;
    }
    return NULL;
}

#pragma var_order(entryBuffer, fileTableDecompressedSize, magic, size, fileTableOffset, fileTableBuffer, header,       \
                  decryptedHeader, decryptedFileTable)
bool PbgArchive::ParseHeader(LPCSTR filename)
{
    LPBYTE entryBuffer;
    i32 fileTableDecompressedSize;
    i32 magic;
    DWORD size;
    i32 fileTableOffset;
    union {
        PbgArchiveHeader asStruct;
        BYTE asBytes[sizeof(PbgArchiveHeader)];
    } header;
    LPBYTE fileTableBuffer;
    LPBYTE decryptedHeader;
    LPBYTE decryptedFileTable;

    fileTableBuffer = NULL;
    entryBuffer = NULL;

    if (m_FileAbstraction == NULL)
    {
        return false;
    }
    if (!m_FileAbstraction->Open(filename, g_PbgFileOpenModes[0]))
    {
        goto parse_error;
    }
#if defined(TH08_PSP_PORT)
    m_PspArchiveHandleRetained = true;
#endif
    if (m_FileAbstraction->ReadInt(&magic) == 0)
    {
        goto parse_error;
    }
    if (magic != 'ZGBP')
    {
        goto parse_error;
    }
    if (m_FileAbstraction->Read(header.asBytes, sizeof(header)) == 0)
    {
        goto parse_error;
    }

    decryptedHeader = FileSystem::Decrypt(header.asBytes, sizeof(header), 0x1b, 0x37, sizeof(header), 0x400);
    memcpy(&header.asStruct, decryptedHeader, sizeof(header));
    g_ZunMemory.Free(decryptedHeader);

    m_NumOfEntries = header.asStruct.encodedEntryCount - 123456;
    fileTableOffset = header.asStruct.encodedFileTableOffset - 345678;
    fileTableDecompressedSize = header.asStruct.encodedFileTableDecompressedSize - 567891;

    if (m_NumOfEntries <= 0)
    {
        goto parse_error;
    }

    size = m_FileAbstraction->GetSize();
    if (fileTableOffset >= size)
    {
        goto parse_error;
    }
    size -= fileTableOffset;

    m_FileAbstraction->Seek(fileTableOffset, g_PbgFileSeekModes[0]);

    fileTableBuffer = (LPBYTE)MemAlloc(size);
    if (fileTableBuffer == NULL)
    {
        goto parse_error;
    }
    if (m_FileAbstraction->Read(fileTableBuffer, size) == 0)
    {
        goto parse_error;
    }

    decryptedFileTable = FileSystem::Decrypt(fileTableBuffer, size, 0x3e, 0x9b, 0x80, 0x400);
    MemFree(fileTableBuffer);
    fileTableBuffer = decryptedFileTable;

    entryBuffer = Lzss::Decode(fileTableBuffer, size, NULL, fileTableDecompressedSize);
    if (entryBuffer == NULL)
    {
        goto parse_error;
    }

    m_Entries = AllocEntries(entryBuffer, m_NumOfEntries, fileTableOffset);
    if (m_Entries == NULL)
    {
        goto parse_error;
    }

    g_ZunMemory.Free(fileTableBuffer);
    MemFree(entryBuffer);
    return true;

parse_error:
#if defined(TH08_PSP_PORT)
    m_PspArchiveHandleRetained = false;
#endif
    g_ZunMemory.Free(fileTableBuffer);
    MemFree(entryBuffer);
    DeleteEx(m_FileAbstraction);
    utils::DebugPrint(TH_ERR_ARCFILE_CORRUPTED, filename);

    while (false)
        ; // Yes this is correct. No, I don't get it either.

    return false;
}

#pragma var_order(entryData, i, buffer)
PbgArchiveEntry *PbgArchive::AllocEntries(LPVOID entryBuffer, i32 count, u32 dataOffset)
{
    LPVOID entryData;
    int i;
    PbgArchiveEntry *buffer = NULL;

    buffer = new PbgArchiveEntry[count + 1]();
    if (buffer == NULL)
    {
        goto buffer_alloc_error;
    }

    entryData = entryBuffer;
    for (i = 0; i < count; i++)
    {
        buffer[i].filename = CopyFileName((char *)entryData);
        SeekPastString(&entryData);
        buffer[i].dataOffset = ReadPbgU32LE(entryData);
        entryData = static_cast<u8 *>(entryData) + sizeof(u32);
        buffer[i].decompressedSize = ReadPbgU32LE(entryData);
        entryData = static_cast<u8 *>(entryData) + sizeof(u32);
        buffer[i].unconsumedMetadata = ReadPbgU32LE(entryData);
        entryData = static_cast<u8 *>(entryData) + sizeof(u32);
    }

    buffer[count].dataOffset = dataOffset;
    buffer[count].decompressedSize = 0;
    return buffer;

buffer_alloc_error:
    DeleteArray(buffer);
    return NULL;
}

char *PbgArchive::CopyFileName(LPCSTR filename)
{
    char *mem = (char *)MemAlloc(strlen(filename) + 1);
    if (mem != NULL)
    {
        strcpy(mem, filename);
    }
    return mem;
}

// FUNCTION: th08 0x4751e0
DWORD CPbgFile::ReadInt(i32 *outData)
{
    return Read(outData, 4);
}

i32 PbgArchive::SeekPastInt(LPVOID *ptr)
{
    *ptr = static_cast<u8 *>(*ptr) + sizeof(i32);
    return static_cast<i32>(ReadPbgU32LE(*ptr));
}

LPVOID PbgArchive::SeekPastString(LPVOID *ptr)
{
    *ptr = (char *)*ptr + (strlen((char *)*ptr) + 1);
    return *ptr;
}

// FUNCTION: th08 0x475270
PbgArchiveEntry::~PbgArchiveEntry()
{
    MemFree(filename);
}
}; // namespace th08
