#include "th_pch.h"

#include "Global.hpp"
#include "EnemyManager.hpp"
#include "Gui.hpp"
#include "ReplayManager.hpp"
#include "ReplaySyncAudit.hpp"
#include "ResultScreen.hpp"
#include "i18n.hpp"

#include "pbg/Lzss.hpp"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

namespace th08
{

DIFFABLE_STATIC(ReplayManager *, g_ReplayManager);

namespace
{
const char *g_ReplayDifficultyList[] = {"Easy", "Normal", "Hard", "Lunatic", "Extra"};

struct ReplayUserDataHeader
{
    u32 magic;
    i32 size;
    u8 reserved;
    u8 padding[3];
};

C_ASSERT(sizeof(ReplayUserDataHeader) == 0xc);

char *AppendFormat(char *buffer, const char *format, ...);

#if defined(PSP)
// The PC executable gives both streams 0xd2f00 bytes.  Input really can use
// that full logical capacity; FPS samples cannot.  One FPS byte is advanced
// per 30 input frames while two adjacent bytes are written, so 0x4000 bytes
// covers all 431,982 input records representable by the original input block.
constexpr size_t kReplayInputCapacity = 0xd2f00U;
constexpr size_t kReplayFpsCapacity = 0x4000U;
constexpr size_t kReplaySaveFinalizationHeadroom = sizeof(u16);
constexpr size_t kReplayInputHeaderBytes = offsetof(StageReplayData, inputStream);
constexpr size_t kReplayInputRecordBytes = sizeof(u16);
constexpr size_t kReplayMaxInputRecords =
    (kReplayInputCapacity - kReplayInputHeaderBytes) / kReplayInputRecordBytes;
constexpr size_t kReplayRequiredFpsBytes = (kReplayMaxInputRecords + 29U) / 30U + 1U;
constexpr size_t kReplayMaxSerializedPayload =
    (TH08_REPLAY_DATA_SIZE - TH08_REPLAY_HEADER_SIZE) +
    MAX_STAGES *
        (kReplayInputCapacity + kReplaySaveFinalizationHeadroom + kReplayFpsCapacity);
static_assert(kReplayRequiredFpsBytes <= kReplayFpsCapacity,
              "PSP FPS replay buffer must cover the original input capacity");

StageReplayData *gPreparedReplayInput = NULL;
u8 *gPreparedReplayFps = NULL;
size_t gReplayInputCapacities[MAX_STAGES]{};
size_t gReplayFpsCapacities[MAX_STAGES]{};

void ResetReplayStageMemoryState(i32 stage)
{
    if (stage < 0 || stage >= MAX_STAGES)
        return;
    gReplayInputCapacities[stage] = 0;
    gReplayFpsCapacities[stage] = 0;
}

void ReleaseRecordedStageBuffers(ReplayData *replayData, i32 stage)
{
    if (replayData == NULL || stage < 0 || stage >= MAX_STAGES)
        return;
    if (TH08_REPLAY_STAGE_DATA(replayData, stage) != NULL)
    {
        g_ZunMemory.Free(TH08_REPLAY_STAGE_DATA(replayData, stage));
        TH08_REPLAY_STAGE_DATA(replayData, stage) = NULL;
    }
    if (TH08_REPLAY_FPS_DATA(replayData, stage) != NULL)
    {
        g_ZunMemory.Free(TH08_REPLAY_FPS_DATA(replayData, stage));
        TH08_REPLAY_FPS_DATA(replayData, stage) = NULL;
    }
    ResetReplayStageMemoryState(stage);
}
#endif
} // namespace

#pragma var_order(decodedReplay, i, replayData, obfuscateOffset, obfuscateCursor, checksum, checksumCursor)
ReplayData *ReplayManager::LoadReplayData(ReplayData *data, int fileSize)
{
    u8 *obfuscateCursor;
    u8 obfuscateOffset;
    u8 *checksumCursor;
    u32 checksum;
    i32 i;
    ReplayData *decodedReplay;
    ReplayData *replayData = data;

    if (replayData == NULL)
    {
        goto err1;
    }

    if (replayData->header.magic != *(u32 *)REPLAY_MAGIC)
    {
        goto err1;
    }

    if (replayData->header.version != REPLAY_VERSION)
    {
        goto err1;
    }

#ifdef TH08_PORTABLE_NATIVE_LAYOUT
    if (fileSize < TH08_REPLAY_HEADER_SIZE ||
        replayData->header.fileSize < TH08_REPLAY_HEADER_SIZE ||
        replayData->header.fileSize > fileSize)
    {
        goto err1;
    }
#endif

    obfuscateCursor = (u8 *)&replayData->header.compressedSize;
    obfuscateOffset = replayData->header.obfuscationKey;

    for (i = 0; i < replayData->header.fileSize - (i32)offsetof(ReplayDataHeader, compressedSize);
         i++, obfuscateCursor++)
    {
        *obfuscateCursor -= obfuscateOffset;
        obfuscateOffset += 7;
    }

    checksumCursor = &replayData->header.obfuscationKey;
    checksum = REPLAY_OBFUSCATION_VALUE;

    for (i = 0; i < replayData->header.fileSize - (i32)offsetof(ReplayDataHeader, obfuscationKey);
         i++, checksumCursor++)
    {
        checksum += *checksumCursor;
    }

    if (checksum != replayData->header.checksum)
    {
        goto err1;
    }

#ifdef TH08_PORTABLE_NATIVE_LAYOUT
    if (replayData->header.compressedSize < 0 ||
        replayData->header.decompressedSize < TH08_REPLAY_DATA_SIZE - TH08_REPLAY_HEADER_SIZE ||
        replayData->header.compressedSize != replayData->header.fileSize - TH08_REPLAY_HEADER_SIZE)
    {
        goto err1;
    }

    {
        const i32 runtimeLayoutGrowth = sizeof(ReplayData) - TH08_REPLAY_DATA_SIZE;
        const i32 stagePayloadSize =
            replayData->header.decompressedSize - (TH08_REPLAY_DATA_SIZE - TH08_REPLAY_HEADER_SIZE);
        const i32 trailingDataSize = fileSize - replayData->header.fileSize;
        const u32 wireDecodedSize = TH08_REPLAY_HEADER_SIZE + replayData->header.decompressedSize;

        decodedReplay = (ReplayData *)g_ZunMemory.Alloc(
            sizeof(ReplayData) + stagePayloadSize + trailingDataSize);
        if (decodedReplay == NULL)
        {
            goto err1;
        }

        memcpy(&decodedReplay->header, data, TH08_REPLAY_HEADER_SIZE);
        Lzss::Decode((u8 *)replayData + TH08_REPLAY_HEADER_SIZE,
                     replayData->header.compressedSize,
                     (u8 *)decodedReplay + TH08_REPLAY_HEADER_SIZE,
                     replayData->header.decompressedSize);

        memmove((u8 *)decodedReplay + sizeof(ReplayData),
                (u8 *)decodedReplay + TH08_REPLAY_DATA_SIZE, stagePayloadSize);
        memcpy((u8 *)decodedReplay + sizeof(ReplayData) + stagePayloadSize,
               (u8 *)data + replayData->header.fileSize, trailingDataSize);

        for (i = 0; i < MAX_STAGES; i++)
        {
            const u32 stageOffset = decodedReplay->header.stageReplayDataOffsets[i];
            const u32 fpsOffset = decodedReplay->header.stageFpsDataOffsets[i];

            if (stageOffset != 0)
            {
                if (stageOffset < TH08_REPLAY_DATA_SIZE ||
                    stageOffset > wireDecodedSize - sizeof(StageReplayData))
                {
                    goto err2;
                }
                decodedReplay->runtimeStageReplayData[i] =
                    reinterpret_cast<StageReplayData *>(reinterpret_cast<u8 *>(decodedReplay) +
                                                        stageOffset + runtimeLayoutGrowth);
            }
            else
            {
                decodedReplay->runtimeStageReplayData[i] = NULL;
            }

            if (fpsOffset != 0)
            {
                if (fpsOffset < TH08_REPLAY_DATA_SIZE || fpsOffset >= wireDecodedSize)
                {
                    goto err2;
                }
                decodedReplay->runtimeStageFpsData[i] =
                    reinterpret_cast<u8 *>(decodedReplay) + fpsOffset + runtimeLayoutGrowth;
            }
            else
            {
                decodedReplay->runtimeStageFpsData[i] = NULL;
            }
        }
    }
#else
    decodedReplay = (ReplayData *)g_ZunMemory.Alloc(replayData->header.decompressedSize + sizeof(ReplayDataHeader) +
                                                    (fileSize - replayData->header.fileSize));

    memcpy(&decodedReplay->header, data, sizeof(ReplayDataHeader));

    Lzss::Decode((u8 *)replayData + sizeof(ReplayDataHeader), replayData->header.compressedSize,
                 (u8 *)decodedReplay + sizeof(ReplayDataHeader), replayData->header.decompressedSize);

    memcpy((u8 *)decodedReplay + sizeof(ReplayDataHeader) + replayData->header.decompressedSize,
           (u8 *)data + replayData->header.fileSize, fileSize - replayData->header.fileSize);
#endif

    replayData = decodedReplay;

    if (replayData->gameConfiguration.slowMode != 0)
    {
        goto err2;
    }

    if (g_Supervisor.CheckVersion(replayData->exeVersion, replayData->exeSize, replayData->exeChecksum) != ZUN_SUCCESS)
    {
        goto err2;
    }

    g_ZunMemory.Free(data);

    return decodedReplay;

err1:
    g_ZunMemory.Free(data);
    return NULL;

err2:
    g_ZunMemory.Free(data);
    g_ZunMemory.Free(decodedReplay);

    return NULL;
}

ZunResult ReplayManager::RegisterChain(i32 replayMode, const char *replayPath)
{
    ReplayManager *replayManager;

    g_GuiMessageInputPrevious = 0;
    g_GuiMessageInputCurrent = 0;

    if (g_ReplayManager == NULL)
    {
        replayManager = ZUN_NEW(ReplayManager, "ReplayInf");
        g_ReplayManager = replayManager;
        memset(replayManager, 0, sizeof(ReplayManager));
        replayManager->replayData = NULL;
        replayManager->isDemo = replayMode;
        replayManager->replayPath = replayPath;

        switch (replayMode)
        {
        case REPLAY_MANAGER_RECORD:
            replayManager->calcChain = g_Chain.CreateElem((ChainCallback)RecordInputAndFps);
            replayManager->calcChain->addedCallback = (ChainLifetimeCallback)BeginRecordingStage;
            replayManager->calcChain->deletedCallback = (ChainLifetimeCallback)DeleteReplayManager;
            replayManager->calcChain->arg = replayManager;

            if (g_Chain.AddToCalcChain(replayManager->calcChain, CHAIN_PRIO_CALC_REPLAYMANAGER_RECORD_HIGH_PRIO))
            {
#if defined(PSP)
                // AddToCalcChain links the element even when its added
                // callback reports failure.  Tear the partial recorder down
                // now so setup can leave through one clean error path.
                g_Chain.Cut(replayManager->calcChain);
#endif
                return ZUN_ERROR;
            }

            replayManager->playbackFrameControlChain = NULL;
            replayManager->frameSyncChain = g_Chain.CreateElem((ChainCallback)CaptureFrameSyncState);
            replayManager->frameSyncChain->arg = replayManager;
            g_Chain.AddToCalcChain(replayManager->frameSyncChain, CHAIN_PRIO_CALC_REPLAYMANAGER_LOW_PRIO);
            CaptureFrameSyncState(replayManager);
            break;

        case REPLAY_MANAGER_PLAYBACK:
            replayManager->calcChain = g_Chain.CreateElem((ChainCallback)PlaybackInputAndFps);
            replayManager->calcChain->addedCallback = (ChainLifetimeCallback)BeginPlaybackStage;
            replayManager->calcChain->deletedCallback = (ChainLifetimeCallback)DeleteReplayManager;
            replayManager->calcChain->arg = replayManager;

            if (g_Chain.AddToCalcChain(replayManager->calcChain, CHAIN_PRIO_CALC_REPLAYMANAGER_PLAYBACK_HIGH_PRIO))
            {
#if defined(PSP)
                // Match the recorder failure path. AddToCalcChain publishes
                // the element even when BeginPlaybackStage reports an error;
                // cutting it here invokes DeleteReplayManager and prevents a
                // half-initialized playback owner from surviving setup.
                g_Chain.Cut(replayManager->calcChain);
#endif
                return ZUN_ERROR;
            }

            if (replayManager->replayData->header.usesExtendedInputRecords != 0)
            {
                replayManager->calcChain->callback = (ChainCallback)PlaybackExtendedInputAndFps;
            }

            replayManager->playbackFrameControlChain =
                g_Chain.CreateElem((ChainCallback)ControlPlaybackFrameAdvance);
            replayManager->playbackFrameControlChain->arg = replayManager;
            g_Chain.AddToCalcChain(
                replayManager->playbackFrameControlChain, CHAIN_PRIO_CALC_REPLAYMANAGER_SKIP_FRAMES);

            replayManager->frameSyncChain = NULL;
            if (replayManager->replayData->header.usesExtendedInputRecords != 0)
            {
                replayManager->frameSyncChain = g_Chain.CreateElem((ChainCallback)CaptureFrameSyncState);
                replayManager->frameSyncChain->arg = replayManager;
                g_Chain.AddToCalcChain(replayManager->frameSyncChain, CHAIN_PRIO_CALC_REPLAYMANAGER_LOW_PRIO);
                CaptureFrameSyncState(replayManager);
            }
            break;
        }
    }
    else
    {
        switch (replayMode)
        {
        case REPLAY_MANAGER_RECORD:
            return BeginRecordingStage(g_ReplayManager);
        case REPLAY_MANAGER_PLAYBACK:
            return BeginPlaybackStage(g_ReplayManager);
        }
    }

    return ZUN_SUCCESS;
}

ChainCallbackResult ReplayManager::CaptureFrameSyncState(ReplayManager *replayManager)
{
    replayManager->frameEventFlags = 0;
    replayManager->frameRngSeed = g_Rng.GetSeed();
    g_Rng.ResetGenerationCount();

    if (g_GameManager.replayPauseRecorded != 0)
    {
        replayManager->frameEventFlags |= 0x100;
    }

    g_GameManager.replayPauseRecorded = 0;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

#pragma var_order(stage, input)
ChainCallbackResult ReplayManager::RecordInputAndFps(ReplayManager *replayManager)
{
    i32 stage;
    u16 input;

    if (!g_GameManager.flags.replayInputEnabled)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    g_GuiMessageInputPrevious = g_GuiMessageInputCurrent;
    g_GuiMessageInputCurrent = g_CurFrameInput;

    if (g_GameManager.cfg->slowMode != 0)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_Supervisor.IsSpeedhackDetected())
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_GameManager.flags.stageClearSequenceActive)
    {
        if (replayManager->inputDelay >= 3)
        {
            return CHAIN_CALLBACK_RESULT_CONTINUE;
        }

        replayManager->inputDelay++;
    }

