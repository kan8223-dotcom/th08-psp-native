#include "psp/antitamper_checksum.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
struct ChecksumResult
{
    std::uint32_t sum;
    std::uint32_t value;
};

ChecksumResult Canonical(const std::uint8_t *address, std::size_t size,
                         std::uint32_t value, std::int32_t increment)
{
    ChecksumResult result{0U, value};
    for (std::size_t index = 0; index < size; ++index)
    {
        result.sum += address[index];
        result.value += static_cast<std::uint32_t>(increment);
    }
    return result;
}

ChecksumResult Candidate(const std::uint8_t *address, std::size_t size,
                         std::uint32_t value, std::int32_t increment)
{
    return {
        th08::psp::AntiTamperSwarByteSum(address, size),
        th08::psp::AntiTamperAdvanceValue(
            value, increment, static_cast<std::uint32_t>(size)),
    };
}

std::uint32_t Next(std::uint32_t *state)
{
    std::uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

bool Check(const std::uint8_t *address, std::size_t size,
           std::uint32_t value, std::int32_t increment,
           std::uint32_t sample)
{
    const ChecksumResult canonical =
        Canonical(address, size, value, increment);
    const ChecksumResult candidate =
        Candidate(address, size, value, increment);
    if (canonical.sum == candidate.sum &&
        canonical.value == candidate.value)
    {
        return true;
    }

    std::fprintf(stderr,
                 "antitamper-checksum: mismatch sample=%lu size=%zu "
                 "canonical_sum=%08lx candidate_sum=%08lx "
                 "canonical_value=%08lx candidate_value=%08lx\n",
                 static_cast<unsigned long>(sample), size,
                 static_cast<unsigned long>(canonical.sum),
                 static_cast<unsigned long>(candidate.sum),
                 static_cast<unsigned long>(canonical.value),
                 static_cast<unsigned long>(candidate.value));
    return false;
}

bool CheckRetailFiveBlockFixture()
{
    constexpr std::array<std::size_t, 5> sizes = {128U, 20U, 60U, 60U, 360U};
    constexpr std::size_t totalBytes = 628U;
    std::array<std::array<std::uint8_t, 364>, 5> storage{};
    std::uint32_t state = 0x88e1a2d3U;
    std::uint32_t canonicalSum = 0U;
    std::uint32_t candidateSum = 0U;
    std::uint32_t canonicalValue = 0xfffffff1U;
    const std::int32_t increment = std::numeric_limits<std::int32_t>::min();

    for (std::size_t block = 0; block < sizes.size(); ++block)
    {
        for (std::size_t index = 0; index < sizes[block]; ++index)
        {
            storage[block][index] = static_cast<std::uint8_t>(Next(&state));
        }
        const ChecksumResult canonical = Canonical(
            storage[block].data(), sizes[block], canonicalValue, increment);
        canonicalSum += canonical.sum;
        canonicalValue = canonical.value;
        candidateSum += th08::psp::AntiTamperSwarByteSum(
            storage[block].data(), sizes[block]);
    }

    const std::uint32_t candidateValue =
        th08::psp::AntiTamperAdvanceValue(
            0xfffffff1U, increment, static_cast<std::uint32_t>(totalBytes));
    if (canonicalSum != candidateSum || canonicalValue != candidateValue)
    {
        std::fprintf(stderr,
                     "antitamper-checksum: five-block fixture mismatch\n");
        return false;
    }

    std::array<std::uint8_t, totalBytes> maximum{};
    maximum.fill(0xffU);
    return th08::psp::AntiTamperSwarByteSum(maximum.data(), maximum.size()) ==
           160140U;
}
} // namespace

int main()
{
    std::uint32_t sample = 0U;
    std::array<std::uint8_t, 2060> storage{};
    const std::array<std::uint32_t, 5> initialValues = {
        0U, 1U, 0x7fffffffU, 0x80000000U, 0xffffffffU,
    };
    const std::array<std::int32_t, 6> increments = {
        0, 1, -1,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max(),
        static_cast<std::int32_t>(0x89abcdefU),
    };

    for (std::size_t index = 0; index < storage.size(); ++index)
    {
        storage[index] = static_cast<std::uint8_t>(index * 73U + 19U);
    }

    // Exhaust every input alignment and every word-tail length around the
    // boundaries at which the SWAR loop starts and stops.
    for (std::size_t alignment = 0; alignment < 4U; ++alignment)
    {
        for (std::size_t size = 0; size <= 1031U; ++size)
        {
            for (std::uint32_t value : initialValues)
            {
                for (std::int32_t increment : increments)
                {
                    if (!Check(storage.data() + alignment, size, value,
                               increment, sample++))
                    {
                        return 1;
                    }
                }
            }
        }
    }

    // Random raw increment bits exercise both signs and modulo-2^32 overflow.
    std::uint32_t state = 0xc001d00dU;
    for (std::uint32_t iteration = 0; iteration < 200000U; ++iteration)
    {
        const std::size_t alignment = Next(&state) & 3U;
        const std::size_t size = Next(&state) % 2049U;
        for (std::size_t index = 0; index < size; ++index)
        {
            storage[alignment + index] =
                static_cast<std::uint8_t>(Next(&state));
        }
        const std::uint32_t value = Next(&state);
        const std::int32_t increment = static_cast<std::int32_t>(Next(&state));
        if (!Check(storage.data() + alignment, size, value, increment,
                   sample++))
        {
            return 2;
        }
    }

    const std::array<std::uint8_t, 4> endianFixture = {
        0x01U, 0x02U, 0x80U, 0xffU,
    };
    if (th08::psp::AntiTamperSwarByteSum(
            endianFixture.data(), endianFixture.size()) != 386U)
    {
        std::fprintf(stderr, "antitamper-checksum: byte-lane fixture failed\n");
        return 3;
    }
    if (!CheckRetailFiveBlockFixture())
    {
        return 4;
    }

    std::printf("antitamper-checksum: PASS samples=%lu retail_bytes=628\n",
                static_cast<unsigned long>(sample));
    return 0;
}
