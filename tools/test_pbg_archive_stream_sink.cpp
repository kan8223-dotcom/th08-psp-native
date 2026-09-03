#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define private public
#include "pbg/PbgArchive.hpp"
#undef private

HGLOBAL GlobalAlloc(UINT, size_t size)
{
    return std::calloc(1, size);
}

HGLOBAL GlobalFree(HGLOBAL value)
{
    std::free(value);
    return NULL;
}

namespace th08
{
namespace utils
{
void DebugPrint(char *, ...)
{
}
} // namespace utils

namespace psp
{
void BootLog(const char *, ...)
{
}
} // namespace psp

IPbgFile::IPbgFile()
{
}

IPbgFile::~IPbgFile()
{
}

CPbgFile::CPbgFile()
{
}

CPbgFile::~CPbgFile()
{
}

bool CPbgFile::Open(const char *, char *) { return false; }
void CPbgFile::Close() {}
DWORD CPbgFile::Read(LPVOID, DWORD) { return 0; }
bool CPbgFile::Write(LPVOID, DWORD) { return false; }
DWORD CPbgFile::Tell() { return 0; }
DWORD CPbgFile::GetSize() { return 0; }
bool CPbgFile::Seek(DWORD, DWORD) { return false; }
HGLOBAL CPbgFile::ReadWholeFile(DWORD) { return NULL; }

i32 g_PbgFileSeekModes[3] = {FILE_BEGIN, FILE_CURRENT, FILE_END};
char *g_PbgFileOpenModes[3] = {const_cast<char *>("r"), const_cast<char *>("w"),
                               const_cast<char *>("a")};
} // namespace th08

class TestFile : public th08::CPbgFile
{
  public:
    explicit TestFile(const std::vector<u8> &data)
        : m_Data(data), m_Offset(0), m_MaxRead(31), m_ReportedSize(static_cast<u32>(data.size())),
          m_OpenFails(false), m_SeekFails(false), m_ReadOnlyOpen(false), m_OpenCalls(0),
          m_CursorMismatchAt(0)
    {
    }

    virtual bool Open(const char *, char *mode)
    {
        m_OpenCalls++;
        m_Offset = 0;
        m_ReadOnlyOpen = mode != NULL && std::strcmp(mode, "r") == 0;
        return !m_OpenFails;
    }
    virtual void Close() {}
    virtual DWORD Read(LPVOID data, DWORD dataLen)
    {
        if (m_Offset >= m_Data.size())
        {
            return 0;
        }
        size_t count = std::min<size_t>(dataLen, m_MaxRead);
        count = std::min(count, m_Data.size() - m_Offset);
        std::memcpy(data, &m_Data[m_Offset], count);
        m_Offset += count;
        return static_cast<DWORD>(count);
    }
    virtual bool Write(LPVOID, DWORD) { return false; }
    virtual DWORD Tell()
    {
        if (m_CursorMismatchAt != 0 && m_Offset >= m_CursorMismatchAt)
        {
            return static_cast<DWORD>(m_Offset - 1);
        }
        return static_cast<DWORD>(m_Offset);
    }
    virtual DWORD GetSize() { return m_ReportedSize; }
    virtual bool Seek(DWORD offset, DWORD seekFrom)
    {
        if (m_SeekFails || seekFrom != FILE_BEGIN || offset > m_Data.size())
        {
            return false;
        }
        m_Offset = offset;
        return true;
    }

    const std::vector<u8> &m_Data;
    size_t m_Offset;
    u32 m_MaxRead;
    u32 m_ReportedSize;
    bool m_OpenFails;
    bool m_SeekFails;
    bool m_ReadOnlyOpen;
    u32 m_OpenCalls;
    u32 m_CursorMismatchAt;
};

struct CollectSink
{
    std::vector<u8> bytes;
    bool shortWrite;

    CollectSink() : shortWrite(false) {}
};

static u32 Collect(void *context, const u8 *data, u32 dataSize)
{
    CollectSink *sink = static_cast<CollectSink *>(context);
    const u32 consumed = sink->shortWrite && dataSize != 0 ? dataSize - 1 : dataSize;
    sink->bytes.insert(sink->bytes.end(), data, data + consumed);
    return consumed;
}

static bool EncodeFixture(const std::vector<u8> &input, std::vector<u8> *encoded)
{
    encoded->assign(input.size() * 2, 0);
    i32 encodedSize = 0;
    if (th08::Lzss::Encode(const_cast<u8 *>(&input[0]), static_cast<i32>(input.size()),
                           &encodedSize, &(*encoded)[0], static_cast<i32>(encoded->size())) == NULL)
    {
        return false;
    }
    encoded->resize(encodedSize);
    return true;
}

struct ArchiveFixture
{
    th08::PbgArchive *archive;
    TestFile *file;
    th08::PbgArchiveEntry *entries;
    u32 dataOffset;
    u32 endOffset;

