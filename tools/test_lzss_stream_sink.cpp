#include "pbg/Lzss.hpp"
#include "pbg/PbgFile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Lzss.cpp uses the original Win32 allocation API through PbgMemory.hpp.
// Keep this fixture standalone and deterministic on the host.
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
IPbgFile::IPbgFile()
{
}

IPbgFile::~IPbgFile()
{
}
} // namespace th08

class MemoryFile : public th08::IPbgFile
{
  public:
    MemoryFile(const std::vector<u8> &data, u32 maxRead, u32 readableBytes)
        : m_Data(data), m_Offset(0), m_MaxRead(maxRead), m_ReadableBytes(readableBytes)
    {
    }

    virtual bool Open(const char *, char *) { return true; }
    virtual void Close() {}
    virtual DWORD Read(LPVOID data, DWORD dataLen)
    {
        if (m_Offset >= m_Data.size() || m_Offset >= m_ReadableBytes)
        {
            return 0;
        }

        size_t count = std::min<size_t>(dataLen, m_MaxRead);
        count = std::min(count, m_Data.size() - m_Offset);
        count = std::min<size_t>(count, m_ReadableBytes - m_Offset);
        std::memcpy(data, &m_Data[m_Offset], count);
        m_Offset += count;
        return static_cast<DWORD>(count);
    }
    virtual bool Write(LPVOID, DWORD) { return false; }
    virtual DWORD Tell() { return static_cast<DWORD>(m_Offset); }
    virtual DWORD GetSize() { return static_cast<DWORD>(m_Data.size()); }
    virtual bool Seek(DWORD offset, DWORD seekFrom)
    {
        if (seekFrom == FILE_BEGIN)
            m_Offset = offset;
        else if (seekFrom == FILE_CURRENT)
            m_Offset += offset;
        else if (seekFrom == FILE_END)
            m_Offset = m_Data.size() + offset;
        return m_Offset <= m_Data.size();
    }

  private:
    const std::vector<u8> &m_Data;
    size_t m_Offset;
    u32 m_MaxRead;
    u32 m_ReadableBytes;
};

struct CollectSink
{
    std::vector<u8> bytes;
    u32 calls;
    u32 shortWriteCall;

    CollectSink() : calls(0), shortWriteCall(0) {}
};

