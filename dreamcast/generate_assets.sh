#!/usr/bin/env bash
# ---------------------------------------------------------------------
# Sonic CD / RSDKv3 — Dreamcast asset conversion
#
#   ./generate_assets.sh <path/to/Data.rsdk> [stage_dir]
#
# Unpacks Data.rsdk and converts the two asset classes the Dreamcast build
# cannot read in their shipped form, leaving everything else alone.
#
# WHAT COMES OUT, AND WHERE
#
#   <stage_dir>/Data/Music/**.ogg     headerless signed 8-bit PCM, mono, 22050
#   <stage_dir>/Data/SoundFX/**.wav   RIFF WAV, unsigned 8-bit, mono, per-file rate
#
# Put that Data/ folder on the disc NEXT TO the untouched Data.rsdk. Reader.cpp
# prefers a loose file over the packed one, so the converted music and sfx win
# and every other asset -- sprites, stages, scripts, palettes -- still streams
# out of Data.rsdk. That is why this script does not unpack a whole game tree.
#
# WHY NOT JUST REUSE THE SONIC MANIA DREAMCAST SCRIPTS
#
# Because they target a different engine and would produce wrong data twice:
#
#   Mania music -> `-ac 2`, stereo.        Ours is MONO (DC_MUSIC_CHANNELS = 1).
#   Mania sfx   -> Yamaha ADPCM via        Ours is plain 8-bit PCM. No ADPCM,
#                  wav2adpcm.              and no fixed rate -- see below.
#
# REQUIREMENTS: bash, python3, ffmpeg, ffprobe.
# ---------------------------------------------------------------------
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
usage() { printf 'Usage: %s <path/to/Data.rsdk> [stage_dir]\n' "${0##*/}" >&2; exit 2; }

[[ $# -ge 1 && $# -le 2 ]] || usage

rsdk=$1
[[ -f "$rsdk" ]] || die "not a file: $rsdk"

for tool in python3 ffmpeg ffprobe; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool not found in PATH"
done

script_dir=$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
rsdk_abs=$(cd -P -- "$(dirname -- "$rsdk")" && pwd -P)/$(basename -- "$rsdk")

# Default the stage next to Data.rsdk -- that is build-dc or build-pc, which is
# exactly where the loose Data/ tree has to end up anyway.
stage_in=${2:-$(dirname -- "$rsdk_abs")}
mkdir -p -- "$stage_in"
stage=$(cd -P -- "$stage_in" && pwd -P)

work="$stage/.rsdk_extract"
rm -rf -- "$work"
trap 'rm -rf -- "$work"' EXIT

printf -- '-- extracting %s\n' "$(basename -- "$rsdk_abs")"
python3 "$script_dir/rsdkv3_extract.py" "$rsdk_abs" "$work"

for d in Music SoundFX; do
  [[ -d "$work/Data/$d" ]] || die "Data.rsdk has no Data/$d -- is this a Sonic CD datafile?"
done

# Only these two classes are staged loose; everything else stays packed.
mkdir -p -- "$stage/Data"
rm -rf -- "$stage/Data/Music" "$stage/Data/SoundFX"
cp -R -- "$work/Data/Music"   "$stage/Data/"
cp -R -- "$work/Data/SoundFX" "$stage/Data/"

printf -- '-- converting music\n'
"$script_dir/music_step_1_ogg_to_pcm.sh"    "$stage/Data"
"$script_dir/music_step_2_rename_to_ogg.sh" "$stage/Data"

printf -- '-- converting sound effects\n'
"$script_dir/sfx_step_1_resample.sh"        "$stage/Data"

printf -- '-- DONE: %s/Data\n' "$stage"
exit 0