    stage = g_GameManager.stageAtStart;
    input = g_CurFrameInput;
    g_GuiMessageInputCurrent = input;

    replayManager->replayInputCursor += sizeof(u16);
    replayManager->replayInputEnds[stage] = replayManager->replayInputCursor + sizeof(u16);
    *(u16 *)replayManager->replayInputCursor = input;

    if (replayManager->frameCounter % 30 == 0)
    {
        replayManager->replayFpsSampleCursor[0] =
            (u8)g_Supervisor.recordedFps | (g_Supervisor.recordingFpsWarning != 0 ? 0x80 : 0);
        replayManager->replayFpsSampleCursor[1] = (u8)g_Supervisor.recordedFps;
        replayManager->replayFpsSampleEnds[stage] = replayManager->replayFpsSampleCursor + 2;
        replayManager->replayFpsSampleCursor++;
    }

    replayManager->frameCounter++;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::ControlPlaybackFrameAdvance(ReplayManager *replayManager)
{
#if defined(TH08_REPLAY_SYNC_AUDIT)
    ReplaySyncAudit::EndFrame();
#endif
    if (!g_GameManager.flags.replayInputEnabled)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_GameManager.flags.stageClearSequenceActive)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_Gui.IsDialoguePresent() && g_Gui.IsDialogueSkippable() && replayManager->frameCounter % 3 != 2)
    {
        return CHAIN_CALLBACK_RESULT_RESTART_FROM_FIRST_JOB;
    }

    if (!g_GameManager.flags.stageClearSequenceActive && g_GameManager.replayMode == 2 && !g_EnemyManager.HasBoss() &&
        replayManager->frameCounter % 5 != 4)
    {
        return CHAIN_CALLBACK_RESULT_RESTART_FROM_FIRST_JOB;
    }

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::PlaybackInputAndFps(ReplayManager *replayManager)
{
    i32 unused;

    if (!g_GameManager.flags.replayInputEnabled)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_GameManager.cfg->slowMode != 0)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_GameManager.flags.stageClearSequenceActive)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    unused = 0;
    g_GuiMessageInputPrevious = g_GuiMessageInputCurrent;
    g_GuiMessageInputCurrent = *(u16 *)replayManager->replayInputCursor;
    replayManager->replayInputCursor += sizeof(u16);
