#include "anm_scratch.hpp"
#include "boot_checkpoint.hpp"
#include "fileio.hpp"
#include "ge4_bridge.hpp"
#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
#include "io_activity_lamp.hpp"
#endif
#include "memory_telemetry.hpp"
#include "newlib_heap_geometry.hpp"
#include "perf_attribution.hpp"
#include "platform.hpp"
#include "render_cadence.hpp"
#include "render_resource_arena.hpp"
#include "video.hpp"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmoduleinfo.h>

#include <cstddef>
#include <exception>
#include <cstdint>

// Uncaught failure (e.g. std::bad_alloc from operator new under -fno-exceptions)
// used to abort with the boot log still buffered, so hardware logs ended
// silently.  Flush the log before the process dies.
static void TerminateHandler()
{
    th08::psp::BootLog("TERMINATE uncaught_failure=1\n");
    th08::psp::FlushBootLog();
    sceKernelExitGame();
    for (;;)
        sceKernelDelayThread(1000000);
}

#if !defined(TH08_PSP_PORT) || !defined(TH08_PSP_SC_ONLY)
#error The PSP bootstrap requires the PSP port and SC-only build gates.
#endif

#if defined(TH08_PSP_COPROCESSOR_PATH)
#error Coprocessor features are forbidden in the initial PSP bootstrap.
#endif

#if !defined(TH08_PSP_TH07_BOOT_PARITY)
#define TH08_PSP_TH07_BOOT_PARITY 1
#endif

// Preserve the existing undefined-means-disabled feature contracts while also
// emitting an unambiguous 0/1 build fingerprint into every hardware boot log.
#if defined(TH08_PSP_COMPACT_BULLET_VM) && TH08_PSP_COMPACT_BULLET_VM
#define TH08_PSP_FEATURE_COMPACT_BULLET_VM 1
#else
#define TH08_PSP_FEATURE_COMPACT_BULLET_VM 0
#endif
#if defined(TH08_PSP_BULLET_FASTPATH) && TH08_PSP_BULLET_FASTPATH
#define TH08_PSP_FEATURE_BULLET_FASTPATH 1
#else
#define TH08_PSP_FEATURE_BULLET_FASTPATH 0
#endif
#if defined(TH08_PSP_BULLET_LIVE_ENUM) && TH08_PSP_BULLET_LIVE_ENUM
#define TH08_PSP_FEATURE_BULLET_LIVE_ENUM 1
#else
#define TH08_PSP_FEATURE_BULLET_LIVE_ENUM 0
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
#define TH08_PSP_FEATURE_BULLET_TRANSFORM_AUDIT 1
#else
#define TH08_PSP_FEATURE_BULLET_TRANSFORM_AUDIT 0
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
#define TH08_PSP_FEATURE_BULLET_TRANSFORM_TERMINAL_FASTPATH 1
#else
#define TH08_PSP_FEATURE_BULLET_TRANSFORM_TERMINAL_FASTPATH 0
#endif
#if defined(TH08_PSP_PREPARE_STATE_CACHE) && TH08_PSP_PREPARE_STATE_CACHE
#define TH08_PSP_FEATURE_PREPARE_STATE_CACHE 1
#else
#define TH08_PSP_FEATURE_PREPARE_STATE_CACHE 0
#endif
#if defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
#define TH08_PSP_FEATURE_BULLET_UNIFIED_QUADS 1
#else
#define TH08_PSP_FEATURE_BULLET_UNIFIED_QUADS 0
#endif
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
#define TH08_PSP_FEATURE_BULLET_DIRECT_GE 1
#else
#define TH08_PSP_FEATURE_BULLET_DIRECT_GE 0
#endif
#if defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
    TH08_PSP_BULLET_ONEPASS_4V_AUDIT
#define TH08_PSP_FEATURE_BULLET_ONEPASS_4V_AUDIT 1
#else
#define TH08_PSP_FEATURE_BULLET_ONEPASS_4V_AUDIT 0
#endif
#if defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
    TH08_PSP_BULLET_ONEPASS_4V_FASTPATH
#define TH08_PSP_FEATURE_BULLET_ONEPASS_4V_FASTPATH 1
#else
#define TH08_PSP_FEATURE_BULLET_ONEPASS_4V_FASTPATH 0
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
    TH08_PSP_BULLET_PACKED_VERTEX_AUDIT
#define TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_AUDIT 1
#else
#define TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_AUDIT 0
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
#define TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_FASTPATH 1
#else
#define TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_FASTPATH 0
#endif
#if defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
    TH08_PSP_BULLET_MIXED_QUADS_AUDIT
#define TH08_PSP_FEATURE_BULLET_MIXED_QUADS_AUDIT 1
#else
#define TH08_PSP_FEATURE_BULLET_MIXED_QUADS_AUDIT 0
#endif
#if defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_BULLET_MIXED_QUADS_FASTPATH
#define TH08_PSP_FEATURE_BULLET_MIXED_QUADS_FASTPATH 1
#else
#define TH08_PSP_FEATURE_BULLET_MIXED_QUADS_FASTPATH 0
#endif
#if defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
    TH08_PSP_ITEM_MIXED_QUADS_AUDIT
#define TH08_PSP_FEATURE_ITEM_MIXED_QUADS_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_MIXED_QUADS_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH
#define TH08_PSP_FEATURE_ITEM_MIXED_QUADS_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_MIXED_QUADS_FASTPATH 0
#endif
#if defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE
#define TH08_PSP_FEATURE_ITEM_DIRECT_GE 1
#else
#define TH08_PSP_FEATURE_ITEM_DIRECT_GE 0
#endif
#if defined(TH08_PSP_ITEM_NATURAL_QUADS) && \
    TH08_PSP_ITEM_NATURAL_QUADS
#define TH08_PSP_FEATURE_ITEM_NATURAL_QUADS 1
#else
#define TH08_PSP_FEATURE_ITEM_NATURAL_QUADS 0
#endif
#if defined(TH08_PSP_ITEM_NATURAL_NATIVE_COPY) && \
    TH08_PSP_ITEM_NATURAL_NATIVE_COPY
