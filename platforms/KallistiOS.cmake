# SEGA Dreamcast (KallistiOS) platform — RSDKv3 / Sonic CD
#
# Build from inside a KOS environment (source /opt/toolchains/dc/kos/environ.sh):
#   mkdir build-dc && cd build-dc
#   kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release ..
#   make -j$(nproc)
#
# No kos-ports packages are required: the Dreamcast build decodes neither Vorbis
# nor Theora (music and sfx are raw PCM; the OGV cutscenes are skipped), so
# libtremor, libogg and libtheora are not linked.
#
# Produces RSDKv3.elf; package with (see BUILD_DREAMCAST.md):
#   ./builddir/mkdcdisc -e '<path to RSDKv3.elf>' \
#     -d '<path to Data dir>' -d '<path to videos dir>' \
#     -f '<path to Data.rsdk>' -f '<path to settings.ini>' \
#     -N -n SonicCD -o SonicCD.cdi
#
# To run over dcload-ip / dcload-serial instead, configure a SEPARATE build
# directory with the data root pointed at the host:
#   mkdir build-pc && cd build-pc
#   kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release -DDC_DATA_ROOT=/pc/ ..
#   make -j$(nproc)
#   dc-tool-ip -t <dreamcast-ip> -x RSDKv3.elf
# Run dc-tool from the directory holding Data.rsdk AND the loose Data/ tree —
# /pc is its working directory.
#
# Use a separate build dir rather than retargeting build-dc: DC_DATA_ROOT is a
# CACHE variable, so once "/pc/" is configured into a build directory it STAYS
# there across later reconfigures unless you pass it again or wipe the cache.

if(NOT DEFINED ENV{KOS_BASE})
    message(FATAL_ERROR "KOS environment not found. Source environ.sh and use kos-cmake.")
endif()

# Video.cpp stays in the build — its RSV path (Retro's own palettised frame
# format) is codec-free and Sonic CD uses it. Only the Theora decoder goes.
list(REMOVE_ITEM RETRO_FILES dependencies/all/theoraplay/theoraplay.c)

# ModAPI.cpp is guarded by RETRO_USE_MOD_LOADER internally, but tinyxml2 is
# pulled in unconditionally and only the mod loader needs it.
list(REMOVE_ITEM RETRO_FILES dependencies/all/tinyxml2/tinyxml2.cpp)

add_executable(RetroEngine ${RETRO_FILES}
    RSDKv3/Dreamcast/DCGraphics.cpp
    RSDKv3/Dreamcast/DCAudio.cpp
    RSDKv3/Dreamcast/DCInput.cpp
    RSDKv3/Dreamcast/DCSystem.cpp
    RSDKv3/Dreamcast/DCSave.cpp
    RSDKv3/Dreamcast/DCVideo.cpp
)

# NOTE: RSDKv3/Dreamcast/mpeg.c is deliberately NOT listed above. It defines
# PL_MPEG_IMPLEMENTATION and is #included by DCVideo.cpp so the decoder is
# exactly one translation unit — the same arrangement the Sonic Mania Dreamcast
# port uses. Listing it here would give you duplicate symbols.
#
# pl_mpeg.h in RSDKv3/Dreamcast is that port's DREAMCAST FORK, which emits
# packed macroblocks as frame->display. It is not interchangeable with upstream
# pl_mpeg.

# Force off features that can't work on the Dreamcast.
#
# RETRO_USE_HW_RENDER drives RETRO_USING_OPENGL in the top-level CMakeLists, and
# there is no GL here. Note this is NOT the same switch as the engine's runtime
# `renderType`: RSDKv3 chooses software vs hardware at RUN time, and on the
# Dreamcast InitUserdata (Userdata.cpp) sets that from DC_HW_RENDER below.
set(RETRO_MOD_LOADER OFF CACHE BOOL "Mod loader disabled on Dreamcast" FORCE)
set(RETRO_USE_HW_RENDER OFF CACHE BOOL "OpenGL HW renderer disabled on Dreamcast" FORCE)
set(RETRO_FORCE_CASE_INSENSITIVE OFF CACHE BOOL "ISO9660 filenames are already normalised" FORCE)

