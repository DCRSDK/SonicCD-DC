// ---------------------------------------------------------------------
// RSDKv3 Dreamcast FMV — adapter around the Sonic Mania DC port's player
// ---------------------------------------------------------------------
// This file used to contain a whole pl_mpeg player. It doesn't any more, and
// the reason is worth recording, because the rewrite fixed four separate bugs
// at once.
//
// The old one OWNED THE PLAYBACK LOOP: DC_PlayVideo blocked until the movie
// ended. Every failure inside it therefore became a frozen console rather than
// a dropped cutscene, and no amount of watchdogging from inside a loop can fix
// a call that never returns. RSDKv3 already has the right structure and has had
// it all along — ENGINE_VIDEOWAIT, with ProcessVideo() called once per frame
// from the main loop (RetroEngine.cpp) and returning 1 when it is done. The
// engine keeps running: input, the frame watchdog, everything.
//
// The other three were in the PVR YUV path, and all three are visible by
// comparison with mpeg.c:
//
//   * The texture must be POWER OF TWO and plain PVR_TXRFMT_YUV422, not a
//     320-wide X32_STRIDE texture with PVR_TEXTURE_MODULO set.
//   * PVR_YUV_CFG is programmed from the TEXTURE's dimensions, not the frame's,
//     and short rows are padded with dummy macroblocks. Programming it from the
//     frame is what desynchronised the converter: it waited for 32 macroblocks
//     a row while being sent 20, so each row landed further off than the last
//     and the picture walked around the texture.
//   * Frames go to PVR_TA_YUV_CONV through STORE QUEUES, not DMA. None of the
//     pvr_dma_init / pvr_dma_ready / timeout machinery needs to exist.
//
// mpeg.c/mpeg.h and this pl_mpeg.h come from the Sonic Mania Dreamcast port
// (RSDKv5). pl_mpeg.h there is a Dreamcast fork that emits packed macroblocks
// as frame->display, so it is NOT interchangeable with upstream pl_mpeg.
// mpeg.c carries two local edits, both in setup_graphics and both commented:
// 640x480 vertex coordinates, and full-height 4:3 geometry instead of Mania's
// 16:9 letterbox.
//
// ASSETS: six MPEG-1 files in videos/ at the root of the disc image —
// Opening, OpeningUS, Good_Ending, Good_EndingUS, Bad_Ending, Bad_EndingUS.
// See convert_videos.bat. 29.97fps and no B-frames matter: this CPU will not
// decode 59.94fps 320x240 MPEG-1 in real time.

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS && DC_FMV

#include <kos.h>
#include <kos/fs.h>
#include <dc/pvr.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "DCCommon.hpp"

// mpeg.c is INCLUDED, not compiled separately: it defines
// PL_MPEG_IMPLEMENTATION and pulls in pl_mpeg.h, so it has to be exactly one
// translation unit. This is how the Mania port does it too.
#include "mpeg.h"
#include "mpeg.c"

#ifndef DC_VIDEO_DIR
#define DC_VIDEO_DIR "videos/"
#endif
#ifndef DC_VIDEO_EXT
#define DC_VIDEO_EXT ".mpg"
#endif

static mpeg_player_t *dcPlayer = NULL;
static bool dcVidAudioTaken    = false;
static bool dcVidFramePending  = false;

// Reported on the perf overlay; see DCCommon.hpp.
volatile int dcVidState  = DC_VID_IDLE;
volatile int dcVidFrames = 0;
volatile int dcVidDmaErr = 0;
volatile int dcVidWOut   = 0;
volatile int dcVidHOut   = 0;