#define TH08_PSP_FEATURE_ITEM_NATURAL_NATIVE_COPY 1
#else
#define TH08_PSP_FEATURE_ITEM_NATURAL_NATIVE_COPY 0
#endif
#if defined(TH08_PSP_PLAYER_SCAN_SIDECAR) && TH08_PSP_PLAYER_SCAN_SIDECAR
#define TH08_PSP_FEATURE_PLAYER_SCAN_SIDECAR 1
#else
#define TH08_PSP_FEATURE_PLAYER_SCAN_SIDECAR 0
#endif
#if defined(TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT) && \
    TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT
#define TH08_PSP_FEATURE_ENEMY_ACTIVE_BITMAP_AUDIT 1
#else
#define TH08_PSP_FEATURE_ENEMY_ACTIVE_BITMAP_AUDIT 0
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
#define TH08_PSP_FEATURE_BULLET_COLLISION_GATE_AUDIT 1
#else
#define TH08_PSP_FEATURE_BULLET_COLLISION_GATE_AUDIT 0
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE) && TH08_PSP_BULLET_COLLISION_GATE
#define TH08_PSP_FEATURE_BULLET_COLLISION_GATE 1
#else
#define TH08_PSP_FEATURE_BULLET_COLLISION_GATE 0
#endif
#if defined(TH08_PSP_CANCEL_EMPTY_FASTPATH) && \
    TH08_PSP_CANCEL_EMPTY_FASTPATH
#define TH08_PSP_FEATURE_CANCEL_EMPTY_FASTPATH 1
#else
#define TH08_PSP_FEATURE_CANCEL_EMPTY_FASTPATH 0
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
#define TH08_PSP_FEATURE_BULLET_CANCEL_SPATIAL 1
#else
#define TH08_PSP_FEATURE_BULLET_CANCEL_SPATIAL 0
#endif
#if defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT
#define TH08_PSP_FEATURE_ITEM_AUTOCOLLECT_CACHE_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_AUTOCOLLECT_CACHE_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE
#define TH08_PSP_FEATURE_ITEM_AUTOCOLLECT_CACHE 1
#else
#define TH08_PSP_FEATURE_ITEM_AUTOCOLLECT_CACHE 0
#endif
#if defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
#define TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH
#define TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_FASTPATH 0
#endif
#if defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT
#define TH08_PSP_FEATURE_ITEM_TIME_ANM_IDLE_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_ANM_IDLE_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH
#define TH08_PSP_FEATURE_ITEM_TIME_ANM_IDLE_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_ANM_IDLE_FASTPATH 0
#endif
#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
#define TH08_PSP_FEATURE_ITEM_TIME_DRAW_PAIR_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_DRAW_PAIR_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH
#define TH08_PSP_FEATURE_ITEM_TIME_DRAW_PAIR_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_DRAW_PAIR_FASTPATH 0
#endif
#if defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
#define TH08_PSP_FEATURE_ITEM_TIME_INLINE_DRAW_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_INLINE_DRAW_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH
#define TH08_PSP_FEATURE_ITEM_TIME_INLINE_DRAW_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_TIME_INLINE_DRAW_FASTPATH 0
#endif
#if defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
#define TH08_PSP_FEATURE_ASCII_POPUP_OCCUPANCY 1
#else
#define TH08_PSP_FEATURE_ASCII_POPUP_OCCUPANCY 0
#endif
#if defined(TH08_PSP_ASCII_POPUP_BATCH) && TH08_PSP_ASCII_POPUP_BATCH
#define TH08_PSP_FEATURE_ASCII_POPUP_BATCH 1
#else
#define TH08_PSP_FEATURE_ASCII_POPUP_BATCH 0
#endif
#if defined(TH08_PSP_ASCII_POPUP_DIRECT_PAIR) && \
    TH08_PSP_ASCII_POPUP_DIRECT_PAIR
#define TH08_PSP_FEATURE_ASCII_POPUP_DIRECT_PAIR 1
#else
#define TH08_PSP_FEATURE_ASCII_POPUP_DIRECT_PAIR 0
#endif
#if defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \
    TH08_PSP_SCORE_POPUP_NATIVE_GE
#define TH08_PSP_FEATURE_SCORE_POPUP_NATIVE_GE 1
#else
#define TH08_PSP_FEATURE_SCORE_POPUP_NATIVE_GE 0
#endif
#if defined(TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING) && \
    TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
#define TH08_PSP_FEATURE_STAGE_RELATIVE_PERF_SAMPLING 1
#else
#define TH08_PSP_FEATURE_STAGE_RELATIVE_PERF_SAMPLING 0
#endif
#if defined(TH08_PSP_PERF_ATTRIBUTION) && TH08_PSP_PERF_ATTRIBUTION
#define TH08_PSP_FEATURE_PERF_ATTRIBUTION 1
#else
#define TH08_PSP_FEATURE_PERF_ATTRIBUTION 0
#endif
#if defined(TH08_PSP_DRAW_PRIORITY_SUBPROFILE) && \
    TH08_PSP_DRAW_PRIORITY_SUBPROFILE
#define TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE 1
#else
#define TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE 0
#endif
#if defined(TH08_PSP_ITEM_UPDATE_SUBPROFILE) && \
    TH08_PSP_ITEM_UPDATE_SUBPROFILE
#define TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE 1
#else
#define TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE 0
#endif
#if defined(TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT) && \
    TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT
#define TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_ATAN2_FASTPATH) && TH08_PSP_ITEM_ATAN2_FASTPATH
#define TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH 0
#endif
#if defined(TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT) && \
    TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT
