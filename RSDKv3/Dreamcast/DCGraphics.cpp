// ---------------------------------------------------------------------
// RSDKv3 Dreamcast video backend — PHASE 1: software presenter only.
// ---------------------------------------------------------------------
// The engine software-renders into Engine.frameBuffer (RGB565, GFX_LINESIZE
// stride) exactly as it does on desktop, and this file copies that to VRAM.
// Nothing here touches the PVR: renderType is pinned to RENDER_SW by
// InitUserdata, so DrawSprite/DrawHLineScrollLayer/etc all take their software
// branch and the frame arrives here finished.
//
// This is deliberately the SMALLEST thing that boots, so that a failure at this
// stage is a failure in the platform layer (files, timing, input, audio) rather
// than in a renderer. See PORT_ANALYSIS.md §2: RSDKv3 already carries a
// hardware quad emitter behind `renderType == RENDER_HW`, and phase 3 of the
// port retargets THAT at the PVR rather than writing a new one. Do not grow
// this file into a half-renderer in the meantime.
//
// Two presentation paths, chosen at build time by DC_USE_PVR:
//
//   DC_USE_PVR = 0 (default for phase 1): 320x240 RGB565 video mode, framebuffer
//     copied straight to VRAM with the store queues. No PVR involvement at all.
//     Simplest possible path — if this shows a picture, the engine is running.
//
//   DC_USE_PVR = 1: 640x480 output, the 320x240 frame uploaded into a 512x256
//     RGB565 texture and drawn as one fullscreen quad. Costs a texture upload
//     per frame but gives a correctly scaled, filtered 480-line image and is
//     the stepping stone to phase 3. Build with -DDC_USE_PVR=ON to try it.
// ---------------------------------------------------------------------

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <arch/timer.h>
#include <dc/video.h>
#include <dc/sq.h>
#include <dc/pvr.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include "DCCommon.hpp"
#include "DCSave.hpp"

#ifndef DC_USE_PVR
#define DC_USE_PVR 0
#endif

// Per-frame timing to the serial log every 120 frames. This is the number that
// decides whether the 3D and FMV work is affordable — see PORT_ANALYSIS.md §4,
// which says to get these before committing to either. On by default during
// bring-up; turn it off once the figures are known.
#ifndef DC_PERF_LOG
#define DC_PERF_LOG 1
#endif

#if DC_USE_PVR
// 320x240 content in a 512x256 power-of-two RGB565 texture (the PVR requires
// power-of-two dimensions for non-stride textures).
#define DC_TEX_W (512)
#define DC_TEX_H (256)

static pvr_ptr_t dcFbTexture = NULL;
static pvr_poly_hdr_t dcFbHdr;
static bool dcPvrReady = false;
#endif

// ---------------------------------------------------------------------
// On-screen performance overlay
// ---------------------------------------------------------------------
// The perf numbers also go to the serial console, but getting at that needs
// dcload-serial or a BBA. This draws them straight into the software
// framebuffer instead, so they are readable on a TV, in a video capture, or in
// an emulator with no cable of any kind.
//
// That matters for more than convenience: an emulator JITs the SH4 and will not
// reproduce real cache and bus behaviour, so its timings are not a valid frame
// budget. The only measurement worth acting on is one taken on hardware — and
// this is how you take it without a debug cable.
//
// Drawn before the frame is presented, into Engine.frameBuffer directly, so it
// works on both presentation paths and costs nothing when DC_PERF_LOG is off.

// The font and the text renderer live outside DC_PERF_LOG: the hang report at
// the bottom of this file needs them in every build, and a build with the perf
// overlay off is exactly the build you most want a hang report from.

// 4x6 glyphs, one byte per row; bit3..bit0 = leftmost..rightmost pixel.
static const char dcFontChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ./-_";
static const byte dcFontGlyphs[41][6] = {
    { 0xF, 0x9, 0x9, 0x9, 0x9, 0xF }, // 0
    { 0x6, 0x2, 0x2, 0x2, 0x2, 0xF }, // 1
    { 0xF, 0x1, 0xF, 0x8, 0x8, 0xF }, // 2
    { 0xF, 0x1, 0xF, 0x1, 0x1, 0xF }, // 3
    { 0x9, 0x9, 0xF, 0x1, 0x1, 0x1 }, // 4
    { 0xF, 0x8, 0xF, 0x1, 0x1, 0xF }, // 5
    { 0xF, 0x8, 0xF, 0x9, 0x9, 0xF }, // 6
    { 0xF, 0x1, 0x2, 0x4, 0x4, 0x4 }, // 7
    { 0xF, 0x9, 0xF, 0x9, 0x9, 0xF }, // 8
    { 0xF, 0x9, 0xF, 0x1, 0x1, 0xF }, // 9
    { 0x6, 0x9, 0xF, 0x9, 0x9, 0x9 }, // A
    { 0xE, 0x9, 0xE, 0x9, 0x9, 0xE }, // B
    { 0x7, 0x8, 0x8, 0x8, 0x8, 0x7 }, // C
    { 0xE, 0x9, 0x9, 0x9, 0x9, 0xE }, // D
    { 0xF, 0x8, 0xE, 0x8, 0x8, 0xF }, // E
    { 0xF, 0x8, 0xE, 0x8, 0x8, 0x8 }, // F
    { 0x7, 0x8, 0xB, 0x9, 0x9, 0x7 }, // G
    { 0x9, 0x9, 0xF, 0x9, 0x9, 0x9 }, // H
    { 0xE, 0x4, 0x4, 0x4, 0x4, 0xE }, // I
    { 0x1, 0x1, 0x1, 0x1, 0x9, 0x6 }, // J
    { 0x9, 0xA, 0xC, 0xC, 0xA, 0x9 }, // K
    { 0x8, 0x8, 0x8, 0x8, 0x8, 0xF }, // L
    { 0x9, 0xF, 0xF, 0x9, 0x9, 0x9 }, // M
    { 0x9, 0xD, 0xF, 0xB, 0x9, 0x9 }, // N
    { 0x6, 0x9, 0x9, 0x9, 0x9, 0x6 }, // O
    { 0xE, 0x9, 0xE, 0x8, 0x8, 0x8 }, // P
    { 0x6, 0x9, 0x9, 0xB, 0xA, 0x5 }, // Q
    { 0xE, 0x9, 0xE, 0xC, 0xA, 0x9 }, // R
    { 0x7, 0x8, 0x6, 0x1, 0x1, 0xE }, // S
    { 0xF, 0x6, 0x6, 0x6, 0x6, 0x6 }, // T
    { 0x9, 0x9, 0x9, 0x9, 0x9, 0x6 }, // U
    { 0x9, 0x9, 0x9, 0x9, 0x6, 0x6 }, // V
    { 0x9, 0x9, 0xF, 0xF, 0xF, 0x9 }, // W
    { 0x9, 0x9, 0x6, 0x6, 0x9, 0x9 }, // X
    { 0x9, 0x9, 0x6, 0x4, 0x4, 0x4 }, // Y
    { 0xF, 0x1, 0x2, 0x4, 0x8, 0xF }, // Z
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, // space
    { 0x0, 0x0, 0x0, 0x0, 0x6, 0x6 }, // .
    { 0x1, 0x2, 0x2, 0x4, 0x4, 0x8 }, // /
    { 0x0, 0x0, 0xF, 0x0, 0x0, 0x0 }, // -
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0xF }, // _
};

// Pixel scale. 2 makes each glyph 8x12, which survives a 480-line capture and
// a CRT comfortably.
#ifndef DC_PERF_SCALE
#define DC_PERF_SCALE (2)
#endif

#define DC_PERF_GLYPH_W (4 * DC_PERF_SCALE + DC_PERF_SCALE)
#define DC_PERF_GLYPH_H (6 * DC_PERF_SCALE + DC_PERF_SCALE)

// Where the overlay draws.
//
// In software mode that is Engine.frameBuffer, in RGB565, exactly as before.
// In hardware mode there IS no software frame to draw into, so the overlay goes
// into a small ARGB1555 staging buffer that becomes a textured quad drawn last
// (see dcOverlayFlush). Same glyph code, same layout, two destinations.
static ushort *dcTextDst   = NULL;
static int dcTextStride    = 0;
static int dcTextW         = 0;
static int dcTextH         = 0;
static bool dcText1555     = false;

// 565 -> 1555, always opaque. R and G lose nothing that matters: R is already 5
// bits and drops one bit of position, G drops its low bit.
static inline ushort dcTextColour(ushort c)
{
    if (!dcText1555)
        return c;
    return (ushort)(0x8000u | ((c & 0xF800u) >> 1) | ((c & 0x07C0u) >> 1) | (c & 0x001Fu));
}

static void dcDrawTextScaled(int x, int y, const char *text, ushort colour565, int scale)
{
    if (!dcTextDst)
        return;

    const ushort colour = dcTextColour(colour565);
    const int glyphW    = 4 * scale + scale;

    for (const char *p = text; *p; ++p) {
        int gi = -1;
        for (int i = 0; dcFontChars[i]; ++i) {
            if (dcFontChars[i] == *p) {
                gi = i;
                break;
            }
        }
        if (gi >= 0) {
            for (int row = 0; row < 6; ++row) {
                const byte bits = dcFontGlyphs[gi][row];
                for (int col = 0; col < 4; ++col) {
                    if (!(bits & (0x8 >> col)))
                        continue;
                    for (int sy = 0; sy < scale; ++sy) {
                        const int py = y + row * scale + sy;
                        if (py < 0 || py >= dcTextH)
                            continue;
                        ushort *dst = &dcTextDst[py * dcTextStride];
                        for (int sx = 0; sx < scale; ++sx) {
                            const int px = x + col * scale + sx;
                            if (px >= 0 && px < dcTextW)
                                dst[px] = colour;
                        }
                    }
                }
            }
        }
        x += glyphW;
    }
}

