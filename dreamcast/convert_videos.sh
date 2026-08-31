#!/usr/bin/env bash
# ---------------------------------------------------------------------
#  Sonic CD FMV -> Dreamcast (MPEG-1, decodable in real time on an SH4)
# ---------------------------------------------------------------------
#  Linux/macOS twin of convert_videos.bat. THE TWO MUST BE KEPT IN STEP:
#  same six mappings, same ffmpeg flags, same verification. If you change
#  one, change the other, and diff them afterwards — the batch file's
#  comment header once drifted a whole encoder profile away from its own
#  command line, and that is the failure mode this pairing invites.
#
#      ./convert_videos.sh [source_dir] [output_dir]
#
#  Defaults to the current directory and ./out, which is what the batch
#  file does. The arguments exist so the Colab notebook can point it at an
#  unpacked upload without cd-ing around.
#
#  WHY THE SETTINGS ARE WHAT THEY ARE:
#
#  -r 30           The mobile files are 59.94 fps, which is a container claim,
#                  not animation: these are hand-drawn cels held for several
#                  frames each, so halving the rate discards duplicates rather
#                  than motion. 30 is a legal MPEG-1 rate (frame_rate_code 5)
#                  so no rounding happens, and it is what these outputs were
#                  verified in sync at on real hardware.
#
#                  VERIFY IT. ffprobe's r_frame_rate is derived from timestamp
#                  granularity for MPEG-1 program streams and can read double
#                  the truth; avg_frame_rate is the honest field, which is why
#                  the check below asks for it.
#
#  -bf 0           No B-frames. B-frames need two reference pictures held and
#                  interpolated per macroblock, which is the most expensive
#                  thing pl_mpeg does. I/P only costs some bitrate and buys a
#                  large amount of SH4 time.
#
#  -s 256x192      4:3, and every dimension a multiple of 16, so the encoder
#                  needs no partial macroblocks. The player scales it to full
#                  height on the PVR at no CPU cost (mpeg.c setup_graphics),
#                  so the decode is what this buys back: 256x192 is 64% of the
#                  macroblocks of 320x240, per frame, for the whole movie.
#
#  -b:v 600k       Enough for 256x192 at 30fps without giving the GD-ROM more
#                  to stream than it comfortably can alongside the audio.
#
#  -g 15           Keyframe every half second. Short GOPs cost bitrate but
#                  bound how far a decode error can propagate.
#
#  -c:a mp2        pl_mpeg decodes MPEG-1 Layer II and nothing else. mp3 or
#                  aac in an .mpg will leave the player with no audio stream.
#
#  -nostdin        NOT IN THE BATCH FILE, AND NOT OPTIONAL HERE. ffmpeg reads
#                  stdin for keyboard control, and inside a shell loop that is
#                  the loop's own input — it swallows the rest of the list and
#                  mangles the next filename. This already happened once in
#                  music_step_1_ogg_to_pcm.sh. cmd.exe does not have the
#                  problem, which is why its twin has no equivalent line.
# ---------------------------------------------------------------------
set -u

SRC_DIR="${1:-.}"
OUT_DIR="${2:-$SRC_DIR/out}"

#  Mobile filename -> the name the engine asks for.
#  Edit the left-hand side if your files are named differently.
#  KEEP THIS TABLE IDENTICAL TO THE :conv CALLS IN convert_videos.bat.
MAPPINGS=(
    "-m:Opening"
    "iJ:OpeningUS"
    "XE:Good_Ending"
    "jl:Good_EndingUS"
    "Xg:Bad_Ending"
    "YJ:Bad_EndingUS"
)

EXTS=(mp4 m4v mpg avi mov)

for tool in ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: $tool not found in PATH." >&2
        exit 1
    }
done

[ -d "$SRC_DIR" ] || { echo "ERROR: source directory '$SRC_DIR' does not exist." >&2; exit 1; }
mkdir -p "$OUT_DIR" || exit 1

# One field per ffprobe call, with default=nw=1:nk=1 rather than csv.
#
# Do NOT collapse this into one csv=p=0 call listing several fields. ffprobe
# emits them in the STREAM's field order, not the order you asked for, and it
# appends an empty field for the stream's side_data_list — so
# "-show_entries stream=avg_frame_rate,width,height -of csv=p=0" prints
# "256,192,30/1," and a string comparison against "30/1,256,192" fails on a
# perfectly good file. Both this script's twin and BUILD_DREAMCAST.md carried
# that wrong expectation for a while.
probe() { ffprobe -v error -select_streams "$1" -show_entries "stream=$2" \
                  -of default=nw=1:nk=1 "$3" 2>/dev/null; }

converted=0
skipped=0
failed=0