#define TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH_AUDIT 1
#else
#define TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH_AUDIT 0
#endif
#if defined(TH08_PSP_ITEM_SINCOS_FASTPATH) && TH08_PSP_ITEM_SINCOS_FASTPATH
#define TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH 1
#else
#define TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH 0
#endif
#if defined(TH08_PSP_SOFTFLOAT_CENSUS) && TH08_PSP_SOFTFLOAT_CENSUS
#define TH08_PSP_FEATURE_SOFTFLOAT_CENSUS 1
#else
#define TH08_PSP_FEATURE_SOFTFLOAT_CENSUS 0
#endif
#if defined(TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT) && \
    TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT
#define TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR_AUDIT 1
#else
#define TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR_AUDIT 0
#endif
#if defined(TH08_PSP_AUDIO_FIXED_CURSOR) && TH08_PSP_AUDIO_FIXED_CURSOR
#define TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR 1
#else
#define TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR 0
#endif
#if defined(TH08_PSP_TRIG_DF_FASTPATH_AUDIT) && TH08_PSP_TRIG_DF_FASTPATH_AUDIT
#define TH08_PSP_FEATURE_TRIG_DF_FASTPATH_AUDIT 1
#else
#define TH08_PSP_FEATURE_TRIG_DF_FASTPATH_AUDIT 0
#endif
#if defined(TH08_PSP_TRIG_DF_FASTPATH) && TH08_PSP_TRIG_DF_FASTPATH
#define TH08_PSP_FEATURE_TRIG_DF_FASTPATH 1
#else
#define TH08_PSP_FEATURE_TRIG_DF_FASTPATH 0
#endif
#if defined(TH08_PSP_PERF_ENV) && TH08_PSP_PERF_ENV
#define TH08_PSP_FEATURE_PERF_ENV 1
#else
#define TH08_PSP_FEATURE_PERF_ENV 0
#endif
#if defined(TH08_PSP_SWAP_NOWAIT) && TH08_PSP_SWAP_NOWAIT
#define TH08_PSP_FEATURE_SWAP_NOWAIT 1
#else
#define TH08_PSP_FEATURE_SWAP_NOWAIT 0
#endif
#if defined(TH08_PSP_SWAP_ASYNC) && TH08_PSP_SWAP_ASYNC
#define TH08_PSP_FEATURE_SWAP_ASYNC 1
#else
#define TH08_PSP_FEATURE_SWAP_ASYNC 0
#endif
#if defined(TH08_PSP_FLIP_GUARD_COLOR_ONLY) && TH08_PSP_FLIP_GUARD_COLOR_ONLY
#define TH08_PSP_FEATURE_FLIP_GUARD_COLOR_ONLY 1
#else
#define TH08_PSP_FEATURE_FLIP_GUARD_COLOR_ONLY 0
#endif
#if defined(TH08_PSP_TICK_GATE_BYPASS) && TH08_PSP_TICK_GATE_BYPASS
#define TH08_PSP_FEATURE_TICK_GATE_BYPASS 1
#else
#define TH08_PSP_FEATURE_TICK_GATE_BYPASS 0
#endif
#if defined(TH08_PSP_SWAP_TRIPLE) && TH08_PSP_SWAP_TRIPLE
#define TH08_PSP_FEATURE_SWAP_TRIPLE 1
#else
#define TH08_PSP_FEATURE_SWAP_TRIPLE 0
#endif
#if defined(TH08_PSP_PSPGL_STREAM_ARENA) && TH08_PSP_PSPGL_STREAM_ARENA
#define TH08_PSP_FEATURE_PSPGL_STREAM_ARENA 1
#else
#define TH08_PSP_FEATURE_PSPGL_STREAM_ARENA 0
#endif
#if defined(TH08_PSP_REPLAY_RESERVE_RECYCLE) && TH08_PSP_REPLAY_RESERVE_RECYCLE
#define TH08_PSP_FEATURE_REPLAY_RESERVE_RECYCLE 1
#else
#define TH08_PSP_FEATURE_REPLAY_RESERVE_RECYCLE 0
#endif
#if defined(TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE) && TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE
#define TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_NO_PROMOTE 1
#else
#define TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_NO_PROMOTE 0
#endif
#if defined(TH08_PSP_DIALOGUE_SNAPSHOT_DIAG) && TH08_PSP_DIALOGUE_SNAPSHOT_DIAG
#define TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_DIAG 1
#else
#define TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_DIAG 0
#endif
#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
#define TH08_PSP_FEATURE_GO_IO_LAMP 1
#else
#define TH08_PSP_FEATURE_GO_IO_LAMP 0
#endif
#if defined(TH08_PSP_IO_SERIALIZE) && TH08_PSP_IO_SERIALIZE
#define TH08_PSP_FEATURE_IO_SERIALIZE 1
#else
#define TH08_PSP_FEATURE_IO_SERIALIZE 0
#endif
#if defined(TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND) && TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND
#define TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_AT_BACKGROUND 1
#else
#define TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_AT_BACKGROUND 0
#endif
#if defined(TH08_PSP_DIALOGUE_LIVE_BACKGROUND) && TH08_PSP_DIALOGUE_LIVE_BACKGROUND
#define TH08_PSP_FEATURE_DIALOGUE_LIVE_BACKGROUND 1
#else
#define TH08_PSP_FEATURE_DIALOGUE_LIVE_BACKGROUND 0
#endif
#if defined(TH08_PSP_IO_BOUNCE_HIGH) && TH08_PSP_IO_BOUNCE_HIGH
#define TH08_PSP_FEATURE_IO_BOUNCE_HIGH 1
#else
#define TH08_PSP_FEATURE_IO_BOUNCE_HIGH 0
#endif
#if defined(TH08_PSP_FONT_STREAM_CACHE) && TH08_PSP_FONT_STREAM_CACHE
#define TH08_PSP_FEATURE_FONT_STREAM_CACHE 1
#else
#define TH08_PSP_FEATURE_FONT_STREAM_CACHE 0
#endif
#if defined(TH08_PSP_DEBUG_START_STAGE) && TH08_PSP_DEBUG_START_STAGE
#define TH08_PSP_FEATURE_DEBUG_START_STAGE 1
#else
#define TH08_PSP_FEATURE_DEBUG_START_STAGE 0
#endif
#if defined(TH08_PSP_STAGE_SCRIPT_ARENA) && TH08_PSP_STAGE_SCRIPT_ARENA
#define TH08_PSP_FEATURE_STAGE_SCRIPT_ARENA 1
#else
#define TH08_PSP_FEATURE_STAGE_SCRIPT_ARENA 0
#endif
#if defined(TH08_PSP_ANM_SCRATCH_COMPACT) && TH08_PSP_ANM_SCRATCH_COMPACT
#define TH08_PSP_FEATURE_ANM_SCRATCH_COMPACT 1
#else
#define TH08_PSP_FEATURE_ANM_SCRATCH_COMPACT 0
#endif
#if defined(TH08_PSP_GUI_BORDER_REPLAY_AUDIT) && TH08_PSP_GUI_BORDER_REPLAY_AUDIT
#define TH08_PSP_FEATURE_GUI_BORDER_REPLAY_AUDIT 1
#else
#define TH08_PSP_FEATURE_GUI_BORDER_REPLAY_AUDIT 0
#endif
#if defined(TH08_PSP_GUI_BORDER_REPLAY) && TH08_PSP_GUI_BORDER_REPLAY
#define TH08_PSP_FEATURE_GUI_BORDER_REPLAY 1
#else
#define TH08_PSP_FEATURE_GUI_BORDER_REPLAY 0
#endif
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
#define TH08_PSP_FEATURE_REPLAY_AUDIT 1
#else
#define TH08_PSP_FEATURE_REPLAY_AUDIT 0
#endif
#if defined(TH08_PSP_RENDER_VFPU) && TH08_PSP_RENDER_VFPU
#define TH08_PSP_FEATURE_RENDER_VFPU 1
#else
#define TH08_PSP_FEATURE_RENDER_VFPU 0
#endif
#if defined(TH08_PSP_X87_TRIG_CACHE) && TH08_PSP_X87_TRIG_CACHE
#define TH08_PSP_FEATURE_X87_TRIG_CACHE 1
#else
#define TH08_PSP_FEATURE_X87_TRIG_CACHE 0
#endif
#if defined(TH08_PSP_ANTITAMPER_SWAR) && TH08_PSP_ANTITAMPER_SWAR
#define TH08_PSP_FEATURE_ANTITAMPER_SWAR 1
#else
#define TH08_PSP_FEATURE_ANTITAMPER_SWAR 0
#endif
#if defined(TH08_PSP_RADIAL_TRAIL_TRIG_REUSE) && \
    TH08_PSP_RADIAL_TRAIL_TRIG_REUSE
