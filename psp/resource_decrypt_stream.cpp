#include "resource_decrypt_stream.hpp"

#include <climits>
#include <cstring>

namespace th08::psp
{
namespace
{
constexpr std::uint8_t kSignature[3] = {0x65U, 0x64U, 0x7aU};

struct DecryptProfile
{
    std::uint8_t key;
    std::uint8_t xorValue;
    std::uint8_t xorValueInc;
    std::uint32_t chunkSize;
    std::uint32_t maxBytes;
};

constexpr DecryptProfile kProfiles[8] = {
    {0x5dU, 0x1bU, 0x37U, 0x0040U, 0x2800U},
    {0x74U, 0x51U, 0xe9U, 0x0040U, 0x3000U},
    {0x71U, 0xc1U, 0x51U, 0x1400U, 0x2000U},
    {0x8aU, 0x03U, 0x19U, 0x1400U, 0x7800U},
    {0x95U, 0xabU, 0xcdU, 0x0200U, 0x1000U},
    {0xb7U, 0x12U, 0x34U, 0x0400U, 0x2800U},
    {0x9dU, 0x35U, 0x97U, 0x0080U, 0x2800U},
    {0xaaU, 0x99U, 0x37U, 0x0400U, 0x1000U},
};

std::uint8_t EncodedProfileKey(std::uint32_t profileIndex)
{
    return static_cast<std::uint8_t>(
        static_cast<std::uint32_t>(kProfiles[profileIndex].key) -
        (profileIndex << 4U) - 0x10U);
}
} // namespace

ResourceDecryptStreamFilter::ResourceDecryptStreamFilter()
{
    Reset();
}

void ResourceDecryptStreamFilter::Reset()
{
    m_Downstream = nullptr;
    m_DownstreamContext = nullptr;
    m_Result = Result_NotActive;
    m_Active = false;
    m_Finished = false;
    m_DecisionMade = false;
    m_Encrypted = false;
    m_ProfileIndex = -1;
    m_ExpectedInputSize = 0;
    m_InputReceived = 0;
    m_OutputProduced = 0;
    m_LogicalPayloadSize = 0;
    m_PayloadReceived = 0;
    m_DecryptBytes = 0;
    m_DecryptProcessed = 0;
    m_ProfileChunkSize = 0;
    m_ChunkTarget = 0;
    m_ChunkFill = 0;
    m_XorValue = 0;
    m_XorValueInc = 0;
    m_HeaderSize = 0;
    m_LastRawCount = 0;
    m_LastRawNext = 0;
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::Begin(
    std::uint32_t inputSize, Lzss::StreamSinkWrite downstream,
    void *downstreamContext)
{
    Reset();
    if (downstream == nullptr || inputSize > static_cast<std::uint32_t>(INT_MAX))
    {
        return Fail(Result_InvalidArgument);
    }

    m_Downstream = downstream;
    m_DownstreamContext = downstreamContext;
    m_Result = Result_Ok;
    m_Active = true;
    m_ExpectedInputSize = inputSize;
    m_LogicalPayloadSize = inputSize;
    return Result_Ok;
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::Fail(Result result)
{
    if (m_Result == Result_Ok || m_Result == Result_NotActive)
    {
        m_Result = result;
    }
    return m_Result;
}

void ResourceDecryptStreamFilter::RememberRawBytes(const std::uint8_t *data,
                                                    std::uint32_t dataSize)
{
    for (std::uint32_t i = 0; i < dataSize; ++i)
    {
        m_LastRaw[m_LastRawNext] = data[i];
        m_LastRawNext = (m_LastRawNext + 1U) & 3U;
        if (m_LastRawCount < kSignatureBytes)
        {
            ++m_LastRawCount;
        }
    }
}

bool ResourceDecryptStreamFilter::DecideFromSignature()
{
    m_DecisionMade = true;
    if (m_HeaderSize != kSignatureBytes || m_Header[0] != kSignature[0] ||
        m_Header[1] != kSignature[1] || m_Header[2] != kSignature[2])
    {
        return false;
    }

    for (std::uint32_t i = 0; i < 8U; ++i)
    {
        if (m_Header[3] != EncodedProfileKey(i))
        {
            continue;
        }

        const DecryptProfile &profile = kProfiles[i];
        m_Encrypted = true;
        m_ProfileIndex = static_cast<int>(i);
        m_LogicalPayloadSize =
            m_ExpectedInputSize - static_cast<std::uint32_t>(kSignatureBytes);
        m_DecryptBytes = CalculateDecryptBytes(profile.chunkSize, profile.maxBytes,
                                                m_LogicalPayloadSize);
        m_ProfileChunkSize = profile.chunkSize;
        m_XorValue = profile.xorValue;
        m_XorValueInc = profile.xorValueInc;
        return true;
    }
    return false;
}

std::uint32_t ResourceDecryptStreamFilter::CalculateDecryptBytes(
    std::uint32_t chunkSize, std::uint32_t maxBytesLimit,
    std::uint32_t payloadSize) const
{
    // Keep the signed arithmetic and loop condition of FileSystem::Decrypt.
    // In particular, a final chunk is not clamped to maxBytes: maxBytes is
    // tested only before the chunk and may become negative afterwards.
    const std::int32_t size = static_cast<std::int32_t>(payloadSize);
    std::int32_t numUnencrypted =
        (size % static_cast<std::int32_t>(chunkSize) <
         static_cast<std::int32_t>(chunkSize / 4U))
            ? size % static_cast<std::int32_t>(chunkSize)
            : 0;
    numUnencrypted += size & 1;

    std::int32_t remaining = size - numUnencrypted;
    std::int32_t maxBytes = static_cast<std::int32_t>(maxBytesLimit);
    std::uint32_t decryptBytes = 0;
    while (remaining > 0 && maxBytes > 0)
    {
        std::int32_t currentChunk = static_cast<std::int32_t>(chunkSize);
        if (remaining < currentChunk)
        {
            currentChunk = remaining;
        }
        decryptBytes += static_cast<std::uint32_t>(currentChunk);
        remaining -= currentChunk;
        maxBytes -= currentChunk;
    }
    return decryptBytes;
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::WriteDownstream(
    const std::uint8_t *data, std::uint32_t dataSize)
{
    if (dataSize == 0)
    {
        return Result_Ok;
    }
    if (m_OutputProduced > m_ExpectedInputSize ||
        dataSize > m_ExpectedInputSize - m_OutputProduced)
    {
        return Fail(Result_InternalStateError);
    }

    const std::uint32_t consumed =
        m_Downstream(m_DownstreamContext, data, dataSize);
    if (consumed != dataSize)
    {
        return Fail(Result_DownstreamWriteFailed);
    }
    m_OutputProduced += dataSize;
    return Result_Ok;
}

std::uint32_t ResourceDecryptStreamFilter::DestinationForSource(
    std::uint32_t source, std::uint32_t chunkSize) const
{
    const std::uint32_t firstHalf = (chunkSize + 1U) / 2U;
    if (source < firstHalf)
    {
        return chunkSize - 1U - source * 2U;
    }
    return chunkSize - 2U - (source - firstHalf) * 2U;
}

bool ResourceDecryptStreamFilter::IsVisited(std::uint32_t index) const
{
    return (m_Visited[index >> 3U] & static_cast<std::uint8_t>(1U << (index & 7U))) != 0;
}

void ResourceDecryptStreamFilter::SetVisited(std::uint32_t index)
{
    m_Visited[index >> 3U] |= static_cast<std::uint8_t>(1U << (index & 7U));
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::TransformAndWriteChunk()
{
    if (m_ChunkTarget == 0 || m_ChunkFill != m_ChunkTarget ||
        m_ChunkTarget > kLargestChunkBytes)
    {
        return Fail(Result_InternalStateError);
    }

    const std::uint32_t visitedBytes = (m_ChunkTarget + 7U) / 8U;
    std::memset(m_Visited, 0, visitedBytes);
    const std::uint8_t chunkXorStart = m_XorValue;

    // Apply P[dest(source)] = E[source] ^ xor(source) in permutation cycles.
    // This retains only the one encrypted chunk plus a 640-byte visited map;
    // no second plaintext chunk or full-resource destination is required.
    for (std::uint32_t start = 0; start < m_ChunkTarget; ++start)
    {
        if (IsVisited(start))
        {
            continue;
        }

        std::uint32_t source = start;
        std::uint8_t carried = m_Chunk[source];
        do
        {
            SetVisited(source);
            const std::uint32_t destination =
                DestinationForSource(source, m_ChunkTarget);
            const std::uint8_t next = m_Chunk[destination];
            const std::uint8_t xorValue = static_cast<std::uint8_t>(
                static_cast<std::uint32_t>(chunkXorStart) +
                source * static_cast<std::uint32_t>(m_XorValueInc));
            m_Chunk[destination] = static_cast<std::uint8_t>(carried ^ xorValue);
            carried = next;
            source = destination;
        } while (source != start);
    }

    m_XorValue = static_cast<std::uint8_t>(
        static_cast<std::uint32_t>(m_XorValue) +
        m_ChunkTarget * static_cast<std::uint32_t>(m_XorValueInc));
    const Result result = WriteDownstream(m_Chunk, m_ChunkTarget);
    if (result != Result_Ok)
    {
        return result;
    }
    m_DecryptProcessed += m_ChunkTarget;
    m_ChunkTarget = 0;
    m_ChunkFill = 0;
    return Result_Ok;
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::ProcessEncrypted(
    const std::uint8_t *data, std::uint32_t dataSize)
{
    const std::uint8_t *cursor = data;
    std::uint32_t remaining = dataSize;
    while (remaining != 0)
    {
        if (m_DecryptProcessed < m_DecryptBytes)
        {
            if (m_ChunkTarget == 0)
            {
                const std::uint32_t decryptRemaining =
                    m_DecryptBytes - m_DecryptProcessed;
                m_ChunkTarget = decryptRemaining < m_ProfileChunkSize
                                    ? decryptRemaining
                                    : m_ProfileChunkSize;
            }

            const std::uint32_t needed = m_ChunkTarget - m_ChunkFill;
            const std::uint32_t take = remaining < needed ? remaining : needed;
            std::memcpy(m_Chunk + m_ChunkFill, cursor, take);
            m_ChunkFill += take;
            m_PayloadReceived += take;
            cursor += take;
            remaining -= take;

            if (m_ChunkFill == m_ChunkTarget)
            {
                const Result result = TransformAndWriteChunk();
                if (result != Result_Ok)
                {
                    return result;
                }
            }
            continue;
        }

        const Result result = WriteDownstream(cursor, remaining);
        if (result != Result_Ok)
        {
            return result;
        }
        m_PayloadReceived += remaining;
        remaining = 0;
    }
    return Result_Ok;
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::Push(
    const std::uint8_t *data, std::uint32_t dataSize)
{
    if (!m_Active || m_Finished)
    {
        return Fail(Result_NotActive);
    }
    if (m_Result != Result_Ok)
    {
        return m_Result;
    }
    if ((data == nullptr && dataSize != 0) ||
        m_InputReceived > m_ExpectedInputSize ||
        dataSize > m_ExpectedInputSize - m_InputReceived)
    {
        return Fail(data == nullptr && dataSize != 0
                        ? Result_InvalidArgument
                        : Result_InputOverflow);
    }
    if (dataSize == 0)
    {
        return Result_Ok;
    }

    RememberRawBytes(data, dataSize);
    m_InputReceived += dataSize;

    const std::uint8_t *cursor = data;
    std::uint32_t remaining = dataSize;
    while (remaining != 0 && m_HeaderSize < kSignatureBytes)
    {
        m_Header[m_HeaderSize++] = *cursor++;
        --remaining;
    }

    if (!m_DecisionMade && m_HeaderSize == kSignatureBytes)
    {
        if (!DecideFromSignature())
        {
            const Result result = WriteDownstream(m_Header, kSignatureBytes);
            if (result != Result_Ok)
            {
                return result;
            }
        }
    }

    if (remaining == 0)
    {
        return Result_Ok;
    }
    if (!m_DecisionMade)
    {
        return Fail(Result_InternalStateError);
    }
    if (!m_Encrypted)
    {
        return WriteDownstream(cursor, remaining);
    }
    return ProcessEncrypted(cursor, remaining);
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::Finish()
{
    if (!m_Active)
    {
        return m_Result == Result_Ok ? Result_NotActive : m_Result;
    }
    if (m_Finished)
    {
        return m_Result;
    }
    if (m_Result != Result_Ok)
    {
        return m_Result;
    }
    if (m_InputReceived != m_ExpectedInputSize)
    {
        return Fail(Result_InputTruncated);
    }

    if (!m_DecisionMade)
    {
        m_DecisionMade = true;
        const Result result = WriteDownstream(m_Header, m_HeaderSize);
        if (result != Result_Ok)
        {
            return result;
        }
    }

    if (m_Encrypted)
    {
        if (m_HeaderSize != kSignatureBytes || m_LastRawCount != kSignatureBytes ||
            m_PayloadReceived != m_LogicalPayloadSize ||
            m_DecryptProcessed != m_DecryptBytes || m_ChunkFill != 0 ||
            m_ChunkTarget != 0)
        {
            return Fail(Result_InternalStateError);
        }

        std::uint8_t compatibilityTail[kSignatureBytes];
        for (std::uint32_t i = 0; i < kSignatureBytes; ++i)
        {
            compatibilityTail[i] = m_LastRaw[(m_LastRawNext + i) & 3U];
        }
        const Result result = WriteDownstream(compatibilityTail, kSignatureBytes);
        if (result != Result_Ok)
        {
            return result;
        }
    }

    if (m_OutputProduced != m_ExpectedInputSize)
    {
        return Fail(Result_InternalStateError);
    }
    m_Finished = true;
    return Result_Ok;
}

u32 ResourceDecryptStreamFilter::ArchiveSinkWrite(
    void *context, const u8 *data, u32 dataSize)
{
    if (context == nullptr)
    {
        return 0;
    }
    ResourceDecryptStreamFilter *filter =
        static_cast<ResourceDecryptStreamFilter *>(context);
    return filter->Push(data, dataSize) == Result_Ok ? dataSize : 0;
}

ResourceDecryptStreamFilter::Result ResourceDecryptStreamFilter::LastResult() const
{
    return m_Result;
}

bool ResourceDecryptStreamFilter::IsEncrypted() const
{
    return m_Encrypted;
}

int ResourceDecryptStreamFilter::ProfileIndex() const
{
    return m_ProfileIndex;
}

std::uint32_t ResourceDecryptStreamFilter::InputSize() const
{
    return m_ExpectedInputSize;
}

std::uint32_t ResourceDecryptStreamFilter::OutputSize() const
{
    return m_OutputProduced;
}

std::uint32_t ResourceDecryptStreamFilter::LogicalPayloadSize() const
{
    return m_LogicalPayloadSize;
}
} // namespace th08::psp
