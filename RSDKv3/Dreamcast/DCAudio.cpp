// ---------------------------------------------------------------------
// RSDKv3 Dreamcast (KallistiOS) audio backend.
// ---------------------------------------------------------------------
// ASSET FORMAT — matches the Sonic 2 / RSDKv4 Dreamcast port.
//
// Everything is headerless raw PCM. No decoder is linked (no libtremor, no
// libogg), so nothing on the disc may still be a container.
//
//   Data/Music/*.ogg      headerless signed 8-bit PCM @ 22050 Hz.
//                         KEEP THE .ogg FILENAME so GameConfig.bin still
//                         resolves it; only the contents change. Channel count
//                         is set by DC_MUSIC_CHANNELS below.
//                           ffmpeg -i in.ogg -f s8 -ac 1 -ar 22050 out.ogg
//
//   Data/SoundFX/*.wav    headerless signed 8-bit MONO PCM @ 22050 Hz, keeping
//                         the .wav filename (RSDKv3's sfx are .wav, unlike v4's
//                         .ogg — that is the one naming difference between the
//                         two ports).
//                           ffmpeg -i in.wav -f s8 -ac 1 -ar 22050 out.wav
//                         A real RIFF .wav is still accepted and parsed, so an
//                         unconverted asset degrades rather than screaming.
//
// A file that is still a real Ogg container is detected by its "OggS" magic and
// refused with a log line, rather than played as samples.
//
// ---------------------------------------------------------------------
// WHY SOUNDS ARE KEPT IN THEIR NATIVE RATE AND CHANNEL COUNT
//
// RSDKv3's stock mixer (ProcessAudioMixing in Audio.cpp) resamples nothing — on
// desktop, SDL converts every sfx at load and every music stream on the fly. The
// naive port of that is to convert each sfx to the device format at load time,
// but on this platform that is the wrong trade: widening s8 mono to S16 stereo
// makes every sound cost 4x its file size, and Sonic CD's global set alone is
// 689 KB, so it would want 2.7 MB resident against a heap that has roughly 6 MB
// free after RSDKv3's ~8.6 MB of static globals.
//
// So sfx stay s8-widened-to-S16 mono (2x file), and Audio.cpp carries a small
// KOS-only mixing path — ported from the v4 port, where it is proven — that
// resamples and expands to stereo one frame at a time straight into the mix
// buffer. That path reads SFXInfo::rate / SFXInfo::channels, which is why this
// file reports them rather than silently converting.
//
// The device runs at 22050 Hz rather than v4's 44100: with 22050 Hz sources
// that makes the resample step exactly 1:1, and it halves the SH4's mixing cost
// on an engine whose software renderer is already the expensive part.
// ---------------------------------------------------------------------

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <kos/fs.h>
#include <fcntl.h>
#include <dc/sound/stream.h>
// The AICA sfx path. sfxmgr is used ONLY for its channel allocator and stopper
// (snd_sfx_chn_alloc / snd_sfx_chn_free / snd_sfx_stop) — the samples themselves
// are placed and keyed on directly, because sfxmgr's loaders all want either a
// filename or a whole file already in RAM and ours arrive streamed out of
// Data.rsdk. aica_comm.h is the documented SH4<->ARM interface and is what
// snd_sfx_play_ex marshals into; we marshal the same struct.
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> // sinf, for the self-test tone

#include "DCCommon.hpp"

// DC_AUDIO_RATE comes from DCCommon.hpp — Audio.cpp's mixing path needs it too.
#define DC_AUDIO_CHANNELS (2) // device is always stereo

// Source format of the converted assets.
#define DC_SRC_RATE      (22050)
// How many output frames each music source frame becomes. 1 when the device
// runs at the asset rate, 2 at 44100. Integer by construction — the only rates
// this port uses are 22050 and 44100.
#define DC_MUSIC_STRETCH (DC_AUDIO_RATE / DC_SRC_RATE)
#define DC_SFX_CHANNELS  (1) // sfx are mono

// Music channel count. The Sonic CD assets converted for this port are MONO
// (verified: side/mid energy 2.9%, and ZoneComplete/GameOver run 8.4s/10.7s at
// mono, which are the correct jingle lengths — halved they would be nonsense).
// Set to 2 and re-convert with `-ac 2` for stereo music; the byte<->frame
// arithmetic below follows automatically.
#ifndef DC_MUSIC_CHANNELS
#define DC_MUSIC_CHANNELS (1)
#endif

// How much audio the SPU holds ahead of the game. THIS IS THE LATENCY.
//
// The device runs at 22050 Hz, stereo, 16-bit = 88200 bytes/second, so the
// delay between the game calling PlaySfx and the sound reaching the speakers is
// roughly (buffer size / 88200):
//
//     65536 (SND_STREAM_BUFFER_MAX)  ->  743 ms   <- what this used to be
//     16384                          ->  186 ms
//      8192                          ->   93 ms   <- default
//      4096                          ->   46 ms
//
// Handing snd_stream_alloc the maximum was the obvious-looking choice and the
// wrong one: it buys underrun headroom nobody needed and pays for it in a delay
// you can hear on every jump and every ring.
//
// The floor is set by how often dcStreamThreadFn gets to run. It polls every
// DC_AUDIO_POLL_MS, so the buffer must comfortably outlast one poll interval
// plus whatever scheduling jitter a stage load introduces. 93 ms against a 5 ms
// poll is roughly 18x headroom. If audio crackles, raise this rather than
// chasing it elsewhere.
#ifndef DC_AUDIO_BUFFER
#define DC_AUDIO_BUFFER (8192)
#endif
#define DC_AUDIO_POLL_MS (5)

// The hang watchdog. Set from CMake; see the option there for what it is for and
// why it now defaults off.
//
// It exists because this machine has no serial cable and no BBA, so a hang is a
// frozen picture and nothing else. That is a debugging tool, not a shipping one:
// with the port stable it can only ever produce a FALSE positive — it has
// already called one healthy 44100 sfx load a hang — so it is off by default and
// switched on when something actually wedges.
#ifndef DC_WATCHDOG
#define DC_WATCHDOG (0)
#endif

// How long the main loop may go without presenting a frame before the watchdog
// calls it dead. Generous, because a stage load legitimately blocks the loop for
// a while: the 1 MB atlas twiddle alone is ~140 ms and the GD-ROM reads around
// it are slower still. Three seconds is far longer than anything legitimate and
// far shorter than a person's patience with a frozen screen.
#ifndef DC_WATCHDOG_MS
#define DC_WATCHDOG_MS (3000)
#endif

static snd_stream_hnd_t dcStream = SND_STREAM_INVALID;
static kthread_t *dcStreamThread = NULL;
static volatile bool dcStreamRun = false;
static bool dcAudioReady         = false;

// The buffer snd_stream reads out of. Static (it must outlive the callback) and
// 32-byte aligned for the AICA DMA path. Sized to the hard maximum regardless of
// DC_AUDIO_BUFFER — it costs 64KB once and means the clamp in the callback can
// never be the thing that truncates a request.
static Sint16 dcMixBuf[SND_STREAM_BUFFER_MAX / sizeof(Sint16)] __attribute__((aligned(32)));

// ---------------------------------------------------------------------
// Music: raw-PCM streaming with a decoupled reader thread
// ---------------------------------------------------------------------
// The track is NOT loaded into RAM. Sonic CD ships two soundtracks and the
// files run to megabytes each; this platform does not have that to spare.
//
// CRITICAL, and the reason for both the reader thread and the fs_* API:
//
//  1. The disc read must NOT happen on the audio callback thread. That thread
//     runs above main-thread priority; a blocking GD-ROM read there starves the
//     game's main loop of the drive and the game appears to hang the instant a
//     track starts. A dedicated reader thread fills a RAM ring and the callback
//     only ever consumes from it — never touches the disc, never blocks.
//
//  2. Music I/O uses KOS's native fs_* API, NOT newlib stdio. A newlib FILE*
//     opened on one thread and read from another faults under KOS (per-thread
//     reentrancy); fs_* handles are plain integers and are safe to open on the
//     main thread and read from the reader thread. The v4 port hit exactly this
//     as a title-screen freeze — do not "simplify" this back to fopen/fread.