#define TH08_PSP_FEATURE_RADIAL_TRAIL_TRIG_REUSE 1
#else
#define TH08_PSP_FEATURE_RADIAL_TRAIL_TRIG_REUSE 0
#endif
#if defined(TH08_PSP_FANTASY_SEAL_WORK_BOUNDS) && \
    TH08_PSP_FANTASY_SEAL_WORK_BOUNDS
#define TH08_PSP_FEATURE_FANTASY_SEAL_WORK_BOUNDS 1
#else
#define TH08_PSP_FEATURE_FANTASY_SEAL_WORK_BOUNDS 0
#endif
#if defined(TH08_PSP_EFFECT_OCCUPANCY_FASTPATH) && \
    TH08_PSP_EFFECT_OCCUPANCY_FASTPATH
#define TH08_PSP_FEATURE_EFFECT_OCCUPANCY_FASTPATH 1
#else
#define TH08_PSP_FEATURE_EFFECT_OCCUPANCY_FASTPATH 0
#endif
#if defined(TH08_PSP_EFFECT_OCCUPANCY_AUDIT) && TH08_PSP_EFFECT_OCCUPANCY_AUDIT
#define TH08_PSP_FEATURE_EFFECT_OCCUPANCY_AUDIT 1
#else
#define TH08_PSP_FEATURE_EFFECT_OCCUPANCY_AUDIT 0
#endif
#if defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
#define TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_AUDIT 1
#else
#define TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_AUDIT 0
#endif
#if defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
#define TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_FASTPATH 1
#else
#define TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_FASTPATH 0
#endif
#if defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS
#define TH08_PSP_FEATURE_EFFECT_INDEXED_QUADS 1
#else
#define TH08_PSP_FEATURE_EFFECT_INDEXED_QUADS 0
#endif
#if defined(TH08_PSP_LOCAL_FONT_SUBSET) && TH08_PSP_LOCAL_FONT_SUBSET
#define TH08_PSP_FEATURE_LOCAL_FONT_SUBSET 1
#else
#define TH08_PSP_FEATURE_LOCAL_FONT_SUBSET 0
#endif
#if defined(TH08_PSP_FONT_GLYPH_CACHE_RETAIN) && \
    TH08_PSP_FONT_GLYPH_CACHE_RETAIN
#define TH08_PSP_FEATURE_FONT_GLYPH_CACHE_RETAIN 1
#else
#define TH08_PSP_FEATURE_FONT_GLYPH_CACHE_RETAIN 0
#endif
#if defined(TH08_PSP_SLIMPLUS_GE4) && TH08_PSP_SLIMPLUS_GE4
#define TH08_PSP_FEATURE_SLIMPLUS_GE4 1
#else
#define TH08_PSP_FEATURE_SLIMPLUS_GE4 0
#endif

