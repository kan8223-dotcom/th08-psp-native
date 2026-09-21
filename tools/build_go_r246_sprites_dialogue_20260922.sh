#!/usr/bin/env bash
# Cached final vertices and TH07-style pre-rendered dialogue rows; sim unchanged.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec bash tools/build_go_r245_thread_reap_20260922.sh \
  TH08_PSP_FONT_GLYPH_CACHE_RETAIN=1 \
  TH08_PSP_BUILD_ID=r246T-sprites-dialogue-20260922 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r246T-sprites-dialogue)' "$@"
