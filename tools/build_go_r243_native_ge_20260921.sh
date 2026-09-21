#!/usr/bin/env bash
# Full native Go texture/list/display pipeline; simulation flags stay r242.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ "${CLEAN_BUILD:-1}" == 1 ]]; then
  make -f Makefile.psp TH08_PSP_NATIVE_GE=1 clean
fi
rm -f PARAM.SFO
exec env CLEAN_BUILD=0 bash tools/build_go_r242_ge_direct_20260921.sh \
  TH08_PSP_NATIVE_GE=1 TH08_PSP_GE_DRAW_DIRECT=0 TH08_PSP_GE_2D_DIRECT=0 \
  TH08_PSP_PSPGL_STREAM_ARENA=0 TH08_PSP_PSPGL_STREAM_LIST=0 \
  TH08_PSP_SWAP_TRIPLE=0 TH08_PSP_SWAP_ASYNC=0 TH08_PSP_SWAP_NOWAIT=0 \
  TH08_PSP_FLIP_GUARD_COLOR_ONLY=0 \
  TH08_PSP_BUILD_ID=r243T-native-ge-20260921 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r243T-native-ge)' "$@"
