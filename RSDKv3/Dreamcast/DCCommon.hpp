// ---------------------------------------------------------------------
// RSDKv3 Dreamcast (KallistiOS) backend
//
// Ported from the RSDKv4 Dreamcast layer. Where v4 had to inject DC_Queue*
// calls into Drawing.cpp's software draw functions, RSDKv3 already carries a
// hardware quad emitter behind `renderType == RENDER_HW` — see PORT_ANALYSIS.md.
// Phase 1 (this file set) does NOT use it: renderType is pinned to RENDER_SW
// and DCGraphics.cpp is a plain software presenter. Phase 3 retargets the
// existing HW branch at the PVR.
// ---------------------------------------------------------------------
#ifndef DC_COMMON_HPP
#define DC_COMMON_HPP

#if RETRO_USING_KOS

#include <kos.h>

// Logging that reaches the emulator/serial console. Plain printf works, but
// only with INIT_FS_PTY in KOS_INIT_FLAGS (see main.cpp). fflush guards
// against buffering swallowing the last message before a hang.
#define DC_LOG(...)                                                                                                                                  \
    do {                                                                                                                                             \
        printf(__VA_ARGS__);                                                                                                                         \
        fflush(stdout);                                                                                                                              \
    } while (0)

// Boot-phase breadcrumb. If the engine dies during init, the last phase logged
// says how far it got:
//   0=static ctors | 1=main entered | 2=settings read | 3=data pack opened
//   4=GameConfig loaded | 5=video init | 6=audio init | 7=first stage
void DC_Probe(int phase);

// ---- Video (DCGraphics.cpp) ----
int DC_InitRenderDevice();
void DC_FlipScreen();
void DC_ReleaseRenderDevice();

// ---- Phase 3: indexed sprite/tile atlas (DCGraphics.cpp) ----
// Behind DC_HW_RENDER so the working software build is untouched while this is
// brought up. See PHASE3_HW_RENDERER.md.
//
// RSDKv3's hardware path keeps SIX 1024x1024 RGBA5551 atlases — one per palette
// bank, selected at draw time by glBindTexture(gfxTextureID[texPaletteNum]) —
// which is 12MB against the Dreamcast's 8MB of VRAM. The index data in all six
// is identical; only the palette lookup differs. So we build ONE 8-bit indexed
// atlas (1MB) and let the PVR's palette banks do what the six textures were
// doing.
#if DC_HW_RENDER
// The staging buffer the engine's three atlas writers fill with palette indices
// instead of colours. 1024*1024 bytes. NULL until DC_AtlasInit succeeds, and the
// writers check for that.
extern byte *dcAtlasIndices;

// Allocate the staging buffer and the VRAM texture. False if either is refused.
bool DC_AtlasInit();
void DC_AtlasRelease();

// Twiddle dcAtlasIndices into the VRAM texture. Call after the engine has
// refilled the staging buffer (stage load / tile edits) — NOT per frame.
void DC_AtlasUpload();

// Push fullPalette[] into the PVR's palette banks as ARGB1555, entry 0
// transparent. Only re-uploads banks whose contents actually changed: Sonic CD's
// time travel churns palettes hard, and 1024 unconditional pvr_set_pal_entry
// calls per frame was a known problem in the v4 port.
void DC_AtlasUploadPalettes();

// Marks a palette bank dirty so the next DC_AtlasUploadPalettes re-sends it.
void DC_MarkPaletteDirty(int bank);

// True while L+R are held (without Start). Shows the atlas debug view.
bool DC_AtlasViewHeld();
#endif