    ArchiveFixture(const std::vector<u8> &archiveData, u32 start, u32 end, u32 decompressedSize)
        : archive(new th08::PbgArchive()), file(new TestFile(archiveData)),
          entries(static_cast<th08::PbgArchiveEntry *>(
              std::calloc(2, sizeof(th08::PbgArchiveEntry)))),
          dataOffset(start), endOffset(end)
    {
        entries[0].filename = const_cast<char *>("fixture.anm");
        entries[0].dataOffset = start;
        entries[0].decompressedSize = decompressedSize;
        entries[1].dataOffset = end;
        archive->m_Entries = entries;
        archive->m_NumOfEntries = 1;
        archive->m_Filename = const_cast<char *>("readonly-fixture.dat");
        archive->m_FileAbstraction = file;
        archive->m_PspArchiveHandleRetained = true;
        archive->m_PspArchiveEntryTransactions = 0;
        archive->m_PspArchiveRecoveryOpens = 0;
    }
};

static bool CheckSuccess(const std::vector<u8> &expected, const std::vector<u8> &archiveData,
                         u32 dataOffset, u32 endOffset)
{
    ArchiveFixture fixture(archiveData, dataOffset, endOffset, static_cast<u32>(expected.size()));
    th08::Lzss::StreamWorkspace workspace;
    th08::PbgArchive::StreamEntryInfo info;
    CollectSink sink;

    th08::PbgArchive *notLoaded = new th08::PbgArchive();
    if (notLoaded->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace, &info) !=
            th08::PbgArchive::STREAM_ARCHIVE_NOT_LOADED ||
        notLoaded->ReadDecompressEntryToSink(NULL, Collect, &sink, &workspace, &info) !=
            th08::PbgArchive::STREAM_INVALID_ARGUMENT)
    {
        std::fprintf(stderr, "archive state/argument validation failed\n");
        return false;
    }
    const th08::PbgArchive::StreamResult result = fixture.archive->ReadDecompressEntryToSink(
        "FIXTURE.ANM", Collect, &sink, &workspace, &info);
    if (result != th08::PbgArchive::STREAM_SUCCESS || sink.bytes != expected ||
        fixture.file->m_OpenCalls != 0 || fixture.file->Tell() != endOffset ||
        info.dataOffset != dataOffset || info.compressedSize != endOffset - dataOffset ||
        info.decompressedSize != expected.size() ||
        info.payloadState !=
            th08::PbgArchive::STREAM_PAYLOAD_PBG_DECOMPRESSED_RESOURCE_TRANSFORM_PENDING)
    {
        std::fprintf(stderr, "archive stream success contract failed result=%d\n",
                     static_cast<int>(result));
        return false;
    }
    return true;
}

static bool CheckFailures(const std::vector<u8> &expected, const std::vector<u8> &archiveData,
                          u32 dataOffset, u32 endOffset)
{
    th08::Lzss::StreamWorkspace workspace;
    th08::PbgArchive::StreamEntryInfo info;
    CollectSink sink;

    ArchiveFixture missing(archiveData, dataOffset, endOffset, static_cast<u32>(expected.size()));
    if (missing.archive->ReadDecompressEntryToSink("missing.anm", Collect, &sink, &workspace, &info) !=
        th08::PbgArchive::STREAM_ENTRY_NOT_FOUND)
    {
        std::fprintf(stderr, "missing entry was not rejected\n");
        return false;
    }

    ArchiveFixture openFailure(archiveData, dataOffset, endOffset, static_cast<u32>(expected.size()));
    openFailure.archive->m_PspArchiveHandleRetained = false;
    openFailure.file->m_OpenFails = true;
    if (openFailure.archive->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace,
                                                       &info) !=
        th08::PbgArchive::STREAM_ARCHIVE_OPEN_FAILED)
    {
        std::fprintf(stderr, "open failure was not propagated\n");
        return false;
    }

    ArchiveFixture invalidRange(archiveData, endOffset, dataOffset, static_cast<u32>(expected.size()));
    if (invalidRange.archive->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace,
                                                        &info) !=
        th08::PbgArchive::STREAM_ENTRY_RANGE_INVALID)
    {
        std::fprintf(stderr, "invalid entry range was not rejected\n");
        return false;
    }

    ArchiveFixture seekFailure(archiveData, dataOffset, endOffset, static_cast<u32>(expected.size()));
    seekFailure.file->m_SeekFails = true;
    if (seekFailure.archive->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace,
                                                       &info) !=
        th08::PbgArchive::STREAM_ARCHIVE_SEEK_FAILED)
    {
        std::fprintf(stderr, "seek failure was not propagated\n");
        return false;
    }

    ArchiveFixture cursorMismatch(archiveData, dataOffset, endOffset,
                                  static_cast<u32>(expected.size()));
    cursorMismatch.file->m_CursorMismatchAt = endOffset;
    if (cursorMismatch.archive->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace,
                                                          &info) !=
        th08::PbgArchive::STREAM_ARCHIVE_CURSOR_MISMATCH)
    {
        std::fprintf(stderr, "final cursor mismatch was not propagated\n");
        return false;
    }

    ArchiveFixture inputFailure(archiveData, dataOffset, endOffset, static_cast<u32>(expected.size()));
    inputFailure.file->m_ReportedSize = endOffset;
    inputFailure.entries[1].dataOffset = endOffset + 1;
    inputFailure.file->m_ReportedSize = endOffset + 1;
    if (inputFailure.archive->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace,
                                                        &info) !=
        th08::PbgArchive::STREAM_DECODE_INPUT_READ_FAILED)
    {
        std::fprintf(stderr, "input short read was not propagated\n");
        return false;
    }

    ArchiveFixture sinkFailure(archiveData, dataOffset, endOffset, static_cast<u32>(expected.size()));
    sink = CollectSink();
    sink.shortWrite = true;
    if (sinkFailure.archive->ReadDecompressEntryToSink("fixture.anm", Collect, &sink, &workspace,
                                                       &info) !=
        th08::PbgArchive::STREAM_DECODE_SINK_WRITE_FAILED)
    {
        std::fprintf(stderr, "sink failure was not propagated\n");
        return false;
    }

    return true;
}

