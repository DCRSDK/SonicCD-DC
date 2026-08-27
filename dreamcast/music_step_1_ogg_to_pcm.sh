#!/usr/bin/env bash
# Sonic CD / RSDKv3 Dreamcast — music.
#
# No Vorbis decoder is linked into the Dreamcast build (no libtremor, no libogg),
# so nothing on the disc may still be a container. Music becomes HEADERLESS
# SIGNED 8-BIT PCM, mono, 22050 Hz, streamed straight off the GD-ROM.
#
# MONO is not a size compromise, it is what the assets are: side/mid energy in
# Sonic CD's tracks measures 2.9%, and ZoneComplete/GameOver run 8.4s/10.7s as
# mono, which are the correct jingle lengths. Halved, they would be nonsense.
# If you re-convert in stereo you must also set DC_MUSIC_CHANNELS to 2.
#
# 22050 regardless of the device rate: DC_MusicReadSamples holds each source
# frame for DC_MUSIC_STRETCH output frames, so a 44100 device costs no extra
# disc bandwidth. Music is the one thing that does NOT want to be native rate.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "Usage: ${0##*/} <stage_dir>"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
[[ -d "$stage/Music" ]] || die "missing directory: $stage/Music"

command -v ffmpeg >/dev/null 2>&1 || die "ffmpeg not found in PATH"

orig_dir=$(pwd -P)
restore_dir() { cd -- "$orig_dir" || true; }
trap restore_dir EXIT

stage_abs=$(cd -P -- "$stage" && pwd -P)
cd -- "$stage_abs/Music"

n=0
while IFS= read -r -d '' in_file; do
  out_file="${in_file%.*}.s8"
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$in_file" \
    -ar 22050 -ac 1 -f s8 -- "$out_file"
  n=$((n+1))
done < <(find . -type f \( -iname '*.ogg' \) -print0)

printf 'music: %d tracks converted to headerless s8 mono 22050\n' "$n"
