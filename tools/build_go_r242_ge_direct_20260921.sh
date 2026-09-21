#!/usr/bin/env bash
# Go SC-only/lite settings + direct GE render state and packed submission.
# Keep the existing Go trig table (including the pre-existing r240 evaluator).
# Clean by default: this lane's BUILD_ID and compiler flags are not all stamped.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ "${CLEAN_BUILD:-1}" == 1 ]]; then
  make -f Makefile.psp clean
fi
# The SDK does not make PARAM.SFO depend on the title command-line value.
rm -f PARAM.SFO
exec bash tools/build_r239_trigtable_20260921.sh \
  TH08_PSP_LOGGING=1 TH08_PSP_DEBUG_START_STAGE=0 \
  TH08_PSP_BULLET_UPDATE_SUBPROFILE=0 \
  TH08_PSP_ME_CORE=0 TH08_PSP_ME_EFFECT_ADOPT=0 \
  TH08_PSP_ME_BULLET_ADOPT=1 TH08_PSP_ME_BG_ADOPT=0 \
  TH08_PSP_ME_BULLET_MOVE=0 TH08_PSP_ME_POPUP=0 \
  TH08_PSP_GE_DRAW_DIRECT=1 \
  TH08_PSP_BUILD_ID=r242T-lite-ge-20260921 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r242T-lite-ge)' "$@"