// ---------------------------------------------------------------------
// Which file
// ---------------------------------------------------------------------
// The Dreamcast cannot carry two audio tracks in one MPEG-1 stream the way the
// PC build's OGV does, so the two soundtracks are two files: "Opening.mpg" and
// "OpeningUS.mpg". The script asks for one name; the region picks the suffix.
//
// The script's own name may already carry the suffix (the mobile scripts differ
// between regions), so a trailing "US" is stripped first and re-applied from
// the setting — otherwise a US save would ask for "OpeningUSUS".
static void dcResolveVideoPath(const char *name, char *out, size_t outLen)
{
    char base[64];
    size_t n = 0;
    for (; name[n] && n < sizeof(base) - 1; ++n) {
        if (name[n] == '.')
            break;
        base[n] = name[n];
    }
    base[n] = 0;

    bool isUS  = false;
    size_t len = n;
    if (len > 2) {
        const char a = base[len - 2], b = base[len - 1];
        if ((a == 'u' || a == 'U') && (b == 's' || b == 'S')) {
            isUS            = true;
            base[len - 2]   = 0;
        }
    }
    if (!isUS)
        isUS = GetGlobalVariableByName("Options.Soundtrack") != 0;

    snprintf(out, outLen, "%s%s%s%s%s", BASE_PATH, DC_VIDEO_DIR, base, isUS ? "US" : "", DC_VIDEO_EXT);
}

// ---------------------------------------------------------------------