static void dcDrawText(int x, int y, const char *text, ushort colour565) { dcDrawTextScaled(x, y, text, colour565, DC_PERF_SCALE); }

// Right-aligned unsigned decimal, so the columns don't jitter frame to frame.
static void dcFormatNum(char *out, uint value, int width)
{
    for (int i = width - 1; i >= 0; --i) {
        out[i] = (char)('0' + (value % 10));
        value /= 10;
    }
    out[width] = 0;
}

// How many distinct palette banks are in use on screen right now.
//
// This is the measurement that decides whether the phase-3 hardware renderer is
// straightforward or awkward, and it can be taken from the CURRENT software
// build — which is why it is here rather than waiting for the renderer.
//
// RSDKv3's hardware path keeps one 1024x1024 atlas per palette bank and selects
// between them with glBindTexture(gfxTextureID[texPaletteNum]). On the PVR the
// equivalent is one indexed atlas plus a palette bank per bank — but the PVR
// only has FOUR banks at 8bpp (1024 palette entries / 256), while RSDKv3 allows
// eight and builds six.
//
// If Sonic CD never has more than four live at once, the mapping is exact and
// free. If it does, banks have to be collapsed (wrong colours) or re-uploaded
// mid-frame (256 pvr_set_pal_entry calls per switch). Guessing either way would
// be expensive to unpick later.
//
// In software mode gfxLineBuffer[y] holds the bank for each scanline, so the
// live set is just the distinct values down the screen.
#if DC_PERF_LOG

static byte dcCountPaletteBanks()
{
    // Hardware mode has no per-scanline palette — gfxLineBuffer is not written
    // at all — so the live set is just the one bank the frame is bound to.
    if (renderType == RENDER_HW)
        return (byte)(texPaletteNum + 1);

    bool seen[PALETTE_COUNT];
    memset(seen, 0, sizeof(seen));
    byte n = 0;
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        const byte b = gfxLineBuffer[y];
        if (b < PALETTE_COUNT && !seen[b]) {
            seen[b] = true;
            ++n;
        }
    }
    return n;
}

// Overlay box extent, in the 320x240 coordinate space the glyphs are laid out
// in. Needed outside dcDrawPerfOverlay so the hardware path can size the quad
// it maps the staging buffer onto.
#define DC_PERF_BOX_W (DC_PERF_GLYPH_W * 11 + 4)

// Four extra rows, drawn at scale 1 so 18 characters fit in the same width, for
// the script-abort block. The space is reserved unconditionally rather than
// grown when an abort happens: these two numbers size a textured quad AND the
// per-frame texture upload, and a diagnostic that changes the shape of the
// render path at the exact moment something goes wrong is a diagnostic you end
// up debugging.
#define DC_PERF_ABORT_LINE (6 + 1)
#define DC_PERF_ABORT_H    (DC_PERF_ABORT_LINE * 10 + 2)

#if DC_HW_RENDER
#define DC_PERF_BOX_H (DC_PERF_GLYPH_H * 10 + 4 + DC_PERF_ABORT_H)
#else
#define DC_PERF_BOX_H (DC_PERF_GLYPH_H * 8 + 4 + DC_PERF_ABORT_H)
#endif

// Defined further down with the hang report; used by both.
static void dcSanitise(char *dst, const char *src, int max);

static void dcDrawPerfOverlay(uint fps, uint workUs, uint waitUs, uint blitUs, uint palPeak)
{
    if (!dcTextDst)
        return;

    // Dark backing box so the digits stay readable over bright tiles.
    const int boxW = DC_PERF_BOX_W;
    const int boxH = DC_PERF_BOX_H;
    const ushort boxColour = dcTextColour(0x0000);
    for (int y = 0; y < boxH && y < dcTextH; ++y) {
        ushort *dst = &dcTextDst[y * dcTextStride];
        for (int x = 0; x < boxW && x < dcTextW; ++x) dst[x] = boxColour;
    }

    char line[16], num[8];

    // FPS: green at 60, amber otherwise — readable at a glance from a sofa.
    const ushort fpsColour = fps >= 58 ? 0x07E0 : (fps >= 40 ? 0xFFE0 : 0xF800);
    dcFormatNum(num, fps, 3);
    sprintf(line, "FPS %s", num);
    dcDrawText(2, 2, line, fpsColour);

    // CPU = everything outside DC_FlipScreen: script, objects, software renderer.
    // This is the number that decides whether the hardware renderer is the fix.
    dcFormatNum(num, workUs > 99999 ? 99999 : workUs, 5);
    sprintf(line, "CPU %s", num);
    dcDrawText(2, 2 + DC_PERF_GLYPH_H, line, workUs > 16667 ? 0xF800 : 0xFFFF);

    // VBL = blocked waiting for the vertical blank.
    dcFormatNum(num, waitUs > 99999 ? 99999 : waitUs, 5);
    sprintf(line, "VBL %s", num);
    dcDrawText(2, 2 + DC_PERF_GLYPH_H * 2, line, 0xFFFF);

    // BLT = pushing the finished frame to VRAM.
    dcFormatNum(num, blitUs > 99999 ? 99999 : blitUs, 5);
    sprintf(line, "BLT %s", num);
    dcDrawText(2, 2 + DC_PERF_GLYPH_H * 3, line, 0xFFFF);

    // PAL = peak simultaneous palette banks. Red above 4 means the PVR's four
    // 8bpp banks are not enough and phase 3 needs a bank-swap strategy.
    dcFormatNum(num, palPeak, 3);
    sprintf(line, "PAL %s", num);
    dcDrawText(2, 2 + DC_PERF_GLYPH_H * 4, line, palPeak > 4 ? 0xF800 : 0x07E0);

    // Pause diagnostics.
    //
    // The original question — "does Start reach the engine?" — is answered: STA
    // climbs, so it does. And `pauseEnabled` is not the gate; Sonic CD's scripts
    // set Stage.PauseEnabled false and never set it true, so it stayed 0 for a
    // whole playthrough by design. Scene.cpp now asks the engine for the pause
    // menu directly (see the RETRO_USING_KOS block there).
    //
    // What is left worth watching:
    //   STA = count of INPUT_START press edges the input layer produced.
    //   PSE = stageMode, then the object type in slot 9. stageMode 1=NORMAL,
    //         2=PAUSED — so tapping Start should flip the first digit to 2.
    //         Slot 9 is where the Pause Menu object lives; it must read 0
    //         (Blank Object) for a pause to be allowed to start, and become
    //         non-zero while the menu is up.
    {
        extern int dcStartPresses;
        char a[6], b[6];
        dcFormatNum(a, (uint)(dcStartPresses % 1000), 3);
        sprintf(line, "STA %s", a);
        dcDrawText(2, 2 + DC_PERF_GLYPH_H * 5, line, dcStartPresses ? 0x07E0 : 0xFFE0);

        dcFormatNum(a, (uint)stageMode, 1);
        dcFormatNum(b, (uint)objectEntityList[9].type, 3);
        sprintf(line, "PSE %s %s", a, b);
        dcDrawText(2, 2 + DC_PERF_GLYPH_H * 6, line, stageMode == STAGEMODE_PAUSED ? 0x07E0 : 0xFFFF);
    }

#if DC_HW_RENDER
    // One-time boot figures for the indexed atlas. On screen because the serial
    // console needs a cable, and these are the two numbers that say whether the
    // 12 MB -> 1 MB plan actually works on hardware.
    //
    // ATL = ms to twiddle+upload 1 MB. This becomes a per-stage-load cost once
    //       the atlas holds real data, so a large number is a design problem.
    // VRM = KB of PVR VRAM left after the atlas and framebuffer texture.
    extern uint dcAtlasUploadMs, dcAtlasVramFreeK;
    dcFormatNum(num, dcAtlasUploadMs > 99999 ? 99999 : dcAtlasUploadMs, 5);
    sprintf(line, "ATL %s", num);
    dcDrawText(2, 2 + DC_PERF_GLYPH_H * 7, line, dcAtlasUploadMs > 100 ? 0xFFE0 : 0x07E0);

    dcFormatNum(num, dcAtlasVramFreeK > 99999 ? 99999 : dcAtlasVramFreeK, 5);
    sprintf(line, "VRM %s", num);
    dcDrawText(2, 2 + DC_PERF_GLYPH_H * 8, line, dcAtlasVramFreeK ? 0x07E0 : 0xF800);
#endif

    // SFX = the rate and channel count of the most recently PLAYED sound, as
    // the mixer sees them. A sound at the wrong pitch is either a file the
    // loader mis-read or not the file you think it is; 22050 means a headerless
    // raw asset (the loader has to assume that rate), anything else came out of
    // a real header. Amber when the source rate does not match the device, since
    // that is the case where the resampler has actual work to do.
    {
        const int r = dcLastSfxRate;
        dcFormatNum(num, (uint)(r < 0 ? 0 : (r > 99999 ? 99999 : r)), 5);
        char c[4];
        dcFormatNum(c, (uint)(dcLastSfxChannels & 7), 1);
        sprintf(line, "SFX %s %s", num, c);
        dcDrawText(2, 2 + DC_PERF_GLYPH_H * (DC_HW_RENDER ? 9 : 7), line, r == DC_AUDIO_RATE ? 0x07E0 : 0xFFE0);
    }

    // Script-abort block. Blank until the opcode budget in ProcessScript kills a
    // runaway call; from then on it STAYS on screen, because by the time anyone
    // reaches for a camera the interesting moment is several seconds gone.
    //
    // LOOP is the pair that matters: the bytecode offsets the interpreter was
    // cycling between. Everything else says which object; that says which code.
    const int y0 = DC_PERF_BOX_H - DC_PERF_ABORT_H;

    // Always on, abort or not: the count of sounds started twice at once.
    {
        char dup[48];
        dcFormatNum(num, (uint)(dcSfxDoubleStarts < 0 ? 0 : (dcSfxDoubleStarts > 99999 ? 99999 : dcSfxDoubleStarts)), 5);
        // PAN is here because the ring alternates two hard-panned slots; a pan
        // that reads 000 when a ring plays means the alternation is collapsing
        // to two centred copies of the same sample.
        char pn[8];
        dcFormatNum(pn, (uint)(dcLastSfxPan < 0 ? -dcLastSfxPan : dcLastSfxPan), 3);
        // HW/SW: how many currently-loaded sfx are living on AICA hardware
        // voices and how many the hardware refused (anything over 65534
        // samples). SW climbing into double figures means a stage is leaning on
        // the software mixer far more than it should be.
        int hw = 0, sw = 0;
        DC_AicaSfxStats(&hw, &sw, NULL);
        char hn[8], sn[8];
        dcFormatNum(hn, (uint)(hw > 99 ? 99 : hw), 2);
        dcFormatNum(sn, (uint)(sw > 99 ? 99 : sw), 2);
        sprintf(dup, "DUP %s PAN %s%s HW %s SW %s", num, dcLastSfxPan < 0 ? "-" : " ", pn, hn, sn);
        dcDrawTextScaled(2, y0, dup, dcSfxDoubleStarts ? 0xFFE0 : 0x07E0, 1);
    }

#if DC_FMV
    // Where the last cutscene got to. Stays on screen after the movie returns.
    if (dcVidState != DC_VID_IDLE) {
        char b[40], n2[12];
        // TWO digits. The states now run past 9, and a one-digit field turned
        // state 10 into "0" — which reads as "no video was ever attempted",
        // the exact opposite of what it meant.
        dcFormatNum(num, (uint)dcVidState, 2);
        dcFormatNum(n2, (uint)(dcVidFrames > 9999 ? 9999 : dcVidFrames), 4);
        char n3[12];
        dcFormatNum(n3, (uint)(dcVidDmaErr > 9999 ? 9999 : dcVidDmaErr), 4);
        sprintf(b, "VID %s F %s E %s", num, n2, n3);
        dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * 9, b,
                         (dcVidState == DC_VID_DONE || dcVidState == DC_VID_SKIPPED) ? 0x07E0 : 0xF800, 1);

    }
