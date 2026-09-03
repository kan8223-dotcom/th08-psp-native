#include "psp/font_glyph_cache_policy.hpp"

#include <cstdio>
#include <initializer_list>

namespace
{
struct GlyphCacheModel
{
    const void *owner;
    int pointSize;
    int hardwareModel;
    int flushCount;
    int closeCount;
    bool retained;

    void Release(bool featureEnabled, const void *descriptorOwner,
                 int descriptorPointSize)
    {
        retained = th08::psp::ShouldRetainPspFontGlyphCache(
            featureEnabled, hardwareModel, owner, pointSize,
            descriptorOwner, descriptorPointSize);
        if (!retained)
            ++flushCount;
    }

    void Teardown()
    {
        if (owner != nullptr)
            ++closeCount;
        owner = nullptr;
        pointSize = 0;
        hardwareModel = -1;
        retained = false;
    }
};

bool Expect(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "font-glyph-cache-policy: %s\n", message);
    return condition;
}
} // namespace

int main()
{
    int faceA = 0;
    int faceB = 0;

    GlyphCacheModel disabled = {&faceA, 24, 2, 0, 0, false};
    disabled.Release(false, &faceA, 24);
    if (!Expect(disabled.flushCount == 1 && !disabled.retained,
                "default-off did not flush"))
        return 1;

    for (const int slimPlusModel : {1, 2, 4, 10})
    {
        GlyphCacheModel same = {&faceA, 24, slimPlusModel, 0, 0, false};
        same.Release(true, &faceA, 24);
        if (!Expect(same.flushCount == 0 && same.retained,
                    "same owner/size did not retain on Slim+"))
            return 2;
    }

    GlyphCacheModel sizeChange = {&faceA, 24, 2, 0, 0, false};
    sizeChange.Release(true, &faceA, 22);
    if (!Expect(sizeChange.flushCount == 1 && !sizeChange.retained,
                "size change did not flush"))
        return 3;

    GlyphCacheModel ownerChange = {&faceA, 24, 2, 0, 0, false};
    ownerChange.Release(true, &faceB, 24);
    if (!Expect(ownerChange.flushCount == 1 && !ownerChange.retained,
                "owner change did not flush"))
        return 4;

    GlyphCacheModel psp1000 = {&faceA, 24, 0, 0, 0, false};
    psp1000.Release(true, &faceA, 24);
    if (!Expect(psp1000.flushCount == 1 && !psp1000.retained,
                "PSP-1000 did not keep the established flush"))
        return 5;

    GlyphCacheModel queryFailure = {&faceA, 24, -1, 0, 0, false};
    queryFailure.Release(true, &faceA, 24);
    if (!Expect(queryFailure.flushCount == 1 && !queryFailure.retained,
                "failed model query did not fail safe"))
        return 6;

    GlyphCacheModel teardown = {&faceA, 24, 4, 0, 0, false};
    teardown.Release(true, &faceA, 24);
    teardown.Teardown();
    if (!Expect(teardown.closeCount == 1 && teardown.owner == nullptr &&
                    teardown.pointSize == 0 && !teardown.retained,
                "teardown retained a face/cache owner"))
        return 7;

    std::puts("font-glyph-cache-policy: PASS");
    return 0;
}
