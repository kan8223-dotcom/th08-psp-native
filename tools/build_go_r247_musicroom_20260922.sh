#!/usr/bin/env bash
# Exact text box-filter specialization and native row-only atlas updates.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
exec bash tools/build_go_r246_sprites_dialogue_20260922.sh \
  TH08_PSP_BUILD_ID=r247T-musicroom-20260922 \
  'PSP_EBOOT_TITLE=Touhou 8 PSP Native (Go r247T-musicroom)' "$@"