#define DC_MUSIC_READ_CHUNK (32 * 1024)
#define DC_MUSIC_RING       (64 * 1024) // ~3s of 22050 Hz s8 mono readahead

static file_t dcMusFile          = FILEHND_INVALID;
static bool dcMusLoop            = false;
static uint dcMusLoopByte        = 0;
static uint dcMusStartByte       = 0;
static uint dcMusFileBytes       = 0;
static volatile uint dcMusReadPos = 0;    // byte position of the reader
static volatile bool dcMusEnded  = false; // reader hit the end of a non-looping track
static mutex_t dcMusMutex        = MUTEX_INITIALIZER;

// Single-producer (reader) / single-consumer (mixer) ring of RAW SOURCE BYTES.
// dcRingHead is written only by the reader, dcRingTail only by the mixer.
static byte *dcRing             = NULL;
static volatile uint dcRingHead = 0;
static volatile uint dcRingTail = 0;
static kthread_t *dcMusThread   = NULL;
static volatile bool dcMusRun   = false;

static inline uint dcRingUsed() { return dcRingHead - dcRingTail; }
static inline uint dcRingFree() { return DC_MUSIC_RING - dcRingUsed(); }

// The engine's loopPoint is a PCM *frame* index into the ORIGINAL Vorbis file
// (on desktop it goes straight to ov_pcm_seek). Sonic CD's music is 44100 Hz, so
// a source frame index has to be rescaled before it means anything as a byte
// offset into our 22050 Hz s8 file. 64-bit intermediate: a loop point times
// 22050 overflows 32 bits well before the end of a long track.
#define DC_MUSIC_OGG_RATE (44100)
static uint dcMusFramesToBytes(uint srcFrames)
{
    const unsigned long long f = (unsigned long long)srcFrames * DC_SRC_RATE / DC_MUSIC_OGG_RATE;
    return (uint)(f * DC_MUSIC_CHANNELS); // s8: one byte per sample
}
static uint dcMusBytesToFrames(uint bytes)
{
    const unsigned long long f = (unsigned long long)bytes / DC_MUSIC_CHANNELS;
    return (uint)(f * DC_MUSIC_OGG_RATE / DC_SRC_RATE);
}

static void *dcMusThreadFn(void *arg)
{
    (void)arg;
    while (dcMusRun) {
        mutex_lock(&dcMusMutex);

        if (dcMusFile == FILEHND_INVALID || !dcRing || dcMusEnded) {
            mutex_unlock(&dcMusMutex);
            thd_sleep(20);
            continue;
        }

        uint space = dcRingFree();
        if (space < DC_MUSIC_READ_CHUNK / 2) {
            mutex_unlock(&dcMusMutex);
            thd_sleep(10); // ring is full enough
            continue;
        }

        uint want = space > DC_MUSIC_READ_CHUNK ? DC_MUSIC_READ_CHUNK : space;
        const uint offset = dcRingHead % DC_MUSIC_RING;
        if (offset + want > DC_MUSIC_RING)
            want = DC_MUSIC_RING - offset; // stop at the wrap; next pass takes the rest
        if (want > dcMusFileBytes - dcMusReadPos)
            want = dcMusFileBytes - dcMusReadPos;

        ssize_t n = want ? fs_read(dcMusFile, dcRing + offset, want) : 0;
        if (n > 0) {
            dcMusReadPos += (uint)n;
            dcRingHead += (uint)n;
        }

        if (dcMusReadPos >= dcMusFileBytes) {
            if (dcMusLoop) {
                fs_seek(dcMusFile, (off_t)dcMusLoopByte, SEEK_SET);
                dcMusReadPos = dcMusLoopByte;
            }
            else {
                dcMusEnded = true; // let the ring drain, then report stopped
            }
        }
        else if (n <= 0) {
            // Contended or failed read mid-track. Re-seek to where we are and
            // back off; the ring is what absorbs the gap.
            fs_seek(dcMusFile, (off_t)dcMusReadPos, SEEK_SET);
            mutex_unlock(&dcMusMutex);
            thd_sleep(5);
            continue;
        }

        mutex_unlock(&dcMusMutex);
    }
    return NULL;
}

bool DC_MusicOpen(const char *path, bool loop, uint loopPoint, uint startFrame)
{
    DC_MusicClose();

    char full[0x120];
    sprintf(full, "%s%s", BASE_PATH, path);

    mutex_lock(&dcMusMutex);
    dcMusFile = fs_open(full, O_RDONLY);
    if (dcMusFile == FILEHND_INVALID) {
        mutex_unlock(&dcMusMutex);
        DC_LOG("[DCAudio] music: '%s' not found (loose raw-PCM file expected)\n", full);
        return false;
    }

    dcMusFileBytes = (uint)fs_total(dcMusFile);

    // Still a real Ogg container? Then this track missed the conversion step.
    char magic[4] = { 0, 0, 0, 0 };
    if (fs_read(dcMusFile, magic, 4) == 4 && !memcmp(magic, "OggS", 4)) {
        fs_close(dcMusFile);
        dcMusFile = FILEHND_INVALID;
        mutex_unlock(&dcMusMutex);
        DC_LOG("[DCAudio] music: '%s' is still Vorbis - convert it (see DCAudio.cpp header)\n", full);
        return false;
    }

    dcMusLoop     = loop;
    dcMusLoopByte = dcMusFramesToBytes(loopPoint);
    dcMusStartByte = dcMusFramesToBytes(startFrame);
    if (dcMusLoopByte >= dcMusFileBytes)
        dcMusLoopByte = 0;
    if (dcMusStartByte >= dcMusFileBytes)
        dcMusStartByte = 0;
    // Align to a whole sample frame so a seek can never desync the channels.
    dcMusLoopByte -= dcMusLoopByte % DC_MUSIC_CHANNELS;
    dcMusStartByte -= dcMusStartByte % DC_MUSIC_CHANNELS;

    fs_seek(dcMusFile, (off_t)dcMusStartByte, SEEK_SET);
    dcMusReadPos = dcMusStartByte;
    dcMusEnded   = false;
    dcRingHead = dcRingTail = 0;
    mutex_unlock(&dcMusMutex);

    DC_LOG("[DCAudio] music: '%s' open (%u KB, %d ch, loop=%d @ %u B)\n", full, dcMusFileBytes / 1024, DC_MUSIC_CHANNELS, (int)loop, dcMusLoopByte);
    return true;
}

// Position already heard, in original-ogg PCM frames. Measured from the
// CONSUMED end of the ring, not the reader's position — the reader can be
// seconds ahead of what the speakers have played, and the only caller that
// matters (a time-travel track swap) wants what the player just heard.
uint DC_MusicTell() { return dcMusBytesToFrames(dcMusStartByte + dcRingTail); }

void DC_MusicClose()
{
    mutex_lock(&dcMusMutex);
    if (dcMusFile != FILEHND_INVALID) {
        fs_close(dcMusFile);
        dcMusFile = FILEHND_INVALID;
    }
    dcMusReadPos = 0;
    dcMusEnded   = false;
    dcRingHead = dcRingTail = 0;
    mutex_unlock(&dcMusMutex);
}