// ---- Hang watchdog (DCSystem.cpp / DCAudio.cpp / DCGraphics.cpp) ----
//
// This machine has no serial cable and no BBA, so a hang is a frozen picture and
// nothing else — the screen keeps showing the last completed frame and there is
// no way to ask the program where it stopped. That has now cost several build
// cycles, so the program answers for itself.
//
// The main loop drops a breadcrumb (DC_PHASE) as it moves through the frame and
// bumps dcHeartbeat once per present. The audio thread — which polls every few
// milliseconds and is NOT the thread that hangs — watches the heartbeat, and if
// it stops moving for DC_WATCHDOG_MS it takes the screen and prints where the
// main thread was.
//
// It cannot catch a CPU exception (that stops every thread), but it catches
// every deadlock and every infinite loop, which is what a frozen frame with no
// KOS register dump on it usually means.
enum DCPhase {
    DC_PHASE_BOOT = 0,
    DC_PHASE_INPUT,      // 1  ProcessInput
    DC_PHASE_STAGE,      // 2  ProcessStage: scripts, objects, collision
    DC_PHASE_PRESENT,    // 3  DC_FlipScreen
    DC_PHASE_STAGELOAD,  // 4  LoadStageFiles and everything it pulls in
    DC_PHASE_SFXLOAD,    // 5  LoadSfx
    DC_PHASE_MUSICLOAD,  // 6  LoadMusic
    DC_PHASE_ATLAS,      // 7  atlas twiddle + upload
    DC_PHASE_DEVMENU,    // 8  dev menu / error screen
    DC_PHASE_DRAW,       // 9  DrawObjectList / the draw half of the frame
    DC_PHASE_VIDEO,      // 10 FMV playback (a legitimately long blocking call)
    DC_PHASE_COUNT
};

extern volatile unsigned int dcHeartbeat;
extern volatile unsigned char dcPhase;
// Set while the main thread is BLOCKED waiting for the audio mutex. If the hang
// report shows this, the fault is an audio deadlock and nothing else — that is
// the single most likely way this port can wedge, and it has happened before
// (LoadMusic held the lock and called FreeMusInfo, which takes it again).
extern volatile unsigned char dcAudioLockWait;

#define DC_PHASE(p) (dcPhase = (unsigned char)(p))

// Called from the audio thread once the heartbeat has stopped. Shuts the PVR
// down, drops to a plain 640x480 framebuffer and prints the report, then parks.
void DC_HangReport();

// The script instruction the interpreter was on. Defined in Script.cpp, which
// owns the opcode-name table.
const char *DC_ScriptOpName();
unsigned short DC_ScriptOp();
unsigned char DC_ScriptSub();

// The opcode budget in ProcessScript (see Script.cpp). A non-zero
// DC_ScriptAborts means a script call ran away and was killed; the rest
// describes the most recent kill. DC_ScriptLoopLo/Hi bracket the bytecode the
// interpreter was cycling over, which is the pair that actually identifies the
// bug.
unsigned int DC_ScriptAborts();
int DC_ScriptAbortPtr();
int DC_ScriptAbortStart();
int DC_ScriptLoopLo();
int DC_ScriptLoopHi();
unsigned short DC_ScriptAbortOp();
unsigned char DC_ScriptAbortSub();
short DC_ScriptAbortObj();
short DC_ScriptAbortType();
const char *DC_ScriptAbortOpName();
// The opcodes immediately before the kill, oldest first — the loop body.
byte DC_ScriptAbortTrail(int i);
int DC_ScriptAbortTrailPtr(int i);
int DC_ScriptAbortTrailLen();
const char *DC_ScriptOpNameOf(int op);

// Rate / channels of the most recently played sound, for the perf overlay.
extern volatile int dcLastSfxRate;
extern volatile int dcLastSfxChannels;
extern volatile int dcLastSfxID;
// Times a sound has been started on a second channel while already playing on
// another. See the comment at its definition in Audio.cpp.
extern volatile int dcSfxDoubleStarts;
// Pan of the most recent sound. See its definition in Audio.cpp.
extern volatile int dcLastSfxPan;

// ---- FMV (DCVideo.cpp) ----
// Where the last video got to. Reported on the perf overlay, since a failure
// inside the player is otherwise silent on hardware.
enum DCVidState {
    DC_VID_IDLE = 0,
    DC_VID_OPENING,   // 1  resolving and opening the file
    DC_VID_NOFILE,    // 2  neither the per-region name nor the bare one exists
    DC_VID_NOMEM,     // 3  allocation refused
    DC_VID_BADSTREAM, // 4  opened, but no usable stream
    DC_VID_PLAYING,   // 5  decoding
    DC_VID_DONE,      // 6  played to the end
    DC_VID_SKIPPED    // 7  cut short by the player
};
extern volatile int dcVidState;
extern volatile int dcVidFrames;
extern volatile int dcVidDmaErr;
extern volatile int dcVidWOut;
extern volatile int dcVidHOut;