#endif

    if (DC_ScriptAborts()) {
        char big[40], n2[12];

        dcFormatNum(num, DC_ScriptAborts() > 99999 ? 99999 : DC_ScriptAborts(), 5);
        sprintf(big, "ABORTS %s", num);
        dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * 1, big, 0xF800, 1);

        // Named, not numbered: object type IDs are assigned per stage, so "004"
        // means one thing in an act and another in the special stage, and
        // reading it off a photograph is how a diagnosis goes wrong.
        {
            const int ty = DC_ScriptAbortType();
            char tn[14];
            dcSanitise(tn, (ty > 0 && ty < OBJECT_COUNT) ? typeNames[ty] : "?", (int)sizeof(tn));
            dcFormatNum(num, (uint)(unsigned short)DC_ScriptAbortObj(), 4);
            sprintf(big, "OBJ %s %s", num, tn);
            dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * 2, big, 0xFFFF, 1);
        }

        char op[13];
        dcSanitise(op, DC_ScriptAbortOpName(), (int)sizeof(op));
        dcFormatNum(num, (uint)DC_ScriptAbortSub(), 1);
        sprintf(big, "%s SUB %s", op, num);
        dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * 3, big, 0xFFE0, 1);

        dcFormatNum(num, (uint)DC_ScriptLoopLo(), 6);
        dcFormatNum(n2, (uint)DC_ScriptLoopHi(), 6);
        sprintf(big, "LOOP %s-%s", num, n2);
        dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * 4, big, 0x07FF, 1);

        // The last ten opcodes before the kill, oldest first, five to a line.
        // For a runaway loop this is the loop body; the numbers index the
        // engine's function table, so they identify the instructions exactly.
        {
            const int trail = DC_ScriptAbortTrailLen();
            for (int row = 0; row < 2; ++row) {
                char ops[40];
                ops[0] = 0;
                for (int k = 0; k < 5; ++k) {
                    const int idx = row * 5 + k;
                    if (idx >= trail)
                        break;
                    dcFormatNum(num, (uint)DC_ScriptAbortTrail(idx), 3);
                    if (k)
                        strcat(ops, " ");
                    strcat(ops, num);
                }
                dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * (5 + row), ops, 0xF81F, 1);
            }

            // And where each of those opcodes was fetched from, as an offset
            // from LOOP's low end. Read down the two rows together: each opcode
            // sits above its address, and the place where the address jumps
            // BACKWARDS is the instruction that closes the cycle.
            const int base = DC_ScriptLoopLo();
            for (int row = 0; row < 2; ++row) {
                char ads[40];
                ads[0] = 0;
                for (int k = 0; k < 5; ++k) {
                    const int idx = row * 5 + k;
                    if (idx >= trail)
                        break;
                    int off = DC_ScriptAbortTrailPtr(idx) - base;
                    if (off < 0)
                        off = 0;
                    if (off > 999)
                        off = 999;
                    dcFormatNum(num, (uint)off, 3);
                    if (k)
                        strcat(ads, " ");
                    strcat(ads, num);
                }
                dcDrawTextScaled(2, y0 + DC_PERF_ABORT_LINE * (7 + row), ads, 0x07E0, 1);
            }
        }
    }
}
#endif // DC_PERF_LOG

// ---------------------------------------------------------------------
// Phase 3: indexed sprite/tile atlas
// ---------------------------------------------------------------------
// See PHASE3_HW_RENDERER.md. Nothing draws with this yet — it is built and
// uploaded so the 12MB -> 1MB claim and the twiddle can be verified on hardware
// before the quad emitter is retargeted.

#if DC_HW_RENDER

#define DC_ATLAS_DIM   (HW_TEXTURE_SIZE)                     // 1024
#define DC_ATLAS_BYTES (DC_ATLAS_DIM * DC_ATLAS_DIM)         // 1 MB at 8bpp

byte *dcAtlasIndices        = NULL;
static pvr_ptr_t dcAtlasTex = NULL;
static bool dcPalDirty[PALETTE_COUNT];

// Surfaced on the perf overlay as well as the log: these are one-time boot
// figures, and the serial console needs a cable that not everyone has.
uint dcAtlasUploadMs  = 0;
uint dcAtlasVramFreeK = 0;

bool DC_AtlasInit()
{
    if (dcAtlasIndices && dcAtlasTex)
        return true;

    if (!dcAtlasIndices) {
        dcAtlasIndices = (byte *)malloc(DC_ATLAS_BYTES);
        if (!dcAtlasIndices) {
            DC_LOG("[DC] atlas: staging alloc of %d KB FAILED (largest free %u KB)\n", DC_ATLAS_BYTES / 1024,
                   (unsigned)(DC_LargestFreeBlock() / 1024));
            return false;
        }
        memset(dcAtlasIndices, 0, DC_ATLAS_BYTES); // index 0 == transparent
    }

    if (!dcAtlasTex) {
        dcAtlasTex = pvr_mem_malloc(DC_ATLAS_BYTES);
        if (!dcAtlasTex) {
            DC_LOG("[DC] atlas: VRAM alloc of %d KB FAILED\n", DC_ATLAS_BYTES / 1024);
            free(dcAtlasIndices);
            dcAtlasIndices = NULL;
            return false;
        }
    }

    for (int i = 0; i < PALETTE_COUNT; ++i) dcPalDirty[i] = true;

    // PAL8BPP draws its entries from a 256-colour bank; the PVR has 1024 palette
    // entries total, so four such banks. RSDKv3 allows eight. Whether that is a
    // problem is what the PAL counter in the perf overlay is measuring.
    pvr_set_pal_format(PVR_PAL_ARGB1555);

    DC_LOG("[DC] atlas: %dx%d PAL8BPP, %d KB VRAM (was 6 x RGBA5551 = %d KB)\n", DC_ATLAS_DIM, DC_ATLAS_DIM, DC_ATLAS_BYTES / 1024,
           (HW_TEXTURE_COUNT * DC_ATLAS_BYTES * 2) / 1024);
    return true;
}

void DC_AtlasRelease()
{
    if (dcAtlasTex) {
        pvr_mem_free(dcAtlasTex);
        dcAtlasTex = NULL;
    }
    if (dcAtlasIndices) {
        free(dcAtlasIndices);
        dcAtlasIndices = NULL;
    }
}

