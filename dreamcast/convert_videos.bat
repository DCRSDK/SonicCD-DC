@echo off
rem ---------------------------------------------------------------------
rem  Sonic CD FMV -> Dreamcast (MPEG-1, decodable in real time on an SH4)
rem ---------------------------------------------------------------------
rem  Run from the folder holding the mobile version's movies. Writes to .\out.
rem
rem  WHY THE SETTINGS ARE WHAT THEY ARE:
rem
rem  -r 30           The mobile files are 59.94 fps, which is a container claim,
rem                  not animation: these are hand-drawn cels held for several
rem                  frames each, so halving the rate discards duplicates rather
rem                  than motion. 30 is a legal MPEG-1 rate (frame_rate_code 5)
rem                  so no rounding happens, and it is what these outputs were
rem                  verified in sync at on real hardware.
rem
rem                  VERIFY IT. ffprobe's r_frame_rate is derived from timestamp
rem                  granularity for MPEG-1 program streams and can read double
rem                  the truth; avg_frame_rate is the honest field, which is why
rem                  the check below asks for it.
rem
rem  -bf 0           No B-frames. B-frames need two reference pictures held and
rem                  interpolated per macroblock, which is the most expensive
rem                  thing pl_mpeg does. I/P only costs some bitrate and buys a
rem                  large amount of SH4 time.
rem
rem  -s 256x192      4:3, and every dimension a multiple of 16, so the encoder
rem                  needs no partial macroblocks. The player scales it to full
rem                  height on the PVR at no CPU cost (mpeg.c setup_graphics),
rem                  so the decode is what this buys back: 256x192 is 64% of the
rem                  macroblocks of 320x240, per frame, for the whole movie.
rem
rem  -b:v 600k       Enough for 256x192 at 30fps without giving the GD-ROM more
rem                  to stream than it comfortably can alongside the audio.
rem
rem  -g 15           Keyframe every half second. Short GOPs cost bitrate but
rem                  bound how far a decode error can propagate.
rem
rem  -c:a mp2        pl_mpeg decodes MPEG-1 Layer II and nothing else. mp3 or
rem                  aac in an .mpg will leave the player with no audio stream.
rem
rem  Verify every output before burning:
rem      ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate,width,height -of csv=p=0 out\Opening.mpg
rem  It prints "256,192,30/1," — width, height, rate, and a trailing empty
rem  field. NOT the order asked for: ffprobe emits fields in the STREAM's
rem  order, and the trailing comma is the stream's empty side_data_list. Read
rem  the numbers, don't string-compare them. What matters is 256, 192, 30/1,
rem  and mp2 / 44100 / 1 on the audio line.
rem
rem  There is a Linux twin of this file, convert_videos.sh, which asserts all
rem  six values and exits non-zero on a mismatch. KEEP THE TWO IN STEP: same
rem  mappings, same ffmpeg flags. The comment block above this one drifted a
rem  whole encoder profile away from the command below it once already.
rem ---------------------------------------------------------------------

setlocal enabledelayedexpansion

if not exist ".\out" mkdir ".\out"

rem  Mobile filename -> the name the engine asks for.
rem  Edit the right-hand side if your files are named differently.
call :conv "iJ" "Opening"
call :conv "-m" "OpeningUS"
call :conv "XE" "Good_Ending"
call :conv "jl" "Good_EndingUS"
call :conv "Xg" "Bad_Ending"
call :conv "YJ" "Bad_EndingUS"

echo.
echo === Verifying ===
for %%F in (".\out\*.mpg") do (
    for /f "delims=" %%A in ('ffprobe -v error -select_streams v:0 -show_entries stream^=avg_frame_rate^,width^,height -of csv^=p^=0 "%%F"') do echo   %%~nxF  video %%A
    for /f "delims=" %%A in ('ffprobe -v error -select_streams a:0 -show_entries stream^=codec_name^,sample_rate^,channels -of csv^=p^=0 "%%F"') do echo   %%~nxF  audio %%A
)
echo.
echo Done. Copy out\*.mpg to videos\ at the root of the disc image.
goto :eof

:conv
rem  ".\" is not decoration. One of these files is called "-m.mp4", and ffmpeg
rem  reads a leading hyphen as the start of an option, not a filename.
set "SRC="
for %%E in (mp4 m4v mpg avi mov) do if exist ".\%~1.%%E" set "SRC=.\%~1.%%E"
if not defined SRC (
    echo   SKIP %~1 - no source file found
    goto :eof
)
echo   %SRC%  ^-^>  %~2.mpg
ffmpeg -hide_banner -loglevel warning -y -i "%SRC%" ^
  -c:v mpeg1video -r 30 -s 256x192 -b:v 600k -bf 0 -g 15 ^
  -c:a mp2 -ar 44100 -ac 1 -b:a 128k ^
  -f mpeg ".\out\%~2.mpg"
goto :eof
