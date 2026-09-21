#!/usr/bin/env bash
# r247 rendering/simulation, dedicated Main-RAM-only ME audio service.
# The inherited native recipe cleans; never enable the general ME core.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash tools/build_go_r247_musicroom_20260922.sh \
  TH08_PSP_ME_CORE=0 TH08_PSP_ME_AUDIO_MIX=1 \
  TH08_PSP_BUILD_ID=r249T-audio-me-20260922 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r249T-audio-me)' "$@"
python3 tools/check_r249_mecc_link.py