#if defined(TH08_REPLAY_SYNC_AUDIT)
    ReplaySyncAudit::BeginFrame(static_cast<u32>(replayManager->frameCounter),
                                g_GuiMessageInputCurrent,
                                ReplaySyncAudit::INPUT_RECORD_NORMAL);
#endif

    g_IsEighthFrameOfHeldInput = 0;
    if (g_GuiMessageInputPrevious == g_GuiMessageInputCurrent)
    {
        if (g_NumOfFramesInputsWereHeld >= 0x1e)
        {
            if (g_NumOfFramesInputsWereHeld % 8 == 0)
            {
                g_IsEighthFrameOfHeldInput = 1;
            }
            if (0x26 <= g_NumOfFramesInputsWereHeld)
            {
                g_NumOfFramesInputsWereHeld = 0x1e;
            }
        }

        g_NumOfFramesInputsWereHeld++;
    }
    else
    {
        g_NumOfFramesInputsWereHeld = 0;
    }

    if (replayManager->frameCounter % 30 == 0)
    {
        g_Supervisor.recordedFps = (i8)replayManager->replayFpsSampleCursor[1] & 0x7f;
        g_Supervisor.playbackFpsWarning = (i8)replayManager->replayFpsSampleCursor[1] >> 7;
        replayManager->replayFpsSampleCursor++;
    }

    replayManager->frameCounter++;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::PlaybackExtendedInputAndFps(ReplayManager *replayManager)
{
    i32 unused;

    if (!g_GameManager.flags.replayInputEnabled)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_GameManager.cfg->slowMode != 0)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_GameManager.flags.stageClearSequenceActive)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    unused = 0;
    g_GuiMessageInputPrevious = g_GuiMessageInputCurrent;
    g_GuiMessageInputCurrent = replayManager->extendedInputCursor->input;
    replayManager->extendedInputCursor++;
#if defined(TH08_REPLAY_SYNC_AUDIT)
    ReplaySyncAudit::BeginFrame(static_cast<u32>(replayManager->frameCounter),
                                g_GuiMessageInputCurrent,
                                ReplaySyncAudit::INPUT_RECORD_EXTENDED);
#endif

    g_IsEighthFrameOfHeldInput = 0;
    if (g_GuiMessageInputPrevious == g_GuiMessageInputCurrent)
    {
        if (g_NumOfFramesInputsWereHeld >= 0x1e)
        {
            if (g_NumOfFramesInputsWereHeld % 8 == 0)
            {
                g_IsEighthFrameOfHeldInput = 1;
            }
            if (0x26 <= g_NumOfFramesInputsWereHeld)
            {
                g_NumOfFramesInputsWereHeld = 0x1e;
            }
        }

        g_NumOfFramesInputsWereHeld++;
    }
    else
    {
        g_NumOfFramesInputsWereHeld = 0;
    }

    if (replayManager->frameCounter % 30 == 0)
    {
        g_Supervisor.recordedFps = (i8)replayManager->replayFpsSampleCursor[1] & 0x7f;
        g_Supervisor.playbackFpsWarning = (i8)replayManager->replayFpsSampleCursor[1] >> 7;
        replayManager->replayFpsSampleCursor++;
    }

    replayManager->frameCounter++;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

#if defined(PSP)
ZunResult ReplayManager::PrepareRecordingStageBuffers()
{
    if (g_GameManager.flags.isReplay)
        return ZUN_SUCCESS;
    if (gPreparedReplayInput != NULL && gPreparedReplayFps != NULL)
        return ZUN_SUCCESS;

    ReleasePreparedRecordingStageBuffers();
    StageReplayData *input = static_cast<StageReplayData *>(
        g_ZunMemory.Alloc(kReplayInputCapacity, "PSP replay input reserve"));
    u8 *fps = static_cast<u8 *>(
        g_ZunMemory.Alloc(kReplayFpsCapacity, "PSP replay FPS reserve"));
    if (input == NULL || fps == NULL)
    {
        g_ZunMemory.Free(input);
        g_ZunMemory.Free(fps);
        return ZUN_ERROR;
    }

    gPreparedReplayInput = input;
    gPreparedReplayFps = fps;
    return ZUN_SUCCESS;
}