static bool CheckPersistentHandle(const std::vector<u8> &expected,
                                  const std::vector<u8> &archiveData,
                                  u32 dataOffset, u32 endOffset)
{
    ArchiveFixture fixture(archiveData, dataOffset, endOffset,
                           static_cast<u32>(expected.size()));
    std::vector<u8> first(expected.size());
    std::vector<u8> second(expected.size());

    if (fixture.archive->ReadDecompressEntry("fixture.anm", &first[0]) == NULL ||
        fixture.archive->ReadDecompressEntry("fixture.anm", &second[0]) == NULL ||
        first != expected || second != expected || fixture.file->m_OpenCalls != 0 ||
        fixture.file->Tell() != endOffset ||
        fixture.archive->m_PspArchiveEntryTransactions != 2 ||
        fixture.archive->m_PspArchiveRecoveryOpens != 0)
    {
        std::fprintf(stderr, "persistent archive handle/absolute seek contract failed\n");
        return false;
    }

    ArchiveFixture recovery(archiveData, dataOffset, endOffset,
                            static_cast<u32>(expected.size()));
    recovery.archive->m_PspArchiveHandleRetained = false;
    th08::Lzss::StreamWorkspace workspace;
    CollectSink firstSink;
    CollectSink secondSink;
    if (recovery.archive->ReadDecompressEntryToSink(
            "fixture.anm", Collect, &firstSink, &workspace, NULL) !=
            th08::PbgArchive::STREAM_SUCCESS ||
        recovery.archive->ReadDecompressEntryToSink(
            "fixture.anm", Collect, &secondSink, &workspace, NULL) !=
            th08::PbgArchive::STREAM_SUCCESS ||
        firstSink.bytes != expected || secondSink.bytes != expected ||
        recovery.file->m_OpenCalls != 1 ||
        !recovery.archive->m_PspArchiveHandleRetained ||
        recovery.archive->m_PspArchiveEntryTransactions != 2 ||
        recovery.archive->m_PspArchiveRecoveryOpens != 1)
    {
        std::fprintf(stderr, "one-time archive recovery open contract failed\n");
        return false;
    }
    return true;
}

int main()
{
    std::vector<u8> expected(7000);
    for (u32 i = 0; i < expected.size(); i++)
    {
        // The prefix deliberately resembles opaque resource data: the archive
        // layer must preserve every byte and leave decryption to its caller.
        expected[i] = static_cast<u8>((i * 37 + i / 5 + 0x1b) & 0xff);
    }

    std::vector<u8> encoded;
    if (!EncodeFixture(expected, &encoded))
    {
        return 1;
    }
    const u32 dataOffset = 13;
    std::vector<u8> archiveData(dataOffset, 0xee);
    archiveData.insert(archiveData.end(), encoded.begin(), encoded.end());
    const u32 endOffset = static_cast<u32>(archiveData.size());

    if (!CheckSuccess(expected, archiveData, dataOffset, endOffset) ||
        !CheckFailures(expected, archiveData, dataOffset, endOffset) ||
        !CheckPersistentHandle(expected, archiveData, dataOffset, endOffset))
    {
        return 1;
    }
    std::printf("pbg-archive-stream-sink: PASS raw=%lu compressed=%lu workspace=%lu\n",
                static_cast<unsigned long>(expected.size()),
                static_cast<unsigned long>(encoded.size()),
                static_cast<unsigned long>(sizeof(th08::Lzss::StreamWorkspace)));
    return 0;
}
