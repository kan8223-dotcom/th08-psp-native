#pragma once

#include "pbg/Lzss.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace th08::psp
{
// Incremental equivalent of TryDecryptFromTable/DecryptPspResourceInPlace.
// One instance belongs to one private resource-load transaction.  It owns no
// heap memory and must not be copied or shared between concurrent loads.
class ResourceDecryptStreamFilter
{
  public:
    static constexpr std::size_t kSignatureBytes = 4U;
    static constexpr std::size_t kLargestChunkBytes = 0x1400U;
    static constexpr std::size_t kVisitedBytes = kLargestChunkBytes / 8U;

    enum Result
    {
        Result_Ok = 0,
        Result_InvalidArgument,
        Result_NotActive,
        Result_InputOverflow,
        Result_InputTruncated,
        Result_DownstreamWriteFailed,
        Result_InternalStateError
    };

    ResourceDecryptStreamFilter();

    // Begin one synchronous filter transaction. inputSize is the complete PBG
    // decompressed size, including a possible four-byte resource signature.
    // downstream must consume bytes synchronously and must not retain data.
    Result Begin(std::uint32_t inputSize, Lzss::StreamSinkWrite downstream,
                 void *downstreamContext);
    Result Push(const std::uint8_t *data, std::uint32_t dataSize);
    Result Finish();

    // Adapter passed directly to PbgArchive::ReadDecompressEntryToSink.
    // Any filter/downstream error becomes a zero-byte/short sink write so the
    // archive transaction also fails. Query LastResult() for the exact cause.
    // Keep this signature in the engine's u32/u8 typedefs: on the PSP ABI,
    // std::uint32_t and u32 are both 32-bit but have different underlying C++
    // types, so only this exact form is callable as Lzss::StreamSinkWrite.
    static u32 ArchiveSinkWrite(void *context, const u8 *data, u32 dataSize);

    Result LastResult() const;
    bool IsEncrypted() const;
    int ProfileIndex() const;
    std::uint32_t InputSize() const;
    std::uint32_t OutputSize() const;
    std::uint32_t LogicalPayloadSize() const;

  private:
    ResourceDecryptStreamFilter(const ResourceDecryptStreamFilter &);
    ResourceDecryptStreamFilter &operator=(const ResourceDecryptStreamFilter &);

    void Reset();
    void RememberRawBytes(const std::uint8_t *data, std::uint32_t dataSize);
    bool DecideFromSignature();
    std::uint32_t CalculateDecryptBytes(std::uint32_t chunkSize,
                                        std::uint32_t maxBytes,
                                        std::uint32_t payloadSize) const;
    Result ProcessEncrypted(const std::uint8_t *data, std::uint32_t dataSize);
    Result TransformAndWriteChunk();
    std::uint32_t DestinationForSource(std::uint32_t source,
                                       std::uint32_t chunkSize) const;
    bool IsVisited(std::uint32_t index) const;
    void SetVisited(std::uint32_t index);
    Result WriteDownstream(const std::uint8_t *data, std::uint32_t dataSize);
    Result Fail(Result result);

    Lzss::StreamSinkWrite m_Downstream;
    void *m_DownstreamContext;
    Result m_Result;
    bool m_Active;
    bool m_Finished;
    bool m_DecisionMade;
    bool m_Encrypted;
    int m_ProfileIndex;
    std::uint32_t m_ExpectedInputSize;
    std::uint32_t m_InputReceived;
    std::uint32_t m_OutputProduced;
    std::uint32_t m_LogicalPayloadSize;
    std::uint32_t m_PayloadReceived;
    std::uint32_t m_DecryptBytes;
    std::uint32_t m_DecryptProcessed;
    std::uint32_t m_ProfileChunkSize;
    std::uint32_t m_ChunkTarget;
    std::uint32_t m_ChunkFill;
    std::uint8_t m_XorValue;
    std::uint8_t m_XorValueInc;
    std::uint8_t m_Header[kSignatureBytes];
    std::uint32_t m_HeaderSize;
    std::uint8_t m_LastRaw[kSignatureBytes];
    std::uint32_t m_LastRawCount;
    std::uint32_t m_LastRawNext;
    alignas(64) std::uint8_t m_Chunk[kLargestChunkBytes];
    std::uint8_t m_Visited[kVisitedBytes];
};

static_assert(sizeof(ResourceDecryptStreamFilter) <= 0x1800U,
              "resource decrypt filter must retain only one bounded chunk");
static_assert(std::is_same<decltype(&ResourceDecryptStreamFilter::ArchiveSinkWrite),
                           Lzss::StreamSinkWrite>::value,
              "archive adapter must exactly match LZSS sink ABI");

// Compatibility note: for an encrypted N-byte resource the current PSP
// in-place path reports N bytes after compacting away the four-byte signature.
// Bytes [0,N-4) are the decrypted logical payload and the final four bytes are
// the unchanged original tail left in the old buffer. This filter deliberately
// emits the same N-byte image for byte-exact rollout. Consumers must use
// LogicalPayloadSize() when parsing and ignore the compatibility tail. Removing
// it is a later, separately validated format change.
//
// Nothing is committed by this class. The downstream must point only at a
// private candidate; publish it only after archive success, Finish()==Result_Ok
// and the eventual ANM-router validation all succeed. On any failure discard
// the complete candidate. The source archive remains read-only.
} // namespace th08::psp