void ReplayManager::ReleasePreparedRecordingStageBuffers()
{
    g_ZunMemory.Free(gPreparedReplayInput);
    g_ZunMemory.Free(gPreparedReplayFps);
    gPreparedReplayInput = NULL;
    gPreparedReplayFps = NULL;
}
#endif

#pragma var_order(stageData, stage, stageFpsData, previousStage)
ZunResult ReplayManager::BeginRecordingStage(ReplayManager *replayManager)
{
    StageReplayData *previousStage;
    u8 *stageFpsData;
    i32 stage;
    StageReplayData *stageData;

    replayManager->frameCounter = 0;
    replayManager->replayFileData = NULL;

    if (replayManager->replayData == NULL)
    {
        replayManager->replayData = (ReplayData *)g_ZunMemory.AddToRegistry(
            new ReplayData, sizeof(ReplayData), "ReplayDataInf");
        memset(replayManager->replayData, 0, sizeof(ReplayData));

        replayManager->replayData->header.magic = *(u32 *)REPLAY_MAGIC;
        replayManager->replayData->header.hasUserDataSection = 0;
        replayManager->replayData->shotType = g_GameManager.shotType;
        replayManager->replayData->header.version = REPLAY_VERSION;
        replayManager->replayData->header.usesExtendedInputRecords = 0;
        replayManager->replayData->majorVersion = 0x100;
        replayManager->replayData->minorVersion = 100;
        strcpy(replayManager->replayData->exeVersion, "0100d");
        replayManager->replayData->exeSize = g_Supervisor.exeSize;
        replayManager->replayData->exeChecksum = g_Supervisor.exeChecksum;
        replayManager->replayData->isPractice = (u8)g_GameManager.IsPracticeMode();
        replayManager->replayData->spellcardNumber =
            g_GameManager.flags.isSpellPractice ? g_GameManager.currentSpellCardNumber : -1;

        replayManager->replayData->clearState =
            g_GameManager.IsStageClearedWithoutRetries(7, g_GameManager.shotType, 0) ||
                    g_GameManager.IsStageClearedWithoutRetries(7, g_GameManager.shotType, 1) ||
                    g_GameManager.IsStageClearedWithoutRetries(7, g_GameManager.shotType, 2) ||
                    g_GameManager.IsStageClearedWithoutRetries(7, g_GameManager.shotType, 3) ||
                    g_GameManager.shotType > 3
                ? 2
                : (g_GameManager.IsStageClearedWithoutRetries(6, g_GameManager.shotType, 0) ||
                   g_GameManager.IsStageClearedWithoutRetries(6, g_GameManager.shotType, 1) ||
                   g_GameManager.IsStageClearedWithoutRetries(6, g_GameManager.shotType, 2) ||
                   g_GameManager.IsStageClearedWithoutRetries(6, g_GameManager.shotType, 3));

        replayManager->replayData->difficulty = (u8)g_GameManager.difficulty;
        memcpy(replayManager->replayData->playerName, "NO NAME", 4);
        replayManager->replayData->gameConfiguration = *g_GameManager.cfg;

        for (stage = 0; stage < MAX_STAGES; stage++)
        {
            TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) = NULL;
            TH08_REPLAY_FPS_DATA(replayManager->replayData, stage) = NULL;
#if defined(PSP)
            ResetReplayStageMemoryState(stage);
#endif
        }
    }
    else
    {
        previousStage = NULL;
        for (stage = 0; stage < g_GameManager.currentStage; stage++)
        {
            if (TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) != NULL)
            {
                previousStage = TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage);
            }
        }

        if (previousStage != NULL)
        {
            previousStage->score = g_GameManager.globals->score;
        }
    }

    stage = g_GameManager.currentStage;
#if defined(PSP)
    if (stage < 0 || stage >= MAX_STAGES)
    {
        ReleasePreparedRecordingStageBuffers();
        return ZUN_ERROR;
    }
    ReleaseRecordedStageBuffers(replayManager->replayData, stage);

    stageData = gPreparedReplayInput;
    stageFpsData = gPreparedReplayFps;
    gPreparedReplayInput = NULL;
    gPreparedReplayFps = NULL;
    if (stageData == NULL || stageFpsData == NULL)
    {
        g_ZunMemory.Free(stageData);
        g_ZunMemory.Free(stageFpsData);
        stageData = static_cast<StageReplayData *>(
            g_ZunMemory.Alloc(kReplayInputCapacity, "PSP replay input fallback"));
        stageFpsData = static_cast<u8 *>(
            g_ZunMemory.Alloc(kReplayFpsCapacity, "PSP replay FPS fallback"));
    }
    if (stageData == NULL || stageFpsData == NULL)
    {
        g_ZunMemory.Free(stageData);
        g_ZunMemory.Free(stageFpsData);
        ResetReplayStageMemoryState(stage);
        return ZUN_ERROR;
    }

    TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) = stageData;
    TH08_REPLAY_FPS_DATA(replayManager->replayData, stage) = stageFpsData;
    gReplayInputCapacities[stage] = kReplayInputCapacity;
    gReplayFpsCapacities[stage] = kReplayFpsCapacity;
#else
    if (TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) != NULL)
    {
        g_ZunMemory.Free(TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage));
    }
    if (TH08_REPLAY_FPS_DATA(replayManager->replayData, stage) != NULL)
    {
        g_ZunMemory.Free(TH08_REPLAY_FPS_DATA(replayManager->replayData, stage));
    }

    TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) =
        (StageReplayData *)g_ZunMemory.Alloc(0xd2f00, "rep data");
    TH08_REPLAY_FPS_DATA(replayManager->replayData, stage) =
        (u8 *)g_ZunMemory.Alloc(0xd2f00, "rep data");

    stageData = TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage);
    stageFpsData = TH08_REPLAY_FPS_DATA(replayManager->replayData, stage);
#endif

    if (stageData == NULL || stageFpsData == NULL)
        return ZUN_ERROR;

    stageData->graze = g_GameManager.globals->graze;
    stageData->bombs = (u8)g_GameManager.GetBombsRemaining();
    stageData->lives = (u8)g_GameManager.GetLives();
    stageData->power = (u8)g_GameManager.GetPower();
    stageData->rank = (u8)g_GameManager.rank;
    stageData->pointItemsCollected = g_GameManager.globals->pointItemsCollected;
    stageData->rngSeed = g_GameManager.stageRngSeed;
    stageData->character = g_GameManager.character;
    stageData->spellcardsCaptured = (u8)g_GameManager.globals->spellcardsCaptured;
    stageData->pointItemExtends = g_GameManager.globals->pointItemExtendsSoFar;
    stageData->nextPointItemExtendThreshold = g_GameManager.globals->nextPointItemExtendThreshold;
    stageData->youkaiGauge = g_GameManager.globals->youkaiGauge;
    stageData->clockTime = g_GameManager.GetClockTime();
    stageData->pointItemValue = g_GameManager.globals->pointItemValue;

    replayManager->replayInputCursor = stageData->inputStream;
    replayManager->extendedInputCursor = reinterpret_cast<ReplayInputSync *>(replayManager->replayInputCursor);
    replayManager->replayFpsSampleCursor = stageFpsData;