// Called from the mixer thread. Never does I/O and never takes dcMusMutex (the
// reader holds that across a disc read) — it only walks the ring.
//
// Converts on the way out: s8 -> S16, and mono -> stereo when the source is
// mono. Source and device rates are both DC_SRC_RATE, so there is no resampling
// here; if that ever stops being true this is where it goes.
int DC_MusicReadSamples(Sint16 *out, size_t samplesWanted)
{
    if (!out || !samplesWanted || !dcRing)
        return 0;

    // samplesWanted is in device samples (interleaved stereo S16).
    const size_t framesWanted = (samplesWanted / DC_AUDIO_CHANNELS + DC_MUSIC_STRETCH - 1) / DC_MUSIC_STRETCH;
    const uint srcBytesWanted = (uint)(framesWanted * DC_MUSIC_CHANNELS);

    uint avail = dcRingUsed();
    if (!avail) {
        if (dcMusEnded)
            return 0; // genuine end of a non-looping track; Audio.cpp stops it
        // Underrun: the reader hasn't caught up. Silence for this callback is
        // the right answer — blocking here would stall the whole mixer.
        memset(out, 0, samplesWanted * sizeof(Sint16));
        return (int)samplesWanted;
    }

    uint take = srcBytesWanted;
    if (take > avail)
        take = avail;
    take -= take % DC_MUSIC_CHANNELS; // whole frames only
    if (!take) {
        memset(out, 0, samplesWanted * sizeof(Sint16));
        return (int)samplesWanted;
    }

    const uint frames = take / DC_MUSIC_CHANNELS;
    uint tail         = dcRingTail;

    // Music assets stay at DC_SRC_RATE (22050) whatever the device runs at:
    // they are streamed from disc, so doubling them would double the disc
    // footprint and the GD-ROM bandwidth for no audible gain — unlike the sfx,
    // the music has nothing above 11 kHz worth keeping.
    //
    // When the device is faster, each source frame is held for DC_MUSIC_STRETCH
    // output frames. Sample-and-hold rather than interpolation: at 2x it is
    // exactly what a zero-order-hold upsampler does, it costs nothing, and the
    // imaging it produces sits above 11 kHz where this material has no content
    // to be confused with.
    for (uint f = 0; f < frames; ++f) {
        const signed char l = (signed char)dcRing[(tail + f * DC_MUSIC_CHANNELS) % DC_MUSIC_RING];
#if DC_MUSIC_CHANNELS >= 2
        const signed char r = (signed char)dcRing[(tail + f * DC_MUSIC_CHANNELS + 1) % DC_MUSIC_RING];
#else
        const signed char r = l;
#endif
        for (int k = 0; k < DC_MUSIC_STRETCH; ++k) {
            const size_t o = ((size_t)f * DC_MUSIC_STRETCH + k) * 2;
            if (o + 1 >= samplesWanted)
                break;
            out[o + 0] = (Sint16)(l << 8);
            out[o + 1] = (Sint16)(r << 8);
        }
    }

    dcRingTail += take;

    const size_t produced = (size_t)frames * DC_MUSIC_STRETCH * DC_AUDIO_CHANNELS;
    if (produced < samplesWanted)
        memset(out + produced, 0, (samplesWanted - produced) * sizeof(Sint16));

    return (int)samplesWanted;
}

// ---------------------------------------------------------------------
// Stage-SFX bump arena
// ---------------------------------------------------------------------
// Stage sfx are reloaded wholesale on every stage change. Freeing and
// re-malloc'ing a couple of dozen buffers of assorted sizes fragments the heap,
// and RSDKv3 has less of it to spare than v4 did — roughly 8.6 MB is already
// gone to static globals before main() runs. A bump arena cannot fragment
// internally: the layout after a reset is byte-identical to the first load.
//
// Taken as several blocks, largest hole first, rather than one slab — a single
// slab is all-or-nothing and gets refused on a heap that has the space but not
// in one piece.

#define DC_ARENA_BLOCKS (6)
// Scaled by the device rate: sfx are stored at their source rate, so a 44100
// device holds twice the samples a 22050 one did. 512 KB was sized for 22050
// and silently overflows into malloc at 44100, which is exactly the heap
// fragmentation the arena exists to prevent.
#define DC_ARENA_TARGET ((512 * 1024) * (DC_AUDIO_RATE / DC_SRC_RATE))

static byte *dcArenaBase[DC_ARENA_BLOCKS];
static size_t dcArenaSize[DC_ARENA_BLOCKS];
static size_t dcArenaUsed[DC_ARENA_BLOCKS];
static int dcArenaCount = 0;

void DC_SfxArenaAcquire()
{
    if (dcArenaCount)
        return; // already held

    size_t total = 0;
    for (int i = 0; i < DC_ARENA_BLOCKS && total < DC_ARENA_TARGET; ++i) {
        size_t want = DC_LargestFreeBlock();
        if (want > 128 * 1024)
            want -= 64 * 1024; // leave the rest of the stage load room to breathe
        if (want < 32 * 1024)
            break;
        if (total + want > DC_ARENA_TARGET)
            want = DC_ARENA_TARGET - total;

        byte *p = (byte *)malloc(want);
        if (!p)
            break;

        dcArenaBase[dcArenaCount] = p;
        dcArenaSize[dcArenaCount] = want;
        dcArenaUsed[dcArenaCount] = 0;
        ++dcArenaCount;
        total += want;
    }

    DC_LOG("[DCAudio] sfx arena: %d block(s), %u KB\n", dcArenaCount, (unsigned)(total / 1024));
}

void *DC_SfxAlloc(size_t bytes, bool stageSfx)
{
    if (stageSfx) {
        bytes = (bytes + 31) & ~(size_t)31; // keep buffers 32-byte aligned
        for (int i = 0; i < dcArenaCount; ++i) {
            if (dcArenaSize[i] - dcArenaUsed[i] >= bytes) {
                void *p = dcArenaBase[i] + dcArenaUsed[i];
                dcArenaUsed[i] += bytes;
                return p;
            }
        }
    }
    return malloc(bytes); // global sfx, or the arena is full/unavailable
}

void DC_SfxFree(void *ptr)
{
    if (!ptr)
        return;
    // Range-check before free(): an arena pointer is an interior pointer into a
    // block malloc still owns, and freeing it would corrupt the heap. EVERY sfx
    // buffer must go through here for exactly this reason.
    for (int i = 0; i < dcArenaCount; ++i) {
        if ((byte *)ptr >= dcArenaBase[i] && (byte *)ptr < dcArenaBase[i] + dcArenaSize[i])
            return; // arena memory; reclaimed wholesale by DC_SfxArenaReset
    }
    free(ptr);
}

void DC_SfxArenaReset()
{
    for (int i = 0; i < dcArenaCount; ++i) {
        free(dcArenaBase[i]);
        dcArenaBase[i] = NULL;
        dcArenaSize[i] = dcArenaUsed[i] = 0;
    }
    dcArenaCount = 0;
}

// The AICA sfx implementation lives further down, after DC_LoadWAVStreamed —
// it reuses that function's RIFF header parser, so it has to follow it.

// ---------------------------------------------------------------------
// SFX loading
// ---------------------------------------------------------------------
// Reads through the engine's already-open file reader (LoadFile must have
// succeeded), so this works whether the sfx is loose on disc or packed inside
// Data.rsdk, and the whole file is never resident at once.
//
// The first four bytes decide the format. Sniffing rather than assuming means a
// half-converted asset set still boots: converted sounds take the fast path,
// anything left as a real RIFF .wav is parsed properly, and a stray Ogg is
// refused with a log line instead of played as noise.

struct DCWavFmt {
    int rate;
    int channels;
    int bits;
    uint dataBytes;
};