for entry in "${MAPPINGS[@]}"; do
    src_name="${entry%%:*}"
    dst_name="${entry##*:}"

    # "$SRC_DIR/" is not decoration. One of these files is called "-m.mp4", and
    # ffmpeg reads a leading hyphen as the start of an option, not a filename.
    # Every path here therefore stays prefixed; do not "tidy" it to a bare name.
    #
    # `find -iname` rather than a glob: cmd.exe's `if exist` is
    # case-insensitive for free, and bash's nocaseglob is NOT a substitute —
    # bash only performs pathname expansion on words that actually contain a
    # wildcard, so nocaseglob never fires on a literal "XE.mp4" and an
    # "XE.MP4" on disk goes unfound. That bug is silent: the file is simply
    # reported missing.
    # THE ONE DELIBERATE DIFFERENCE FROM THE BATCH FILE: the destination name
    # is accepted as a source too, so you can tell people "rename your six
    # movies to Opening / OpeningUS / Good_Ending / Good_EndingUS /
    # Bad_Ending / Bad_EndingUS" instead of asking them to work out which
    # two-character mobile name is which. Delete the second find below for
    # strict parity with the batch file.
    #
    # Mobile name wins when both exist. Two separate calls rather than one
    # `-iname A -o -iname B`, because find's -o does not impose a preference —
    # -print -quit returns whichever it happens to reach first in directory
    # order, which is not a decision anyone made.
    src=""
    for ext in "${EXTS[@]}"; do
        for want in "$src_name" "$dst_name"; do
            src=$(find "$SRC_DIR" -maxdepth 1 -type f -iname "$want.$ext" \
                  -print -quit 2>/dev/null)
            [ -n "$src" ] && break 2
        done
    done

    if [ -z "$src" ]; then
        echo "  SKIP $src_name - no source file found"
        skipped=$((skipped + 1))
        continue
    fi

    dst="$OUT_DIR/$dst_name.mpg"

    # Refuse to read and write the same file. Reachable in one step now that a
    # source may be called "Opening.mpg": point the output at the source
    # directory and ffmpeg is asked to overwrite its own input. ffmpeg itself
    # bails, but the failure handler below would then delete what it thinks is
    # a half-written output and is actually the user's only copy.
    # `cd && pwd -P` rather than `readlink -f`: BSD readlink had no -f until
    # macOS 12.3, and this is the idiom the other dreamcast/ scripts already use.
    abspath() { printf '%s/%s\n' "$(cd -P -- "$(dirname -- "$1")" && pwd -P)" "$(basename -- "$1")"; }
    if [ "$(abspath "$src")" = "$(abspath "$dst")" ]; then
        echo "  SKIP $dst_name - source and destination are the same file" >&2
        echo "         (use a different output directory)" >&2
        skipped=$((skipped + 1))
        continue
    fi

    # Already in the target format? Copy it. Re-encoding MPEG-1 to MPEG-1 is
    # generation loss for no reason, and this is the path someone lands on when
    # they feed back a set this script produced earlier.
    if [ "$(probe v:0 codec_name "$src")" = "mpeg1video" ] \
    && [ "$(probe v:0 width "$src")" = "256" ] \
    && [ "$(probe v:0 height "$src")" = "192" ] \
    && [ "$(probe v:0 avg_frame_rate "$src")" = "30/1" ] \
    && [ "$(probe a:0 codec_name "$src")" = "mp2" ] \
    && [ "$(probe a:0 sample_rate "$src")" = "44100" ] \
    && [ "$(probe a:0 channels "$src")" = "1" ]; then
        echo "  $src  ->  $dst_name.mpg  (already conforming, copied)"
        if cp -- "$src" "$dst"; then
            converted=$((converted + 1))
        else
            echo "  FAILED $dst_name" >&2
            failed=$((failed + 1))
        fi
        continue
    fi

    # Encode to a temp file and move into place only on success, so a failed or
    # interrupted run never leaves a truncated .mpg looking like a good one,
    # and nothing here ever deletes a file it did not create.
    echo "  $src  ->  $dst_name.mpg"
    tmp="$OUT_DIR/.$dst_name.$$.mpg"
    if ffmpeg -nostdin -hide_banner -loglevel warning -y -i "$src" \
        -c:v mpeg1video -r 30 -s 256x192 -b:v 600k -bf 0 -g 15 \
        -c:a mp2 -ar 44100 -ac 1 -b:a 128k \
        -f mpeg "$tmp" && mv -f "$tmp" "$dst"; then
        converted=$((converted + 1))
    else
        echo "  FAILED $dst_name" >&2
        rm -f "$tmp"
        failed=$((failed + 1))
    fi
done

echo
echo "=== Verifying ==="
for f in "$OUT_DIR"/*.mpg; do
    [ -e "$f" ] || continue
    w=$(probe v:0 width "$f");        h=$(probe v:0 height "$f")
    r=$(probe v:0 avg_frame_rate "$f")
    ac=$(probe a:0 codec_name "$f");  ar=$(probe a:0 sample_rate "$f")
    ch=$(probe a:0 channels "$f")
    name=$(basename "$f")
    echo "  $name  video ${w}x${h} @ ${r}   audio ${ac} ${ar} Hz ${ch} ch"

    # A mismatch here is not cosmetic: the wrong frame rate desynchronises
    # against the audio, and any audio codec but Layer II leaves the player
    # silent. Count once per file, not once per bad field.
    bad=0
    [ "$w" = "256" ] && [ "$h" = "192" ] || bad=1
    [ "$r" = "30/1" ] || bad=1
    [ "$ac" = "mp2" ] && [ "$ar" = "44100" ] && [ "$ch" = "1" ] || bad=1
    if [ "$bad" -ne 0 ]; then
        echo "    ^ WRONG FORMAT — want 256x192 @ 30/1, mp2 44100 Hz 1 ch" >&2
        failed=$((failed + 1))
    fi
done

echo
echo "Converted $converted, skipped $skipped, failed $failed."
if [ "$failed" -ne 0 ]; then
    echo "Do NOT burn this set." >&2
    exit 1
fi
if [ "$converted" -ne 6 ]; then
    echo "WARNING: expected 6 movies, produced $converted. The disc will still" >&2
    echo "boot - a missing movie is skipped cleanly - but that cutscene is gone." >&2
    exit 2
fi
echo "Done. Copy $OUT_DIR/*.mpg to videos/ at the root of the disc image."