#if defined(PSP)
    // A very short aborted stage may have no 30-frame FPS sample.  Give both
    // streams a valid empty end instead of inheriting a stale stage pointer.
    replayManager->replayInputEnds[stage] = replayManager->replayInputCursor;
    replayManager->replayFpsSampleEnds[stage] = replayManager->replayFpsSampleCursor;
#endif
    *reinterpret_cast<u16 *>(replayManager->replayInputCursor) = 0;
    replayManager->extendedInputCursor->eventFlags = 0;
    replayManager->extendedInputCursor->rngSeed = g_Rng.GetSeed();
    replayManager->inputDelay = 0;
    replayManager->stageResetWord = 0;

    return ZUN_SUCCESS;
}

#pragma var_order(stageData, stage, fileSize, stageFpsData, previousStage)
ZunResult ReplayManager::BeginPlaybackStage(ReplayManager *replayManager)
{
    StageReplayData *previousStage;
    u8 *stageFpsData;
    i32 fileSize;
    i32 stage;
    StageReplayData *stageData;

    replayManager->frameCounter = 0;

    if (replayManager->replayData == NULL)
    {
        replayManager->replayData = (ReplayData *)FileSystem::OpenFile(
            replayManager->replayPath, &fileSize, !g_GameManager.flags.isDemoMode);
        replayManager->replayData = LoadReplayData(replayManager->replayData, fileSize);
        if (replayManager->replayData == NULL)
        {
            return ZUN_ERROR;
        }

        replayManager->replayFileData = NULL;
#ifndef TH08_PORTABLE_NATIVE_LAYOUT
        for (stage = 0; stage < MAX_STAGES; stage++)
        {
            if (replayManager->replayData->header.stageReplayData[stage] != NULL)
            {
                replayManager->replayData->header.stageReplayData[stage] =
                    (StageReplayData *)(reinterpret_cast<u8 *>(replayManager->replayData->header.stageReplayData[stage]) +
                                        reinterpret_cast<i32>(replayManager->replayData));
            }
            if (replayManager->replayData->header.stageFpsData[stage] != NULL)
            {
                replayManager->replayData->header.stageFpsData[stage] +=
                    reinterpret_cast<i32>(replayManager->replayData);
            }
        }
#endif
    }

    stage = g_GameManager.currentStage;
    if (TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) == NULL)
    {
        return ZUN_ERROR;
    }

    stageData = TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage);
    stageFpsData = TH08_REPLAY_FPS_DATA(replayManager->replayData, stage);

    g_GameManager.shotType = replayManager->replayData->shotType / 1;
    g_GameManager.fullShotType = replayManager->replayData->shotType % 1;
    g_GameManager.shotType = replayManager->replayData->shotType;
    g_GameManager.difficulty = replayManager->replayData->difficulty;

    g_GameManager.globals->pointItemsCollected = stageData->pointItemsCollected;
    g_GameManager.rank = stageData->rank;
    g_GameManager.SetLives(stageData->lives);
    g_GameManager.SetBombCount(stageData->bombs);
    g_GameManager.SetPower(stageData->power);
    g_GameManager.globals->graze = stageData->graze;

    replayManager->replayInputCursor = stageData->inputStream;
    replayManager->extendedInputCursor = reinterpret_cast<ReplayInputSync *>(replayManager->replayInputCursor);
    g_GameManager.character = stageData->character;
    g_GameManager.globals->pointItemValue = stageData->pointItemValue;
    *g_GameManager.cfg = replayManager->replayData->gameConfiguration;
    g_Rng.SetSeed(stageData->rngSeed);
    g_GameManager.globals->spellcardsCaptured = stageData->spellcardsCaptured;
    g_GameManager.globals->pointItemExtendsSoFar = stageData->pointItemExtends;
    g_GameManager.globals->nextPointItemExtendThreshold = stageData->nextPointItemExtendThreshold;
    g_GameManager.globals->youkaiGauge = stageData->youkaiGauge;
    g_GameManager.SetClockTime(stageData->clockTime);

    replayManager->replayFpsSampleCursor = stageFpsData;
    replayManager->inputDelay = 0;

    previousStage = NULL;
    for (stage = 0; stage < g_GameManager.currentStage; stage++)
    {
        if (TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage) != NULL)
        {
            previousStage = TH08_REPLAY_STAGE_DATA(replayManager->replayData, stage);
        }
    }

    if (previousStage != NULL)
    {
        g_GameManager.globals->score = previousStage->score;
        g_GameManager.globals->displayScore = g_GameManager.globals->score;
    }

    return ZUN_SUCCESS;
}