static u32 Collect(void *context, const u8 *data, u32 dataSize)
{
    CollectSink *sink = static_cast<CollectSink *>(context);
    sink->calls++;
    u32 consumed = dataSize;
    if (sink->shortWriteCall != 0 && sink->calls == sink->shortWriteCall)
    {
        consumed = dataSize == 0 ? 0 : dataSize - 1;
    }
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

static bool DecodeAndCompare(const std::vector<u8> &encoded, const std::vector<u8> &expected,
                             u32 maxRead)
{
    const u32 encodedSize = static_cast<u32>(encoded.size());
    const u32 expectedSize = static_cast<u32>(expected.size());
    std::vector<u8> legacy(expected.size());
    MemoryFile legacyFile(encoded, maxRead, encodedSize);
    if (th08::Lzss::DecodeFile(&legacyFile, encodedSize, &legacy[0], expectedSize) == NULL ||
        legacyFile.Tell() != encodedSize || legacy != expected)
    {
        std::fprintf(stderr, "legacy DecodeFile regression\n");
        return false;
    }

    th08::Lzss::StreamWorkspace workspace;
    CollectSink sink;
    MemoryFile streamFile(encoded, maxRead, encodedSize);
    const th08::Lzss::StreamResult result =
        th08::Lzss::DecodeFileToSink(&streamFile, encodedSize, expectedSize,
                                     Collect, &sink, &workspace);
    if (result != th08::Lzss::STREAM_SUCCESS || streamFile.Tell() != encodedSize ||
        sink.bytes != expected)
    {
        std::fprintf(stderr, "stream mismatch result=%d read=%lu/%lu output=%lu/%lu\n",
                     static_cast<int>(result), static_cast<unsigned long>(streamFile.Tell()),
                     static_cast<unsigned long>(encoded.size()),
                     static_cast<unsigned long>(sink.bytes.size()),
                     static_cast<unsigned long>(expected.size()));
        return false;
    }
    return true;
}

static bool CheckExactDecode()
{
    const u8 implicitBytes[] = {0xa0, 0x80};
    const std::vector<u8> implicit(implicitBytes, implicitBytes + sizeof(implicitBytes));
    const std::vector<u8> letter(1, 'A');
    if (!DecodeAndCompare(implicit, letter, 1))
    {
        return false;
    }

    std::vector<u8> input(10000);
    for (u32 i = 0; i < input.size(); i++)
    {
        input[i] = static_cast<u8>((i % 97) < 80 ? (i % 13) : ((i * 29 + 7) & 0xff));
    }

    std::vector<u8> encoded;
    if (!EncodeFixture(input, &encoded))
    {
        std::fprintf(stderr, "fixture encode failed\n");
        return false;
    }

    static const u32 readSizes[] = {1, 7, 31, 997, 4096};
    for (u32 i = 0; i < sizeof(readSizes) / sizeof(readSizes[0]); i++)
    {
        if (!DecodeAndCompare(encoded, input, readSizes[i]))
        {
            return false;
        }
    }
    return true;
}

static bool CheckFailureContracts()
{
    std::vector<u8> input(5000);
    for (u32 i = 0; i < input.size(); i++)
    {
        input[i] = static_cast<u8>((i * 17 + i / 11) & 0xff);
    }
    std::vector<u8> encoded;
    if (!EncodeFixture(input, &encoded))
    {
        return false;
    }
    const u32 encodedSize = static_cast<u32>(encoded.size());
    const u32 inputSize = static_cast<u32>(input.size());

    th08::Lzss::StreamWorkspace workspace;
    CollectSink sink;

    MemoryFile shortOutput(encoded, 31, encodedSize);
    if (th08::Lzss::DecodeFileToSink(&shortOutput, encodedSize, inputSize - 1,
                                     Collect, &sink, &workspace) != th08::Lzss::STREAM_OUTPUT_OVERFLOW)
    {
        std::fprintf(stderr, "output overflow was not reported\n");
        return false;
    }

    sink = CollectSink();
    MemoryFile longOutput(encoded, 31, encodedSize);
    if (th08::Lzss::DecodeFileToSink(&longOutput, encodedSize, inputSize + 1,
                                     Collect, &sink, &workspace) !=
        th08::Lzss::STREAM_OUTPUT_SIZE_MISMATCH)
    {
        std::fprintf(stderr, "output size mismatch was not reported\n");
        return false;
    }

    sink = CollectSink();
    MemoryFile truncated(encoded, 31, encodedSize - 1);
    if (th08::Lzss::DecodeFileToSink(&truncated, encodedSize, inputSize,
                                     Collect, &sink, &workspace) !=
        th08::Lzss::STREAM_INPUT_READ_FAILED)
    {
        std::fprintf(stderr, "truncated input was not propagated\n");
        return false;
    }

    sink = CollectSink();
    sink.shortWriteCall = 1;
    MemoryFile shortSink(encoded, 31, encodedSize);
    if (th08::Lzss::DecodeFileToSink(&shortSink, encodedSize, inputSize,
                                     Collect, &sink, &workspace) !=
        th08::Lzss::STREAM_SINK_WRITE_FAILED)
    {
        std::fprintf(stderr, "short sink write was not propagated\n");
        return false;
    }

    MemoryFile valid(encoded, 31, encodedSize);
    if (th08::Lzss::DecodeFileToSink(NULL, encodedSize, inputSize,
                                     Collect, &sink, &workspace) !=
            th08::Lzss::STREAM_INVALID_ARGUMENT ||
        th08::Lzss::DecodeFileToSink(&valid, encodedSize, inputSize,
                                     NULL, &sink, &workspace) !=
            th08::Lzss::STREAM_INVALID_ARGUMENT ||
        th08::Lzss::DecodeFileToSink(&valid, encodedSize, inputSize,
                                     Collect, &sink, NULL) !=
            th08::Lzss::STREAM_INVALID_ARGUMENT)
    {
        std::fprintf(stderr, "invalid arguments were not rejected\n");
        return false;
    }

    return true;
}

int main()
{
    if (!CheckExactDecode() || !CheckFailureContracts())
    {
        return 1;
    }
    std::printf("lzss-stream-sink: PASS workspace=%lu output-chunk=%u\n",
                static_cast<unsigned long>(sizeof(th08::Lzss::StreamWorkspace)),
                th08::Lzss::STREAM_OUTPUT_BUFFER_SIZE);
    return 0;
}