// The PVR samples twiddled textures only — palettised formats have no
// non-twiddled variant — so the linear staging buffer has to be interleaved into
// Morton order on the way to VRAM.
//
// pvr_txr_load_ex does exactly this and is the maintained path; rolling our own
// per-pixel bit-interleave would be ~10M operations for a 1024x1024 atlas and
// materially slower. Its docs warn it is slower than plain pvr_txr_load, but
// plain pvr_txr_load does not twiddle, so it is not an option here.
//
// This runs on stage load, not per frame — and it replaces SIX 2 MB uploads
// with one 1 MB one, so it is already far less work than what it displaces.
void DC_AtlasUpload()
{
    if (!dcAtlasIndices || !dcAtlasTex)
        return;

    const unsigned long long t0 = timer_ns_gettime64();
    pvr_txr_load_ex(dcAtlasIndices, dcAtlasTex, DC_ATLAS_DIM, DC_ATLAS_DIM, PVR_TXRLOAD_8BPP);
    dcAtlasUploadMs = (uint)((timer_ns_gettime64() - t0) / 1000000ULL);
    DC_LOG("[DC] atlas: uploaded in %ums\n", dcAtlasUploadMs);
}

// Draw the indexed atlas full-screen, as a PAL8BPP textured quad.
//
// This is the cheapest possible end-to-end test of the phase-3 data path, and it
// tests things the upload alone cannot: that the twiddling is right, that the
// PVR samples PAL8BPP correctly, that palette bank selection works, and that
// index 0 really is transparent. If tiles and sprites appear here in a 1024x1024
// sheet, everything downstream of the atlas is sound and only the emitter is
// left. If it comes up garbled, we find out now rather than while also debugging
// a renderer switch.
//
// Returns false if there is nothing to show, so the caller can fall back to the
// normal framebuffer present.
static bool dcDrawAtlasView()
{
    if (!dcAtlasTex)
        return false;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    // No NONTWIDDLED flag: paletted formats are twiddled-only, which is what
    // DC_AtlasUpload produced.
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(0), DC_ATLAS_DIM, DC_ATLAS_DIM, dcAtlasTex, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr, sizeof(hdr));

    // Square, centred on the 640x480 output, so the 1:1 atlas is not distorted.
    const float x0 = 80.0f, x1 = 560.0f, y0 = 0.0f, y1 = 480.0f;

    pvr_vertex_t v;
    v.flags = PVR_CMD_VERTEX;
    v.argb  = 0xFFFFFFFF;
    v.oargb = 0;
    v.z     = 1.0f;

    v.x = x0; v.y = y1; v.u = 0.0f; v.v = 1.0f; pvr_prim(&v, sizeof(v));
    v.x = x0; v.y = y0; v.u = 0.0f; v.v = 0.0f; pvr_prim(&v, sizeof(v));
    v.x = x1; v.y = y1; v.u = 1.0f; v.v = 1.0f; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = x1; v.y = y0; v.u = 1.0f; v.v = 0.0f; pvr_prim(&v, sizeof(v));

    pvr_list_finish();
    pvr_scene_finish();
    return true;
}

void DC_MarkPaletteDirty(int bank)
{
    if (bank >= 0 && bank < PALETTE_COUNT)
        dcPalDirty[bank] = true;
}

// Shadow copy of what is actually in the PVR's palette RAM, so a per-frame call
// can tell whether anything changed without the rest of the engine having to
// announce it.
//
// This is not just an optimisation, it is a feature the desktop GL renderer does
// not have. RSDKv3's hardware path bakes colours into its six atlases at stage
// load, so anything that edits a palette during play — fades, water lines,
// Sonic CD's time-travel palette swaps — simply does not show up. Because our
// atlas stores indices, re-sending 256 palette entries is the whole update, and
// at 512 bytes of memcmp per bank per frame we can afford to just watch for it.
static ushort dcPalShadow[4][PALETTE_SIZE];
static bool dcPalShadowValid = false;

void DC_AtlasUploadPalettes()
{
    // Only the four banks the PVR can hold at 8bpp. If the PAL counter shows
    // Sonic CD wanting more than four at once, this is where the bank-swap
    // strategy lands.
    const int banks = PALETTE_COUNT < 4 ? PALETTE_COUNT : 4;

    for (int b = 0; b < banks; ++b) {
        if (!dcPalDirty[b] && dcPalShadowValid && memcmp(dcPalShadow[b], fullPalette[b], sizeof(dcPalShadow[b])) == 0)
            continue;
        dcPalDirty[b] = false;
        memcpy(dcPalShadow[b], fullPalette[b], sizeof(dcPalShadow[b]));

        for (int i = 0; i < PALETTE_SIZE; ++i) {
            // Format conversion is a one-bit rotate, and it is worth spelling
            // out why rather than trusting the coincidence.
            //
            // In hardware mode PACK_RGB888 uses RGB888_TO_RGB5551 (Palette.hpp),
            // which lays the engine's colour out as:
            //     bits 15..11 R | 10..6 G | 5..1 B | bit 0 alpha
            // and SetPaletteEntry sets that alpha bit on every non-zero index,
            // leaving index 0 clear. So the engine ALREADY encodes "index 0 is
            // the colour key" in exactly the bit the PVR wants.
            //
            // PVR_PAL_ARGB1555 wants:
            //     bit 15 alpha | 14..10 R | 9..5 G | 4..0 B
            //
            // The three colour fields are contiguous in 15..1, so shifting right
            // by one lands them in 14..0, and the alpha bit moves from 0 to 15.
            const ushort c   = fullPalette[b][i];
            const uint argb  = i ? ((uint)(c >> 1) | ((uint)(c & 1u) << 15)) : 0u;
            pvr_set_pal_entry((uint)(b * PALETTE_SIZE + i), argb);
        }
    }

    dcPalShadowValid = true;
}

// ---------------------------------------------------------------------
// Phase 3 steps 4+5: the quad emitter, retargeted at the PVR
// ---------------------------------------------------------------------
// RSDKv3's hardware path does not draw anything itself. Every Draw* function
// APPENDS to two flat arrays and returns:
//
//   gfxPolyList[]  four DrawVertex per quad, in TL,TR,BL,BR order
//                  x,y are screen pixels in 1/16ths (XPos << 4)
//                  u,v are texels into the 1024x1024 atlas
//                  colour is a per-vertex RGBA modulate
//   polyList3D[]   the same, but float xyz in world space, for the 3D floor
//
// and two split points: everything before gfxVertexSizeOpaque was emitted with
// blending off, everything after with blending on. The desktop renderer then
// makes exactly three glDrawElements calls out of that (FlipScreenNoFB).
//
// So there is no renderer to write — only a submission path. The index buffer
// RSDKv3 builds is (0,1,2),(1,3,2) per quad, and a PVR triangle strip of
// v0,v1,v2,v3 expands to (v0,v1,v2),(v1,v2,v3): the same two triangles over the
// same four vertices, differing only in winding, which does not matter with
// culling off. The vertices go to the hardware in the order they already sit in
// memory, four at a time, with no reordering and no index buffer at all.

// Untextured quads are the one thing that does NOT survive the switch to an
// indexed atlas. ClearScreen, DrawRectangle (which is how fades are drawn) and
// DrawFace are all "textured" quads in RSDKv3's HW path — they point every
// vertex at the same texel inside a 16x16 white block that
// UpdateTextureBufferWithTiles stamps over tile 0. That block is written as a
// COLOUR straight into texBuffer, so it cannot exist in an index atlas: there is
// no palette index guaranteed to be white.
//
// The fix is to spot those quads and draw them with a flat-colour context
// instead, which is what the engine actually meant. The test is exact rather
// than a UV range guess: a real textured quad spans a rectangle of the atlas, so
// its four U (or V) values are never all identical. Only the degenerate
// "sample one texel" quads are.
static inline bool dcQuadIsFlat(const DrawVertex *q)
{
    return q[0].u == q[1].u && q[1].u == q[2].u && q[2].u == q[3].u && q[0].v == q[1].v && q[1].v == q[2].v && q[2].v == q[3].v;
}

// Screen mapping. The engine works in SCREEN_XSIZE x SCREEN_YSIZE at 1/16 pixel;
// the PVR draws at viewWidth x viewHeight. On the Dreamcast that is 320x240 into
// 640x480, so this is exactly 1/8 — but it is written out in full so a different
// output size does not silently break it.
static float dcVtxScaleX = 1.0f / 8.0f;
static float dcVtxScaleY = 1.0f / 8.0f;

