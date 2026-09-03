#pragma once

#include "inttypes.hpp"

namespace th08
{
namespace ReplaySyncAudit
{

// Values written into the sidecar.  They describe the replay wire record that
// supplied the input; they do not alter or extend the .rpy format itself.
enum InputRecordKind
{
    INPUT_RECORD_NORMAL = 0,
    INPUT_RECORD_EXTENDED = 1,
    INPUT_RECORD_NONE = 0xff
};

// SpawnItem reports its already-decided exit path through this diagnostic
// enum.  The audit observes those decisions; it never chooses one.
enum ItemSpawnAuditOutcome
{
    ITEM_SPAWN_AUDIT_ACCEPTED = 1,
    ITEM_SPAWN_AUDIT_REJECT_X = 2,
    ITEM_SPAWN_AUDIT_REJECT_TIME_FIRST_SLOT = 3,
    ITEM_SPAWN_AUDIT_REJECT_POOL_FULL = 4
};

#if defined(TH08_REPLAY_SYNC_AUDIT)

// InitializeRuntime is the only pre-shutdown entry point that may inspect the
// desktop environment.  Frame/stage hooks only touch fixed static storage.
void InitializeRuntime();

// Called after every subsystem has been registered, immediately before
// GAMEPLAY_SETUP_COMPLETE is published.
void StageBegin();

// BeginFrame runs immediately after one replay input record is consumed.
// EndFrame runs at the beginning of the priority-18 playback-control job,
// after the game subsystems have processed that logical replay frame.
void BeginFrame(u32 replayFrame, u16 input, u8 inputRecordKind);
void EndFrame();

// Frame-local SpawnItem witness.  positionBytes points at the existing
// Float3 object and is copied as raw bits; no floating-point expression is
// evaluated by the audit.  Result must be called only on the return path the
// uninstrumented SpawnItem implementation already selected.
void RecordItemSpawnRequest(const void *positionBytes, i32 itemType,
                            i32 state, i32 nextIndex);
void RecordItemSpawnResult(u32 outcome, i32 itemType, i32 state,
                           i32 nextIndex);

// Called at the beginning of GameManager's deleted callback, before any stage
// owner is cut.  It never tries to complete a pending frame implicitly.
void StageTerminal();

// PSP emulators and the HOME menu may terminate the emulated process without
// allowing WinMain to unwind.  Call this only after stage teardown is complete;
// it checkpoints an already-terminal trace and never inspects gameplay state.
void CheckpointAfterStage();

// Final I/O entry point. Call only after the process-wide chain teardown.
void FlushAtShutdown();

#else

// Keep call sites identical in ordinary builds.  These inline no-ops let the
// compiler remove every argument-independent audit operation.
inline void RecordItemSpawnRequest(const void *, i32, i32, i32)
{
}

inline void RecordItemSpawnResult(u32, i32, i32, i32)
{
}

#endif

} // namespace ReplaySyncAudit
} // namespace th08
