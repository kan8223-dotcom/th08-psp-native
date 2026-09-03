#include "resource_decrypt_stream.hpp"
#include "Global.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// Global.cpp owns the retail decrypt oracle and also instantiates the process
// archive singleton. The standalone fixture does not link the archive loader.
namespace th08
{
PbgArchive::PbgArchive()
{
}

PbgArchive::~PbgArchive()
{
}
} // namespace th08

namespace
{
struct Profile
{
    std::uint8_t key;
    std::uint8_t xorValue;
    std::uint8_t xorValueInc;
    std::uint32_t chunkSize;
    std::uint32_t maxBytes;
};

constexpr Profile kProfiles[8] = {
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

std::vector<std::uint8_t> EncryptReference(const std::vector<std::uint8_t> &plain,
                                           const Profile &profile)
{
    std::vector<std::uint8_t> encrypted(plain.size());
    std::int32_t size = static_cast<std::int32_t>(plain.size());
    std::int32_t numUnencrypted =
        (size % static_cast<std::int32_t>(profile.chunkSize) <
         static_cast<std::int32_t>(profile.chunkSize / 4U))
            ? size % static_cast<std::int32_t>(profile.chunkSize)
            : 0;
    numUnencrypted += size & 1;
    size -= numUnencrypted;

    std::uint32_t inputOffset = 0;
    std::uint32_t outputOffset = 0;
    std::uint8_t xorValue = profile.xorValue;
    std::int32_t maxBytes = static_cast<std::int32_t>(profile.maxBytes);
    while (size > 0 && maxBytes > 0)
    {
        std::int32_t currentChunk = static_cast<std::int32_t>(profile.chunkSize);
        if (size < currentChunk)
        {
            currentChunk = size;
        }

        const std::uint32_t chunk = static_cast<std::uint32_t>(currentChunk);
        std::int32_t inputIndex = static_cast<std::int32_t>(inputOffset + chunk - 1U);
        for (std::uint32_t i = (chunk + 1U) / 2U; i > 0; --i)
        {
            encrypted[outputOffset++] =
                static_cast<std::uint8_t>(plain[static_cast<std::uint32_t>(inputIndex)] ^ xorValue);
            inputIndex -= 2;
            xorValue = static_cast<std::uint8_t>(xorValue + profile.xorValueInc);
        }

        inputIndex = static_cast<std::int32_t>(inputOffset + chunk - 2U);
        for (std::uint32_t i = chunk / 2U; i > 0; --i)
        {
            encrypted[outputOffset++] =
                static_cast<std::uint8_t>(plain[static_cast<std::uint32_t>(inputIndex)] ^ xorValue);
            inputIndex -= 2;
            xorValue = static_cast<std::uint8_t>(xorValue + profile.xorValueInc);
        }

        inputOffset += chunk;
        size -= currentChunk;
        maxBytes -= currentChunk;
    }

    size += numUnencrypted;
    if (size > 0)
    {
        const std::uint32_t tail = static_cast<std::uint32_t>(size);
        std::memcpy(&encrypted[outputOffset], &plain[inputOffset], tail);
    }
    return encrypted;
}

std::vector<std::uint8_t> MakeEncrypted(std::uint32_t profileIndex,
                                        std::uint32_t payloadSize)
{
    std::vector<std::uint8_t> plain(payloadSize);
    for (std::uint32_t i = 0; i < payloadSize; ++i)
    {
        plain[i] = static_cast<std::uint8_t>(
            (i * 73U + i / 7U + profileIndex * 29U + (i >> 8U)) & 0xffU);
    }
    const std::vector<std::uint8_t> encrypted =
        EncryptReference(plain, kProfiles[profileIndex]);

    std::vector<std::uint8_t> resource(4U + payloadSize);
    resource[0] = 0x65U;
    resource[1] = 0x64U;
    resource[2] = 0x7aU;
    resource[3] = EncodedProfileKey(profileIndex);
    if (!encrypted.empty())
    {
        std::memcpy(&resource[4], &encrypted[0], encrypted.size());
    }
    return resource;
}

struct CollectSink
{
    std::vector<std::uint8_t> bytes;
    std::uint32_t calls;
    std::uint32_t shortWriteCall;

    CollectSink() : calls(0), shortWriteCall(0) {}
};

std::uint32_t Collect(void *context, const std::uint8_t *data,
                      std::uint32_t dataSize)
{
    CollectSink *sink = static_cast<CollectSink *>(context);
    ++sink->calls;
    std::uint32_t consumed = dataSize;
    if (sink->shortWriteCall != 0 && sink->calls == sink->shortWriteCall)
    {
        consumed = dataSize == 0 ? 0 : dataSize - 1U;
    }
    sink->bytes.insert(sink->bytes.end(), data, data + consumed);
    return consumed;
}

bool FilterResource(const std::vector<std::uint8_t> &resource,
                    std::uint32_t splitSeed,
                    std::vector<std::uint8_t> *output,
                    th08::psp::ResourceDecryptStreamFilter *filter)
{
    CollectSink sink;
    if (filter->Begin(static_cast<std::uint32_t>(resource.size()), Collect, &sink) !=
        th08::psp::ResourceDecryptStreamFilter::Result_Ok)
    {
        return false;
    }

    static constexpr std::uint32_t kSplits[] = {1U, 2U, 3U, 4U, 7U, 31U, 511U, 4096U};
    std::uint32_t offset = 0;
    std::uint32_t splitIndex = splitSeed;
    while (offset < resource.size())
    {
        const std::uint32_t available =
            static_cast<std::uint32_t>(resource.size()) - offset;
        const std::uint32_t requested = kSplits[splitIndex % (sizeof(kSplits) / sizeof(kSplits[0]))];
        const std::uint32_t take = available < requested ? available : requested;
        if (th08::psp::ResourceDecryptStreamFilter::ArchiveSinkWrite(
                filter, &resource[offset], take) != take)
        {
            return false;
        }
        offset += take;
        ++splitIndex;
    }
    if (filter->Finish() != th08::psp::ResourceDecryptStreamFilter::Result_Ok)
    {
        return false;
    }
    output->swap(sink.bytes);
    return true;
}

bool CompareBulkAndStream(std::uint32_t profileIndex, std::uint32_t payloadSize,
                          std::uint32_t splitSeed)
{
    const std::vector<std::uint8_t> resource = MakeEncrypted(profileIndex, payloadSize);
    std::vector<std::uint8_t> bulk(resource);
    if (th08::FileSystem::TryDecryptFromTable(&bulk[0], NULL,
                                              static_cast<i32>(bulk.size())) != &bulk[0])
    {
        std::fprintf(stderr, "bulk decrypt returned a different owner\n");
        return false;
    }

    th08::psp::ResourceDecryptStreamFilter filter;
    std::vector<std::uint8_t> streamed;
    if (!FilterResource(resource, splitSeed, &streamed, &filter) || streamed != bulk ||
        !filter.IsEncrypted() || filter.ProfileIndex() != static_cast<int>(profileIndex) ||
        filter.InputSize() != resource.size() || filter.OutputSize() != resource.size() ||
        filter.LogicalPayloadSize() != payloadSize)
    {
        std::fprintf(stderr,
                     "bulk/stream mismatch profile=%u payload=%u split=%u result=%d "
                     "stream=%lu bulk=%lu\n",
                     profileIndex, payloadSize, splitSeed,
                     static_cast<int>(filter.LastResult()),
                     static_cast<unsigned long>(streamed.size()),
                     static_cast<unsigned long>(bulk.size()));
        return false;
    }
    return true;
}

void AddUnique(std::vector<std::uint32_t> *values, std::uint32_t value)
{
    if (std::find(values->begin(), values->end(), value) == values->end())
    {
        values->push_back(value);
    }
}

bool CheckAllProfiles()
{
    std::uint32_t caseIndex = 0;
    for (std::uint32_t profileIndex = 0; profileIndex < 8U; ++profileIndex)
    {
        const Profile &profile = kProfiles[profileIndex];
        std::vector<std::uint32_t> sizes;
        AddUnique(&sizes, 0U);
        AddUnique(&sizes, 1U);
        AddUnique(&sizes, 2U);
        AddUnique(&sizes, 3U);
        AddUnique(&sizes, profile.chunkSize / 4U - 1U);
        AddUnique(&sizes, profile.chunkSize / 4U);
        AddUnique(&sizes, profile.chunkSize / 4U + 1U);
        AddUnique(&sizes, profile.chunkSize - 1U);
        AddUnique(&sizes, profile.chunkSize);
        AddUnique(&sizes, profile.chunkSize + 1U);
        AddUnique(&sizes, profile.chunkSize * 2U - 1U);
        AddUnique(&sizes, profile.maxBytes - 1U);
        AddUnique(&sizes, profile.maxBytes);
        AddUnique(&sizes, profile.maxBytes + 1U);
        AddUnique(&sizes, profile.maxBytes + profile.chunkSize + 1U);

        for (std::uint32_t size : sizes)
        {
            if (!CompareBulkAndStream(profileIndex, size, caseIndex++))
            {
                return false;
            }
        }
    }
    return true;
}

bool CheckNoOp()
{
    static constexpr std::uint32_t kSizes[] = {0U, 1U, 2U, 3U, 4U, 5U, 4097U};
    for (std::uint32_t size : kSizes)
    {
        std::vector<std::uint8_t> resource(size);
        for (std::uint32_t i = 0; i < size; ++i)
        {
            resource[i] = static_cast<std::uint8_t>((i * 19U + 0x42U) & 0xffU);
        }
        if (size >= 4U)
        {
            resource[0] = 0x65U;
            resource[1] = 0x64U;
            resource[2] = 0x7aU;
            resource[3] = 0xffU; // Signature prefix, deliberately unknown key.
        }

        std::vector<std::uint8_t> bulk(resource);
        std::uint8_t dummy = 0;
        std::uint8_t *bulkPtr = bulk.empty() ? &dummy : &bulk[0];
        if (th08::FileSystem::TryDecryptFromTable(bulkPtr, NULL,
                                                  static_cast<i32>(bulk.size())) != bulkPtr)
        {
            return false;
        }

        th08::psp::ResourceDecryptStreamFilter filter;
        std::vector<std::uint8_t> streamed;
        if (!FilterResource(resource, size, &streamed, &filter) || streamed != resource ||
            filter.IsEncrypted() || filter.ProfileIndex() != -1 ||
            filter.LogicalPayloadSize() != resource.size())
        {
            std::fprintf(stderr, "no-op mismatch size=%u\n", size);
            return false;
        }
    }
    return true;
}

bool CheckFailures()
{
    const std::vector<std::uint8_t> resource = MakeEncrypted(2U, 0x1401U);
    CollectSink sink;
    th08::psp::ResourceDecryptStreamFilter filter;
    if (filter.Begin(static_cast<std::uint32_t>(resource.size()), Collect, &sink) !=
        th08::psp::ResourceDecryptStreamFilter::Result_Ok)
    {
        return false;
    }
    if (th08::psp::ResourceDecryptStreamFilter::ArchiveSinkWrite(
            &filter, &resource[0], static_cast<std::uint32_t>(resource.size() - 1U)) !=
            resource.size() - 1U ||
        filter.Finish() !=
            th08::psp::ResourceDecryptStreamFilter::Result_InputTruncated)
    {
        std::fprintf(stderr, "truncation was not reported\n");
        return false;
    }

    sink = CollectSink();
    if (filter.Begin(static_cast<std::uint32_t>(resource.size() - 1U), Collect, &sink) !=
            th08::psp::ResourceDecryptStreamFilter::Result_Ok ||
        filter.Push(&resource[0], static_cast<std::uint32_t>(resource.size())) !=
            th08::psp::ResourceDecryptStreamFilter::Result_InputOverflow)
    {
        std::fprintf(stderr, "input overflow was not reported\n");
        return false;
    }

    sink = CollectSink();
    sink.shortWriteCall = 1U;
    if (filter.Begin(static_cast<std::uint32_t>(resource.size()), Collect, &sink) !=
            th08::psp::ResourceDecryptStreamFilter::Result_Ok ||
        th08::psp::ResourceDecryptStreamFilter::ArchiveSinkWrite(
            &filter, &resource[0], static_cast<std::uint32_t>(resource.size())) != 0U ||
        filter.LastResult() !=
            th08::psp::ResourceDecryptStreamFilter::Result_DownstreamWriteFailed)
    {
        std::fprintf(stderr, "short downstream write was not propagated\n");
        return false;
    }
    return true;
}
} // namespace

int main()
{
    if (!CheckAllProfiles() || !CheckNoOp() || !CheckFailures())
    {
        return 1;
    }
    std::printf("resource-decrypt-stream: PASS profiles=8 workspace=%lu max-chunk=%lu\n",
                static_cast<unsigned long>(
                    sizeof(th08::psp::ResourceDecryptStreamFilter)),
                static_cast<unsigned long>(
                    th08::psp::ResourceDecryptStreamFilter::kLargestChunkBytes));
    return 0;
}