ZunResult ReplayManager::DeleteReplayManager(ReplayManager *replayManager)
{
    if (replayManager->playbackFrameControlChain != NULL)
    {
        g_Chain.Cut(replayManager->playbackFrameControlChain);
        replayManager->playbackFrameControlChain = NULL;
    }

    if (replayManager->frameSyncChain != NULL)
    {
        g_Chain.Cut(replayManager->frameSyncChain);
        replayManager->frameSyncChain = NULL;
    }

#if defined(PSP)
    ReleasePreparedRecordingStageBuffers();
    if (!replayManager->IsDemo() && replayManager->replayData != NULL)
    {
        for (i32 stage = 0; stage < MAX_STAGES; ++stage)
            ReleaseRecordedStageBuffers(replayManager->replayData, stage);
    }
#endif
    g_ZunMemory.Free(g_ReplayManager->replayData);

    if (replayManager->replayFileData != NULL)
    {
        g_ZunMemory.Free(replayManager->replayFileData);
    }

    ZUN_DELETE(g_ReplayManager);
    g_ReplayManager = NULL;

    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x453160
ReplayManager::ReplayManager()
{
}

void ReplayManager::StopRecording()
{
    ReplayManager *mgr = g_ReplayManager;
    i32 stage;

    if (mgr != NULL)
    {
#if defined(PSP)
        stage = g_GameManager.currentStage;
        if (mgr->replayData == NULL || stage < 0 || stage >= MAX_STAGES)
            return;

        u8 *const inputBase =
            reinterpret_cast<u8 *>(TH08_REPLAY_STAGE_DATA(mgr->replayData, stage));
        if (inputBase == NULL || mgr->replayInputCursor == NULL)
            return;

        const uintptr_t begin = reinterpret_cast<uintptr_t>(inputBase);
        const uintptr_t cursor = reinterpret_cast<uintptr_t>(mgr->replayInputCursor);
        const size_t capacity = gReplayInputCapacities[stage];
        constexpr size_t kFinalRecordBytes = sizeof(u16) + 6U;
        if (cursor < begin || cursor - begin > capacity ||
            kFinalRecordBytes > capacity - static_cast<size_t>(cursor - begin))
        {
            utils::DebugPrint("error: PSP replay terminator exceeds stage %d input capacity\r\n",
                              stage + 1);
            return;
        }
#endif
        mgr->replayInputCursor += sizeof(u16);
        *(u16 *)mgr->replayInputCursor = 0;
#if !defined(PSP)
        stage = g_GameManager.currentStage;
#endif
        mgr->replayInputEnds[stage] = mgr->replayInputCursor + 6;
    }
}

#if defined(PSP)
void ReplayManager::CompactRecordedStage(i32 stage)
{
    ReplayManager *mgr = g_ReplayManager;
    if (mgr == NULL || mgr->IsDemo() || mgr->replayData == NULL ||
        stage < 0 || stage >= MAX_STAGES)
    {
        return;
    }

    StageReplayData *inputBase = TH08_REPLAY_STAGE_DATA(mgr->replayData, stage);
    u8 *fpsBase = TH08_REPLAY_FPS_DATA(mgr->replayData, stage);
    if (inputBase == NULL || fpsBase == NULL || mgr->replayInputEnds[stage] == NULL ||
        mgr->replayFpsSampleEnds[stage] == NULL)
    {
        return;
    }

    const uintptr_t inputBegin = reinterpret_cast<uintptr_t>(inputBase);
    const uintptr_t inputEnd = reinterpret_cast<uintptr_t>(mgr->replayInputEnds[stage]);
    const uintptr_t fpsBegin = reinterpret_cast<uintptr_t>(fpsBase);
    const uintptr_t fpsEnd = reinterpret_cast<uintptr_t>(mgr->replayFpsSampleEnds[stage]);
    const size_t inputCapacity = gReplayInputCapacities[stage];
    const size_t fpsCapacity = gReplayFpsCapacities[stage];
    const uintptr_t inputCursorAddress = reinterpret_cast<uintptr_t>(mgr->replayInputCursor);
    const uintptr_t extendedCursorAddress = reinterpret_cast<uintptr_t>(mgr->extendedInputCursor);
    const uintptr_t fpsCursorAddress = reinterpret_cast<uintptr_t>(mgr->replayFpsSampleCursor);
    if (inputEnd < inputBegin + kReplayInputHeaderBytes + 8U ||
        inputEnd - inputBegin > inputCapacity || fpsEnd < fpsBegin ||
        fpsEnd - fpsBegin > fpsCapacity || inputCursorAddress < inputBegin ||
        inputCursorAddress > inputEnd || extendedCursorAddress < inputBegin ||
        extendedCursorAddress > inputEnd || fpsCursorAddress < fpsBegin ||
        fpsCursorAddress > fpsEnd)
    {
        return;
    }

    const size_t inputUsed = static_cast<size_t>(inputEnd - inputBegin);
    const size_t fpsUsed = static_cast<size_t>(fpsEnd - fpsBegin);
    const uintptr_t inputCursorOffset = inputCursorAddress - inputBegin;
    const uintptr_t extendedCursorOffset = extendedCursorAddress - inputBegin;
    const uintptr_t fpsCursorOffset = fpsCursorAddress - fpsBegin;

    // GameManager finalizes at stage teardown and SaveReplay finalizes once
    // more, exactly like the original call sequence.  Keep the second u16 of
    // physical headroom outside the first serialized span so compaction does
    // not turn that later terminator into a two-byte out-of-bounds read.
    const size_t compactInputCapacity = inputUsed + kReplaySaveFinalizationHeadroom;
    StageReplayData *shrunkInput = static_cast<StageReplayData *>(
        th08_psp_tracked_realloc(inputBase, compactInputCapacity, "PSP replay input compact"));
    if (shrunkInput != NULL)
    {
        TH08_REPLAY_STAGE_DATA(mgr->replayData, stage) = shrunkInput;
        mgr->replayInputEnds[stage] = reinterpret_cast<u8 *>(shrunkInput) + inputUsed;
        mgr->replayInputCursor = reinterpret_cast<u8 *>(shrunkInput) + inputCursorOffset;
        mgr->extendedInputCursor = reinterpret_cast<ReplayInputSync *>(
            reinterpret_cast<u8 *>(shrunkInput) + extendedCursorOffset);
        gReplayInputCapacities[stage] = compactInputCapacity;
    }

    u8 *shrunkFps = static_cast<u8 *>(
        th08_psp_tracked_realloc(fpsBase, fpsUsed, "PSP replay FPS compact"));
    if (shrunkFps != NULL)
    {
        TH08_REPLAY_FPS_DATA(mgr->replayData, stage) = shrunkFps;
        mgr->replayFpsSampleEnds[stage] = shrunkFps + fpsUsed;
        mgr->replayFpsSampleCursor = shrunkFps + fpsCursorOffset;
        gReplayFpsCapacities[stage] = fpsUsed;
    }

    utils::DebugPrint(
        "info : PSP replay compact stage %d input %lu/%lu fps %lu/%lu\r\n",
        stage + 1, static_cast<unsigned long>(inputUsed),
        static_cast<unsigned long>(kReplayInputCapacity), static_cast<unsigned long>(fpsUsed),
        static_cast<unsigned long>(kReplayFpsCapacity));
}
#endif

#pragma var_order(i, mgr, infoCursor, bytesWritten, compressedData, slowDownRate, compressedSize, stageSize,          \
                  tempBuffer, replayCopy, infoBuffer, file, infoHeader, currentOffset, localTime, currentTime,      \
                  dateBuffer, checksum, checksumCursor, obfuscateOffset, obfuscateCursor)
