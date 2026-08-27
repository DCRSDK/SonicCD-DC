#ifndef AUDIO_H
#define AUDIO_H

#include <stdlib.h>

#if !RETRO_USING_KOS
#include <vorbis/vorbisfile.h>
#endif

#if RETRO_PLATFORM != RETRO_VITA && RETRO_PLATFORM != RETRO_OSX && !RETRO_USING_KOS
#include "SDL.h"
#endif

#if RETRO_USING_KOS
#include <kos/mutex.h>
// The KOS stream thread runs the mixer, so the engine still needs the mutual
// exclusion SDL_LockAudio() gave it on desktop — and, like SDL_LockAudio, this
// must actually exclude the audio callback, so DCAudio.cpp's stream callback
// takes the same lock around ProcessAudioPlayback.
//
// mutex_t, not spinlock_t: <arch/spinlock.h> is deprecated in current KOS (it
// warns and no longer provides the type), and the moved <kos/spinlock.h> is not
// present on every KOS a user might have. mutex_t has been stable for years.
// It is also the right primitive now that the lock can be held across a few
// hundred sfx-table writes — spinning through that would burn the audio thread.
extern mutex_t dcAudioLock;
// dcAudioLockWait is the watchdog's most useful single bit: if the hang report
// comes up with it set, the main thread died waiting for this mutex and there is
// nothing else to investigate. Costs one byte store either side of the lock.
#define LockAudioDevice()                                                                                                                            \
    do {                                                                                                                                             \
        dcAudioLockWait = 1;                                                                                                                         \
        mutex_lock(&dcAudioLock);                                                                                                                    \
        dcAudioLockWait = 0;                                                                                                                         \
    } while (0)
#define UnlockAudioDevice() mutex_unlock(&dcAudioLock)
#elif RETRO_USING_SDL1 || RETRO_USING_SDL2

#define LockAudioDevice()   SDL_LockAudio()
#define UnlockAudioDevice() SDL_UnlockAudio()

#else
#define LockAudioDevice()   ;
#define UnlockAudioDevice() ;
#endif

// True on every platform that actually mixes audio in software. Used instead of
// spelling out the SDL1/SDL2 pair everywhere, so the Dreamcast (which has
// neither, but does mix) takes the same paths.
#define RETRO_HAS_MIXER (RETRO_USING_SDL1 || RETRO_USING_SDL2 || RETRO_USING_KOS)

#define TRACK_COUNT   (0x10)
#define SFX_COUNT     (0x100)
#define CHANNEL_COUNT (0x4)
#define SFXDATA_COUNT (0x400000)

#define MAX_VOLUME (100)

#define STREAMFILE_COUNT (2)

#define MIX_BUFFER_SAMPLES (256)

struct TrackInfo {
    char fileName[0x40];
    bool trackLoop;
    uint loopPoint;
};

struct StreamInfo {
#if !RETRO_USING_KOS
    OggVorbis_File vorbisFile;
#endif
    int vorbBitstream;
#if RETRO_USING_SDL1
    SDL_AudioSpec spec;
#endif
#if RETRO_USING_SDL2
    SDL_AudioStream *stream;
#endif
    Sint16 buffer[MIX_BUFFER_SAMPLES];
    bool trackLoop;
    uint loopPoint;
    bool loaded;
};

struct SFXInfo {
    char name[0x40];
    Sint16 *buffer;
    size_t length;
    bool loaded;
#if RETRO_USING_KOS
    // Sounds are stored in their SOURCE rate and channel count rather than
    // converted to the device format at load: Sonic CD's global sfx set alone is
    // 689KB of s8 mono, and widening it to device-format stereo would cost 2.7MB
    // resident instead of 1.35MB. The mixer converts per frame instead — see the
    // KOS block in ProcessAudioPlayback.
    int rate;
    byte channels;
#endif
};

struct ChannelInfo {
    size_t sampleLength;
    Sint16 *samplePtr;
    int sfxID;
    byte loopSFX;
    sbyte pan;
#if RETRO_USING_KOS
    uint stepPos; // 16.16 fractional read position for the resampling mixer
#endif
};

struct StreamFile {
    byte *buffer;
    int fileSize;
    int filePos;
};

enum MusicStatuses {
    MUSIC_STOPPED = 0,
    MUSIC_PLAYING = 1,
    MUSIC_PAUSED  = 2,
    MUSIC_LOADING = 3,
    MUSIC_READY   = 4,
};

extern int globalSFXCount;
extern int stageSFXCount;

extern int masterVolume;
extern int trackID;
extern int sfxVolume;
extern int bgmVolume;
extern bool audioEnabled;

extern int nextChannelPos;
extern bool musicEnabled;
extern int musicStatus;
extern TrackInfo musicTracks[TRACK_COUNT];
extern SFXInfo sfxList[SFX_COUNT];

extern ChannelInfo sfxChannels[CHANNEL_COUNT];

extern int currentStreamIndex;
extern StreamFile streamFile[STREAMFILE_COUNT];
extern StreamInfo streamInfo[STREAMFILE_COUNT];
extern StreamFile *streamFilePtr;
extern StreamInfo *streamInfoPtr;

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
extern SDL_AudioSpec audioDeviceFormat;
#endif

int InitAudioPlayback();
void LoadGlobalSfx();

#if RETRO_HAS_MIXER
void ProcessMusicStream(void *data, Sint16 *stream, int len);
void ProcessAudioPlayback(void *data, Uint8 *stream, int len);
void ProcessAudioMixing(Sint32 *dst, const Sint16 *src, int len, int volume, sbyte pan);

