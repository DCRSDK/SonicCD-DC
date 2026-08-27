#!/usr/bin/env bash
# Put the .ogg name back on the raw PCM.
#
# GameConfig.bin and the StageConfigs resolve tracks BY FILENAME, and those names
# end in .ogg. Only the contents changed in step 1, so the extension has to go
# back or nothing resolves. The loader sniffs for the "OggS" magic and refuses a
# file that is still a real container, so a half-converted set fails loudly
# instead of playing noise.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "Usage: ${0##*/} <stage_dir>"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
[[ -d "$stage/Music" ]] || die "missing directory: $stage/Music"

orig_dir=$(pwd -P)
restore_dir() { cd -- "$orig_dir" || true; }
trap restore_dir EXIT

stage_abs=$(cd -P -- "$stage" && pwd -P)
cd -- "$stage_abs/Music"

while IFS= read -r -d '' f; do
  mv -f -- "$f" "${f%.*}.ogg"
done < <(find . -type f -iname '*.s8' -print0)
