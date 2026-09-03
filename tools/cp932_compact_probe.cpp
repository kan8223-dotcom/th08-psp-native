#include "src/modern/linux/cp932_compact.generated.hpp"

#include <stdint.h>
#include <stdio.h>

namespace
{

bool WriteResult(const th08::cp932::DecodeResult &result)
{
    const uint32_t codepoint = result.codepoint;
    const uint8_t record[8] = {
        static_cast<uint8_t>(result.status),
        result.consumed,
        0u,
        0u,
        static_cast<uint8_t>(codepoint & 0xFFu),
        static_cast<uint8_t>((codepoint >> 8) & 0xFFu),
        static_cast<uint8_t>((codepoint >> 16) & 0xFFu),
        static_cast<uint8_t>((codepoint >> 24) & 0xFFu),
    };
    return fwrite(record, sizeof(record), 1u, stdout) == 1u;
}

} // namespace

int main()
{
    uint8_t input[2] = {0u, 0u};
    for (unsigned int first = 0u; first <= 0xFFu; ++first)
    {
        input[0] = static_cast<uint8_t>(first);
        if (!WriteResult(th08::cp932::DecodeOne(input, 1u)))
            return 2;
    }
    for (unsigned int encoded = 0u; encoded <= 0xFFFFu; ++encoded)
    {
        input[0] = static_cast<uint8_t>(encoded >> 8);
        input[1] = static_cast<uint8_t>(encoded & 0xFFu);
        if (!WriteResult(th08::cp932::DecodeOne(input, 2u)))
            return 2;
    }
    return fflush(stdout) == 0 ? 0 : 2;
}
