#pragma once

// Process-wide owner of the 4 MiB GE aperture lifecycle. The frozen model-3
// path and the TH07-hardware-proven Slim+ predicate both retain the same
// physical-size/base/visible-size/readback and fail-closed gates. Model 0
// remains on the ordinary 2 MiB path.
//
// Prepare must run while the visible aperture is still exactly 2 MiB.  Enable
// must run only after the renderer has drained GE.  Shutdown must run after the
// renderer has released every upper-eDRAM allocation, drained GE, and
// terminated its GU/PSPGL context.  A safely rejected platform remains on the
// ordinary 2 MiB path; an uncertain aperture or power-lock state never returns
// and requires a cold power-off.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Th08PspGe4UpperTelemetry
{
    unsigned int allocated_bytes;
    unsigned int freed_bytes;
    unsigned int live_bytes;
    unsigned int peak_live_bytes;
    unsigned int allocation_count;
    unsigned int free_count;
    unsigned int live_allocation_count;
    unsigned int peak_live_allocation_count;
    unsigned int promotion_attempt_count;
    unsigned int promotion_success_count;
    unsigned int promotion_fallback_count;
    unsigned int promotion_requested_bytes;
    unsigned int lower_eviction_block_count;
    unsigned int compaction_block_count;
    unsigned int upper_alloc_no_aperture_count;
    unsigned int upper_alloc_capacity_count;
    unsigned int upper_alloc_fragmentation_count;
    unsigned int upper_alloc_metadata_count;
    unsigned int upper_largest_gap_bytes;
    unsigned int upper_smallest_largest_gap_bytes;
    unsigned int upper_map_denied_count;
    unsigned int static_upload_depth;
    unsigned int violations;
} Th08PspGe4UpperTelemetry;

int th08_psp_ge4_prepare(void);
int th08_psp_ge4_enable_after_gu_idle(void);
int th08_psp_ge4_active(void);
int th08_psp_ge4_power_lock_held(void);
void th08_psp_ge4_fail_closed(const char *reason);
void th08_psp_ge4_shutdown(void);

// Renderer/allocator telemetry hooks.  They never allocate memory.  The range
// predicate returns true only while the proven 4 MiB path is active, preventing
// PPSSPP's mirrored 2 MiB window from being mistaken for independent upper
// eDRAM.  The allocation owner must pass the same address and byte count on
// release; shutdown refuses to shrink while telemetry still describes a live
// upper allocation.
int th08_psp_ge4_is_upper_range(const void *address, unsigned int bytes);
void th08_psp_ge4_note_upper_alloc(const void *address, unsigned int bytes);
void th08_psp_ge4_note_upper_free(const void *address, unsigned int bytes);
void th08_psp_ge4_note_promotion(unsigned int bytes, int promoted,
                                 unsigned int texture_id,
                                 unsigned int width, unsigned int height,
                                 unsigned int hardware_format,
                                 unsigned int old_domain,
                                 const void *new_address);
void th08_psp_ge4_note_allocator_block(int compaction);
void th08_psp_ge4_note_upper_alloc_failure(int reason,
                                           unsigned int requested_bytes,
                                           unsigned int total_free_bytes,
                                           unsigned int largest_gap_bytes);
void th08_psp_ge4_note_upper_map_denied(const void *address,
                                        unsigned int bytes);
void th08_psp_ge4_get_upper_telemetry(Th08PspGe4UpperTelemetry *out);

// A nestable hint for immutable/static texture uploads.  It is deliberately
// only policy metadata: the bridge never chooses an allocation or moves data.
// PSPGL integration may query it while selecting its render-only backing.
void th08_psp_ge4_static_upload_hint_begin(void);
void th08_psp_ge4_static_upload_hint_end(void);
int th08_psp_ge4_static_upload_hint_active(void);

#ifdef __cplusplus
}
#endif