option(RSDK_DEBUG "Enable debug logging + dev menu" OFF)
option(DC_NO_AUDIO "Disable all audio init (bisection aid)" OFF)

# Boot-time audio self-test: two identical 1000 Hz tones, one generated at the
# device and one pushed through the engine's sfx mixer, then Jump/Ring/LoseRings
# as bare PlaySfx calls. If the tones differ in pitch the mixing path is at
# fault; if both are an octave high the device rate is. Adds a few seconds to
# boot, so it defaults OFF.
option(DC_AUDIO_SELFTEST "Play reference tones and sounds at boot" OFF)

# Audio device rate.
#
# 44100 plays Sonic CD's sfx at their native rate with nothing discarded. 22050
# halves the SH4's mixing cost and the sfx RAM, but a 22050 device cannot carry
# anything above 11 kHz — and Ring.wav and LoseRings.wav keep 13.5% and 11.0% of
# their energy up there, which is why those two (and only those two) sounded
# tinny at 22050. Everything else in the game is under 5%.
#
# Music assets stay 22050 regardless; DC_MusicReadSamples holds each source
# frame for two output frames at 44100. Only the sfx need to be native.
#
# NOTE: this rate only governs sounds the SOFTWARE mixer handles. With
# DC_AICA_SFX on (below), each sfx carries its own rate into a hardware voice and
# is unaffected by this setting — which is what makes a 22050 sound and a 44100
# sound able to sit in the same build without either being wrong.
set(DC_AUDIO_RATE "44100" CACHE STRING "Audio device rate: 44100 or 22050")

# How much audio the SPU holds ahead of the game, in bytes. THIS IS THE
# LATENCY: at 22050 Hz stereo 16-bit the device eats 88200 bytes/second, so
# 8192 is ~93 ms and 65536 (the KOS maximum, which this used to pass) is
# ~743 ms of delay between a jump and its sound. Raise it if audio crackles;
# do not raise it "to be safe".
set(DC_AUDIO_BUFFER "8192" CACHE STRING "SPU buffer size in bytes; latency = size / (DC_AUDIO_RATE*4) s")

# Play sound effects on AICA hardware voices instead of mixing them on the SH4.
#
# Samples go into the AICA's own 2MB of sound RAM, so they cost the main heap
# nothing, and the AICA's wavetable engine does the resampling, the volume and
# the panning. The point that matters most: each voice has its OWN pitch
# register, so a 22050 Hz sound and a 44100 Hz sound play correctly side by side
# and the device rate stops being a compromise between them.
#
# The AICA holds loop points in 16-bit registers, so no voice can address more
# than 65534 samples. Sounds past that (TimeWarp, BombCarrier, Achievement,
# LoseRings) fall back to the software mixer automatically — the two coexist per
# sound, not per build. The perf overlay's "HW nn SW nn" says which went where.
#
# Turn OFF to put every sfx back through the software mixer, which is the A/B
# test if a sound starts behaving oddly.
option(DC_AICA_SFX "Play sfx on AICA hardware voices instead of mixing in software" ON)
option(DC_USE_PVR "Present via a PVR textured quad at 640x480 instead of a direct 320x240 VRAM copy" ON)
# The on-screen performance overlay and the 120-frame timing log.
#
# DEFAULTS OFF now that the port is stable: it is a development instrument, it
# covers part of the picture, and drawing it costs time on the very frames you
# would be measuring. Turn it on to read frame time, palette churn, atlas upload
# cost, the sfx rate/pan line, or the AICA "HW nn SW nn" residency counters.
option(DC_PERF_LOG "Log frame/flip timing every 120 frames, and draw it on screen" OFF)