static void dcBuildHeaders(pvr_list_t list, pvr_poly_hdr_t *hdrTex, pvr_poly_hdr_t *hdrCol)
{
    pvr_poly_cxt_t cxt;

    // PVR_TXRFMT_8BPP_PAL picks which 256-entry bank this batch samples, which
    // is the direct equivalent of the desktop path's
    // glBindTexture(gfxTextureID[texPaletteNum]).
    int bank = texPaletteNum;
    if (bank < 0)
        bank = 0;
    if (bank > 3)
        bank = 3;

    pvr_poly_cxt_txr(&cxt, list, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(bank), DC_ATLAS_DIM, DC_ATLAS_DIM, dcAtlasTex, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    // The desktop renderer runs with GL_DEPTH_TEST off and relies purely on
    // submission order. Matching that means never letting depth reject anything;
    // combined with autosort turned off in pvr_init, the PVR then draws the
    // translucent list strictly in the order given, which is the painter's
    // ordering the engine's draw lists already encode.
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write      = false;
    cxt.txr.env          = PVR_TXRENV_MODULATEALPHA; // GL_MODULATE on RGBA
    pvr_poly_compile(hdrTex, &cxt);

    pvr_poly_cxt_col(&cxt, list);
    cxt.gen.culling      = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write      = false;
    pvr_poly_compile(hdrCol, &cxt);
}

// Submit gfxPolyList vertices [vStart, vEnd) as quads.
static void dcSubmitQuads(int vStart, int vEnd, pvr_list_t list)
{
    if (vEnd - vStart < 4)
        return;

    pvr_poly_hdr_t hdrTex, hdrCol;
    dcBuildHeaders(list, &hdrTex, &hdrCol);

    pvr_vertex_t v;
    v.oargb = 0;
    v.z     = 1.0f; // depth is unused; see dcBuildHeaders

    int mode = -1; // -1 none yet, 0 textured, 1 flat colour
    for (int i = vStart; i + 3 < vEnd; i += 4) {
        const DrawVertex *q = &gfxPolyList[i];

        // Skip zero-area quads.
        //
        // The engine emits four vertices for an animation frame even when that
        // frame has no extent, which is how the blue shield blinks: its off
        // frames are empty. OpenGL throws a degenerate triangle away; the PVR
        // does not necessarily, and a quad with no height can still cover the
        // pixel centres along a single row — which is exactly the thin bright
        // line that showed up across Sonic on the shield's off frames.
        //
        // Worth doing regardless of that bug: a zero-area quad is four vertices
        // of pure TA traffic even when it happens to rasterise to nothing.
        if ((q[0].x == q[1].x && q[1].x == q[2].x && q[2].x == q[3].x)
            || (q[0].y == q[1].y && q[1].y == q[2].y && q[2].y == q[3].y))
            continue;

        const int want = dcQuadIsFlat(q) ? 1 : 0;
        if (want != mode) {
            mode = want;
            pvr_prim(want ? &hdrCol : &hdrTex, sizeof(pvr_poly_hdr_t));
        }

        for (int k = 0; k < 4; ++k) {
            v.flags = (k == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x     = q[k].x * dcVtxScaleX;
            v.y     = q[k].y * dcVtxScaleY;
            v.u     = q[k].u * (1.0f / DC_ATLAS_DIM);
            v.v     = q[k].v * (1.0f / DC_ATLAS_DIM);
            v.argb  = ((uint)q[k].colour.a << 24) | ((uint)q[k].colour.r << 16) | ((uint)q[k].colour.g << 8) | (uint)q[k].colour.b;
            pvr_prim(&v, sizeof(v));
        }
    }
}

// The 3D floor (special stages, and the 3D-sky layers).
//
// The desktop path hands polyList3D to GL with a perspective projection and a
// modelview of scale * rotate-Y * translate, drawn into a viewport that is
// deliberately a few lines taller than the screen. There is no GL here, so the
// same transform is done by hand — it is a dozen multiplies per vertex, and at
// the couple of thousand vertices this list actually reaches that is well under
// a millisecond on the SH4.
//
// One difference from GL: no near-plane clipping. GL would clip a quad that
// straddles the eye plane; we drop it instead. That can pop a single row of
// floor tiles right at the camera, which is a far better failure than the
// wrap-around garbage an unclipped w <= 0 vertex produces on the PVR.
static void dcSubmit3D()
{
    if (indexSize3D < 6 || vertexSize3D < 4)
        return;

    pvr_poly_hdr_t hdrTex, hdrCol; // the floor is always textured; hdrCol unused
    dcBuildHeaders(PVR_LIST_TR_POLY, &hdrTex, &hdrCol);
    (void)hdrCol;
    pvr_prim(&hdrTex, sizeof(hdrTex));

    // CalcPerspective(1.8326f, viewAspect, 0.1f, 2000.0f), reduced: only the two
    // scale terms survive, because the depth output is unused (the PVR wants
    // 1/w, not a remapped z).
    const float pw = 1.0f / tanf(1.8326f * 0.5f);
    const float ph = (1.0f / (pw * viewAspect)) * 0.5f;

    // glScalef(1.35, -0.9, -1.0) * glRotatef(angle + 180, Y) * glTranslatef(pos)
    const float ang = (floor3DAngle + 180.0f) * 0.01745329252f;
    const float ca  = cosf(ang);
    const float sa  = sinf(ang);

    // glViewport(viewOffsetX, floor3DTop, viewWidth, floor3DBottom)
    const float vpScale = (float)viewHeight / (float)SCREEN_YSIZE;
    const float vpX     = (float)viewOffsetX;
    const float vpW     = (float)viewWidth;
    const float vpY     = -2.0f * vpScale;
    const float vpH     = (float)viewHeight - 4.0f * vpScale;
    const float fbH     = (float)viewHeight + (float)viewOffsetY;

    float sx[4], sy[4], sz[4];

    const int vEnd = vertexSize3D;
    for (int i = 0; i + 3 < vEnd; i += 4) {
        const DrawVertex3D *q = &polyList3D[i];

        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            const float tx = q[k].x + floor3DXPos;
            const float ty = q[k].y + floor3DYPos;
            const float tz = q[k].z + floor3DZPos;

            const float rx = tx * ca + tz * sa;
            const float rz = -tx * sa + tz * ca;

            const float ex = rx * 1.35f;
            const float ey = ty * -0.9f;
            const float ez = -rz; // the -1.0 of the scale; also the clip w

            if (ez < 0.1f) { // near plane
                ok = false;
                break;
            }

            const float inv = 1.0f / ez;
            sx[k]           = vpX + ((pw * ex * inv) + 1.0f) * 0.5f * vpW;
            sy[k]           = fbH - (vpY + ((ph * ey * inv) + 1.0f) * 0.5f * vpH);
            sz[k]           = inv;
        }
        if (!ok)
            continue;

        pvr_vertex_t v;
        v.oargb = 0;
        for (int k = 0; k < 4; ++k) {
            v.flags = (k == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x     = sx[k];
            v.y     = sy[k];
            v.z     = sz[k];
            v.u     = q[k].u * (1.0f / DC_ATLAS_DIM);
            v.v     = q[k].v * (1.0f / DC_ATLAS_DIM);
            v.argb  = ((uint)q[k].colour.a << 24) | ((uint)q[k].colour.r << 16) | ((uint)q[k].colour.g << 8) | (uint)q[k].colour.b;
            pvr_prim(&v, sizeof(v));
        }
    }
}

// ---- the perf overlay, as a textured quad ----------------------------------
//
// In software mode the overlay is simply drawn into the frame before it is
// presented. There is no such frame here, so it goes into a small ARGB1555
// staging buffer and is uploaded as a texture drawn last, over everything.
//
// Only the rows the box actually occupies are uploaded — the texture has to be
// a power of two tall, but pushing all 256 rows every frame would be 64 KB of
// store-queue traffic for 34 KB of content.
#if DC_PERF_LOG

#define DC_OVL_W    (128)
#define DC_OVL_H    (256)
#define DC_OVL_ROWS (DC_PERF_BOX_H + 2)

static pvr_ptr_t dcOverlayTex = NULL;
static ushort *dcOverlayBuf   = NULL;
static pvr_poly_hdr_t dcOverlayHdr;

static bool dcOverlayInit()
{
    if (dcOverlayBuf && dcOverlayTex)
        return true;

    if (!dcOverlayBuf) {
        dcOverlayBuf = (ushort *)memalign(32, DC_OVL_W * DC_OVL_H * 2);
        if (!dcOverlayBuf)
            return false;
        memset(dcOverlayBuf, 0, DC_OVL_W * DC_OVL_H * 2);
    }
    if (!dcOverlayTex) {
        dcOverlayTex = pvr_mem_malloc(DC_OVL_W * DC_OVL_H * 2);
        if (!dcOverlayTex)
            return false;
        // Once, so the rows below the box are transparent rather than whatever
        // was in VRAM. After this only DC_OVL_ROWS are ever re-sent.
        pvr_txr_load(dcOverlayBuf, dcOverlayTex, DC_OVL_W * DC_OVL_H * 2);
    }

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED, DC_OVL_W, DC_OVL_H, dcOverlayTex, PVR_FILTER_NONE);
    cxt.gen.culling      = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write      = false;
    pvr_poly_compile(&dcOverlayHdr, &cxt);
    return true;
}

static void dcOverlayFlush()
{
    if (!dcOverlayTex || !dcOverlayBuf)
        return;

    pvr_txr_load(dcOverlayBuf, dcOverlayTex, DC_OVL_W * DC_OVL_ROWS * 2);

    // The glyphs are laid out in the engine's 320x240 space, so the quad is
    // scaled by the same factor as everything else and the overlay stays the
    // size it was in the software build.
    const float x1 = DC_PERF_BOX_W * (16.0f * dcVtxScaleX);
    const float y1 = DC_PERF_BOX_H * (16.0f * dcVtxScaleY);
    const float u1 = (float)DC_PERF_BOX_W / DC_OVL_W;
    const float v1 = (float)DC_PERF_BOX_H / DC_OVL_H;

    pvr_prim(&dcOverlayHdr, sizeof(dcOverlayHdr));

    pvr_vertex_t v;
    v.oargb = 0;
    v.argb  = 0xFFFFFFFF;
    v.z     = 1.0f;

    v.flags = PVR_CMD_VERTEX;
    v.x = 0.0f; v.y = y1;   v.u = 0.0f; v.v = v1;   pvr_prim(&v, sizeof(v));
    v.x = 0.0f; v.y = 0.0f; v.u = 0.0f; v.v = 0.0f; pvr_prim(&v, sizeof(v));
    v.x = x1;   v.y = y1;   v.u = u1;   v.v = v1;   pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = x1;   v.y = 0.0f; v.u = u1;   v.v = 0.0f; pvr_prim(&v, sizeof(v));
}

#endif // DC_PERF_LOG

// ---- one hardware frame ----------------------------------------------------
//
// The list order is fixed by the PVR (opaque, then translucent) and happens to
// be exactly the order FlipScreenNoFB draws in: the pre-split batch with
// blending off, the 3D floor, then the post-split batch with blending on.
void DC_PresentHW()
{
    // Palette edits during play are picked up here. See dcPalShadow.
    DC_AtlasUploadPalettes();

    pvr_wait_ready();
    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    dcSubmitQuads(0, gfxVertexSizeOpaque, PVR_LIST_OP_POLY);
    pvr_list_finish();

    // Only open the translucent list if something is actually going into it.
    //
    // Until DC_PERF_LOG defaulted off, dcOverlayFlush() put a quad in this list
    // on every single frame, so it was never empty and this was never a
    // question. With the overlay gone, a screen that draws nothing blended —
    // a title screen or a menu is the likely case — would open the list and
    // immediately close it with zero primitives. That is a shape the TA is not
    // asked to handle anywhere else in this port, and it is a code path that
    // has therefore never actually run on hardware. Cheap to make impossible.
    bool trWork = render3DEnabled || gfxVertexSize > gfxVertexSizeOpaque;
#if DC_PERF_LOG
    // The overlay only submits when its texture and staging buffer both exist;
    // if the alloc failed at init it draws nothing, so this has to check rather
    // than assume.
    trWork = trWork || (dcOverlayTex && dcOverlayBuf);
#endif

    if (trWork) {
        pvr_list_begin(PVR_LIST_TR_POLY);
        if (render3DEnabled)
            dcSubmit3D();
        dcSubmitQuads(gfxVertexSizeOpaque, gfxVertexSize, PVR_LIST_TR_POLY);
#if DC_PERF_LOG
        dcOverlayFlush();
#endif
        pvr_list_finish();
    }

    pvr_scene_finish();
}

#endif // DC_HW_RENDER

int DC_InitRenderDevice()
{
    DC_LOG("[DC] video: init (DC_USE_PVR=%d)\n", DC_USE_PVR);

    // 320x240 is the native Dreamcast mode and the engine's own base size, so
    // no scaling is needed anywhere in the software path.
    SCREEN_XSIZE        = 320;
    SCREEN_XSIZE_CONFIG = 320;
    SCREEN_CENTERX      = SCREEN_XSIZE / 2;
    viewOffsetX         = 0;
    viewOffsetY         = 0;

    // SetScreenSize is what actually establishes GFX_LINESIZE and the
    // SCREEN_SCROLL_* / OBJECT_BORDER_X2 values the rest of the engine reads.
    // The (width + 9) & -0x8 rounding is the software path's own rule, lifted
    // from SetScreenDimensions so we don't have to call that (it drags in
    // SDL/GL framebuffer teardown that has no meaning here).
    SetScreenSize(SCREEN_XSIZE, (SCREEN_XSIZE + 9) & -0x8);

    touchWidth  = SCREEN_XSIZE;
    touchHeight = SCREEN_YSIZE;

#if DC_HW_RENDER && DC_USE_PVR
    // The hardware path draws at the PVR's output resolution, not the engine's
    // internal one, so the quad emitter's 1/16-pixel coordinates scale straight
    // up to 640x480 with no intermediate 320x240 buffer. viewWidth/viewHeight
    // are what the desktop renderer's viewport uses and what the 3D floor's
    // projection is derived from, so they have to be the real output size.
    viewWidth  = 640;
    viewHeight = 480;
#else
    viewWidth  = SCREEN_XSIZE;
    viewHeight = SCREEN_YSIZE;
#endif
    // Height over width, matching SetScreenDimensions. Feeds CalcPerspective for
    // the 3D floor; it is a shape, not a pixel count, so it does not change with
    // the output size.
    viewAspect = 0.75f;

    Engine.frameBuffer = new ushort[GFX_LINESIZE * SCREEN_YSIZE];
    if (!Engine.frameBuffer) {
        DC_LOG("[DC] video: framebuffer alloc FAILED (%d bytes)\n", (int)(GFX_LINESIZE * SCREEN_YSIZE * sizeof(ushort)));
        return 0;
    }
    memset(Engine.frameBuffer, 0, (GFX_LINESIZE * SCREEN_YSIZE) * sizeof(ushort));

    // HQ (2x) mode and the GL upload buffers have no Dreamcast equivalent.
    Engine.frameBuffer2x = NULL;
    Engine.texBuffer     = NULL;
    Engine.texBuffer2x   = NULL;
    Engine.useHQModes    = false;
    drawStageGFXHQ       = false;

    // GROUND DRAW DISTANCE in the special stage.
    //
    // Despite the name this is NOT the 2x framebuffer mode — the only thing it
    // controls is the extent of the tile grid Draw3DFloorLayer builds:
    //
    //   off   20x20 tiles starting at -0xA0   ->  160 units of floor ahead
    //   on    32x32 tiles starting at -0x100  ->  256 units of floor ahead
    //
    // On desktop it is tied to `viewHeight > SCREEN_YSIZE * 2`, i.e. to the HQ
    // framebuffer, which is why porting the HQ modes off dragged this off with
    // them and shortened the horizon. The PVR does not care: 1024 quads instead
    // of 400 is nothing to it, and VERTEX3D_COUNT (0x1904) has room for the
    // 4100 vertices the full grid needs.
    //
    // The cost is on the SH4, in the tile loop that builds those quads — 2.5x
    // the iterations. Turn DC_HQ_3D_FLOOR off if the special stage drops frames.
    hq3DFloorEnabled = DC_HQ_3D_FLOOR ? true : false;

    Engine.borderless   = false;
    Engine.isFullScreen = true;

#if DC_USE_PVR
    vid_set_mode(DM_640x480, PM_RGB565);

    // Bin order is opaque, opaque-modifier, translucent, translucent-modifier,
    // punch-through.
    //
    // AUTOSORT IS OFF, deliberately. With it on the PVR sorts the translucent
    // list by depth, and every quad the engine emits has the same depth — so the
    // order would be undefined exactly where it matters most. The engine's draw
    // lists ARE the sort: layer 0 behind layer 1 behind the sprites. Turning
    // autosort off makes the hardware honour submission order, which is what the
    // desktop renderer gets for free by disabling the depth test.
    // One frame's worth of geometry for ALL lists. Double-buffered (the
    // vbuf_doublebuf_disabled field below is left zero), so this costs twice
    // this much VRAM but a single frame may only use this much.
#define DC_PVR_VBUF_BYTES (1024 * 1024)

#if DC_HW_RENDER
    // The special-stage floor is by far the largest thing that ever enters this
    // buffer: HQ_FLOOR_GRID squared quads, four 32-byte pvr_vertex_t each. It
    // has to leave room for the stage itself — four tile layers at roughly 340
    // quads apiece, plus every sprite — so it is capped at three quarters here.
    //
    // Exceeding the buffer does not fail cleanly; the TA runs out of room
    // mid-frame and geometry silently vanishes. Better to refuse to build.
    // FLOOR3D_QUAD_BUDGET is the fine grid plus the coarse ring (Drawing.hpp).
    static_assert((long)FLOOR3D_QUAD_BUDGET * 4 * 32 <= (long)DC_PVR_VBUF_BYTES * 3 / 4,
                  "floor too large: the fine grid plus the LOD ring would take over 3/4 of the PVR vertex buffer");
#endif

    static const pvr_init_params_t pvrParams = {
#if DC_HW_RENDER
        // Real geometry now: a full 320x240 screen of 16px tiles is ~340 quads
        // per layer, four layers plus sprites.
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        DC_PVR_VBUF_BYTES, // vertex buffer
#else
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        256 * 1024, // vertex buffer — one fullscreen quad
#endif
        0, // no DMA
        0, // no FSAA
        1, // autosort DISABLED (see above)
        2  // extra OPBs
    };
    pvr_init(&pvrParams);

    dcFbTexture = pvr_mem_malloc(DC_TEX_W * DC_TEX_H * 2);
    if (!dcFbTexture) {
        DC_LOG("[DC] video: framebuffer texture alloc FAILED\n");
        return 0;
    }

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, DC_TEX_W, DC_TEX_H, dcFbTexture, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE; // a fullscreen quad must never be culled
    pvr_poly_compile(&dcFbHdr, &cxt);

    dcPvrReady = true;

#if DC_HW_RENDER
    // The atlas is zero-filled here (index 0 = transparent) and gets its real
    // contents on the first stage load, from UpdateHardwareTextures.
    if (DC_AtlasInit()) {
        DC_AtlasUpload();
        DC_AtlasUploadPalettes();
        dcAtlasVramFreeK = (uint)(pvr_mem_available() / 1024);
        DC_LOG("[DC] atlas: VRAM free after atlas = %u KB\n", dcAtlasVramFreeK);
    }

    // Screen-space scale for the quad emitter, derived once. On the Dreamcast
    // this is 1/8: the engine's coordinates are pixels << 4 in a 320-wide space,
    // and the output is 640 wide.
    dcVtxScaleX = (float)viewWidth / (float)(SCREEN_XSIZE << 4);
    dcVtxScaleY = (float)viewHeight / (float)(SCREEN_YSIZE << 4);

#if DC_PERF_LOG
    if (!dcOverlayInit())
        DC_LOG("[DC] video: perf overlay texture alloc FAILED (overlay disabled)\n");
#endif
#endif
#else
    vid_set_mode(vid_check_cable() != CT_VGA ? DM_320x240_NTSC : DM_320x240_VGA, PM_RGB565);
    vid_empty();
#endif

    DC_LOG("[DC] video: init ok (%dx%d, linesize=%d)\n", SCREEN_XSIZE, SCREEN_YSIZE, GFX_LINESIZE);
    return 1;
}

// ---------------------------------------------------------------------
// Hang report
// ---------------------------------------------------------------------
// Called from the audio thread when the main loop's heartbeat stops. See the
// DC_PHASE block in DCCommon.hpp for why this exists.
//
// It runs on a thread that is by definition still alive, and the thread it is
// reporting on is stuck somewhere that is not going to touch the PVR again — so
// taking the video hardware away from it is safe in exactly the situation this
// is called in, and pointless to worry about in any other.
static const char *dcPhaseName(unsigned char p)
{
    switch (p) {
        case DC_PHASE_INPUT: return "INPUT";
        case DC_PHASE_STAGE: return "STAGE SCRIPTS";
        case DC_PHASE_PRESENT: return "PRESENT";
        case DC_PHASE_STAGELOAD: return "STAGE LOAD";
        case DC_PHASE_SFXLOAD: return "SFX LOAD";
        case DC_PHASE_MUSICLOAD: return "MUSIC LOAD";
        case DC_PHASE_ATLAS: return "ATLAS UPLOAD";
        case DC_PHASE_DEVMENU: return "DEV MENU";
        case DC_PHASE_DRAW: return "DRAWING OBJECTS";
        case DC_PHASE_VIDEO: return "FMV PLAYBACK";
        default: return "BOOT";
    }
}

// Uppercase, and only the characters the 4x6 font actually has — anything else
// becomes a dot rather than silently disappearing.
static void dcSanitise(char *dst, const char *src, int max)
{
    int n = 0;
    for (; src[n] && n < max - 1; ++n) {
        char c = src[n];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        dst[n] = (strchr(dcFontChars, c) && c != 0) ? c : '.';
    }
    dst[n] = 0;
}

void DC_HangReport()
{
    static bool reported = false;
    if (reported)
        return;
    reported = true;

    const unsigned char phase = dcPhase;
    const unsigned char alock = dcAudioLockWait;
    const uint frames         = dcHeartbeat;

    DC_LOG("[DC] HANG: phase=%u (%s) audioLockWait=%u frames=%u\n", phase, dcPhaseName(phase), alock, frames);

#if DC_USE_PVR
    if (dcPvrReady) {
        pvr_shutdown();
        dcPvrReady = false;
    }
#endif
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_empty();

    dcTextDst    = (ushort *)vram_s;
    dcTextStride = 640;
    dcTextW      = 640;
    dcTextH      = 480;
    dcText1555   = false;

    char line[64], num[12];

    dcDrawTextScaled(24, 32, "MAIN LOOP STOPPED", 0xF800, 4);
    dcDrawTextScaled(24, 84, "THE GAME IS NOT CRASHED. IT IS STUCK.", 0xFFFF, 2);

    sprintf(line, "PHASE %u  %s", (unsigned)phase, dcPhaseName(phase));
    dcDrawTextScaled(24, 140, line, 0xFFE0, 3);

    // The one that matters most: a main thread blocked here means the audio
    // mutex deadlocked, and nothing else needs investigating.
    dcDrawTextScaled(24, 190, alock ? "WAITING ON AUDIO LOCK  YES" : "WAITING ON AUDIO LOCK  NO", alock ? 0xF800 : 0x07E0, 3);

    dcFormatNum(num, frames, 8);
    sprintf(line, "FRAMES %s", num);
    dcDrawTextScaled(24, 240, line, 0xFFFF, 3);

    dcFormatNum(num, (uint)activeStageList, 1);
    dcDrawTextScaled(24, 290, "STAGE LIST", 0xFFFF, 3);
    dcDrawTextScaled(24 + 17 * 11, 290, num, 0xFFFF, 3);

    dcFormatNum(num, (uint)stageListPosition, 3);
    dcDrawTextScaled(24, 285, "STAGE POS", 0xFFFF, 3);
    dcDrawTextScaled(24 + 17 * 11, 285, num, 0xFFFF, 3);

    // Which object the engine was on. objectLoop is the engine's own cursor —
    // ProcessObjects and DrawObjectList both drive it — so this needs nothing
    // added to the hot path and still names the culprit outright. A script that
    // loops forever hangs inside one object's code, and this is that object.
    {
        const int slot = objectLoop;
        const int type = (slot >= 0 && slot < ENTITY_COUNT) ? objectEntityList[slot].type : 0;
        char name[20];
        dcSanitise(name, (type >= 0 && type < OBJECT_COUNT) ? typeNames[type] : "?", (int)sizeof(name));

        dcFormatNum(num, (uint)slot, 4);
        sprintf(line, "OBJECT %s TYPE ", num);
        dcFormatNum(num, (uint)type, 3);
        strcat(line, num);
        dcDrawTextScaled(24, 325, line, 0xFFFF, 3);
        dcDrawTextScaled(24, 362, name, 0x07FF, 3);
    }

    // And which instruction inside that object's script. sub: 0=main 1=draw
    // 2=setup 3=player-interaction.
    {
        char op[24];
        dcSanitise(op, DC_ScriptOpName(), (int)sizeof(op));
        dcFormatNum(num, (uint)DC_ScriptOp(), 3);
        sprintf(line, "OP %s SUB ", num);
        dcFormatNum(num, (uint)DC_ScriptSub(), 1);
        strcat(line, num);
        dcDrawTextScaled(24, 399, line, 0xFFFF, 3);
        dcDrawTextScaled(24, 436, op, 0x07FF, 3);
    }
}

void DC_FlipScreen()
{
    // Heartbeat for the watchdog in DCAudio.cpp. Bumped at the top rather than
    // the bottom deliberately: a hang inside the present path itself should
    // still stop the count.
    ++dcHeartbeat;
    DC_PHASE(DC_PHASE_PRESENT);

    static bool firstFlip = true;
    if (firstFlip) {
        DC_LOG("[DC] video: first frame presented\n");
        firstFlip = false;
    }

#if DC_FMV
    // A movie owns the screen outright: one scene, one quad, nothing else. The
    // engine still runs its frame around this — that is the whole point of
    // driving playback from ENGINE_VIDEOWAIT rather than blocking — but there
    // is no game geometry to present while a cutscene is up.
    if (dcPvrReady && DC_VideoActive()) {
        pvr_wait_ready();
        // Between the wait and the scene: the previous scene has finished with
        // the texture, and the next has not started reading it.
        DC_VideoUpload();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY); // see the list-type note in DCVideo.cpp
        DC_VideoDraw();
        pvr_list_finish();
        pvr_scene_finish();
        return;
    }
#endif

    // Pending VMU writes land here once the pause menu has settled: the scene is
    // frozen, so the ~300ms write is invisible, and the half-second delay lets
    // the menu finish appearing first. Fires once per pause (nothing can become
    // pending while paused). The other flush point is the stage load in
    // Scene.cpp's LoadStageFiles(); see DCSave.cpp for why writes are deferred.
    static int dcPausedFrames = 0;
    if (stageMode == STAGEMODE_PAUSED) {
        if (dcPausedFrames < 64)
            ++dcPausedFrames;
        if (dcPausedFrames == 30)
            DC_FlushSaves();
    }
    else
        dcPausedFrames = 0;

    if (!Engine.frameBuffer)
        return;

#if DC_PERF_LOG
    // Three numbers, because they point at three different fixes:
    //   logic+render : the engine's own frame (software renderer, script, etc.)
    //                  -> too big means move tiles/sprites to the PVR (phase 3)
    //   wait         : blocked waiting for the vertical blank
    //                  -> big AND logic+render < 16.6ms means we are only just
    //                     missing the deadline and quantising to 30fps
    //   blit         : pushing the finished frame to VRAM
    //                  -> big means the copy itself needs work, not the renderer
    static unsigned long long pfLast = 0, pfFrameAcc = 0, pfFlipAcc = 0, pfWaitAcc = 0, pfBlitAcc = 0;
    static uint pfFrames          = 0;
    const unsigned long long pfT0 = timer_ns_gettime64();
    if (pfLast)
        pfFrameAcc += pfT0 - pfLast;
    pfLast = pfT0;

    // Last completed averages, held so the overlay can show them every frame
    // rather than flashing once every 120.
    static uint pbFps = 0, pbWork = 0, pbWait = 0, pbBlit = 0, pbPal = 0;

    // Sampled every frame, reported as the window's PEAK — an occasional 5-bank
    // frame matters just as much as a constant one, and an average would hide it.
    static uint pfPalPeak = 0;
    {
        const uint banks = dcCountPaletteBanks();
        if (banks > pfPalPeak)
            pfPalPeak = banks;
    }

    // Point the glyph renderer at whichever surface this build presents from:
    // the software frame (drawn before it is blitted, so both the direct-VRAM
    // and the PVR-textured path get it for free), or the small ARGB1555 staging
    // buffer that becomes a quad in the hardware path.
#if DC_HW_RENDER && DC_USE_PVR
    if (renderType == RENDER_HW && dcOverlayBuf) {
        dcTextDst    = dcOverlayBuf;
        dcTextStride = DC_OVL_W;
        dcTextW      = DC_OVL_W;
        dcTextH      = DC_OVL_H;
        dcText1555   = true;
    }
    else
#endif
    {
        dcTextDst    = Engine.frameBuffer;
        dcTextStride = GFX_LINESIZE;
        dcTextW      = SCREEN_XSIZE;
        dcTextH      = SCREEN_YSIZE;
        dcText1555   = false;
    }

    dcDrawPerfOverlay(pbFps, pbWork, pbWait, pbBlit, pbPal);
#endif

    // Set once a present path has run, so the software blit below knows to stay
    // out of the way. A plain flag rather than an early return, because the perf
    // accounting at the bottom has to run either way.
    bool dcPresented = false;

#if DC_HW_RENDER && DC_USE_PVR
    // L+R (without Start) swaps the screen for the raw atlas. Held, not toggled.
    if (dcPvrReady && DC_AtlasViewHeld()) {
        pvr_wait_ready();
        if (dcDrawAtlasView())
            return;
    }

    // The hardware path. Everything the engine drew this frame is sitting in
    // gfxPolyList/polyList3D waiting to be handed to the TA; there is no
    // software frame to blit.
    if (dcPvrReady && renderType == RENDER_HW) {
#if DC_PERF_LOG
        const unsigned long long pfH0 = timer_ns_gettime64();
#endif
        DC_PresentHW();
#if DC_PERF_LOG
        // Reported as BLT: with no blit left to measure, this slot now holds the
        // cost of building and submitting the display lists — including the wait
        // for the previous scene, which the PVR overlaps with its own rendering.
        pfBlitAcc += timer_ns_gettime64() - pfH0;
#endif
        dcPresented = true;
    }
#endif

#if DC_USE_PVR
    if (dcPvrReady && !dcPresented) {
        // Wait for the previous scene to finish sampling before overwriting the
        // (single-buffered) texture, then upload this frame.
#if DC_PERF_LOG
        const unsigned long long pfW0 = timer_ns_gettime64();
#endif
        pvr_wait_ready();
#if DC_PERF_LOG
        const unsigned long long pfW1 = timer_ns_gettime64();
        pfWaitAcc += pfW1 - pfW0;
#endif

        const ushort *src = Engine.frameBuffer;
        ushort *dst       = (ushort *)dcFbTexture;
        for (int y = 0; y < SCREEN_YSIZE; ++y) {
            sq_cpy(dst, src, SCREEN_XSIZE * sizeof(ushort));
            src += GFX_LINESIZE;
            dst += DC_TEX_W;
        }

        const float uMax = (float)SCREEN_XSIZE / DC_TEX_W; // 320/512
        const float vMax = (float)SCREEN_YSIZE / DC_TEX_H; // 240/256

        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&dcFbHdr, sizeof(dcFbHdr));

        pvr_vertex_t v;
        v.flags = PVR_CMD_VERTEX;
        v.argb  = 0xFFFFFFFF;
        v.oargb = 0;
        v.z     = 1.0f;

        v.x = 0.0f;   v.y = 480.0f; v.u = 0.0f; v.v = vMax; pvr_prim(&v, sizeof(v));
        v.x = 0.0f;   v.y = 0.0f;   v.u = 0.0f; v.v = 0.0f; pvr_prim(&v, sizeof(v));
        v.x = 640.0f; v.y = 480.0f; v.u = uMax; v.v = vMax; pvr_prim(&v, sizeof(v));
        v.flags = PVR_CMD_VERTEX_EOL;
        v.x = 640.0f; v.y = 0.0f;   v.u = uMax; v.v = 0.0f; pvr_prim(&v, sizeof(v));

        pvr_list_finish();
        pvr_scene_finish();
    }