#if TH08_PSP_TH07_BOOT_PARITY
// Exact startup declaration parity with the hardware-proven TH07 port.
// Deliberately leave the main stack and heap threshold undeclared so PSPSDK's
// crt0 supplies the same defaults used by TH07.
PSP_MODULE_INFO("TH08 PSP Native", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-2048);
#else
// Reversible control for a single-variable hardware A/B.
PSP_MODULE_INFO("TH08 PSP Native", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(-1024);
PSP_HEAP_THRESHOLD_SIZE_KB(1024);
#endif

static_assert(sizeof(void *) == 4, "PSP runtime pointers must be 32-bit");

extern "C" int th08_psp_run_engine(int argc, char **argv);

namespace
{
char gLastBootPhase[64] = "bootstrap";
char gLastBootState[64] = "not_entered";

void CopyBootLabel(char *destination, std::size_t capacity, const char *source)
{
    if (capacity == 0)
    {
        return;
    }
    if (source == nullptr)
    {
        source = "<null>";
    }
    std::size_t index = 0;
    while (index + 1 < capacity && source[index] != '\0')
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

void LogProbe(const char *name, const th08::psp::FileProbe &probe)
{
    th08::psp::BootLog("DATA %s result=%s bytes=%llu magic=%.4s\n",
                       name,
                       th08::psp::ProbeResultName(probe.result),
                       static_cast<unsigned long long>(probe.observedBytes),
                       probe.observedMagic);
}

void LogMemory(const char *phase, const th08::psp::MemorySnapshot &memory)
{
    th08::psp::BootLog(
        "MEM phase=%s total_free=%lu largest_free=%lu edram_base=0x%08lx edram_bytes=%lu\n",
        phase,
        memory.totalFreeBytes,
        memory.largestFreeBlockBytes,
        static_cast<unsigned long>(memory.edramBase),
        memory.edramBytes);
}

bool FinalizeBootLogWithRetries()
{
    // A short bounded retry handles a transient Memory Stick write/sync error.
    // FinalizeBootLog's state machine never queues FINAL twice.
    for (unsigned int attempt = 0; attempt < 3; ++attempt)
    {
        if (th08::psp::FinalizeBootLog())
        {
            return true;
        }
    }
    return false;
}

void WaitForDiagnosticExit()
{
    std::uint32_t previousButtons = 0;
    while (th08::psp::PlatformRunning())
    {
        if (!th08::psp::PlatformSuspended())
        {
            SceCtrlData pad{};
            if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
            {
                const std::uint32_t buttons = pad.Buttons;
                const std::uint32_t pressed = buttons & ~previousButtons;
                previousButtons = buttons;
                if ((pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) != 0)
                {
                    th08::psp::PlatformRequestExit();
                }
            }
        }
        sceDisplayWaitVblankStart();
    }
}
} // namespace

extern "C" void th08_psp_boot_checkpoint(const char *phase,
                                           const char *state,
                                           int result)
{
    CopyBootLabel(gLastBootPhase, sizeof(gLastBootPhase), phase);
    CopyBootLabel(gLastBootState, sizeof(gLastBootState), state);

    const th08::psp::NewlibHeapGeometrySnapshot heap =
        th08::psp::CaptureNewlibHeapGeometry();
    const th08::psp::MemorySnapshot kernel =
        th08::psp::CaptureMemorySnapshot();
    // freeBytes describes only the currently committed dlmalloc arena.  It is
    // telemetry, never a capacity estimate or startup admission gate.
    th08::psp::BootLog(
        "BOOT_STAGE phase=%s state=%s result=%d "
        "heap_committed_free=%lu heap_no_grow_largest=%lu "
        "heap_scan_valid=%lu heap_scan_flags=0x%08lx "
        "kernel_free=%lu kernel_largest=%lu edram_bytes=%lu\n",
        phase != nullptr ? phase : "<null>",
        state != nullptr ? state : "<null>", result,
        static_cast<unsigned long>(heap.freeBytes),
        static_cast<unsigned long>(heap.largestNoGrowRequestBytes),
        static_cast<unsigned long>(heap.scanValid),
        static_cast<unsigned long>(heap.scanErrorFlags),
        static_cast<unsigned long>(kernel.totalFreeBytes),
        static_cast<unsigned long>(kernel.largestFreeBlockBytes),
        static_cast<unsigned long>(kernel.edramBytes));
    // Do not batch startup breadcrumbs: a PSP hard fault loses userspace and
    // the in-memory 4 KiB boot-log buffer immediately.
    th08::psp::FlushBootLog();
}

int main(int argc, char **argv)
{
    th08::psp::PlatformInitialize();
    th08::psp::FileIoInitialize(argc > 0 ? argv[0] : nullptr);
#if TH08_PSP_FEATURE_GO_IO_LAMP
    th08::psp::IoActivityLampInitialize();
#endif
#if TH08_PSP_FEATURE_PERF_ATTRIBUTION
    th08::psp::PerfAttributionInitialize();
#endif
    // PSPSDK's default exception installer imports the kernel-only
    // sceKernelRegisterDefaultExceptionHandler and cannot link into this
    // proven user-module contract.  Keep the build user-mode and rely on the
    // synchronously flushed breadcrumbs plus the orderly-failure screen.
    th08::psp::BootLog(
        "EXCEPTION_HANDLER installed=0 reason=user_module_kernel_import_forbidden\n");
    th08::psp::FlushBootLog();
    TH08_PSP_BOOT_CHECKPOINT("memory_telemetry", "before_initialize", 0);
    th08::psp::MemoryTelemetryInitialize(th08::psp::GameDirectory());
    TH08_PSP_BOOT_CHECKPOINT("memory_telemetry", "after_initialize", 0);
    TH08_PSP_BOOT_CHECKPOINT("ge4_prepare", "before", 0);
    const bool ge4Prepared = th08_psp_ge4_prepare() != 0;
    TH08_PSP_BOOT_CHECKPOINT("ge4_prepare", "after", ge4Prepared ? 1 : 0);
    TH08_PSP_BOOT_CHECKPOINT("anm_scratch", "before_initialize", 0);
    const bool anmScratchReady = th08::psp::AnmScratchInitialize();
    TH08_PSP_BOOT_CHECKPOINT("anm_scratch", "after_initialize",
                             anmScratchReady ? 1 : 0);
    th08::psp::MemoryTelemetryMarkPhase(anmScratchReady ? "anm_scratch_ready"
                                                        : "anm_scratch_failed");
    TH08_PSP_BOOT_CHECKPOINT("render_arena", "before_initialize", 0);
    const bool renderArenaReady = th08::psp::RenderResourceArenaInitialize();
    TH08_PSP_BOOT_CHECKPOINT("render_arena", "after_initialize",
                             renderArenaReady ? 1 : 0);
    th08::psp::MemoryTelemetryMarkPhase(renderArenaReady ? "render_arena_ready"
                                                         : "render_arena_failed");

    th08::psp::BootLog("TH08 PSP SC-ONLY ENGINE BRING-UP\n");
    th08::psp::BootLog("STATUS UNTESTED ON HARDWARE\n");
    th08::psp::BootLog("BUILD id=%s\n", TH08_PSP_BUILD_ID);
    th08::psp::BootLog("SOURCE th08_portable=%s\n", TH08_UPSTREAM_COMMIT);
    th08::psp::BootLog("SOURCE th08_oracle=%s\n", TH08_ORACLE_COMMIT);
    th08::psp::BootLog("SOURCE th07_backend=%s\n", TH07_BACKEND_COMMIT);
    th08::psp::BootLog("SOURCE th07_ge4_bridge=%s\n", TH07_GE4_BRIDGE_COMMIT);
    std::set_terminate(&TerminateHandler);
    th08::psp::BootLog(
        "FEATURE SC_ONLY=1 ME=DISABLED MIST=DISABLED engine=LINKED "
        "audio=SC_LINKED GE4_PREPARED=%d GE4_SLIMPLUS=%d "
        "TH07_BOOT_PARITY=%d "
        "SELECT_CADENCE=60_30_20 RENDER_CADENCE_INITIAL_MODE=%u "
        "HOME_ORDERLY_EXIT=1 "
        "COMPACT_BULLET_VM=%d BULLET_FASTPATH=%d BULLET_LIVE_ENUM=%d "
        "BULLET_TRANSFORM_AUDIT=%d BULLET_TRANSFORM_TERMINAL_FASTPATH=%d "
        "REPLAY_AUDIT=%d RUNTIME_TELEMETRY=%d PERF_ATTRIBUTION=%d "
        "DRAW_PRIORITY_SUBPROFILE=%d ITEM_UPDATE_SUBPROFILE=%d "
        "ITEM_ATAN2_FASTPATH_AUDIT=%d ITEM_ATAN2_FASTPATH=%d "
        "ITEM_SINCOS_FASTPATH_AUDIT=%d ITEM_SINCOS_FASTPATH=%d "
        "SOFTFLOAT_CENSUS=%d "
        "AUDIO_FIXED_CURSOR_AUDIT=%d AUDIO_FIXED_CURSOR=%d "
        "TRIG_DF_FASTPATH_AUDIT=%d TRIG_DF_FASTPATH=%d "
        "PERF_ENV=%d "
        "SWAP_NOWAIT=%d SWAP_ASYNC=%d "
        "FLIP_GUARD_COLOR_ONLY=%d "
        "TICK_GATE_BYPASS=%d "
        "SWAP_TRIPLE=%d "
        "PSPGL_STREAM_ARENA=%d "
        "REPLAY_RESERVE_RECYCLE=%d DIALOGUE_SNAPSHOT_NO_PROMOTE=%d "
        "DIALOGUE_SNAPSHOT_DIAG=%d "
        "GO_IO_LAMP=%d "
        "IO_SERIALIZE=%d DIALOGUE_SNAPSHOT_AT_BACKGROUND=%d "
        "DIALOGUE_LIVE_BACKGROUND=%d IO_BOUNCE_HIGH=%d FONT_STREAM_CACHE=%d "
        "DEBUG_START_STAGE=%d STAGE_SCRIPT_ARENA=%d ANM_SCRATCH_COMPACT=%d "
        "GUI_BORDER_REPLAY_AUDIT=%d GUI_BORDER_REPLAY=%d "
        "RENDER_VFPU=%d X87_TRIG_CACHE=%d ANTITAMPER_SWAR=%d "
        "RADIAL_TRAIL_TRIG_REUSE=%d "
        "FANTASY_SEAL_WORK_BOUNDS=%d "
        "EFFECT_OCCUPANCY_FASTPATH=%d EFFECT_OCCUPANCY_AUDIT=%d EFFECT_SPRITE_PAIR_AUDIT=%d "
        "EFFECT_SPRITE_PAIR_FASTPATH=%d "
        "EFFECT_INDEXED_QUADS=%d "
        "PREPARE_STATE_CACHE=%d "
        "BULLET_UNIFIED_QUADS=%d "
        "BULLET_DIRECT_GE=%d BULLET_ONEPASS_4V_AUDIT=%d "
        "BULLET_ONEPASS_4V_FASTPATH=%d BULLET_PACKED_VERTEX_AUDIT=%d "
        "BULLET_PACKED_VERTEX_FASTPATH=%d "
        "BULLET_MIXED_QUADS_AUDIT=%d "
        "BULLET_MIXED_QUADS_FASTPATH=%d ITEM_MIXED_QUADS_AUDIT=%d "
        "ITEM_MIXED_QUADS_FASTPATH=%d ITEM_DIRECT_GE=%d "
        "ITEM_NATURAL_QUADS=%d ITEM_NATURAL_NATIVE_COPY=%d "
        "PLAYER_SCAN_SIDECAR=%d ENEMY_ACTIVE_BITMAP_AUDIT=%d "
        "BULLET_COLLISION_GATE_AUDIT=%d BULLET_COLLISION_GATE=%d "
        "CANCEL_EMPTY_FASTPATH=%d BULLET_CANCEL_SPATIAL=%d "
        "ITEM_AUTOCOLLECT_CACHE_AUDIT=%d ITEM_AUTOCOLLECT_CACHE=%d "
        "ITEM_TIME_SPAWN_INIT_AUDIT=%d ITEM_TIME_SPAWN_INIT_FASTPATH=%d "
        "ITEM_TIME_ANM_IDLE_AUDIT=%d ITEM_TIME_ANM_IDLE_FASTPATH=%d "
        "ITEM_TIME_DRAW_PAIR_AUDIT=%d ITEM_TIME_DRAW_PAIR_FASTPATH=%d "
        "ITEM_TIME_INLINE_DRAW_AUDIT=%d ITEM_TIME_INLINE_DRAW_FASTPATH=%d "
        "ASCII_POPUP_OCCUPANCY=%d ASCII_POPUP_BATCH=%d "
        "ASCII_POPUP_DIRECT_PAIR=%d "
        "SCORE_POPUP_NATIVE_GE=%d "
        "STAGE_RELATIVE_PERF_SAMPLING=%d "
        "LOCAL_FONT_SUBSET=%d FONT_GLYPH_CACHE_RETAIN=%d\n",
        ge4Prepared ? 1 : 0, TH08_PSP_FEATURE_SLIMPLUS_GE4,
        TH08_PSP_TH07_BOOT_PARITY,
        static_cast<unsigned int>(
            th08::psp::ConfiguredRenderCadenceInitialMode()),
        TH08_PSP_FEATURE_COMPACT_BULLET_VM,
        TH08_PSP_FEATURE_BULLET_FASTPATH,
        TH08_PSP_FEATURE_BULLET_LIVE_ENUM,
        TH08_PSP_FEATURE_BULLET_TRANSFORM_AUDIT,
        TH08_PSP_FEATURE_BULLET_TRANSFORM_TERMINAL_FASTPATH,
        TH08_PSP_FEATURE_REPLAY_AUDIT,
        TH08_PSP_RUNTIME_TELEMETRY,
        TH08_PSP_FEATURE_PERF_ATTRIBUTION,
        TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE,
        TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE,
        TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH_AUDIT,
        TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH,
        TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH_AUDIT,
        TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH,
        TH08_PSP_FEATURE_SOFTFLOAT_CENSUS,
        TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR_AUDIT,
        TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR,
        TH08_PSP_FEATURE_TRIG_DF_FASTPATH_AUDIT,
        TH08_PSP_FEATURE_TRIG_DF_FASTPATH,
        TH08_PSP_FEATURE_PERF_ENV,
        TH08_PSP_FEATURE_SWAP_NOWAIT,
        TH08_PSP_FEATURE_SWAP_ASYNC,
        TH08_PSP_FEATURE_FLIP_GUARD_COLOR_ONLY,
        TH08_PSP_FEATURE_TICK_GATE_BYPASS,
        TH08_PSP_FEATURE_SWAP_TRIPLE,
        TH08_PSP_FEATURE_PSPGL_STREAM_ARENA,
        TH08_PSP_FEATURE_REPLAY_RESERVE_RECYCLE,
        TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_NO_PROMOTE,
        TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_DIAG,
        TH08_PSP_FEATURE_GO_IO_LAMP,
        TH08_PSP_FEATURE_IO_SERIALIZE,
        TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_AT_BACKGROUND,
        TH08_PSP_FEATURE_DIALOGUE_LIVE_BACKGROUND,
        TH08_PSP_FEATURE_IO_BOUNCE_HIGH,
        TH08_PSP_FEATURE_FONT_STREAM_CACHE,
        TH08_PSP_FEATURE_DEBUG_START_STAGE,
        TH08_PSP_FEATURE_STAGE_SCRIPT_ARENA,
        TH08_PSP_FEATURE_ANM_SCRATCH_COMPACT,
        TH08_PSP_FEATURE_GUI_BORDER_REPLAY_AUDIT,
        TH08_PSP_FEATURE_GUI_BORDER_REPLAY,
        TH08_PSP_FEATURE_RENDER_VFPU,
        TH08_PSP_FEATURE_X87_TRIG_CACHE,
        TH08_PSP_FEATURE_ANTITAMPER_SWAR,
        TH08_PSP_FEATURE_RADIAL_TRAIL_TRIG_REUSE,
        TH08_PSP_FEATURE_FANTASY_SEAL_WORK_BOUNDS,
        TH08_PSP_FEATURE_EFFECT_OCCUPANCY_FASTPATH,
        TH08_PSP_FEATURE_EFFECT_OCCUPANCY_AUDIT,
        TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_AUDIT,
        TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_FASTPATH,
        TH08_PSP_FEATURE_EFFECT_INDEXED_QUADS,
        TH08_PSP_FEATURE_PREPARE_STATE_CACHE,
        TH08_PSP_FEATURE_BULLET_UNIFIED_QUADS,
        TH08_PSP_FEATURE_BULLET_DIRECT_GE,
        TH08_PSP_FEATURE_BULLET_ONEPASS_4V_AUDIT,
        TH08_PSP_FEATURE_BULLET_ONEPASS_4V_FASTPATH,
        TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_AUDIT,
        TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_FASTPATH,
        TH08_PSP_FEATURE_BULLET_MIXED_QUADS_AUDIT,
        TH08_PSP_FEATURE_BULLET_MIXED_QUADS_FASTPATH,
        TH08_PSP_FEATURE_ITEM_MIXED_QUADS_AUDIT,
        TH08_PSP_FEATURE_ITEM_MIXED_QUADS_FASTPATH,
        TH08_PSP_FEATURE_ITEM_DIRECT_GE,
        TH08_PSP_FEATURE_ITEM_NATURAL_QUADS,
        TH08_PSP_FEATURE_ITEM_NATURAL_NATIVE_COPY,
        TH08_PSP_FEATURE_PLAYER_SCAN_SIDECAR,
        TH08_PSP_FEATURE_ENEMY_ACTIVE_BITMAP_AUDIT,
        TH08_PSP_FEATURE_BULLET_COLLISION_GATE_AUDIT,
        TH08_PSP_FEATURE_BULLET_COLLISION_GATE,
        TH08_PSP_FEATURE_CANCEL_EMPTY_FASTPATH,
        TH08_PSP_FEATURE_BULLET_CANCEL_SPATIAL,
        TH08_PSP_FEATURE_ITEM_AUTOCOLLECT_CACHE_AUDIT,
        TH08_PSP_FEATURE_ITEM_AUTOCOLLECT_CACHE,
        TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_AUDIT,
        TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_FASTPATH,
        TH08_PSP_FEATURE_ITEM_TIME_ANM_IDLE_AUDIT,
        TH08_PSP_FEATURE_ITEM_TIME_ANM_IDLE_FASTPATH,
        TH08_PSP_FEATURE_ITEM_TIME_DRAW_PAIR_AUDIT,
        TH08_PSP_FEATURE_ITEM_TIME_DRAW_PAIR_FASTPATH,
        TH08_PSP_FEATURE_ITEM_TIME_INLINE_DRAW_AUDIT,
        TH08_PSP_FEATURE_ITEM_TIME_INLINE_DRAW_FASTPATH,
        TH08_PSP_FEATURE_ASCII_POPUP_OCCUPANCY,
        TH08_PSP_FEATURE_ASCII_POPUP_BATCH,
        TH08_PSP_FEATURE_ASCII_POPUP_DIRECT_PAIR,
        TH08_PSP_FEATURE_SCORE_POPUP_NATIVE_GE,
        TH08_PSP_FEATURE_STAGE_RELATIVE_PERF_SAMPLING,
        TH08_PSP_FEATURE_LOCAL_FONT_SUBSET,
        TH08_PSP_FEATURE_FONT_GLYPH_CACHE_RETAIN);
#if TH08_PSP_TH07_BOOT_PARITY
    th08::psp::BootLog(
        "BOOT_CONTRACT module_attr=0 version=1.0 stack=pspsdk_default "
        "heap_kb=-2048 threshold=pspsdk_default source=TH07\n");
#else
    th08::psp::BootLog(
        "BOOT_CONTRACT module_attr=user version=0.1 stack_kb=512 "
        "heap_kb=-1024 threshold_kb=1024 source=TH08_control\n");
#endif
    th08::psp::BootLog("PATH game=%s\n", th08::psp::GameDirectory());
    th08::psp::BootLog("PATH log=%s\n", th08::psp::BootLogPath());

    const th08::psp::MemorySnapshot startupMemory = th08::psp::CaptureMemorySnapshot();
    LogMemory("startup", startupMemory);
    th08::psp::BootLog("SYSTEM devkit=0x%08lx clock_cpu=%d clock_bus=%d\n",
                       startupMemory.devkitVersion,
                       startupMemory.cpuClockMhz,
                       startupMemory.busClockMhz);

    const th08::psp::DataDiscovery &data = th08::psp::DiscoverOriginalData();
    th08::psp::BootLog("DATA root=%s ready=%d\n",
                       data.root[0] != '\0' ? data.root : "not-found",
                       data.ready ? 1 : 0);
    LogProbe("th08.dat", data.gameArchive);
    LogProbe("thbgm.dat", data.bgmArchive);

    const th08::psp::MemorySnapshot postProbeMemory = th08::psp::CaptureMemorySnapshot();
    LogMemory("post_data_probe", postProbeMemory);

    if (data.ready)
    {
        const int chdirResult = sceIoChdir(data.root);
        th08::psp::BootLog("ENGINE data_chdir=%d root=%s\n", chdirResult, data.root);
        if (chdirResult >= 0)
        {
            th08::psp::BootLog("PHASE engine_handoff\n");
            th08::psp::MemoryTelemetryMarkPhase("engine_handoff");
            th08::psp::FlushBootLog();

            TH08_PSP_BOOT_CHECKPOINT("engine", "before_entry", 0);
            const int engineResult = th08_psp_run_engine(argc, argv);
            TH08_PSP_BOOT_CHECKPOINT("engine", "after_return", engineResult);
            // WinMain has released the D3D device, SDL/PSPGL context, and every
            // upper texture owner before returning here.
            th08_psp_ge4_shutdown();
            th08::psp::MemoryTelemetryMarkPhase("engine_exit");
            th08::psp::BootLog("PHASE engine_exit result=%d\n", engineResult);
            LogMemory("engine_exit", th08::psp::CaptureMemorySnapshot());
            if (engineResult != 0 && th08::psp::PlatformRunning())
            {
                const bool failureVideoReady = th08::psp::VideoInitialize();
                th08::psp::BootLog(
                    "VIDEO engine_failure_gu_init=%d phase=%s state=%s result=%d\n",
                    failureVideoReady ? 1 : 0, gLastBootPhase, gLastBootState,
                    engineResult);
                if (failureVideoReady)
                {
                    th08::psp::RenderEngineFailureStatus(
                        gLastBootPhase, gLastBootState, engineResult,
                        th08::psp::CaptureMemorySnapshot());
                }
                th08::psp::FlushBootLog();
                WaitForDiagnosticExit();
                th08::psp::VideoShutdown();
            }
            th08::psp::MemoryTelemetryShutdown();
            FinalizeBootLogWithRetries();
            sceKernelExitGame();
            return engineResult;
        }
        th08::psp::BootLog("ENGINE handoff_blocked reason=data_chdir_failed\n");
    }
    else
    {
        th08::psp::BootLog("ENGINE handoff_blocked reason=original_data_not_ready\n");
    }

    const bool videoReady = th08::psp::VideoInitialize();
    th08::psp::BootLog("VIDEO gu_init=%d framebuffer=RGB565 double_buffer=1 depth=16\n",
                       videoReady ? 1 : 0);
    if (videoReady)
    {
        th08::psp::RenderBootstrapStatus(data, startupMemory, postProbeMemory);
    }
    th08::psp::BootLog("PHASE diagnostic_fallback_ready\n");
    th08::psp::FlushBootLog();

    WaitForDiagnosticExit();

    th08::psp::BootLog("PHASE orderly_exit\n");
    th08::psp::MemoryTelemetryMarkPhase("diagnostic_exit");
    LogMemory("exit", th08::psp::CaptureMemorySnapshot());
    th08::psp::VideoShutdown();
    th08_psp_ge4_shutdown();
    th08::psp::MemoryTelemetryShutdown();
    FinalizeBootLogWithRetries();
    sceKernelExitGame();
    return 0;
}
