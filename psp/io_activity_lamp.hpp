#pragma once

#include <cstdint>

namespace th08::psp
{

enum class IoActivityKind : std::uint8_t
{
    Open,
    Read,
    Write,
    Seek,
    Metadata,
    Sync,
    Close,
    Directory,
};

enum class IoActivityLampState : std::uint8_t
{
    Off,
    Activity,
    Slow,
};

// The diagnostic is compiled only into an explicitly gated build.  At
// runtime it enables itself only on PSP Go (hardware model 4), whose ef0
// storage driver has shown occasional multi-second open latency.
void IoActivityLampInitialize();
bool IoActivityLampEnabled();
bool IoSerializeEnabled();
IoActivityLampState IoActivityLampQuery();

// Bracket one synchronous storage transaction.  The detail string is read
// only before the destructor returns, so callers may pass a stack-backed
// resolved path.  Ordinary accesses light amber briefly; a >=100 ms call is
// latched red long enough to remain visible after a main-thread stall.
class IoActivityScope
{
public:
    IoActivityScope(IoActivityKind kind, const char *detail = nullptr,
                    bool recordSlowLog = true, const void *address = nullptr);
    ~IoActivityScope();

    IoActivityScope(const IoActivityScope &) = delete;
    IoActivityScope &operator=(const IoActivityScope &) = delete;

private:
    IoActivityKind kind_;
    const char *detail_;
    std::uint32_t startedUs_;
    bool armed_;
    bool recordSlowLog_;
    bool locked_;
    const void *address_;
};

} // namespace th08::psp
