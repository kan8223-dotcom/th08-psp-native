#!/usr/bin/env bash
# Public Go r249: MECC audio, no diagnostic log output or CPU graph.
# Preserve simulation/cadence and all ME validation/fallback/stop checks.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec bash tools/build_go_r249_audio_me_20260922.sh \
  TH08_PSP_LOGGING=0 TH08_PSP_USAGE_METER=0 \
  TH08_PSP_RUNTIME_TELEMETRY=0 TH08_REPLAY_SYNC_AUDIT=0 \
  TH08_PSP_DEBUG_START_STAGE=0 \
  TH08_PSP_BUILD_ID=r249T-audio-me-nolog-20260922 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r249 MECC)' "$@"
