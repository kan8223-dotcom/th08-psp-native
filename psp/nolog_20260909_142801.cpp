#include "fileio.hpp"
#include <pspiofilemgr.h>
#include <cstring>

#if !TH08_PSP_LOGGING
// Keep the proven PSPGL archive unchanged. Its normal cross-object diagnostic
// calls are discarded without formatting. Assertions retain their caller-side
// behavior; no error handling or rendering operation is bypassed.
extern "C" void __wrap___pspgl_log(const char *, ...) {}

extern "C" SceUID __real_sceIoOpen(const char *, int, SceMode);
extern "C" SceUID __wrap_sceIoOpen(const char *path, int flags, SceMode mode)
{
    // --wrap alone cannot redirect calls to a logger defined in the same
    // pspgl_misc.o (notably the assert reporter). Deny only its two exact,
    // hard-coded diagnostic destinations. All other opens, modes, errors,
    // saves, data reads and ME helper extraction pass through unchanged.
    if (path != nullptr &&
        (std::strcmp(path, "ms0:/log.txt") == 0 ||
         std::strcmp(path, "ms0:/pspgl.ge") == 0))
    {
        return -1;
    }
    return __real_sceIoOpen(path, flags, mode);
}
#endif