inline void FreeMusInfo()
{
    LockAudioDevice();

#if RETRO_USING_KOS
    // No decoder state to clear and no in-RAM copy of the track: DC music is
    // streamed straight off the disc by DCAudio.cpp, so closing the handle is
    // the whole job. streamFile[].buffer is never allocated on this platform.
    DC_MusicClose();
#else
#if RETRO_USING_SDL2
    if (streamInfo[currentStreamIndex].stream)
        SDL_FreeAudioStream(streamInfo[currentStreamIndex].stream);
#endif
    ov_clear(&streamInfo[currentStreamIndex].vorbisFile);
#if RETRO_USING_SDL2
    streamInfo[currentStreamIndex].stream = nullptr;
#endif
    if (streamFile[currentStreamIndex].buffer)
        free(streamFile[currentStreamIndex].buffer);
#endif
    streamFile[currentStreamIndex].buffer = NULL;

    UnlockAudioDevice();
}
#else
void ProcessMusicStream() {}
void ProcessAudioPlayback() {}
void ProcessAudioMixing() {}

inline void FreeMusInfo() { ov_clear(&streamInfo[currentStreamIndex].vorbisFile); }
#endif

#if RETRO_USE_MOD_LOADER
extern char globalSfxNames[SFX_COUNT][0x40];
extern char stageSfxNames[SFX_COUNT][0x40];
void SetSfxName(const char *sfxName, int sfxID, bool global);
#endif

void LoadMusic();
void SetMusicTrack(char *filePath, byte trackID, bool loop, uint loopPoint);
bool PlayMusic(int track);
inline void StopMusic()
{
    musicStatus = MUSIC_STOPPED;
    FreeMusInfo();
}

void LoadSfx(char *filePath, byte sfxID);
void PlaySfx(int sfx, bool loop);
inline void StopSfx(int sfx)
{
#if RETRO_USING_KOS
    // Sfx living in AICA sound RAM never occupy a sfxChannels[] slot, so the
    // loop below cannot reach them. No-op for anything not on the hardware.
    DC_AicaSfxStop(sfx);
#endif
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx) {
            MEM_ZERO(sfxChannels[i]);
            sfxChannels[i].sfxID = -1;
        }
    }
}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan);

inline void SetMusicVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > MAX_VOLUME)
        volume = MAX_VOLUME;
    masterVolume = volume;
}

inline bool PauseSound()
{
    if (musicStatus == MUSIC_PLAYING) {
        musicStatus = MUSIC_PAUSED;
        return true;
    }
    return false;
}

inline void ResumeSound()
{
    if (musicStatus == MUSIC_PAUSED)
        musicStatus = MUSIC_PLAYING;
}

inline void StopAllSfx()
{
#if RETRO_USING_KOS
    DC_AicaSfxStopAll();
#endif
    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;
}
// On the Dreamcast a stage-sfx buffer may be an interior pointer into the bump
// arena, which free() would treat as a heap block and corrupt. DC_SfxFree
// range-checks and does the right thing for both arena and malloc'd buffers,
// which is why EVERY sfx release has to go through it.
#if RETRO_USING_KOS
#define SfxBufferFree(p) DC_SfxFree(p)
#else
#define SfxBufferFree(p) free(p)
#endif

// Both Release*Sfx free buffers the mixer may still be reading. StopAllSfx()
// only marks the channels dead; on desktop SDL's own callback lock closes the
// window, so on Dreamcast these have to hold the audio lock for the same reason.
inline void ReleaseGlobalSfx()
{
    LockAudioDevice();
    StopAllSfx();
    for (int i = globalSFXCount - 1; i >= 0; --i) {
        if (sfxList[i].loaded) {
            StrCopy(sfxList[i].name, "");
            SfxBufferFree(sfxList[i].buffer);
            sfxList[i].length = 0;
            sfxList[i].loaded = false;
        }
    }
    globalSFXCount = 0;
#if RETRO_USING_KOS
    // Global sfx in sound RAM are not in sfxList[].buffer, so the free loop
    // above cannot reclaim them. Inside the lock for the same reason the loop
    // is: it drops memory a voice could still be reading.
    DC_AicaSfxUnloadGlobal();
#endif
    UnlockAudioDevice();
}
inline void ReleaseStageSfx()
{
    LockAudioDevice();
    for (int i = stageSFXCount + globalSFXCount; i >= globalSFXCount; --i) {
        if (sfxList[i].loaded) {
            StrCopy(sfxList[i].name, "");
            SfxBufferFree(sfxList[i].buffer);
            sfxList[i].length = 0;
            sfxList[i].loaded = false;
        }
    }
    stageSFXCount = 0;
#if RETRO_USING_KOS
    // Hand the arena's blocks back to the heap at the same moment the sfx that
    // lived in them are dropped. Inside the lock: this frees the very memory
    // the mixer reads samples from.
    DC_SfxArenaReset();
    // The stage's hardware sfx go back to AICA sound RAM at the same moment,
    // for the same reason: nothing above can reach them, because they were
    // never in sfxList[].buffer.
    DC_AicaSfxUnloadStage();
#endif
    UnlockAudioDevice();
}

inline void ReleaseAudioDevice()
{
    StopMusic();
    StopAllSfx();
    ReleaseStageSfx();
    ReleaseGlobalSfx();
}

#endif // !AUDIO_H