#if DC_FMV
// The player does NOT own the loop. Start it, then call DC_VideoStep once per
// engine frame from ENGINE_VIDEOWAIT until it returns true. See DCVideo.cpp for
// why that distinction is the whole design.
bool DC_VideoStart(const char *name);
bool DC_VideoStep(bool skipRequested);
void DC_VideoStop();
// The movie is uploaded by DC_VideoStep and drawn by the normal present path,
// so there is exactly one PVR scene per frame. DC_FlipScreen checks
// DC_VideoActive() and hands the scene to DC_VideoDraw() instead of the game.
bool DC_VideoActive();
void DC_VideoUpload(); // must be called after pvr_wait_ready(); see DCVideo.cpp
void DC_VideoDraw();
#endif

// Hand the audio device over to something else and take it back. The FMV player
// runs at the movie's rate (44100) while the game's mixer is 22050, and KOS
// allows four streams, so this stops the game's rather than reconfiguring it —
// stopping is reversible in a way that reinitialising the device is not.
void DC_SuspendAudio();
void DC_ResumeAudio();

// Put the PVR back the way the engine expects after something else has been
// drawing with it. Restores the texture stride register and re-uploads the
// atlas palettes, both of which an FMV disturbs.
void DC_ReinitRenderState();

// ---- VMU LCD (DCSystem.cpp) ----
void DC_VmuShowIcon();
// Per-frame VMU poll: puts the icon up on the first frame rather than waiting
// for the first stage load, and re-asserts it when a VMU is hot-plugged.
// Cheap — it only enumerates the maple bus twice a second.
void DC_VmuTick();

// ---- Input (DCInput.cpp) ----
void DC_ProcessInput();
// One-frame rising edge of the dev-menu chord (Y, or L+R+Start). RSDKv3's
// input enum has no spare slot to route this through, so it is read directly
// by ProcessEvents. Reading it clears it.
bool DC_DevMenuEdge();

// ---- Timing / system (DCSystem.cpp) ----
unsigned long long DC_GetTicks();          // monotonic, in DC_GetTicksPerSecond() units
unsigned long long DC_GetTicksPerSecond();
// Milliseconds since boot — stands in for SDL_GetTicks(), which Video.cpp and
// the audio code use for frame pacing.
unsigned int DC_GetTicksMS();

// Total free heap / hole count / largest hole, to the log. Built from nothing
// but malloc and free — mallinfo() is not dependable here. RSDKv3 puts ~8.6MB
// of the Dreamcast's 16MB into static globals before main() even runs, so
// knowing what's left matters more here than it did in v4.
void DC_HeapReport(const char *tag);
// Largest block malloc will still hand out, found by binary search (~10 probes).
size_t DC_LargestFreeBlock();

// ---- Audio (DCAudio.cpp) ----
// Device format. Audio.cpp's KOS mixing path needs the rate to compute its
// resample step, so it lives here rather than privately in DCAudio.cpp.
// 22050 rather than v4's 44100: the converted assets are 22050, which makes the
// step exactly 1:1, and it halves the SH4's mixing cost.
// Device rate. Audio.cpp's mixing path divides by this to get its resample
// step, so it has to agree with what DC_InitAudioDevice actually opened.
//
// 22050 was chosen because the converted assets were 22050, making the step
// exactly 1:1 and halving the SH4's mixing cost. That was the wrong trade for
// two sounds: Ring and LoseRings carry 13.5% and 11.0% of their energy ABOVE
// 11 kHz, and a 22050 device cannot represent any of it. Downsampling deletes
// it — which is what "tinny" is — and leaving them at 44100 against a 22050
// device is worse still, because the mixer's 2:1 decimation has no anti-alias
// filter and folds that same energy back as noise. Nothing else in the game
// comes close: Jump is 0.1%, Hurt 0.7%, Spring 4.1%.
//
// At 44100 the sfx play at their native rate, 1:1, with nothing thrown away.
// The cost is double the mixing work and double the sfx RAM.
#ifndef DC_AUDIO_RATE
#define DC_AUDIO_RATE (44100)
#endif