# The hang watchdog. The audio thread watches a heartbeat the main loop bumps
# once per present, and if it stops moving for three seconds it takes the screen
# and prints where the main thread was.
#
# DEFAULTS OFF. With no serial cable and no BBA this was the only way to ask a
# frozen machine where it stopped, and it earned its keep — but on a build that
# does not hang, the only report it can produce is a FALSE one. It has already
# called a healthy 44100 sfx load a hang once. Turn it on when something wedges.
#
# The DC_PHASE breadcrumbs and the heartbeat bumps stay compiled in either way
# (a byte store and an increment), so enabling this is a reconfigure and nothing
# more.
option(DC_WATCHDOG "Hang watchdog: report where the main thread stopped" OFF)

# Ground draw distance in the special stage.
#
# Draw3DFloorLayer samples a square grid of 16-unit floor tiles centred on the
# camera and emits a quad for each one carrying graphics. The floor stops dead
# where that grid stops, so the grid IS the draw distance.
#
# OFF samples 20x20 tiles (a 320x320 unit window), which is what the desktop
# build draws at 1x. ON samples DC_FLOOR_GRID squared. See DC_InitRenderDevice.
option(DC_HQ_3D_FLOOR "Full-distance 3D floor in the special stage" ON)

# Tiles per side of that grid when DC_HQ_3D_FLOOR is ON.
#
#   32   512x512 units   1024 tiles   stock RSDKv3, 125KB of polyList3D
#   40   640x640         1600         1.6x the tile work, 125KB   <- DEFAULT
#   48   768x768         2304         2.3x, 184KB
#   56   896x896         3136         3.1x, 251KB   <- ran clean, pre-LOD default
#   64  1024x1024        4096         4.0x, 320KB   <- measured: visible slowdown
#
# THIS IS NO LONGER THE DRAW DISTANCE. Since DC_FLOOR_LOD_GRID below took over
# carrying the horizon, this is a DETAIL RADIUS: how far out the floor is drawn
# at full 16-unit resolution before the coarse ring takes over. It dropped from
# 56 to 40 when the ring arrived, because the two budgets add up:
#
#   40 fine + 28x64 ring   2284 quads   reach 1792   <- 0.73x the old cost
#   56 fine (no ring)      3136 quads   reach  896
#   64 fine (no ring)      4096 quads   reach 1024   <- measured: visible slowdown
#
# So the default now draws twice as far for three quarters of the work.
#
# Measurement that still applies: on hardware 48 and 56 were indistinguishable
# and 64 slowed visibly, so the frame budget is crossed between 3136 and 4096
# quads. Note what that means - 48 and 56 looked identical because BOTH finished
# inside the 16.67ms vsync deadline, not because the extra 832 tiles were free.
# Everything under the deadline presents at 60fps; the moment you cross it you
# quantise straight to 30. There is no gentle degradation to warn you, which is
# why the totals above matter more than how any single setting feels.
#
# So the headroom at 56 is real but unmeasured by eye. If you want to push to 60,
# build with DC_PERF_LOG=ON and read `logic+render` against the 16667us budget -
# that number tells you how much margin is actually left, which watching the
# frame counter cannot.
#
# Cost is QUADRATIC and lands in two places: the tile loop that builds the quads,
# and four perspective-divided vertex transforms per tile per frame in
# dcSubmit3D, all of it going into the PVR's translucent list.
#
# Past 40, polyList3D grows to fit automatically (Drawing.hpp), costing static
# RAM at 20 bytes per vertex.
#
# THE REAL CEILING IS THE PVR VERTEX BUFFER, not that array. Every quad is four
# 32-byte pvr_vertex_t, so the floor costs GRID*GRID*128 bytes of the 1MB the TA
# gets per frame - and the stage's own tile layers and sprites have to fit in
# there too:
#
#   48    288KB   28% of the buffer
#   56    392KB   38%
#   64    512KB   50%
#   72    648KB   63%   <- practical maximum
#   96   1152KB  112%   <- overflows on its own, before any stage geometry
#
# Overrunning it does not fail cleanly: the TA runs out mid-frame and geometry
# silently disappears. A static_assert in DCGraphics.cpp refuses anything over
# three quarters of the buffer, which puts the hard stop at 72.
set(DC_FLOOR_GRID "40" CACHE STRING "Special-stage floor grid, tiles per side (max 72)")