// Parse a RIFF header forward through FileRead, leaving the reader positioned
// at the first sample. `probe` is the four bytes already consumed by the caller.
static bool dcParseWavHeader(const byte *probe, size_t fileSize, DCWavFmt *out)
{
    byte rest[8];
    if (fileSize < 44)
        return false;
    FileRead(rest, 8); // size + "WAVE"
    if (memcmp(probe, "RIFF", 4) || memcmp(rest + 4, "WAVE", 4))
        return false;

    uint pos     = 12;
    bool haveFmt = false;
    while (pos + 8 <= fileSize) {
        byte ch[8];
        FileRead(ch, 8);
        pos += 8;
        const uint chunkSize = (uint)ch[4] | ((uint)ch[5] << 8) | ((uint)ch[6] << 16) | ((uint)ch[7] << 24);

        if (!memcmp(ch, "fmt ", 4)) {
            byte fmt[16];
            const uint take = chunkSize < 16 ? chunkSize : 16;
            FileRead(fmt, (int)take);
            const int format = (int)fmt[0] | ((int)fmt[1] << 8);
            if (format != 1) // PCM only
                return false;
            out->channels = (int)fmt[2] | ((int)fmt[3] << 8);
            out->rate     = (int)fmt[4] | ((int)fmt[5] << 8) | ((int)fmt[6] << 16) | ((int)fmt[7] << 24);
            out->bits     = (int)fmt[14] | ((int)fmt[15] << 8);
            haveFmt       = true;
            for (uint i = take; i < chunkSize; ++i) { // skip an extended fmt tail
                byte sink;
                FileRead(&sink, 1);
            }
            pos += chunkSize;
        }
        else if (!memcmp(ch, "data", 4)) {
            if (!haveFmt)
                return false;
            out->dataBytes = chunkSize;
            if (out->dataBytes > fileSize - pos)
                out->dataBytes = (uint)(fileSize - pos); // truncated file; take what's there
            return out->channels > 0 && out->rate > 0 && (out->bits == 8 || out->bits == 16);
        }
        else {
            for (uint i = 0; i < chunkSize; ++i) {
                byte sink;
                FileRead(&sink, 1);
            }
            pos += chunkSize;
        }
    }
    return false;
}

Sint16 *DC_LoadWAVStreamed(size_t fileSize, size_t *outSamples, int *outRate, int *outChannels, bool stageSfx)
{
    if (fileSize < 4)
        return NULL;

    byte probe[4];
    FileRead(probe, 4);

    if (!memcmp(probe, "OggS", 4)) {
        DC_LOG("[DCAudio] sfx: still an Ogg container - convert it (see DCAudio.cpp header)\n");
        return NULL;
    }

    // ---- headerless signed 8-bit mono @ DC_SRC_RATE (the converted form) ----
    if (memcmp(probe, "RIFF", 4)) {
        const size_t samples = fileSize; // one s8 sample per byte, mono
        Sint16 *dst          = (Sint16 *)DC_SfxAlloc(samples * sizeof(Sint16), stageSfx);
        if (!dst) {
            DC_LOG("[DCAudio] sfx: %u KB refused (largest free block %u KB)\n", (unsigned)(samples * sizeof(Sint16) / 1024),
                   (unsigned)(DC_LargestFreeBlock() / 1024));
            return NULL;
        }

        // The four probe bytes are samples too — they were consumed above.
        for (int i = 0; i < 4; ++i) dst[i] = (Sint16)(((signed char)probe[i]) << 8);

        // Read through a small static block rather than slurping and converting
        // in place: peak heap == what we keep.
        static signed char staging[4096];
        size_t done = 4;
        while (done < samples) {
            // The watchdog in dcStreamThreadFn asks "has the main loop made
            // progress in the last three seconds", and a stage's worth of sfx
            // is one long stretch with no frame presented. At 22050 it fitted
            // inside the window; at 44100 there is twice as much to read and
            // convert, and it does not — so the watchdog started calling a
            // healthy load a hang. Bumping here keeps the meaning of the
            // watchdog honest: it detects NO progress, not SLOW progress.
            ++dcHeartbeat;
            size_t want = samples - done;
            if (want > sizeof(staging))
                want = sizeof(staging);
            FileRead(staging, (int)want);
            for (size_t k = 0; k < want; ++k) dst[done + k] = (Sint16)(staging[k] << 8);
            done += want;
        }

        if (outSamples)
            *outSamples = samples;
        if (outRate)
            *outRate = DC_SRC_RATE;
        if (outChannels)
            *outChannels = DC_SFX_CHANNELS;
        return dst;
    }

    // ---- real RIFF .wav (unconverted asset) ----
    DCWavFmt fmt;
    memset(&fmt, 0, sizeof(fmt));
    if (!dcParseWavHeader(probe, fileSize, &fmt)) {
        DC_LOG("[DCAudio] sfx: unsupported or malformed WAV\n");
        return NULL;
    }

    const int srcBytesPerSample = fmt.bits / 8;
    const int srcFrameBytes     = srcBytesPerSample * fmt.channels;
    if (srcFrameBytes <= 0)
        return NULL;

    const uint frames = fmt.dataBytes / (uint)srcFrameBytes;
    if (!frames)
        return NULL;

    // Kept in the file's own rate and channel count — the mixer converts.
    const size_t samples = (size_t)frames * (size_t)fmt.channels;
    Sint16 *dst          = (Sint16 *)DC_SfxAlloc(samples * sizeof(Sint16), stageSfx);
    if (!dst) {
        DC_LOG("[DCAudio] sfx: %u KB refused for RIFF wav\n", (unsigned)(samples * sizeof(Sint16) / 1024));
        return NULL;
    }

    static byte stage[4096];
    const int stageFrames = (int)(sizeof(stage) / (size_t)srcFrameBytes);
    uint done             = 0;
    while (done < frames) {
        ++dcHeartbeat; // see the note in the headerless path above
        int take = stageFrames;
        if ((uint)take > frames - done)
            take = (int)(frames - done);
        FileRead(stage, take * srcFrameBytes);
        for (int f = 0; f < take; ++f) {
            const byte *src = stage + (size_t)f * (size_t)srcFrameBytes;
            for (int c = 0; c < fmt.channels; ++c) {
                Sint16 v;
                if (fmt.bits == 8)
                    v = (Sint16)((int)((int)src[c] - 128) << 8);
                else
                    v = (Sint16)((int)src[c * 2] | ((int)(signed char)src[c * 2 + 1] << 8));
                dst[(size_t)(done + f) * fmt.channels + c] = v;
            }
        }
        done += (uint)take;
    }

    if (outSamples)
        *outSamples = samples;
    if (outRate)
        *outRate = fmt.rate;
    if (outChannels)
        *outChannels = fmt.channels;
    return dst;
}

// ---------------------------------------------------------------------
// SFX on AICA hardware voices
// ---------------------------------------------------------------------
// A sound that lives here costs the main heap NOTHING and the SH4 NOTHING: the
// samples sit in the AICA's own 2 MB of sound RAM, and the AICA's wavetable
// engine does the resampling, the volume and the panning in hardware. The
// software mixer in Audio.cpp never sees them.
//
// Three things this buys, in order of how much they matter here:
//
//  1. PITCH IS A PER-VOICE REGISTER. The AICA is handed the sample's own rate
//     and converts it itself, so a 22050 Hz sound and a 44100 Hz sound play
//     side by side at their correct pitches with no resampling in software and
//     no shared device rate to compromise between them. That is the whole
//     tinny-ring saga made structurally impossible rather than fixed by
//     picking a better compromise.
//
//  2. PANNING IS A REGISTER TOO. Sonic CD's rings alternate two slots holding
//     the same sample hard-panned +/-100. On the software path that is a
//     multiply per sample per channel; here it is one byte in the key-on.
//
//  3. The SH4 stops mixing sfx at all, which is the CPU cost that went up when
//     the device moved to 44100.
//
// WHAT DOES NOT FIT, AND WHY
//
// The AICA holds loop points in 16-bit registers, so no voice can address more
// than 65534 samples — about 3.0 s at 22050 Hz, 1.5 s at 44100. Sonic CD has a
// handful of sounds past that (TimeWarp, BombCarrier, Achievement, LoseRings).
// Those return false from DC_AicaSfxLoad and LoadSfx rewinds the file and takes
// the ordinary heap path, so the two systems coexist per sound rather than
// per build. The overlay's "HW nn SW nn" counters say which went where.
//
// SIGNEDNESS. AICA_SM_8BIT is SIGNED 8-bit. 8-bit PCM inside a RIFF .wav is
// UNSIGNED, and the headerless assets this port converts with `ffmpeg -f s8`
// are signed. Getting this backwards does not sound subtly wrong — it puts a
// full-scale DC offset on the sample and every sound becomes a bang. The
// conversion below is keyed off which container the sound came out of, and it
// is the single most breakable line in this file.