#else
    if (!dcPresented) {
#if DC_PERF_LOG
        const unsigned long long pfW0 = timer_ns_gettime64();
#endif
        vid_waitvbl();
#if DC_PERF_LOG
        const unsigned long long pfW1 = timer_ns_gettime64();
        pfWaitAcc += pfW1 - pfW0;
#endif

        const ushort *src = Engine.frameBuffer;
        ushort *dst       = vram_s;
        for (int y = 0; y < SCREEN_YSIZE; ++y) {
            sq_cpy(dst, src, SCREEN_XSIZE * sizeof(ushort));
            src += GFX_LINESIZE;
            dst += SCREEN_XSIZE;
        }
#if DC_PERF_LOG
        pfBlitAcc += timer_ns_gettime64() - pfW1;
#endif
    }
#endif

#if DC_PERF_LOG
    pfFlipAcc += timer_ns_gettime64() - pfT0;
    if (++pfFrames >= 120) {
        const uint frameUs = (uint)(pfFrameAcc / pfFrames / 1000u);
        const uint flipUs  = (uint)(pfFlipAcc / pfFrames / 1000u);
        const uint waitUs  = (uint)(pfWaitAcc / pfFrames / 1000u);
        const uint blitUs  = (uint)(pfBlitAcc / pfFrames / 1000u);
        // Everything outside DC_FlipScreen: the script interpreter, object
        // processing and the software renderer. This is the number that decides
        // whether the hardware renderer is the fix.
        const uint workUs = frameUs > flipUs ? frameUs - flipUs : 0;
        const uint fps    = pfFrameAcc ? (uint)(1000000000ULL * pfFrames / pfFrameAcc) : 0u;

        DC_LOG("[DC] perf: frame=%uus (%u fps) | logic+render=%uus wait=%uus blit=%uus | palbanks=%u | budget 16667us\n", frameUs, fps, workUs,
               waitUs, blitUs, pfPalPeak);

        pbFps  = fps;
        pbWork = workUs;
        pbWait = waitUs;
        pbBlit = blitUs;
        pbPal  = pfPalPeak;

        pfFrameAcc = pfFlipAcc = pfWaitAcc = pfBlitAcc = 0;
        pfFrames                                       = 0;
        pfPalPeak                                      = 0;
    }
