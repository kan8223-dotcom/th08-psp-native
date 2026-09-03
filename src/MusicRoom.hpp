#pragma once

#include "AnmManager.hpp"
#include "Global.hpp"
#include "ZunResult.hpp"
#include "diffbuild.hpp"
#include "inttypes.hpp"

namespace th08
{

#if defined(TH08_MODERN_PORT)
// Stock TH08 1.00d contains one 68-byte CP932 music-comment row.  The
// original 66-byte row cannot hold that text and its terminating NUL, so the
// portable runtime uses the smallest lossless capacity.  Keep the original
// layout for the VC7 reconstruction target.
enum
{
    MUSIC_ROOM_DESCRIPTION_CAPACITY = 69
};
#else
enum
{
    MUSIC_ROOM_DESCRIPTION_CAPACITY = 66
};
#endif

struct TrackDescriptor
{
    char path[64];
    char title[66];
    char descriptors[8][MUSIC_ROOM_DESCRIPTION_CAPACITY];

    TrackDescriptor()
    {
        memset(this, 0, sizeof(TrackDescriptor));
    }
};

#if defined(TH08_MODERN_PORT)
C_ASSERT(sizeof(TrackDescriptor) == 0x2aa);
#else
C_ASSERT(sizeof(TrackDescriptor) == 0x292);
#endif

struct MusicRoom
{
    MusicRoom()
    {
        memset(this, 0, sizeof(MusicRoom));
    }

    ZunResult CheckInputEnable();
    i32 ProcessInput();

    static ZunResult RegisterChain();

    static ChainCallbackResult OnUpdate(MusicRoom *musicRoom);
    static ChainCallbackResult OnDraw(MusicRoom *musicRoom);
    static ZunResult AddedCallback(MusicRoom *musicRoom);
    static ZunResult DeletedCallback(MusicRoom *musicRoom);

    ChainElem *calcChain;
    ChainElem *drawChain;

    AnmLoaded *musicAnm;

    ZunBool bgmUnlocked[24];

    i32 frameCount;
    i32 inputState;
    i32 cursor;
    i32 selectedSongIndex;
    i32 listingOffset;

    i32 numDescriptors;
    TrackDescriptor *trackDescriptors;

    AnmVm mainVms[1];
    AnmVm songNameVms[31];
    AnmVm descriptionVms[8];
};

C_ASSERT(sizeof(MusicRoom) == 0x6a28);

} // namespace th08