bool DC_VideoStart(const char *name)
{
    char path[256];
    dcResolveVideoPath(name, path, sizeof(path));

    dcVidState  = DC_VID_OPENING;
    dcVidFrames = 0;
    dcVidDmaErr = 0;
    DC_PHASE(DC_PHASE_VIDEO);

    // Existence is checked here rather than left to plm_create_with_filename so
    // a missing per-region file can fall back to the bare name, and so a
    // genuinely absent movie reports DC_VID_NOFILE instead of a null player.
    file_t probe = fs_open(path, O_RDONLY);
    if (probe == FILEHND_INVALID) {
        char bare[256];
        snprintf(bare, sizeof(bare), "%s%s%s%s", BASE_PATH, DC_VIDEO_DIR, name, DC_VIDEO_EXT);
        probe = fs_open(bare, O_RDONLY);
        if (probe == FILEHND_INVALID) {
            DC_LOG("[DCVideo] '%s' not found (tried %s)\n", name, path);
            dcVidState = DC_VID_NOFILE;
            return false;
        }
        strncpy(path, bare, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }
    fs_close(probe);

    // The movie runs its own snd_stream at its own rate while the game's mixer
    // is at 22050. Stopping the game's stream is reversible; reconfiguring the
    // device is not.
    DC_SuspendAudio();
    dcVidAudioTaken = true;

    mpeg_player_options_t opts;
    memset(&opts, 0, sizeof(opts));
    // THE OPAQUE LIST, not punch-through.
    //
    // The Mania port uses PVR_LIST_PT_POLY, and copying that is what left the
    // screen blank: DC_InitRenderDevice sizes the TA's bins for this renderer,
    // and in the hardware-renderer configuration the punch-through bin is
    // PVR_BINSIZE_0 — disabled. Geometry submitted to a list the TA is not
    // binning is simply discarded, silently, which looks exactly like a decoder
    // that never produced a frame.
    //
    // The opaque list is enabled in every configuration and costs nothing here:
    // a movie fills the screen and has no transparency to punch through.
    opts.player_list_type   = PVR_LIST_OP_POLY;
    opts.player_filter_mode = PVR_FILTER_BILINEAR;
    opts.player_volume      = 255;
    opts.player_loop        = false;
    opts.extra_letterbox    = false; // Sonic CD's movies are 4:3 and full-screen

    dcPlayer = mpeg_player_create_ex(path, &opts);
    if (!dcPlayer) {
        DC_LOG("[DCVideo] '%s' opened but no player\n", path);
        dcVidState = DC_VID_BADSTREAM;
        DC_ResumeAudio();
        dcVidAudioTaken = false;
        return false;
    }

    dcVidState = DC_VID_PLAYING;
    DC_LOG("[DCVideo] playing %s\n", path);
    return true;
}

void DC_VideoStop()
{
    if (dcPlayer) {
        mpeg_player_destroy(dcPlayer);
        dcPlayer = NULL;
    }
    dcVidFramePending = false;
    if (dcVidAudioTaken) {
        DC_ResumeAudio();
        dcVidAudioTaken = false;
    }
    // The player leaves the PVR set up for its own texture; put the registers
    // and the atlas palettes back the way the engine expects.
    DC_ReinitRenderState();
    DC_PHASE(DC_PHASE_STAGE);
}

// One step per engine frame. Returns true when the movie is over — by ending,
// by erroring, or by being skipped — at which point the caller drops back out
// of ENGINE_VIDEOWAIT.
bool DC_VideoStep(bool skipRequested)
{
    if (!dcPlayer) {
        DC_VideoStop();
        return true;
    }

    DC_PHASE(DC_PHASE_VIDEO);

    // Decode until the player says it is caught up, not just once.
    //
    // mpeg_decode_step decodes AT MOST ONE frame per call, and only when the
    // wall clock has reached that frame's timestamp. Called once per engine
    // frame that caps playback at the engine's frame rate — so the moment a
    // frame costs more than its share, the video falls behind the audio (which
    // runs on its own stream, in real time) and can never recover. It just
    // drifts further out for the rest of the movie.
    //
    // Looping lets it catch up: each extra call decodes the next overdue frame.
    // The intermediate frames still have to be DECODED, because the ones after
    // them are P-frames that reference them, but only the last is uploaded, so
    // catching up costs decode time and no upload time.
    //
    // The cap is deliberately LOW. Decoding four frames in one engine frame and
    // showing only the last recovers the clock quickly but makes playback lurch,
    // which reads worse than being slightly behind. Two catches up at double
    // rate while still showing every other frame, so it converges without the
    // picture jumping. If two is not enough to keep up, the movie is beyond
    // this CPU and the answer is a lighter encode, not a bigger number here.
    mpeg_decode_result_t res = MPEG_DECODE_IDLE;
    bool haveFrame           = false;
    for (int i = 0; i < 2; ++i) {
        res = mpeg_decode_step(dcPlayer);
        if (res == MPEG_DECODE_FRAME) {
            haveFrame = true;
            continue; // maybe another one is already overdue
        }
        break; // IDLE means caught up; EOF/ERROR end it
    }

    if (skipRequested || res == MPEG_DECODE_EOF || res == MPEG_DECODE_ERROR) {
        dcVidState = skipRequested ? DC_VID_SKIPPED : (res == MPEG_DECODE_ERROR ? DC_VID_BADSTREAM : DC_VID_DONE);
        DC_VideoStop();
        return true;
    }

    // NOT uploaded here. See DC_VideoUpload.
    if (haveFrame)
        dcVidFramePending = true;

    // NOTHING IS DRAWN HERE, deliberately.
    //
    // The engine's main loop calls ProcessVideo() and then DC_FlipScreen(), and
    // DC_FlipScreen opens a scene of its own. Presenting here as well gave two
    // scenes per frame: the second one overwrote the first, so the movie's
    // audio played over a blank screen, and each frame waited on the PVR twice,
    // which is what made the sound hitch. The frame is uploaded here and drawn
    // from the present path — see DC_VideoDraw.
    return false;
}

// True while a movie owns the screen. DC_FlipScreen asks before presenting the
// game.
bool DC_VideoActive() { return dcPlayer != NULL; }

// Push the decoded frame into the texture. Called from the present path AFTER
// pvr_wait_ready(), and that timing is the entire point.
//
// The YUV converter writes into the same texture the PVR samples. Uploading
// from ProcessVideo — which runs before DC_FlipScreen — means writing into it
// while the hardware may still be rendering the PREVIOUS scene out of it. The
// part of the frame written before the PVR finishes survives; the rest is
// overwritten mid-read, so the picture tears part-way down and the damage
// varies with how long the last frame took. That is the band of corruption
// along the bottom, and it is why it came and went rather than being constant.
//
// pvr_wait_ready() returns once the previous scene is done with the texture, so
// uploading immediately after it is safe.
void DC_VideoUpload()
{
    if (!dcPlayer || !dcVidFramePending)
        return;
    mpeg_upload_frame(dcPlayer);
    ++dcVidFrames;
    dcVidFramePending = false;
}

// Called from inside the engine's scene, with the punch-through list open.
void DC_VideoDraw()
{
    if (dcPlayer)
        mpeg_draw_frame(dcPlayer);
}

#endif // RETRO_USING_KOS && DC_FMV