#endif
}

// Put the PVR back the way the engine expects after something else has been
// drawing with it — currently only the FMV player.
//
// The one register that genuinely must be undone is PVR_TEXTURE_MODULO: the
// movie uses a stride texture and sets it, and leaving it set makes every
// power-of-two texture afterwards sample at the wrong pitch. That failure looks
// like the atlas has been shredded, which would send us hunting in entirely the
// wrong place.
#if DC_AUDIO_SELFTEST
// A solid-colour frame, presented immediately.
//
// This exists to MEASURE AUDIO LATENCY. The self-test flips the screen white on
// the same line of code that starts a sound; in a recording, the gap between
// the frame going white and the sound arriving IS the latency, to within one
// frame, with no guessing about what the player perceives.
//
// It presents directly rather than setting a flag for the main loop, because
// the self-test runs during boot, before there is a main loop to set it for.
void DC_DebugFlash(bool on)
{
    if (!dcPvrReady)
        return;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling      = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write      = false;
    pvr_poly_compile(&hdr, &cxt);

    const uint32 col = on ? 0xFFFFFFFF : 0xFF000000;

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr, sizeof(hdr));

    pvr_vertex_t v;
    v.oargb = 0;
    v.argb  = col;
    v.z     = 1.0f;
    v.u = v.v = 0.0f;
    v.flags = PVR_CMD_VERTEX;
    v.x = 0.0f;   v.y = 480.0f; pvr_prim(&v, sizeof(v));
    v.x = 0.0f;   v.y = 0.0f;   pvr_prim(&v, sizeof(v));
    v.x = 640.0f; v.y = 480.0f; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = 640.0f; v.y = 0.0f;   pvr_prim(&v, sizeof(v));

    pvr_list_finish();
    pvr_scene_finish();
}
#endif