# Distant floor, drawn coarse. THE FINE GRID ABOVE IS NO LONGER THE DRAW
# DISTANCE - it is only the distance at which the floor is drawn at full detail.
#
# Beyond it the floor is sampled every DC_FLOOR_LOD_STEP units instead of every
# 16, and each coarse quad is textured by stretching the one 16x16 sub-tile its
# corner lands in across the whole quad. That is what the iOS build and the Mania
# Dreamcast port do, and past the point where a tile is a few pixels tall the
# difference does not survive the perspective divide anyway.
#
# STEP 64 covers 16 fine tiles per quad. Reach is DC_FLOOR_LOD_GRID * STEP:
#
#   GRID 28, STEP 64   1792 units   784 coarse quads before the inner skip
#   GRID 24, STEP 64   1536          576
#   GRID 32, STEP 64   2048         1024
#   GRID 28, STEP 128  3584          784   <- twice the reach, same quad count,
#                                             visibly blockier up close
#
# Set DC_FLOOR_LOD_GRID to 0 to switch the ring off and get fine-grid-only back.
# STEP must be a power of two and at least 16 (asserted in Drawing.hpp).
set(DC_FLOOR_LOD_STEP "64" CACHE STRING "Coarse floor sample size in units; power of two, >= 16")
set(DC_FLOOR_LOD_GRID "28" CACHE STRING "Coarse floor quads per side; 0 disables the distant ring")

if(DC_FLOOR_GRID GREATER 72)
    message(FATAL_ERROR "DC_FLOOR_GRID ${DC_FLOOR_GRID} would take over 3/4 of the PVR vertex buffer, leaving no room for the stage (max 72).")
endif()

# The hardware renderer. Builds ONE 1024x1024 8-bit indexed atlas instead of the
# six 1024x1024 RGBA5551 ones RSDKv3 normally keeps (12MB -> 1MB), sets
# renderType to RENDER_HW, and feeds the engine's existing quad emitter straight
# to the PVR. See PHASE3_HW_RENDERER.md.
#
# With this OFF the build is the software presenter, which still works and is
# the fallback if something on the hardware path misbehaves.
# DEFAULTS ON since the hardware renderer was verified on real hardware: 59fps
# in gameplay and in the special stage against 24-30 with the software path, and
# roughly 45x less CPU per frame. Turning BOTH this and DC_USE_PVR off gives the
# software presenter, which still works and is the fallback.
option(DC_HW_RENDER "Render tiles and sprites on the PVR via an indexed atlas" ON)

# FMV playback, via the Sonic Mania Dreamcast port's pl_mpeg wrapper
# (RSDKv3/Dreamcast/mpeg.c). Needs the six movies in videos/ at the root of the
# disc image: Opening / OpeningUS / Good_Ending / Good_EndingUS / Bad_Ending /
# Bad_EndingUS. See convert_videos.bat and the header of DCVideo.cpp.
#
# DEFAULTS ON since playback was verified end to end on real hardware: video and
# audio in sync, no glitching, and the JP/US soundtrack setting swapping between
# the two name variants correctly.
#
# With it OFF, DCVideo.cpp compiles to nothing and PlayVideoFile skips the movie
# — which is still the right setting for a build chasing something else, since
# it takes the decoder out of the picture entirely. It is also what you want if
# the disc image has no videos/ directory: the movies are skipped cleanly either
# way, but there is no sense carrying the decoder for files that are not there.
option(DC_FMV "Play the FMV cutscenes with pl_mpeg" ON)