#ifndef DC_AICA_SFX
#define DC_AICA_SFX (1)
#endif

// 65534, not 65535: snd_sfx_play_ex clamps to 65534 and the loop-end register
// is `loopend & 0xffff`, so 65535 is the value that means "wrapped".
#define DC_AICA_MAX_SAMPLES (65534)
// One hardware voice per software mixer channel, so voice stealing behaves the
// same way the game already expects it to.
#define DC_AICA_VOICES (CHANNEL_COUNT)

struct DCAicaSample {
    uint32_t addr;    // offset in AICA sound RAM; 0 == this slot is empty
    uint32_t bytes;   // what snd_mem_malloc is holding for it
    uint32_t samples; // what the AICA counts, NOT bytes
    uint32_t rate;
    byte fmt;         // AICA_SM_8BIT / AICA_SM_16BIT
    bool stage;       // dropped on stage change; globals survive
};

static DCAicaSample dcAicaSfx[SFX_COUNT];

static int dcAicaVoiceCh[DC_AICA_VOICES];      // hardware channel number
static int dcAicaVoiceSfx[DC_AICA_VOICES];     // sfxID keyed on it, -1 == idle
static byte dcAicaVoiceLoop[DC_AICA_VOICES];
// When each voice's sound is due to finish, in ns. Used instead of
// snd_is_playing(): that reads KEYONB, which is the key-on REQUEST bit and stays
// set after a one-shot has run out, so it answers "was this ever started", not
// "is this still sounding". A deadline is both cheaper (no G2 read) and correct.
static uint64_t dcAicaVoiceEnd[DC_AICA_VOICES];
static int dcAicaNextVoice = 0;
static bool dcAicaVoicesUp = false;

static uint32_t dcAicaSpuBytes = 0;

// Reserve our voices out of the same pool snd_stream allocates from, so the
// music stream's channels and ours can never be the same channel. Must happen
// AFTER the stream is up for that reason.
static bool dcAicaEnsureVoices()
{
    if (dcAicaVoicesUp)
        return true;
    if (!dcAudioReady)
        return false;

    for (int i = 0; i < DC_AICA_VOICES; ++i) {
        dcAicaVoiceCh[i]   = snd_sfx_chn_alloc();
        dcAicaVoiceSfx[i]  = -1;
        dcAicaVoiceLoop[i] = 0;
        dcAicaVoiceEnd[i]  = 0;
        if (dcAicaVoiceCh[i] < 0) {
            for (int k = 0; k < i; ++k) snd_sfx_chn_free(dcAicaVoiceCh[k]);
            DC_LOG("[DCAudio] AICA: no free hardware voices; sfx stay in software\n");
            return false;
        }
    }

    dcAicaVoicesUp = true;
    DC_LOG("[DCAudio] AICA: %d hardware voices from ch%d, %u KB sound RAM free\n", DC_AICA_VOICES, dcAicaVoiceCh[0],
           (unsigned)(snd_mem_available() / 1024));
    return true;
}

// sfxVolume is 0..MAX_VOLUME(100); the AICA wants 0..255 and applies its own
// logarithmic curve. Note the software path mixes sfx at sfxVolume WITHOUT
// masterVolume (only music is scaled by it), so this matches deliberately.
static int dcAicaVol()
{
    int v = sfxVolume;
    if (v < 0)
        v = 0;
    if (v > MAX_VOLUME)
        v = MAX_VOLUME;
    return (v * 255) / MAX_VOLUME;
}

// RSDK pan is -100..+100 with NEGATIVE meaning left (ProcessAudioMixing holds
// panL at 1.0 and ducks panR when pan < 0). AICA pan is 0 left, 128 centre,
// 255 right.
static int dcAicaPan(sbyte pan)
{
    if (pan == 0)
        return 128;
    int p = 128 + ((int)pan * 127) / 100;
    if (p < 0)
        p = 0;
    if (p > 255)
        p = 255;
    return p;
}

static void dcAicaKeyOn(int voice, int sfxID, bool loop, sbyte pan)
{
    const DCAicaSample *s = &dcAicaSfx[sfxID];
    if (!s->rate)
        return; // never stored without one; guards the division below

    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd       = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size      = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id    = dcAicaVoiceCh[voice];

    chan->cmd       = AICA_CH_CMD_START;
    chan->base      = s->addr;
    chan->type      = s->fmt;
    chan->length    = s->samples;
    chan->loop      = loop ? 1 : 0;
    chan->loopstart = 0;
    chan->loopend   = s->samples;
    chan->freq      = s->rate; // the AICA converts; nothing resamples in software
    chan->vol       = (uint32_t)dcAicaVol();
    chan->pan       = (uint32_t)dcAicaPan(pan);
    snd_sh4_to_aica(tmp, cmd->size);

    dcAicaVoiceSfx[voice]  = sfxID;
    dcAicaVoiceLoop[voice] = loop ? 1 : 0;
    dcAicaVoiceEnd[voice]  = loop ? (uint64_t)-1 : timer_ns_gettime64() + ((uint64_t)s->samples * 1000000000ull) / (uint64_t)s->rate;
}

static int dcAicaPickVoice(int sfxID)
{
    // A sound already on a voice retriggers IN PLACE. PlaySfx does the same
    // thing on the software path (it searches sfxChannels for a matching sfxID
    // before taking a new one), and it is what stops a rapidly repeated sound
    // from eating all four channels with copies of itself.
    for (int i = 0; i < DC_AICA_VOICES; ++i)
        if (dcAicaVoiceSfx[i] == sfxID)
            return i;

    const uint64_t now = timer_ns_gettime64();
    for (int i = 0; i < DC_AICA_VOICES; ++i) {
        if (dcAicaVoiceSfx[i] < 0)
            return i;
        if (now >= dcAicaVoiceEnd[i]) { // finished; reclaim it
            dcAicaVoiceSfx[i]  = -1;
            dcAicaVoiceLoop[i] = 0;
            return i;
        }
    }

    // All four genuinely busy — steal round-robin.
    const int v     = dcAicaNextVoice;
    dcAicaNextVoice = (dcAicaNextVoice + 1) % DC_AICA_VOICES;
    return v;
}

bool DC_AicaSfxHas(int sfxID) { return sfxID >= 0 && sfxID < SFX_COUNT && dcAicaSfx[sfxID].addr != 0; }

int DC_AicaSfxRate(int sfxID) { return DC_AicaSfxHas(sfxID) ? (int)dcAicaSfx[sfxID].rate : 0; }

void DC_AicaSfxStop(int sfxID)
{
    if (!dcAicaVoicesUp)
        return;
    for (int i = 0; i < DC_AICA_VOICES; ++i) {
        if (dcAicaVoiceSfx[i] != sfxID)
            continue;
        snd_sfx_stop(dcAicaVoiceCh[i]);
        dcAicaVoiceSfx[i]  = -1;
        dcAicaVoiceLoop[i] = 0;
        dcAicaVoiceEnd[i]  = 0;
    }
}

void DC_AicaSfxStopAll()
{
    if (!dcAicaVoicesUp)
        return;
    for (int i = 0; i < DC_AICA_VOICES; ++i) {
        snd_sfx_stop(dcAicaVoiceCh[i]);
        dcAicaVoiceSfx[i]  = -1;
        dcAicaVoiceLoop[i] = 0;
        dcAicaVoiceEnd[i]  = 0;
    }
}

bool DC_AicaSfxPlay(int sfxID, bool loop)
{
    if (!DC_AicaSfxHas(sfxID) || !dcAicaVoicesUp)
        return false;
    dcAicaKeyOn(dcAicaPickVoice(sfxID), sfxID, loop, 0);
    return true;
}

