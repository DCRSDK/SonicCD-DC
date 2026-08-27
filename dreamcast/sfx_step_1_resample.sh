#!/usr/bin/env bash
# Sonic CD / RSDKv3 Dreamcast — sound effects.
#
# TARGET FORMAT, taken from the validated Data folder the shipping CDIs are
# built from (all 74 files measured):
#
#   RIFF WAV, PCM, mono, UNSIGNED 8-bit
#   22050 Hz  -- 72 of 74 files
#   44100 Hz  -- Ring.wav and LoseRings.wav, and only those two
#
# WHY THOSE TWO ARE DIFFERENT
#
# Ring.wav and LoseRings.wav carry 13.5% and 11.0% of their energy above 11kHz.
# Every other sound in the game is under 5%. A 22050 signal cannot represent
# anything above 11025Hz, so downsampling those two throws away the part that
# makes them sound like themselves -- they came out tinny, and nothing else did.
# They are the exception because they were measured to be the exception.
#
# WHY NOT KEEP EVERYTHING AT THE SOURCE 44100
#
# Because 22050 is what this port was tuned and tested against. It halves the
# sfx footprint, and the AICA's 65534-sample ceiling reaches 2.97s at 22050
# against 1.49s at 44100, so MORE sounds land on hardware voices, not fewer.
# Under this rule 70 of 74 play on AICA voices; the four that do not are
# Achievement, TimeWarp, BombCarrier (too long at any rate) and LoseRings
# (deliberately native).
#
# RIFF, not headerless: the loader assumes DC_SRC_RATE for a headerless file, so
# a per-file rate only survives if there is a header to carry it. That is the
# whole reason the two exceptions above can exist at all.
set -euo pipefail
IFS=$'\n\t'

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "Usage: ${0##*/} <stage_dir>"
stage=$1
[[ -d "$stage" ]] || die "not a directory: $stage"
[[ -d "$stage/SoundFX" ]] || die "missing directory: $stage/SoundFX"

command -v ffmpeg  >/dev/null 2>&1 || die "ffmpeg not found in PATH"
command -v ffprobe >/dev/null 2>&1 || die "ffprobe not found in PATH"

SFX_RATE=22050                                  # everything, unless listed below
SFX_NATIVE_RATE=44100                           # the two bright ones
SFX_NATIVE=( "Ring.wav" "LoseRings.wav" )
AICA_MAX_SAMPLES=65534                          # reporting only

orig_dir=$(pwd -P)
restore_dir() { cd -- "$orig_dir" || true; }
trap restore_dir EXIT

stage_abs=$(cd -P -- "$stage" && pwd -P)
cd -- "$stage_abs/SoundFX"

hw=0; sw=0
while IFS= read -r -d '' file; do
  base=$(basename -- "$file")

  rate=$SFX_RATE
  for k in "${SFX_NATIVE[@]}"; do
    [[ "$base" == "$k" ]] && rate=$SFX_NATIVE_RATE
  done

  dur=$(ffprobe -v error -select_streams a:0 -show_entries stream=duration \
        -of default=nokey=1:noprint_wrappers=1 -- "$file" | head -n1 || true)
  [[ -n "$dur" && "$dur" != "N/A" ]] || die "cannot read duration: $file"

  if awk -v d="$dur" -v r="$rate" -v lim="$AICA_MAX_SAMPLES" 'BEGIN{exit !(d*r <= lim)}'; then
    hw=$((hw+1))
  else
    sw=$((sw+1))
    printf '  %-30s %5.2fs @ %d Hz -> over %d samples, software mixer\n' \
      "${file#./}" "$dur" "$rate" "$AICA_MAX_SAMPLES"
  fi

  tmp="${file%.wav}.tmp.wav"
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$file" \
    -ar "$rate" -ac 1 -acodec pcm_u8 -- "$tmp"
  mv -f -- "$tmp" "$file"
done < <(find . -type f -iname '*.wav' -print0)

printf 'sfx: %d on AICA voices, %d on the software mixer\n' "$hw" "$sw"
