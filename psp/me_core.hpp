#pragma once
// TH08_PSP_ME_CORE: boot the Media Engine through m-c/d's me-custom-core
// (MECC) and run a worker loop on it.  Slim+ models only (PSP-2000/3000/Go/
// Street); skipped on PSP-1000 and on PPSSPP.  The first job is a "hello"
// checksum that feeds the ME lane of the usage meter; background/effect
// vertex jobs come next.  Default OFF.
#include <cstdint>
#if defined(PSP) && defined(TH08_PSP_ME_CORE) && TH08_PSP_ME_CORE
#define TH08_PSP_ME_CORE_ENABLED 1
#else
#define TH08_PSP_ME_CORE_ENABLED 0
#endif
extern "C"
{
// Boot the ME (idempotent).  Returns 1 when the worker reported READY.
int th08_me_core_init(void);
// Per Present: collect finished jobs, submit the next one, feed the meter.
void th08_me_core_frame(void);
// Stop the worker before the process exits or the PSP suspends.
void th08_me_core_shutdown(void);
// Emergency exit: notification only; no logging, locks, joins or resource frees.
void th08_me_core_request_stop(void);
// "CORE" / "DISABLED" / "SKIPPED" for the FEATURE line.
const char *th08_me_core_feature_string(void);
int th08_me_core_ready(void);
// Generic job: `job` is a PspMeEffectJob (me_effect_shadow.hpp).  Returns 1 when accepted.
int th08_me_core_submit_job(const void *job);
// kind 2 = effect job (PspMeEffectJob), 3 = bullet job (PspMeBulletJob).  Two jobs may be in flight.
int th08_me_core_submit_job_kind(const void *job, unsigned int kind);
// 1 when the job submitted with that header has completed.
int th08_me_core_job_done(const void *job);
}