bool DC_AicaSfxSetAttr(int sfxID, int loopCount, sbyte pan)
{
    if (!DC_AicaSfxHas(sfxID) || !dcAicaVoicesUp)
        return false;

    const int v = dcAicaPickVoice(sfxID);

    // SetSfxAttributes rewinds samplePtr on the software path, so this is a
    // restart, not a parameter tweak — that is exactly why it is what actually
    // starts a ring. loopCount == -1 means "leave looping alone", which can only
    // mean anything if the voice was already playing THIS sound.
    bool loop;
    if (loopCount == -1)
        loop = (dcAicaVoiceSfx[v] == sfxID) && dcAicaVoiceLoop[v] != 0;
    else
        loop = loopCount != 0;

    dcAicaKeyOn(v, sfxID, loop, pan);
    return true;
}

static void dcAicaRelease(int sfxID)
{
    if (!dcAicaSfx[sfxID].addr)
        return;
    // Stop first: freeing only returns the range to our own allocator, but a
    // voice still reading it would keep sounding until something overwrote it.
    DC_AicaSfxStop(sfxID);
    snd_mem_free(dcAicaSfx[sfxID].addr);
    if (dcAicaSpuBytes >= dcAicaSfx[sfxID].bytes)
        dcAicaSpuBytes -= dcAicaSfx[sfxID].bytes;
    memset(&dcAicaSfx[sfxID], 0, sizeof(DCAicaSample));
}

void DC_AicaSfxUnloadStage()
{
    for (int i = 0; i < SFX_COUNT; ++i)
        if (dcAicaSfx[i].addr && dcAicaSfx[i].stage)
            dcAicaRelease(i);
}

void DC_AicaSfxUnloadGlobal()
{
    for (int i = 0; i < SFX_COUNT; ++i)
        if (dcAicaSfx[i].addr && !dcAicaSfx[i].stage)
            dcAicaRelease(i);
}

void DC_AicaSfxStats(int *resident, int *fallback, int *spuKB)
{
    int res = 0, fall = 0;
    int n = globalSFXCount + stageSFXCount;
    if (n > SFX_COUNT)
        n = SFX_COUNT;
    for (int i = 0; i < n; ++i) {
        if (!sfxList[i].loaded)
            continue;
        if (dcAicaSfx[i].addr)
            ++res;
        else
            ++fall;
    }
    if (resident)
        *resident = res;
    if (fallback)
        *fallback = fall;
    if (spuKB)
        *spuKB = (int)(dcAicaSpuBytes / 1024);
}

// Reads through the engine's already-open file reader, exactly like
// DC_LoadWAVStreamed, and leaves it wherever it got to. On false the caller MUST
// rewind with SetFilePosition(0) before trying the heap path — by then this has
// consumed at least the four probe bytes and usually a whole RIFF header.
bool DC_AicaSfxLoad(size_t fileSize, int sfxID)
{
    // A plain `if` on a build constant rather than an #if: the compiler folds it
    // away just the same, and the code below stays COMPILED in a DC_AICA_SFX=0
    // build. A path that only builds when it is switched on is a path that
    // silently rots while it is switched off.
    if (!DC_AICA_SFX)
        return false;

    if (sfxID < 0 || sfxID >= SFX_COUNT || fileSize < 4)
        return false;
    if (!dcAicaEnsureVoices())
        return false;

    if (dcAicaSfx[sfxID].addr)
        dcAicaRelease(sfxID); // reloading over a slot that is still occupied

    byte probe[4];
    FileRead(probe, 4);
    if (!memcmp(probe, "OggS", 4))
        return false;

    int rate, bits, channels;
    uint dataBytes;
    bool srcSigned8;

    if (memcmp(probe, "RIFF", 4)) {
        // Headerless signed 8-bit mono at the asset rate: the four probe bytes
        // are the first four SAMPLES, not a header.
        rate       = DC_SRC_RATE;
        bits       = 8;
        channels   = 1;
        dataBytes  = (uint)fileSize;
        srcSigned8 = true;
    }
    else {
        DCWavFmt fmt;
        memset(&fmt, 0, sizeof(fmt));
        if (!dcParseWavHeader(probe, fileSize, &fmt))
            return false;
        rate      = fmt.rate;
        bits      = fmt.bits;
        channels  = fmt.channels;
        dataBytes = fmt.dataBytes;
        // 8-bit PCM in a RIFF file is UNSIGNED. See the header note above.
        srcSigned8 = false;
    }

    if (channels < 1 || channels > 2 || (bits != 8 && bits != 16))
        return false;

    const int srcFrameBytes = (bits / 8) * channels;
    const uint frames       = dataBytes / (uint)srcFrameBytes;
    if (!frames || frames > DC_AICA_MAX_SAMPLES)
        return false; // too long for a 16-bit loop register; heap path takes it

    // Stereo is folded to mono. Every sfx in Sonic CD is mono, and the engine's
    // own panning model (one pan value per channel) has nowhere to put a stereo
    // image anyway. A stereo voice pair would also cost two of our four voices.
    const uint outBytes = frames * (uint)(bits / 8);

    const uint32_t addr = snd_mem_malloc(outBytes);
    if (!addr) {
        DC_LOG("[DCAudio] AICA: %u KB refused (%u KB sound RAM free)\n", (unsigned)(outBytes / 1024), (unsigned)(snd_mem_available() / 1024));
        return false;
    }

    // +32 because spu_memload_sq rounds the length up and may read a few bytes
    // past what we asked it to send.
    byte *staging = (byte *)malloc(outBytes + 32);
    if (!staging) {
        snd_mem_free(addr);
        return false;
    }

    static byte blk[4096];
    const int blockFrames = (int)(sizeof(blk) / (size_t)srcFrameBytes);
    byte *dst             = staging;
    uint done             = 0;

    if (srcSigned8) {
        for (int i = 0; i < 4 && done < frames; ++i, ++done) *dst++ = probe[i];
    }

    while (done < frames) {
        // See the note in DC_LoadWAVStreamed: the watchdog measures PROGRESS,
        // and a stage's worth of sfx is one long stretch with no frame drawn.
        ++dcHeartbeat;

        int take = blockFrames;
        if ((uint)take > frames - done)
            take = (int)(frames - done);
        FileRead(blk, take * srcFrameBytes);

        if (bits == 8) {
            if (channels == 1) {
                if (srcSigned8)
                    memcpy(dst, blk, (size_t)take);
                else
                    for (int f = 0; f < take; ++f) dst[f] = (byte)(blk[f] ^ 0x80); // unsigned -> signed
                dst += take;
            }
            else {
                for (int f = 0; f < take; ++f) {
                    const int a = srcSigned8 ? (int)(signed char)blk[f * 2] : (int)blk[f * 2] - 128;
                    const int b = srcSigned8 ? (int)(signed char)blk[f * 2 + 1] : (int)blk[f * 2 + 1] - 128;
                    *dst++      = (byte)(signed char)((a + b) / 2);
                }
            }
        }
        else { // 16-bit signed little-endian, which is already the AICA's format
            if (channels == 1) {
                memcpy(dst, blk, (size_t)take * 2);
                dst += take * 2;
            }
            else {
                for (int f = 0; f < take; ++f) {
                    const int a = (int)(Sint16)((uint)blk[f * 4] | ((uint)blk[f * 4 + 1] << 8));
                    const int b = (int)(Sint16)((uint)blk[f * 4 + 2] | ((uint)blk[f * 4 + 3] << 8));
                    const int m = (a + b) / 2;
                    *dst++      = (byte)(m & 0xFF);
                    *dst++      = (byte)((m >> 8) & 0xFF);
                }
            }
        }
        done += (uint)take;
    }

    memset(dst, 0, 32); // the rounding tail, so nothing uninitialised is sent
    spu_memload_sq(addr, staging, outBytes);
    free(staging);

    dcAicaSfx[sfxID].addr    = addr;
    dcAicaSfx[sfxID].bytes   = outBytes;
    dcAicaSfx[sfxID].samples = frames;
    dcAicaSfx[sfxID].rate    = (uint32_t)rate;
    dcAicaSfx[sfxID].fmt     = (bits == 16) ? AICA_SM_16BIT : AICA_SM_8BIT;
    // Same classification LoadSfx uses for the heap arena, so the two stay in
    // step: anything at or past globalSFXCount belongs to the current stage.
    dcAicaSfx[sfxID].stage = (globalSFXCount > 0 && sfxID >= globalSFXCount);
    dcAicaSpuBytes += outBytes;
    return true;
}