void ReplayManager::SaveReplay(const char *replayPath, const char *replayName)
{
    ReplayManager *mgr;
    u8 *tempBuffer;
    ReplayData replayCopy;
    i32 currentOffset;
    i32 stageSize;
    float slowDownRate;
    ReplayUserDataHeader infoHeader;
    char infoBuffer[1024];
    char dateBuffer[252];
    char *infoCursor;
    time_t currentTime;
    tm *localTime;
    u8 *compressedData;
    i32 compressedSize;
    u8 *checksumCursor;
    u32 checksum;
    u8 *obfuscateCursor;
    u8 obfuscateOffset;
    HANDLE file;
    DWORD bytesWritten;
    i32 i;
#if defined(PSP)
    size_t replayInputSizes[MAX_STAGES]{};
    size_t replayFpsSizes[MAX_STAGES]{};
    size_t serializedPayloadSize;
#endif

    if (g_ReplayManager != NULL)
    {
        mgr = g_ReplayManager;

        if (!mgr->IsDemo())
        {
            if (!g_GameManager.IsPracticeMode() && g_GameManager.difficulty < LUNATIC + 1 &&
                memcmp(&g_Supervisor.cfg, &mgr->replayData->gameConfiguration, sizeof(GameConfiguration)) != 0)
            {
                goto release_stage_data;
            }

            if (mgr->replayData->gameConfiguration.slowMode != 0)
            {
                goto release_stage_data;
            }

            if (replayPath != NULL)
            {
                utils::DebugPrint("info : Replay File write %s\r\n", replayPath);

#if defined(PSP)
    // The desktop routine reserves an unconditional 4 MiB before it even
    // knows the replay size.  First finalize and validate every discontiguous
    // source span, then reserve only the exact wire payload.  This does not
    // change the byte order presented to the original LZSS encoder.
    replayCopy = *mgr->replayData;
    ReplayManager::StopRecording();

    i = g_GameManager.stageAtStart;
    if (i < 0 || i >= MAX_STAGES || TH08_REPLAY_STAGE_DATA(mgr->replayData, i) == NULL)
    {
        utils::DebugPrint("error: PSP replay save has no current stage %d\r\n", i);
        goto release_stage_data;
    }
    TH08_REPLAY_STAGE_DATA(mgr->replayData, i)->score = g_GameManager.globals->score;

    serializedPayloadSize = TH08_REPLAY_DATA_SIZE - TH08_REPLAY_HEADER_SIZE;
    for (i = 0; i < MAX_STAGES; ++i)
    {
        u8 *const input = reinterpret_cast<u8 *>(TH08_REPLAY_STAGE_DATA(mgr->replayData, i));
        if (input != NULL)
        {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(input);
            const uintptr_t end = reinterpret_cast<uintptr_t>(mgr->replayInputEnds[i]);
            if (mgr->replayInputEnds[i] == NULL || end < begin ||
                end - begin > gReplayInputCapacities[i])
            {
                utils::DebugPrint("error: PSP replay input span invalid at stage %d\r\n", i + 1);
                goto release_stage_data;
            }
            replayInputSizes[i] = static_cast<size_t>(end - begin);
            if (replayInputSizes[i] > kReplayMaxSerializedPayload - serializedPayloadSize)
            {
                utils::DebugPrint("error: PSP replay input size overflow at stage %d\r\n", i + 1);
                goto release_stage_data;
            }
            serializedPayloadSize += replayInputSizes[i];
        }
    }
    for (i = 0; i < MAX_STAGES; ++i)
    {
        u8 *const fps = TH08_REPLAY_FPS_DATA(mgr->replayData, i);
        if (fps != NULL)
        {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(fps);
            const uintptr_t end = reinterpret_cast<uintptr_t>(mgr->replayFpsSampleEnds[i]);
            if (mgr->replayFpsSampleEnds[i] == NULL || end < begin ||
                end - begin > gReplayFpsCapacities[i])
            {
                utils::DebugPrint("error: PSP replay FPS span invalid at stage %d\r\n", i + 1);
                goto release_stage_data;
            }
            replayFpsSizes[i] = static_cast<size_t>(end - begin);
            if (replayFpsSizes[i] > kReplayMaxSerializedPayload - serializedPayloadSize)
            {
                utils::DebugPrint("error: PSP replay FPS size overflow at stage %d\r\n", i + 1);
                goto release_stage_data;
            }
            serializedPayloadSize += replayFpsSizes[i];
        }
    }
    if (serializedPayloadSize > kReplayMaxSerializedPayload)
    {
        utils::DebugPrint("error: PSP replay payload exceeds logical stream capacities\r\n");
        goto release_stage_data;
    }

    tempBuffer = static_cast<u8 *>(
        g_ZunMemory.Alloc(serializedPayloadSize, "PSP replay exact serialize"));
    if (tempBuffer == NULL)
    {
        utils::DebugPrint("error: PSP replay exact payload allocation failed (%lu bytes)\r\n",
                          static_cast<unsigned long>(serializedPayloadSize));
        goto release_stage_data;
    }
    currentOffset = TH08_REPLAY_DATA_SIZE;
    utils::DebugPrint("info : PSP replay serialize scratch %lu bytes (saved %lu vs 4 MiB)\r\n",
                      static_cast<unsigned long>(serializedPayloadSize),
                      static_cast<unsigned long>(
                          serializedPayloadSize < 0x400000U ? 0x400000U - serializedPayloadSize : 0U));
#else
    tempBuffer = (u8 *)g_ZunMemory.Alloc(0x400000, "rep tmp");
    replayCopy = *mgr->replayData;

    ReplayManager::StopRecording();

    i = g_GameManager.stageAtStart;
    TH08_REPLAY_STAGE_DATA(mgr->replayData, i)->score = g_GameManager.globals->score;

    currentOffset = TH08_REPLAY_HEADER_SIZE;
    currentOffset += TH08_REPLAY_DATA_SIZE - TH08_REPLAY_HEADER_SIZE;
#endif

    for (i = 0; i < MAX_STAGES; i++)
    {
        if (TH08_REPLAY_STAGE_DATA(mgr->replayData, i) != NULL)
        {
#if defined(PSP)
            stageSize = static_cast<i32>(replayInputSizes[i]);
#else
            stageSize = mgr->replayInputEnds[i] - (u8 *)TH08_REPLAY_STAGE_DATA(mgr->replayData, i);
#endif
            memcpy(tempBuffer + currentOffset - TH08_REPLAY_HEADER_SIZE,
                   TH08_REPLAY_STAGE_DATA(mgr->replayData, i), stageSize);
#ifdef TH08_PORTABLE_NATIVE_LAYOUT
            replayCopy.header.stageReplayDataOffsets[i] = currentOffset;
#else
            replayCopy.header.stageReplayData[i] = (StageReplayData *)currentOffset;
#endif
            currentOffset += stageSize;
#if defined(PSP)
            // The source is never consulted again after serialization and the
            // common exit path already discarded it.  Releasing it here keeps
            // completed-stage storage from overlapping the LZSS output peak.
            g_ZunMemory.Free(TH08_REPLAY_STAGE_DATA(mgr->replayData, i));
            TH08_REPLAY_STAGE_DATA(mgr->replayData, i) = NULL;
            mgr->replayInputEnds[i] = NULL;
            gReplayInputCapacities[i] = 0;
#endif
        }
    }

    for (i = 0; i < MAX_STAGES; i++)
    {
        if (TH08_REPLAY_FPS_DATA(mgr->replayData, i) != NULL)
        {
#if defined(PSP)
            stageSize = static_cast<i32>(replayFpsSizes[i]);
#else
            stageSize = mgr->replayFpsSampleEnds[i] - TH08_REPLAY_FPS_DATA(mgr->replayData, i);
#endif
            memcpy(tempBuffer + currentOffset - TH08_REPLAY_HEADER_SIZE,
                   TH08_REPLAY_FPS_DATA(mgr->replayData, i), stageSize);
#ifdef TH08_PORTABLE_NATIVE_LAYOUT
            replayCopy.header.stageFpsDataOffsets[i] = currentOffset;
#else
            replayCopy.header.stageFpsData[i] = (u8 *)currentOffset;
#endif
            currentOffset += stageSize;
#if defined(PSP)
            g_ZunMemory.Free(TH08_REPLAY_FPS_DATA(mgr->replayData, i));
            TH08_REPLAY_FPS_DATA(mgr->replayData, i) = NULL;
            mgr->replayFpsSampleEnds[i] = NULL;
            gReplayFpsCapacities[i] = 0;
#endif
        }
    }

    replayCopy.spellcardScore = g_GameManager.globals->displayScore;

    if (replayCopy.spellcardNumber >= 0)
    {
        memcpy(replayCopy.spellcardName, g_GameManager.catkData[replayCopy.spellcardNumber].spellName,
               sizeof(replayCopy.spellcardName));
    }

    slowDownRate = ((g_Supervisor.lagNumerator / g_Supervisor.lagDenominator) - 0.5f) * 2.0f;

    if (slowDownRate < 0.0f)
    {
        slowDownRate = 0.0f;
    }
    else if (slowDownRate >= 1.0f)
    {
        slowDownRate = 1.0f;
    }

    replayCopy.slowDownRate = (1.0f - slowDownRate) * 100.0f;

    infoHeader.magic = *(u32 *)"USER";
    infoHeader.reserved = 0;
    memset(infoBuffer, 0, sizeof(infoBuffer));
    infoCursor = infoBuffer;

    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_PLAYER_NAME, replayName);

    time(&currentTime);
    localTime = localtime(&currentTime);
    strftime(dateBuffer, 20, "%Y/%m/%d %H:%M:%S", localTime);

    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_PLAY_TIME, dateBuffer);
    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_CHARACTER, ResultScreen::GetCharacterName(g_GameManager.shotType));
    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_SCORE, g_GameManager.globals->displayScore);
    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_DIFFICULTY, g_ReplayDifficultyList[g_GameManager.difficulty]);

    if (replayCopy.spellcardNumber >= 0)
    {
        infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_SPELLCARD, replayCopy.spellcardNumber + 1,
                                  replayCopy.spellcardName);
    }
    else
    {
        infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_FINAL_STAGE,
                                  g_GameManager.flags.gameCleared ? "Clear" : ResultScreen::GetStageName(g_GameManager.stageAtStart));
    }

    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_DEATHS, g_GameManager.GetDeaths());
    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_BOMBS, g_GameManager.GetBombsUsed());
    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_SLOWDOWN, replayCopy.slowDownRate);

    g_GameManager.hscr.humanityRate = (i32)(((float)g_GameManager.humanityRateNumerator / g_GameManager.humanityRateDenominator) * 10.0f);
    infoCursor =
        AppendFormat(infoCursor, TH_REPLAY_INFO_HUMAN_RATE, (float)g_GameManager.hscr.humanityRate / 100.0f);
    infoCursor = AppendFormat(infoCursor, TH_REPLAY_INFO_VERSION, 1, 0, 'd');

    infoHeader.size = strlen(infoBuffer) + sizeof(ReplayUserDataHeader);
    infoHeader.size += infoHeader.size & 1;

    replayCopy.header.hasUserDataSection = 1;
    strcpy(replayCopy.playerName, replayName);
    ResultScreen::FormatDate(replayCopy.date);

    replayCopy.header.obfuscationKey = (u8)(g_Rng.GetRandomU16InRange(0x80) + 0x40);
    replayCopy.randomPayloadByte = (u8)g_Rng.GetRandomU16InRange(0x100);
    replayCopy.header.randomHeaderByte = (u8)g_Rng.GetRandomU16InRange(0x100);
    replayCopy.slowDownRate2 = replayCopy.slowDownRate + 1.12f;
    replayCopy.unconsumedConstant30 = 30;