// Wired up in phase 2 of the port; declared here so the hooks in Audio.cpp
// have something to resolve against.
bool DC_InitAudioDevice();
void DC_ReleaseAudioDevice();
// Raw-PCM music streaming from BASE_PATH "Data/Music/*.ogg" (the files keep
// their .ogg names so GameConfig.bin still resolves them; the contents are
// headerless PCM). loopPoint and startFrame are PCM frame indices in the
// ORIGINAL 44100 Hz ogg the loop tables were authored against — the same units
// ov_pcm_seek takes on desktop — NOT byte offsets. DC_MusicOpen rescales.
bool DC_MusicOpen(const char *path, bool loop, uint loopPoint, uint startFrame);
uint DC_MusicTell();
void DC_MusicClose();
// Pulls music into the software mixer. Returns the number of Sint16 samples
// written to `out` (interleaved stereo, device rate). Handles looping and
// end-of-track state changes.
int DC_MusicReadSamples(Sint16 *out, size_t samplesWanted);

// Streaming WAV loader used by LoadSfx. Reads through the engine's already-open
// file reader so it never holds the whole file in RAM, and decimates straight
// into the final buffer. Leaves the file open; the caller closes it.
Sint16 *DC_LoadWAVStreamed(size_t fileSize, size_t *outSamples, int *outRate, int *outChannels, bool stageSfx);
// Headerless raw PCM sfx sitting loose under BASE_PATH. Returns NULL when the
// file is absent, is still real Vorbis (starts "OggS"), or on read/alloc
// failure. Never half-loads.
Sint16 *DC_LoadRawSfx(const char *fullPath, size_t *outSamples, int *outRate, int *outChannels);

#if DC_AUDIO_SELFTEST
// Presents a solid white or black frame immediately. Used by the self-test to
// put a visual marker on the same instant a sound starts, so the latency can be
// measured off a recording instead of estimated by ear.
void DC_DebugFlash(bool on);

// Two identical 1000 Hz tones by two different routes — the device directly,
// then the engine's sfx mixer. Blocks for about four seconds. See DCAudio.cpp.
void DC_AudioSelfTest();
#endif

// ---- stage-SFX arena (DCAudio.cpp) ----
// Stage sfx are reloaded wholesale on every stage change, which fragments the
// heap. A bump arena cannot fragment internally: layout after a reset is
// byte-identical to the first load. DC_SfxAlloc falls back to malloc when
// stageSfx is false or the arena is unavailable, so DC_SfxFree — which must be
// used for EVERY sfx buffer — range-checks the pointer and does the right thing.
void DC_SfxArenaAcquire();
void *DC_SfxAlloc(size_t bytes, bool stageSfx);
void DC_SfxFree(void *ptr);
void DC_SfxArenaReset();

// ---- SFX in AICA sound RAM (DCAudio.cpp) ----
// The AICA has its own 2MB the main heap never touches. Sfx go there and play on
// hardware voices, costing the heap nothing, the SH4 no mixing time, and — the
// reason this exists at all — giving every sound its own pitch register, so a
// 22050 Hz sfx and a 44100 Hz sfx play correctly side by side with no shared
// device rate to compromise between. Sfx that live here have
// sfxList[].buffer == NULL, which the software mixer already skips, so playback
// routes cleanly.
//
// DC_AicaSfxLoad reads through the engine's OPEN file reader and consumes from
// it. On false the caller must SetFilePosition(0) before taking the heap path.
// It returns false for anything the hardware cannot hold — chiefly sounds over
// 65534 samples, which is a 16-bit register in the AICA and not negotiable — so
// the two systems coexist per sound rather than per build.
bool DC_AicaSfxLoad(size_t fileSize, int sfxID);
bool DC_AicaSfxHas(int sfxID);
bool DC_AicaSfxPlay(int sfxID, bool loop);
bool DC_AicaSfxSetAttr(int sfxID, int loopCount, sbyte pan);
void DC_AicaSfxStop(int sfxID);
void DC_AicaSfxStopAll();
void DC_AicaSfxUnloadStage();
void DC_AicaSfxUnloadGlobal();
// Source rate of a resident sfx, 0 if it is not on the hardware. LoadSfx puts
// this into sfxList[].rate so the perf overlay still reports something true.
int DC_AicaSfxRate(int sfxID);
// resident = sfx currently living in sound RAM, fallback = loaded sfx the
// hardware refused. Surfaced on the perf overlay as "HW nn SW nn".
void DC_AicaSfxStats(int *resident, int *fallback, int *spuKB);

#endif // RETRO_USING_KOS
#endif // DC_COMMON_HPP