// Loose headerless sfx opened directly rather than through the engine reader.
// Kept for parity with the v4 backend; RSDKv3's sfx come through LoadFile, so
// nothing calls this yet.
Sint16 *DC_LoadRawSfx(const char *fullPath, size_t *outSamples, int *outRate, int *outChannels)
{
    char full[0x120];
    sprintf(full, "%s%s", BASE_PATH, fullPath);

    file_t f = fs_open(full, O_RDONLY);
    if (f == FILEHND_INVALID)
        return NULL;

    const uint len = (uint)fs_total(f);
    if (len < 4) {
        fs_close(f);
        return NULL;
    }

    char magic[4];
    if (fs_read(f, magic, 4) != 4 || !memcmp(magic, "OggS", 4) || !memcmp(magic, "RIFF", 4)) {
        fs_close(f);
        return NULL; // not headerless PCM; let the caller use the packed asset
    }
    fs_seek(f, 0, SEEK_SET);

    Sint16 *out = (Sint16 *)malloc((size_t)len * sizeof(Sint16));
    if (!out) {
        fs_close(f);
        return NULL;
    }

    static signed char staging[4096];
    uint done = 0;
    while (done < len) {
        uint want = len - done;
        if (want > sizeof(staging))
            want = sizeof(staging);
        const ssize_t got = fs_read(f, staging, want);
        if (got <= 0)
            break;
        for (ssize_t k = 0; k < got; ++k) out[done + k] = (Sint16)(staging[k] << 8);
        done += (uint)got;
    }
    fs_close(f);

    if (done != len) {
        free(out); // never half-load
        return NULL;
    }

    if (outSamples)
        *outSamples = len;
    if (outRate)
        *outRate = DC_SRC_RATE;
    if (outChannels)
        *outChannels = DC_SFX_CHANNELS;
    return out;
}

// ---------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------
// snd_stream asks for `req` BYTES of interleaved stereo S16 and we hand back a
// pointer to them. The engine's own ProcessAudioPlayback does the mixing,
// exactly as it does on desktop — this backend supplies the device and the
// music source and nothing else, so mixer behaviour cannot drift between
// platforms.

#if DC_AUDIO_SELFTEST
// Declared here rather than with DC_AudioSelfTest below because the stream
// callback generates the tone and is defined first.
static volatile uint dcToneFrames = 0;
static uint dcTonePhase           = 0;
#define DC_TONE_HZ (1000.0f)
#endif

static void *dcStreamCallback(snd_stream_hnd_t hnd, int req, int *got)
{
    (void)hnd;

    if (req > (int)sizeof(dcMixBuf))
        req = (int)sizeof(dcMixBuf);
    req &= ~3; // whole stereo frames only

    memset(dcMixBuf, 0, (size_t)req);

#if DC_AUDIO_SELFTEST
    // Tone A: synthesised HERE, at the device rate, with nothing else touching
    // it — assets, loader, resampler and mixer all removed from the question.
    if (dcToneFrames) {
        const int frames = req / 4;
        int n            = (int)dcToneFrames < frames ? (int)dcToneFrames : frames;
        for (int i = 0; i < frames; ++i) {
            Sint16 s = 0;
            if (i < n) {
                s = (Sint16)(9000.0f * sinf(6.2831853f * DC_TONE_HZ * (float)dcTonePhase / (float)DC_AUDIO_RATE));
                ++dcTonePhase;
            }
            dcMixBuf[i * 2]     = s;
            dcMixBuf[i * 2 + 1] = s;
        }
        dcToneFrames -= (uint)n;
        *got = req;
        return dcMixBuf;
    }
#endif

    // Takes the engine's audio lock for the same reason SDL_LockAudio() blocks
    // the SDL callback: sfxChannels[]/sfxList[] are rewritten from the main
    // thread while this runs. Without it LockAudioDevice() would exclude
    // nothing and freeing an sfx mid-mix would be a use-after-free.
    //
    // Everything the main thread does under this lock is a short burst of
    // table writes — the disc reads in LoadMusic and LoadSfx are deliberately
    // outside it, so this never waits on the GD-ROM.
    LockAudioDevice();
    ProcessAudioPlayback(NULL, (Uint8 *)dcMixBuf, req);
    UnlockAudioDevice();

    *got = req;
    return dcMixBuf;
}

#if DC_AUDIO_SELFTEST
// ---------------------------------------------------------------------
// Audio self-test
// ---------------------------------------------------------------------
// Two 1000 Hz tones, then three of the game's own sounds, played once at boot.
//
//   Tone A  synthesised in the stream callback at the device rate. Tests the
//           device and nothing else.
//   Tone B  the same 1000 Hz as a mono sfx played with PlaySfx. Tests the
//           loader's rate bookkeeping, the resample step and the mono-to-stereo
//           expansion.
//   then    Jump, Ring, LoseRings as bare PlaySfx calls with no script involved.
//
// So: both tones the same pitch means the audio path is correct end to end; B an
// octave above A means the sfx mixing path doubles the read rate; both an octave
// above 1000 Hz means the device is running at the wrong rate. 1000 Hz because
// it is unambiguous by ear, nowhere near the music, and trivial to measure in a
// phone recording.
void DC_AudioSelfTest()
{
    if (!dcAudioReady)
        return;

    DC_LOG("[DCAudio] self-test: two 1000 Hz tones, then Jump/Ring/LoseRings\n");

    dcTonePhase  = 0;
    dcToneFrames = (uint)(DC_AUDIO_RATE * 3 / 2);
    while (dcToneFrames) thd_sleep(20);
    thd_sleep(500);

    const int n = DC_AUDIO_RATE * 3 / 2;
    Sint16 *buf = (Sint16 *)malloc((size_t)n * sizeof(Sint16));
    if (buf) {
        for (int i = 0; i < n; ++i) buf[i] = (Sint16)(9000.0f * sinf(6.2831853f * DC_TONE_HZ * (float)i / (float)DC_AUDIO_RATE));

        // The last slot: global sfx load from 0 upwards and the stage set is not
        // loaded yet, so nothing real can be sitting here.
        const int id = SFX_COUNT - 1;
        LockAudioDevice();
        StrCopy(sfxList[id].name, "DCTONE");
        sfxList[id].buffer   = buf;
        sfxList[id].length   = n;
        sfxList[id].rate     = DC_AUDIO_RATE;
        sfxList[id].channels = 1;
        sfxList[id].loaded   = true;
        UnlockAudioDevice();

        const int savedSfx = sfxVolume, savedMaster = masterVolume;
        sfxVolume = masterVolume = MAX_VOLUME;
        PlaySfx(id, false);
        thd_sleep(2000);
        sfxVolume = savedSfx; masterVolume = savedMaster;

        StopSfx(id);
        LockAudioDevice();
        sfxList[id].loaded = false;
        sfxList[id].buffer = NULL;
        sfxList[id].length = 0;
        UnlockAudioDevice();
        free(buf);
    }

    // The real sounds, driven as plainly as possible: no script, no pan, no
    // SetSfxAttributes. Jump first because it is the known-good one, so it
    // calibrates the ear and the recording.
    static const char *const names[] = { "Jump.wav", "Ring.wav", "LoseRings.wav" };
    for (int k = 0; k < 3; ++k) {
        int found = -1;
        for (int s = 0; s < SFX_COUNT && found < 0; ++s) {
            if (!sfxList[s].loaded)
                continue;
            const char *nm = sfxList[s].name;
            int ln = 0, lt = 0;
            while (nm[ln]) ++ln;
            while (names[k][lt]) ++lt;
            if (ln >= lt && !strcmp(nm + ln - lt, names[k]))
                found = s;
        }
        if (found < 0) {
            DC_LOG("[DCAudio] self-test: %s not loaded\n", names[k]);
            continue;
        }
        DC_LOG("[DCAudio] self-test: %s (id %d, %d Hz, %d ch)\n", names[k], found, sfxList[found].rate, sfxList[found].channels);
        PlaySfx(found, false);
        thd_sleep(4000);
    }

    DC_LOG("[DCAudio] self-test done\n");
}
#endif // DC_AUDIO_SELFTEST