#if defined(PSP)
    if (static_cast<size_t>(currentOffset - TH08_REPLAY_HEADER_SIZE) != serializedPayloadSize)
    {
        utils::DebugPrint("error: PSP replay serialized size changed during assembly\r\n");
        g_ZunMemory.Free(tempBuffer);
        goto release_stage_data;
    }
#endif
    memcpy(tempBuffer, &replayCopy.randomPayloadByte, TH08_REPLAY_DATA_SIZE - TH08_REPLAY_HEADER_SIZE);

    utils::DebugPrint("info : original size %d\r\n", currentOffset);

    replayCopy.header.decompressedSize = currentOffset - TH08_REPLAY_HEADER_SIZE;
    compressedData = Lzss::Encode(tempBuffer, replayCopy.header.decompressedSize, &replayCopy.header.compressedSize);
    g_ZunMemory.Free(tempBuffer);
#if defined(PSP)
    if (compressedData == NULL)
    {
        utils::DebugPrint("error: PSP replay LZSS output allocation failed (input %d bytes)\r\n",
                          replayCopy.header.decompressedSize);
        goto release_stage_data;
    }
#endif
    compressedSize = replayCopy.header.compressedSize;

    checksumCursor = &replayCopy.header.obfuscationKey;
    checksum = REPLAY_OBFUSCATION_VALUE;

    for (i = 0; i < TH08_REPLAY_HEADER_SIZE - offsetof(ReplayDataHeader, obfuscationKey); i++, checksumCursor++)
    {
        checksum += *checksumCursor;
    }

    checksumCursor = compressedData;

    for (i = 0; i < compressedSize; i++, checksumCursor++)
    {
        checksum += *checksumCursor;
    }

    replayCopy.header.checksum = checksum;
    obfuscateCursor = (u8 *)&replayCopy.header.compressedSize;
    obfuscateOffset = replayCopy.header.obfuscationKey;

    for (i = 0; i < TH08_REPLAY_HEADER_SIZE - offsetof(ReplayDataHeader, compressedSize); i++, obfuscateCursor++)
    {
        *obfuscateCursor += obfuscateOffset;
        obfuscateOffset += 7;
    }

    obfuscateCursor = compressedData;

    for (i = 0; i < compressedSize; i++, obfuscateCursor++)
    {
        *obfuscateCursor += obfuscateOffset;
        obfuscateOffset += 7;
    }

    replayCopy.header.fileSize = compressedSize + TH08_REPLAY_HEADER_SIZE;

    file = CreateFileA(replayPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
#if defined(PSP)
        GlobalFree(compressedData);
#endif
        goto release_stage_data;
    }

    {
        WriteFile(file, &replayCopy.header, TH08_REPLAY_HEADER_SIZE, &bytesWritten, NULL);
        WriteFile(file, compressedData, compressedSize, &bytesWritten, NULL);
        WriteFile(file, &infoHeader, sizeof(infoHeader), &bytesWritten, NULL);
        WriteFile(file, infoBuffer, infoHeader.size - sizeof(infoHeader), &bytesWritten, NULL);
        CloseHandle(file);

        utils::DebugPrint("info : Size %d -> %d\r\n", currentOffset, compressedSize + TH08_REPLAY_HEADER_SIZE);
        GlobalFree(compressedData);
    }
            }
release_stage_data:
    for (i = 0; i < MAX_STAGES; i++)
    {
#if defined(PSP)
        ReleaseRecordedStageBuffers(g_ReplayManager->replayData, i);
#else
        if (TH08_REPLAY_STAGE_DATA(g_ReplayManager->replayData, i) != NULL)
        {
            g_ZunMemory.Free(TH08_REPLAY_STAGE_DATA(g_ReplayManager->replayData, i));
        }

        if (TH08_REPLAY_FPS_DATA(mgr->replayData, i) != NULL)
        {
            g_ZunMemory.Free(TH08_REPLAY_FPS_DATA(g_ReplayManager->replayData, i));
        }
#endif
    }
        }

        g_Chain.Cut(g_ReplayManager->calcChain);
    }
}

namespace
{
char *AppendFormat(char *buffer, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

    return buffer + strlen(buffer);
}
} // namespace

// FUNCTION: th08 0x453c60
void GameManager::SetClockTime(i32 value)
{
    this->globals->clockTime = value;
}

// FUNCTION: th08 0x453cc0
i32 ReplayManager::IsDemo()
{
    return this->isDemo;
}

} // namespace th08