# Hard dependency, not a preference: the atlas calls pvr_mem_malloc and
# pvr_set_pal_format, and pvr_init() only runs in the DC_USE_PVR branch of
# DC_InitRenderDevice. Enabling one without the other allocates against an
# uninitialised PVR, which fails in a confusing way at runtime rather than here.
if(DC_HW_RENDER AND NOT DC_USE_PVR)
    message(FATAL_ERROR "DC_HW_RENDER requires DC_USE_PVR=ON (the atlas lives in PVR VRAM).")
endif()

# Root the game data is read from. "/cd/" is the disc; "/pc/" is the dcload host
# directory. This is a compile-time literal decided once at configure time.
#
# BASE_PATH is used in literal-concatenation form (BASE_PATH "settings.ini" in
# Userdata.cpp, BASE_PATH "log.txt" in Debug.cpp), so it must stay a plain
# string literal — it cannot become a variable or a function call without
# editing those sites too.
set(DC_DATA_ROOT "/cd/" CACHE STRING "Game data root: /cd/ for a disc image, /pc/ for dcload")

target_compile_definitions(RetroEngine PRIVATE
    BASE_PATH="${DC_DATA_ROOT}"
    RSDK_DEBUG=$<BOOL:${RSDK_DEBUG}>
    DC_NO_AUDIO=$<BOOL:${DC_NO_AUDIO}>
    DC_USE_PVR=$<BOOL:${DC_USE_PVR}>
    DC_PERF_LOG=$<BOOL:${DC_PERF_LOG}>
    DC_HW_RENDER=$<BOOL:${DC_HW_RENDER}>
    DC_HQ_3D_FLOOR=$<BOOL:${DC_HQ_3D_FLOOR}>
    DC_FMV=$<BOOL:${DC_FMV}>
    DC_AUDIO_SELFTEST=$<BOOL:${DC_AUDIO_SELFTEST}>
    DC_AUDIO_BUFFER=${DC_AUDIO_BUFFER}
    DC_AUDIO_RATE=${DC_AUDIO_RATE}
    DC_AICA_SFX=$<BOOL:${DC_AICA_SFX}>
    DC_WATCHDOG=$<BOOL:${DC_WATCHDOG}>
    DC_FLOOR_GRID=${DC_FLOOR_GRID}
    DC_FLOOR_LOD_STEP=${DC_FLOOR_LOD_STEP}
    DC_FLOOR_LOD_GRID=${DC_FLOOR_LOD_GRID}
)

# Say which root this build was configured for — a mis-set cache variable is
# otherwise only discoverable by running the binary and watching it fail.
message(STATUS "RSDKv3/DC: game data root is ${DC_DATA_ROOT}")

target_link_libraries(RetroEngine -lm)

# NOTE: -ffunction-sections/-fdata-sections + --gc-sections are deliberately NOT
# used: with KOS's linker script, section GC can discard .init_array (static
# constructors) and other KEEP-worthy data, producing a binary that boots to the
# KOS banner and then silently misbehaves.
set(RELEASE_FLAGS
    -Os
    -fno-exceptions
    -fomit-frame-pointer
)

# Hot paths get -O3: the script interpreter and the software renderer dominate
# frame time on SH4. Drawing.cpp in particular is 7200 lines of per-pixel work
# in v3 (it holds both the software and hardware paths in one file).
set(CRITICAL_FILES
    RSDKv3/Script.cpp
    RSDKv3/Drawing.cpp
    RSDKv3/Scene.cpp
    RSDKv3/Object.cpp
    RSDKv3/Collision.cpp
    RSDKv3/Math.cpp
    RSDKv3/Scene3D.cpp
    RSDKv3/Sprite.cpp
    RSDKv3/Palette.cpp
    RSDKv3/Animation.cpp
)

target_compile_options(RetroEngine PRIVATE $<$<NOT:$<CONFIG:Debug>>:${RELEASE_FLAGS}>)
set_source_files_properties(${CRITICAL_FILES} PROPERTIES COMPILE_FLAGS "$<$<NOT:$<CONFIG:Debug>>:-O3>")

set_target_properties(RetroEngine PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)