// ---------------------------------------------------------------------
// Handing the AICA over
// ---------------------------------------------------------------------
// The FMV player wants the device at the movie's sample rate while the game's
// mixer runs at DC_AUDIO_RATE. KOS allows four streams (SND_STREAM_MAX) and the
// game only uses one, so rather than tearing this one down and rebuilding it at
// a different rate — which is not reversible if the movie fails to open — the
// game's stream is simply stopped and its poll thread parked. Everything the
// mixer owns stays allocated and exactly where it was.
static volatile bool dcAudioSuspended = false;

void DC_SuspendAudio()
{
    if (!dcAudioReady || dcAudioSuspended)
        return;

    // Set the flag BEFORE stopping, and give the poll thread a couple of its own
    // intervals to notice: snd_stream_poll on a stopped stream is not something
    // to race against.
    dcAudioSuspended = true;
    thd_sleep(DC_AUDIO_POLL_MS * 2);

    if (dcStream != SND_STREAM_INVALID)
        snd_stream_stop(dcStream);

    DC_LOG("[DCAudio] suspended\n");
}

void DC_ResumeAudio()
{
    if (!dcAudioReady || !dcAudioSuspended)
        return;

    if (dcStream != SND_STREAM_INVALID)
        snd_stream_start(dcStream, DC_AUDIO_RATE, 1 /* stereo */);

    dcAudioSuspended = false;
    DC_LOG("[DCAudio] resumed\n");
}

static void *dcStreamThreadFn(void *arg)
{
    (void)arg;
    // Watchdog state. This thread is the right place for it: it runs every few
    // milliseconds, it never touches the game's data, and it is not the thread
    // that can deadlock on the audio mutex (it is the one the main thread waits
    // FOR, not the other way round).
    uint lastBeat  = 0;
    int stalledMs  = 0;
    bool everBeat  = false;

    while (dcStreamRun) {
        // Nothing to poll while the FMV player owns the device.
        if (!dcAudioSuspended && dcStream != SND_STREAM_INVALID)
            snd_stream_poll(dcStream);
        thd_sleep(DC_AUDIO_POLL_MS);

        // A plain `if` on a build constant, not an #if: the compiler folds the
        // whole block away when the watchdog is off, but it stays COMPILED, so
        // it cannot rot while it is switched off. The DC_PHASE breadcrumbs and
        // the dcHeartbeat bumps elsewhere are left alone for the same reason —
        // they are a byte store and an increment, and leaving them in means
        // re-enabling the watchdog is a reconfigure and nothing more.
        if (DC_WATCHDOG) {
            const uint beat = dcHeartbeat;
            if (beat != lastBeat) {
                lastBeat  = beat;
                stalledMs = 0;
                everBeat  = true;
            }
            else if (everBeat) {
                // Only armed once the main loop has presented at least one frame,
                // so the long silence during boot and the data-pack scan never
                // trips it.
                stalledMs += DC_AUDIO_POLL_MS;
                if (stalledMs >= DC_WATCHDOG_MS) {
                    DC_HangReport();
                    stalledMs = 0;
                    everBeat  = false; // report once; keep the audio thread alive
                }
            }
        }
    }
    return NULL;
}

bool DC_InitAudioDevice()
{
#if defined(DC_NO_AUDIO) && DC_NO_AUDIO
    DC_LOG("[DCAudio] disabled at build time (DC_NO_AUDIO)\n");
    return false;
#else
    if (dcAudioReady)
        return true;

    if (snd_stream_init() < 0) {
        DC_LOG("[DCAudio] snd_stream_init failed\n");
        return false;
    }

    dcStream = snd_stream_alloc(dcStreamCallback, DC_AUDIO_BUFFER);
    if (dcStream == SND_STREAM_INVALID) {
        DC_LOG("[DCAudio] snd_stream_alloc failed\n");
        snd_stream_shutdown();
        return false;
    }

    snd_stream_start(dcStream, DC_AUDIO_RATE, 1 /* 1 == stereo */);
    DC_LOG("[DCAudio] %d Hz stereo, %d byte buffer (~%d ms latency), poll %d ms\n", DC_AUDIO_RATE, DC_AUDIO_BUFFER,
           (DC_AUDIO_BUFFER * 1000) / (DC_AUDIO_RATE * DC_AUDIO_CHANNELS * 2), DC_AUDIO_POLL_MS);

    dcRing = (byte *)malloc(DC_MUSIC_RING);
    if (!dcRing) {
        // Not fatal: sfx still work, music simply never fills. Better than
        // refusing to boot over a 64KB allocation.
        DC_LOG("[DCAudio] music ring alloc (%d KB) FAILED - music disabled\n", DC_MUSIC_RING / 1024);
    }

    dcStreamRun    = true;
    dcMusRun       = true;
    dcStreamThread = thd_create(0, dcStreamThreadFn, NULL);
    dcMusThread    = thd_create(0, dcMusThreadFn, NULL);
    if (!dcStreamThread) {
        DC_LOG("[DCAudio] stream thread failed to start\n");
        dcStreamRun = false;
        dcMusRun    = false;
        if (dcMusThread) {
            thd_join(dcMusThread, NULL);
            dcMusThread = NULL;
        }
        free(dcRing);
        dcRing = NULL;
        snd_stream_destroy(dcStream);
        dcStream = SND_STREAM_INVALID;
        snd_stream_shutdown();
        return false;
    }

    dcAudioReady = true;
    DC_LOG("[DCAudio] device open: %d Hz stereo S16; sources %d Hz (music %d ch, sfx %d ch)\n", DC_AUDIO_RATE, DC_SRC_RATE, DC_MUSIC_CHANNELS,
           DC_SFX_CHANNELS);
    return true;
#endif
}

void DC_ReleaseAudioDevice()
{
    if (!dcAudioReady)
        return;

    // Stop both threads before touching anything either of them reads.
    dcStreamRun = false;
    dcMusRun    = false;
    if (dcStreamThread) {
        thd_join(dcStreamThread, NULL);
        dcStreamThread = NULL;
    }
    if (dcMusThread) {
        thd_join(dcMusThread, NULL);
        dcMusThread = NULL;
    }
    // Sound RAM and the hardware voices go back BEFORE the stream is torn down:
    // snd_stream_shutdown takes the SPU with it, and freeing voices afterwards
    // would be talking to hardware that is no longer listening.
    DC_AicaSfxStopAll();
    DC_AicaSfxUnloadStage();
    DC_AicaSfxUnloadGlobal();
    if (dcAicaVoicesUp) {
        for (int i = 0; i < DC_AICA_VOICES; ++i) snd_sfx_chn_free(dcAicaVoiceCh[i]);
        dcAicaVoicesUp = false;
    }

    if (dcStream != SND_STREAM_INVALID) {
        snd_stream_stop(dcStream);
        snd_stream_destroy(dcStream);
        dcStream = SND_STREAM_INVALID;
    }
    snd_stream_shutdown();
    DC_MusicClose();
    if (dcRing) {
        free(dcRing);
        dcRing = NULL;
    }
    DC_SfxArenaReset();
    dcAudioReady = false;
}

#endif // RETRO_USING_KOS