void DC_ReinitRenderState()
{
#if DC_USE_PVR
    if (!dcPvrReady)
        return;

    PVR_SET(PVR_TEXTURE_MODULO, 0);

#if DC_HW_RENDER
    // Cheap insurance: the palette RAM is shared hardware and a re-send costs
    // 1024 stores once, against a whole stage of wrong colours if it is stale.
    for (int p = 0; p < PALETTE_COUNT; ++p) DC_MarkPaletteDirty(p);
    DC_AtlasUploadPalettes();
#endif
#endif
}

void DC_ReleaseRenderDevice()
{
#if DC_HW_RENDER
    DC_AtlasRelease(); // before pvr_shutdown — it frees VRAM
#if DC_PERF_LOG
    if (dcOverlayTex) {
        pvr_mem_free(dcOverlayTex);
        dcOverlayTex = NULL;
    }
    if (dcOverlayBuf) {
        free(dcOverlayBuf);
        dcOverlayBuf = NULL;
    }
    dcTextDst = NULL;
#endif
#endif

    if (Engine.frameBuffer) {
        delete[] Engine.frameBuffer;
        Engine.frameBuffer = NULL;
    }
#if DC_USE_PVR
    if (dcPvrReady) {
        pvr_shutdown();
        dcPvrReady = false;
    }
#endif
}

#endif // RETRO_USING_KOS
