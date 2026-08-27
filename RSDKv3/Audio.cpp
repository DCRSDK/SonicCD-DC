#include "RetroEngine.hpp"
#include <cmath>
#include <iostream>
#include <thread>

int globalSFXCount = 0;
int stageSFXCount  = 0;

int masterVolume  = MAX_VOLUME;
int trackID       = -1;
int sfxVolume     = MAX_VOLUME;
int bgmVolume     = MAX_VOLUME;
bool audioEnabled = false;

int nextChannelPos;
bool musicEnabled;
int musicStatus;
TrackInfo musicTracks[TRACK_COUNT];
SFXInfo sfxList[SFX_COUNT];

ChannelInfo sfxChannels[CHANNEL_COUNT];

int currentStreamIndex = 0;
StreamFile streamFile[STREAMFILE_COUNT];
StreamInfo streamInfo[STREAMFILE_COUNT];
StreamFile *streamFilePtr = NULL;
StreamInfo *streamInfoPtr = NULL;

int currentMusicTrack = -1;

#if RETRO_USING_KOS
// Guards the engine's shared audio state against the KOS stream thread; see
// LockAudioDevice() in Audio.hpp.
//
// Recursive where KOS offers it, because SDL_LockAudio() is a counter and the
// engine's own call graph relies on that (StopMusic -> FreeMusInfo both lock).
// The Dreamcast paths have been restructured so no nesting actually remains, so
// the non-recursive fallback is safe — the recursive form is insurance against
// a future caller reintroducing one.
#ifdef RECURSIVE_MUTEX_INITIALIZER
mutex_t dcAudioLock = RECURSIVE_MUTEX_INITIALIZER;
#else
mutex_t dcAudioLock = MUTEX_INITIALIZER;
#endif
#endif

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
SDL_AudioSpec audioDeviceFormat;

#if RETRO_USING_SDL2
SDL_AudioDeviceID audioDevice;
SDL_AudioStream *ogv_stream;
#endif

#define AUDIO_FREQUENCY (44100)
#define AUDIO_FORMAT    (AUDIO_S16SYS) /**< Signed 16-bit samples */
#define AUDIO_SAMPLES   (0x800)
#define AUDIO_CHANNELS  (2)
#endif

#if RETRO_USING_KOS
// Device rate, from DCCommon.hpp. The sfx mixing path below divides by this to
// get its resample step, so it has to agree with what DC_InitAudioDevice
// actually opened.
#define AUDIO_FREQUENCY DC_AUDIO_RATE
#endif

#if RETRO_HAS_MIXER
#define ADJUST_VOLUME(s, v) (s = (s * v) / MAX_VOLUME)
#endif

int InitAudioPlayback()
{
    StopAllSfx(); //"init"
#if RETRO_USING_KOS
    audioEnabled = DC_InitAudioDevice();
    if (!audioEnabled)
        PrintLog("Audio device unavailable; continuing silently");
#elif RETRO_USING_SDL1 || RETRO_USING_SDL2
    SDL_AudioSpec want;
    want.freq     = AUDIO_FREQUENCY;
    want.format   = AUDIO_FORMAT;
    want.samples  = AUDIO_SAMPLES;
    want.channels = AUDIO_CHANNELS;
    want.callback = ProcessAudioPlayback;

#if RETRO_USING_SDL2
    if ((audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &audioDeviceFormat, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)) > 0) {
        audioEnabled = true;
        SDL_PauseAudioDevice(audioDevice, 0);
        PrintLog("Opened audio device: %d", audioDevice);
    }
    else {
        PrintLog("Unable to open audio device: %s", SDL_GetError());
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }

    // Init video sound stuff
    // TODO: Unfortunately, we're assuming that video sound is stereo at 48000Hz.
    // This is true of every .ogv file in the game (the Steam version, at least),
    // but it would be nice to make this dynamic. Unfortunately, THEORAPLAY's API
    // makes this awkward.
    ogv_stream = SDL_NewAudioStream(AUDIO_F32SYS, 2, 48000, audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
    if (!ogv_stream) {
        PrintLog("Failed to create stream: %s", SDL_GetError());
        SDL_CloseAudioDevice(audioDevice);
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#elif RETRO_USING_SDL1
    if (SDL_OpenAudio(&want, &audioDeviceFormat) == 0) {
        audioEnabled = true;
        SDL_PauseAudio(0);
    }
    else {
        PrintLog("Unable to open audio device: %s", SDL_GetError());
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#endif // !RETRO_USING_SDL1

#endif

    LoadGlobalSfx();

    return true;
}

void LoadGlobalSfx()
{
    FileInfo info;
    FileInfo infoStore;
    char strBuffer[0x100];
    byte fileBuffer = 0;
    int fileBuffer2 = 0;

    globalSFXCount = 0;

    if (LoadFile("Data/Game/GameConfig.bin", &info)) {
        infoStore = info;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);
        strBuffer[fileBuffer] = 0;

        FileRead(&fileBuffer, 1);
        FileRead(&strBuffer, fileBuffer); // Load 'Data'
        strBuffer[fileBuffer] = 0;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);
        strBuffer[fileBuffer] = 0;

        // Read Obect Names
        byte objectCount = 0;
        FileRead(&objectCount, 1);
        for (byte o = 0; o < objectCount; ++o) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;
        }

        // Read Script Paths
        for (byte s = 0; s < objectCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;
        }

        byte varCnt = 0;
        FileRead(&varCnt, 1);
        for (byte v = 0; v < varCnt; ++v) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            // Read Variable Value
            FileRead(&fileBuffer2, 4);
        }

        // Read SFX
        FileRead(&fileBuffer, 1);
        globalSFXCount = fileBuffer;
        for (byte s = 0; s < globalSFXCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            GetFileInfo(&infoStore);
            CloseFile();
            LoadSfx(strBuffer, s);
            SetFileInfo(&infoStore);

#if RETRO_USE_MOD_LOADER
            SetSfxName(strBuffer, s, true);
#endif
        }

        CloseFile();

#if RETRO_USE_MOD_LOADER
        Engine.LoadXMLSoundFX();
#endif
    }

    // sfxDataPosStage = sfxDataPos;
    nextChannelPos = 0;
    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;

#if RETRO_USING_KOS && DC_AUDIO_SELFTEST
    // Here rather than in DC_InitAudioDevice: by this point the device is open,
    // settings.ini has been read so the volumes are real, and the sfx table
    // exists — all three of which the test needs.
    DC_AudioSelfTest();
#endif
}

#if !RETRO_USING_KOS
size_t readVorbis(void *mem, size_t size, size_t nmemb, void *ptr)
{
    StreamFile *file = (StreamFile *)ptr;

    size_t n = size * nmemb;
    if (size * nmemb > file->fileSize - file->filePos)
        n = file->fileSize - file->filePos;

    if (n) {
        memcpy(mem, &file->buffer[file->filePos], n);
        file->filePos += n;
    }
    return n;
}
int seekVorbis(void *ptr, ogg_int64_t offset, int whence)
{
    StreamFile *file = (StreamFile *)ptr;

    switch (whence) {
        case SEEK_SET: whence = 0; break;
        case SEEK_CUR: whence = file->filePos; break;
        case SEEK_END: whence = file->fileSize; break;
        default: break;
    }
    file->filePos = (int)(whence + offset);
    return 0;
}
long tellVorbis(void *ptr)
{
    StreamFile *file = (StreamFile *)ptr;
    return file->filePos;
}
int closeVorbis(void *ptr) { return 1; }
#endif // !RETRO_USING_KOS

void ProcessMusicStream(Sint32 *stream, size_t bytes_wanted)
{
#if RETRO_USING_KOS
    // No decoder and no in-RAM copy of the track: DCAudio.cpp streams raw PCM
    // off the disc already at the device's rate and channel count, so this is
    // just "pull and mix". streamFilePtr/streamInfoPtr are not used on this
    // platform, hence no null check on them.
    if (musicStatus != MUSIC_PLAYING && musicStatus != MUSIC_READY)
        return;

    Sint16 buffer[MIX_BUFFER_SAMPLES];
    size_t wanted = bytes_wanted / sizeof(Sint16);
    if (wanted > MIX_BUFFER_SAMPLES)
        wanted = MIX_BUFFER_SAMPLES;

    const int got = DC_MusicReadSamples(buffer, wanted);
    if (got <= 0) {
        // A non-looping track that has run out. DC_MusicReadSamples returns
        // short only at a genuine end (a failed read is zero-filled instead),
        // so this is the end-of-track signal.
        musicStatus = MUSIC_STOPPED;
        return;
    }

    ProcessAudioMixing(stream, buffer, got, (bgmVolume * masterVolume) / MAX_VOLUME, 0);
    return;
#else
    if (!streamFilePtr || !streamInfoPtr)
        return;
    if (!streamFilePtr->fileSize)
        return;
    switch (musicStatus) {
        case MUSIC_READY:
        case MUSIC_PLAYING: {
#if RETRO_USING_SDL2
            while (musicStatus == MUSIC_PLAYING && SDL_AudioStreamAvailable(streamInfoPtr->stream) < bytes_wanted) {
                // We need more samples: get some
                long bytes_read = ov_read(&streamInfoPtr->vorbisFile, (char *)streamInfoPtr->buffer, sizeof(streamInfoPtr->buffer), 0, 2, 1,
                                          &streamInfoPtr->vorbBitstream);

                if (bytes_read == 0) {
                    // We've reached the end of the file
                    if (streamInfoPtr->trackLoop) {
                        ov_pcm_seek(&streamInfoPtr->vorbisFile, streamInfoPtr->loopPoint);
                        continue;
                    }
                    else {
                        musicStatus = MUSIC_STOPPED;
                        break;
                    }
                }

                if (musicStatus != MUSIC_PLAYING || SDL_AudioStreamPut(streamInfoPtr->stream, streamInfoPtr->buffer, (int)bytes_read) == -1)
                    return;
            }

            // Now that we know there are enough samples, read them and mix them
            int bytes_done = SDL_AudioStreamGet(streamInfoPtr->stream, streamInfoPtr->buffer, (int)bytes_wanted);
            if (bytes_done == -1) {
                return;
            }
            if (bytes_done != 0)
                ProcessAudioMixing(stream, streamInfoPtr->buffer, bytes_done / sizeof(Sint16), (bgmVolume * masterVolume) / MAX_VOLUME, 0);
#endif

#if RETRO_USING_SDL1
            size_t bytes_gotten = 0;
            byte *buffer        = (byte *)malloc(bytes_wanted);
            memset(buffer, 0, bytes_wanted);
            while (bytes_gotten < bytes_wanted) {
                // We need more samples: get some
                long bytes_read = ov_read(&streamInfoPtr->vorbisFile, (char *)streamInfoPtr->buffer,
                                          sizeof(streamInfoPtr->buffer) > (bytes_wanted - bytes_gotten) ? (bytes_wanted - bytes_gotten)
                                                                                                        : sizeof(streamInfoPtr->buffer),
                                          0, 2, 1, &streamInfoPtr->vorbBitstream);

                if (bytes_read == 0) {
                    // We've reached the end of the file
                    if (streamInfoPtr->trackLoop) {
                        ov_pcm_seek(&streamInfoPtr->vorbisFile, streamInfoPtr->loopPoint);
                        continue;
                    }
                    else {
                        musicStatus = MUSIC_STOPPED;
                        break;
                    }
                }

                if (bytes_read > 0) {
                    memcpy(buffer + bytes_gotten, streamInfoPtr->buffer, bytes_read);
                    bytes_gotten += bytes_read;
                }
                else {
                    PrintLog("Music read error: vorbis error: %d", bytes_read);
                }
            }

            if (bytes_gotten > 0) {
                SDL_AudioCVT convert;
                MEM_ZERO(convert);
                int cvtResult = SDL_BuildAudioCVT(&convert, streamInfoPtr->spec.format, streamInfoPtr->spec.channels, streamInfoPtr->spec.freq,
                                                  audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
                if (cvtResult == 0) {
                    if (convert.len_mult > 0) {
                        convert.buf = (byte *)malloc(bytes_gotten * convert.len_mult);
                        convert.len = bytes_gotten;
                        memcpy(convert.buf, buffer, bytes_gotten);
                        SDL_ConvertAudio(&convert);
                    }
                }

                if (cvtResult == 0)
                    ProcessAudioMixing(stream, (const Sint16 *)convert.buf, bytes_gotten / sizeof(Sint16), (bgmVolume * masterVolume) / MAX_VOLUME,
                                       0);

                if (convert.len > 0 && convert.buf)
                    free(convert.buf);
            }
            if (bytes_wanted > 0)
                free(buffer);
#endif
            break;
        }
        case MUSIC_STOPPED:
        case MUSIC_PAUSED:
        case MUSIC_LOADING:
            // dont play
            break;
    }
#endif // !RETRO_USING_KOS
}

void ProcessAudioPlayback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata; // Unused

    if (!audioEnabled)
        return;

    Sint16 *output_buffer = (Sint16 *)stream;

    size_t samples_remaining = (size_t)len / sizeof(Sint16);
    while (samples_remaining != 0) {
        Sint32 mix_buffer[MIX_BUFFER_SAMPLES];
        memset(mix_buffer, 0, sizeof(mix_buffer));

        const size_t samples_to_do = (samples_remaining < MIX_BUFFER_SAMPLES) ? samples_remaining : MIX_BUFFER_SAMPLES;

        // Mix music
        ProcessMusicStream(mix_buffer, samples_to_do * sizeof(Sint16));

#if RETRO_USING_SDL2
        // Process music being played by a ogv video
        if (videoPlaying == 1) {
            // Fetch THEORAPLAY audio packets, and shove them into the SDL Audio Stream
            const size_t bytes_to_do = samples_to_do * sizeof(Sint16);

            const THEORAPLAY_AudioPacket *packet;

            while ((packet = THEORAPLAY_getAudio(videoDecoder)) != NULL) {
                SDL_AudioStreamPut(ogv_stream, packet->samples, packet->frames * sizeof(float) * 2); // 2 for stereo
                THEORAPLAY_freeAudio(packet);
            }

            Sint16 buffer[MIX_BUFFER_SAMPLES];

            // If we need more samples, assume we've reached the end of the file,
            // and flush the audio stream so we can get more. If we were wrong, and
            // there's still more file left, then there will be a gap in the audio. Sorry.
            if (SDL_AudioStreamAvailable(ogv_stream) < bytes_to_do)
                SDL_AudioStreamFlush(ogv_stream);

            // Fetch the converted audio data, which is ready for mixing.
            int get = SDL_AudioStreamGet(ogv_stream, buffer, (int)bytes_to_do);

            // Mix the converted audio data into the final output
            if (get != -1)
                ProcessAudioMixing(mix_buffer, buffer, get / sizeof(Sint16), bgmVolume, 0);
        }
        else {
            SDL_AudioStreamClear(ogv_stream); // Prevent leftover audio from playing at the start of the next video
        }
#endif

#if RETRO_USING_SDL1
        // Process music being played by a video
        // TODO: SDL1.2 lacks SDL_AudioStream so until someone finds good way to replicate that, I'm gonna leave this commented out
        /*if (videoPlaying) {
            // Fetch THEORAPLAY audio packets
            const size_t bytes_to_do = samples_to_do * sizeof(Sint16);
            size_t bytes_done        = 0;

            byte *vid_buffer             = (byte *)malloc(bytes_to_do);
            memset(vid_buffer, 0, bytes_to_do);

            const THEORAPLAY_AudioPacket *packet;

            while ((packet = THEORAPLAY_getAudio(videoDecoder)) != NULL) {
                int data_size = packet->frames * sizeof(float) * 2;
                if (bytes_done < bytes_to_do) {
                    memcpy(vid_buffer + bytes_done, packet->samples, data_size >= bytes_to_do ? bytes_to_do : data_size); // 2 for stereo
                    bytes_done += data_size >= bytes_to_do ? bytes_to_do : data_size;
                }
                THEORAPLAY_freeAudio(packet);
            }

            Sint16 convBuffer[MIX_BUFFER_SAMPLES];

            // If we need more samples, assume we've reached the end of the file,
            // and flush the audio stream so we can get more. If we were wrong, and
            // there's still more file left, then there will be a gap in the audio. Sorry.
            if (bytes_done < bytes_to_do) {
                memset(vid_buffer, 0, bytes_to_do);
            }

            if (bytes_done > 0) {
                SDL_AudioCVT convert;
                MEM_ZERO(convert);
                int cvtResult =
                    SDL_BuildAudioCVT(&convert, AUDIO_S16SYS, 2, 48000, audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
                if (cvtResult == 0) {
                    if (convert.len_mult > 0) {
                        convert.buf = (byte *)malloc(bytes_done * convert.len_mult);
                        convert.len = bytes_done;
                        memcpy(convert.buf, vid_buffer, bytes_done);
                        SDL_ConvertAudio(&convert);
                    }
                }

                if (cvtResult == 0)
                    ProcessAudioMixing(mix_buffer, (const Sint16 *)convert.buf, bytes_done / sizeof(Sint16), MAX_VOLUME, 0);

                if (convert.len > 0 && convert.buf)
                    free(convert.buf);
            }
        }*/
#endif

        // Mix SFX
        for (byte i = 0; i < CHANNEL_COUNT; ++i) {
            ChannelInfo *sfx = &sfxChannels[i];
            if (sfx == NULL)
                continue;

            if (sfx->sfxID < 0)
                continue;

#if RETRO_USING_KOS
            // Sfx are held in their source rate/channels (see SFXInfo in
            // Audio.hpp). Resample and expand to the device's stereo format one
            // frame at a time, straight into the mix buffer. With 22050 Hz
            // mono sources and a 22050 Hz device, `step` is exactly 1<<16 and
            // this degenerates to a copy-with-duplication.
            if (sfx->samplePtr) {
                Sint16 buffer[MIX_BUFFER_SAMPLES];
                SFXInfo *sfxData = &sfxList[sfx->sfxID];
                const int schan  = sfxData->channels >= 2 ? 2 : 1;
                const uint step  = (uint)(((unsigned long long)sfxData->rate << 16) / AUDIO_FREQUENCY);

                size_t frames_done        = 0;
                const size_t frames_to_do = samples_to_do / 2;
                while (frames_done < frames_to_do && sfx->samplePtr) {
                    Sint16 l = sfx->samplePtr[0];
                    Sint16 r = (schan == 2 && sfx->sampleLength > 1) ? sfx->samplePtr[1] : l;

                    buffer[frames_done * 2]     = l;
                    buffer[frames_done * 2 + 1] = r;
                    ++frames_done;

                    sfx->stepPos += step;
                    size_t adv = (size_t)(sfx->stepPos >> 16) * schan;
                    sfx->stepPos &= 0xFFFF;

                    if (adv >= sfx->sampleLength) {
                        if (sfx->loopSFX) {
                            sfx->samplePtr    = sfxData->buffer;
                            sfx->sampleLength = sfxData->length;
                            sfx->stepPos      = 0;
                        }
                        else {
                            MEM_ZEROP(sfx);
                            sfx->sfxID = -1;
                            break;
                        }
                    }
                    else {
                        sfx->samplePtr += adv;
                        sfx->sampleLength -= adv;
                    }
                }

                if (frames_done > 0)
                    ProcessAudioMixing(mix_buffer, buffer, (int)(frames_done * 2), sfxVolume, sfx->pan);
            }
#else
            if (sfx->samplePtr) {
                Sint16 buffer[MIX_BUFFER_SAMPLES];

                size_t samples_done = 0;
                while (samples_done != samples_to_do) {
                    size_t sampleLen = (sfx->sampleLength < samples_to_do - samples_done) ? sfx->sampleLength : samples_to_do - samples_done;
                    memcpy(&buffer[samples_done], sfx->samplePtr, sampleLen * sizeof(Sint16));

                    samples_done += sampleLen;
                    sfx->samplePtr += sampleLen;
                    sfx->sampleLength -= sampleLen;

                    if (sfx->sampleLength == 0) {
                        if (sfx->loopSFX) {
                            sfx->samplePtr    = sfxList[sfx->sfxID].buffer;
                            sfx->sampleLength = sfxList[sfx->sfxID].length;
                        }
                        else {
                            MEM_ZEROP(sfx);
                            sfx->sfxID = -1;
                            break;
                        }
                    }
                }

#if RETRO_HAS_MIXER
                ProcessAudioMixing(mix_buffer, buffer, (int)samples_done, sfxVolume, sfx->pan);
#endif
            }
#endif // !RETRO_USING_KOS
        }

        // Clamp mixed samples back to 16-bit and write them to the output buffer.
        //
        // Bounded by samples_to_do, NOT by the size of mix_buffer. Upstream uses
        // the array size and gets away with it because SDL always asks for a
        // whole number of MIX_BUFFER_SAMPLES chunks; KOS's snd_stream does not,
        // so the final short chunk of a callback wrote up to 255 samples past
        // the end of the caller's buffer — silently, into whatever follows it.
        for (size_t i = 0; i < samples_to_do; ++i) {
            const Sint16 max_audioval = ((1 << (16 - 1)) - 1);
            const Sint16 min_audioval = -(1 << (16 - 1));

            const Sint32 sample = mix_buffer[i];

            if (sample > max_audioval)
                *output_buffer++ = max_audioval;
            else if (sample < min_audioval)
                *output_buffer++ = min_audioval;
            else
                *output_buffer++ = sample;
        }

        samples_remaining -= samples_to_do;
    }
}

#if RETRO_HAS_MIXER
void ProcessAudioMixing(Sint32 *dst, const Sint16 *src, int len, int volume, sbyte pan)
{
    if (volume == 0)
        return;

    if (volume > MAX_VOLUME)
        volume = MAX_VOLUME;

    float panL = 0;
    float panR = 0;
    int i      = 0;

    if (pan < 0) {
        // fabsf, not abs: a float argument to a function that may resolve to
        // int abs(int) truncates, turning every partial pan into no pan at all.
        // Sonic CD only pans hard (+/-100), where both readings agree, so this
        // has been invisible - but it is a trap under any partial pan.
        panR = 1.0f - fabsf(pan / 100.0f);
        panL = 1.0f;
    }
    else if (pan > 0) {
        panL = 1.0f - fabsf(pan / 100.0f);
        panR = 1.0f;
    }

    while (len--) {
        Sint32 sample = *src++;
        ADJUST_VOLUME(sample, volume);

        if (pan != 0) {
            if ((i % 2) != 0) {
                sample *= panR;
            }
            else {
                sample *= panL;
            }
        }

        *dst++ += sample;

        i++;
    }
}
#endif

#if RETRO_USE_MOD_LOADER
char globalSfxNames[SFX_COUNT][0x40];
char stageSfxNames[SFX_COUNT][0x40];
void SetSfxName(const char *sfxName, int sfxID, bool global)
{
    char *sfxNamePtr = global ? globalSfxNames[sfxID] : stageSfxNames[sfxID];

    int sfxNamePos = 0;
    int sfxPtrPos  = 0;
    byte mode      = 0;
    while (sfxName[sfxNamePos]) {
        if (sfxName[sfxNamePos] == '.' && mode == 1)
            mode = 2;
        else if ((sfxName[sfxNamePos] == '/' || sfxName[sfxNamePos] == '\\') && !mode)
            mode = 1;
        else if (sfxName[sfxNamePos] != ' ' && mode == 1)
            sfxNamePtr[sfxPtrPos++] = sfxName[sfxNamePos];
        ++sfxNamePos;
    }
    sfxNamePtr[sfxPtrPos] = 0;
    PrintLog("Set %s SFX (%d) name to: %s", (global ? "Global" : "Stage"), sfxID, sfxNamePtr);
}
#endif

void LoadMusic()
{
    currentStreamIndex++;
    currentStreamIndex %= STREAMFILE_COUNT;

#if RETRO_USING_KOS
    // Music is a loose raw-PCM file streamed straight off the disc — it is
    // deliberately NOT read through LoadFile, because that resolves against
    // Data.rsdk first and a loose file sitting next to the datapack is
    // invisible to it. DC_MusicOpen prepends BASE_PATH itself.
    //
    // musicTracks[].fileName is "Data/Music/<name>.ogg"; the file on disc keeps
    // that name so GameConfig.bin still resolves it, but its CONTENTS are raw
    // PCM. See the asset-pipeline note at the top of DCAudio.cpp.
    //
    // Both calls are made OUTSIDE the audio lock, deliberately:
    //   - the stream callback takes that same lock, and a GD-ROM open costs on
    //     the order of 100ms; holding it across one would underrun the mixer;
    //   - FreeMusInfo() takes the lock itself, so calling it from inside a
    //     locked region would deadlock on a non-recursive mutex.
    // Nothing else can be starting a track concurrently — LoadMusic is only
    // ever reached from the main thread via PlayMusic.
    DC_MusicClose();

    const bool dcOpened = DC_MusicOpen(musicTracks[currentMusicTrack].fileName, musicTracks[currentMusicTrack].trackLoop,
                                       musicTracks[currentMusicTrack].loopPoint, 0);

    LockAudioDevice();
    if (dcOpened) {
        StreamInfo *strmInfo = &streamInfo[currentStreamIndex];
        musicStatus          = MUSIC_PLAYING;
        masterVolume         = MAX_VOLUME;
        trackID              = currentMusicTrack;
        strmInfo->trackLoop  = musicTracks[currentMusicTrack].trackLoop;
        strmInfo->loopPoint  = musicTracks[currentMusicTrack].loopPoint;
        strmInfo->loaded     = true;
        // fileSize is what StopMusic/FreeMusInfo test to decide there is
        // something to tear down, so it has to be non-zero even though no
        // buffer is allocated on this platform.
        streamFile[currentStreamIndex].fileSize = 1;
        streamFile[currentStreamIndex].buffer   = NULL;
        streamFilePtr                           = &streamFile[currentStreamIndex];
        streamInfoPtr                           = &streamInfo[currentStreamIndex];
        currentMusicTrack                       = -1;
    }
    else {
        musicStatus = MUSIC_STOPPED;
    }
    UnlockAudioDevice();
    return;
#else

    LockAudioDevice();

    if (streamFile[currentStreamIndex].fileSize > 0)
        FreeMusInfo();

    FileInfo info;
    if (LoadFile(musicTracks[currentMusicTrack].fileName, &info)) {
        StreamInfo *strmInfo = &streamInfo[currentStreamIndex];

        StreamFile *musFile                   = &streamFile[currentStreamIndex];
        musFile->filePos                      = 0;
        musFile->fileSize                     = info.vFileSize;
        streamFile[currentStreamIndex].buffer = (byte *)malloc(musFile->fileSize);

        FileRead(streamFile[currentStreamIndex].buffer, musFile->fileSize);
        CloseFile();

        ov_callbacks callbacks;

        callbacks.read_func  = readVorbis;
        callbacks.seek_func  = seekVorbis;
        callbacks.tell_func  = tellVorbis;
        callbacks.close_func = closeVorbis;

        int error = ov_open_callbacks(musFile, &strmInfo->vorbisFile, NULL, 0, callbacks);
        if (error == 0) {
            strmInfo->vorbBitstream = -1;
            strmInfo->vorbisFile.vi = ov_info(&strmInfo->vorbisFile, -1);

#if RETRO_USING_SDL2
            strmInfo->stream = SDL_NewAudioStream(AUDIO_S16, strmInfo->vorbisFile.vi->channels, (int)strmInfo->vorbisFile.vi->rate,
                                                  audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
            if (!strmInfo->stream) {
                PrintLog("Failed to create stream: %s", SDL_GetError());
            }
#endif

#if RETRO_USING_SDL1
            strmInfo->spec.format   = AUDIO_S16;
            strmInfo->spec.channels = strmInfo->vorbisFile.vi->channels;
            strmInfo->spec.freq     = (int)strmInfo->vorbisFile.vi->rate;
#endif

            musicStatus         = MUSIC_PLAYING;
            masterVolume        = MAX_VOLUME;
            trackID             = currentMusicTrack;
            strmInfo->trackLoop = musicTracks[currentMusicTrack].trackLoop;
            strmInfo->loopPoint = musicTracks[currentMusicTrack].loopPoint;
            strmInfo->loaded    = true;
            streamFilePtr       = &streamFile[currentStreamIndex];
            streamInfoPtr       = &streamInfo[currentStreamIndex];
            currentMusicTrack   = -1;
        }
        else {
            musicStatus = MUSIC_STOPPED;
            PrintLog("Failed to load vorbis! error: %d", error);
            switch (error) {
                default: PrintLog("Vorbis open error: Unknown (%d)", error); break;
                case OV_EREAD: PrintLog("Vorbis open error: A read from media returned an error"); break;
                case OV_ENOTVORBIS: PrintLog("Vorbis open error: Bitstream does not contain any Vorbis data"); break;
                case OV_EVERSION: PrintLog("Vorbis open error: Vorbis version mismatch"); break;
                case OV_EBADHEADER: PrintLog("Vorbis open error: Invalid Vorbis bitstream header"); break;
                case OV_EFAULT: PrintLog("Vorbis open error: Internal logic fault; indicates a bug or heap / stack corruption"); break;
            }
        }
    }
    else {
        musicStatus = MUSIC_STOPPED;
    }
    UnlockAudioDevice();
#endif // !RETRO_USING_KOS
}

void SetMusicTrack(char *filePath, byte trackID, bool loop, uint loopPoint)
{
    LockAudioDevice();
    TrackInfo *track = &musicTracks[trackID];
    StrCopy(track->fileName, "Data/Music/");
    StrAdd(track->fileName, filePath);
    track->trackLoop = loop;
    track->loopPoint = loopPoint;
    UnlockAudioDevice();
}
bool PlayMusic(int track)
{
    if (!audioEnabled)
        return false;

#if !RETRO_USE_ORIGINAL_CODE
    if (StrComp(musicTracks[track].fileName, "Data/Music/")) {
        StopMusic();
        return false;
    }
#endif

    if (musicTracks[track].fileName[0]) {
        if (musicStatus != MUSIC_LOADING) {
            currentMusicTrack = track;
            musicStatus       = MUSIC_LOADING;
            LoadMusic();
            return true;
        }
        else {
            PrintLog("WARNING music tried to play while music was loading!");
        }
    }
    else {
        StopMusic();
    }
    return false;
}

void LoadSfx(char *filePath, byte sfxID)
{
    if (!audioEnabled)
        return;

#if RETRO_USING_KOS
    DC_PHASE(DC_PHASE_SFXLOAD);
#endif

    FileInfo info;
    char fullPath[0x80];

    StrCopy(fullPath, "Data/SoundFX/");
    StrAdd(fullPath, filePath);

#if RETRO_USING_KOS
    // Stage sfx are everything loaded after the global set, and they are what
    // the bump arena exists for (see DCAudio.cpp). globalSFXCount is still 0
    // while LoadGlobalSfx runs, so this correctly classifies both.
    const bool stageSfx = globalSFXCount > 0 && sfxID >= globalSFXCount;

    if (LoadFile(fullPath, &info)) {
        // AICA sound RAM first. A sound that lands there costs this heap nothing,
        // costs the SH4 no mixing time, and carries its own pitch register — so
        // sounds at different sample rates coexist without the device rate having
        // to be a compromise between them.
        if (DC_AicaSfxLoad((size_t)info.vFileSize, sfxID)) {
            CloseFile();
            LockAudioDevice();
            StrCopy(sfxList[sfxID].name, filePath);
            // buffer == NULL is the marker: the software mixer skips a channel
            // with no samplePtr, and PlaySfx/SetSfxAttributes never get that far
            // because DC_AicaSfx* claims the sound first.
            sfxList[sfxID].buffer   = NULL;
            sfxList[sfxID].length   = 0;
            sfxList[sfxID].rate     = DC_AicaSfxRate(sfxID);
            sfxList[sfxID].channels = 1;
            sfxList[sfxID].loaded   = true;
            UnlockAudioDevice();
            return;
        }
        // DC_AicaSfxLoad reads from the shared file reader to decide, so by here
        // the position is somewhere inside the file. Rewind before the heap path
        // reads it again — the datafile cipher state is reset by this too, which
        // is why it has to be SetFilePosition rather than a raw seek.
        SetFilePosition(0);

        size_t samples = 0;
        int rate = 0, channels = 0;
        // Deliberately NOT holding the audio lock across this: it reads from the
        // GD-ROM, and LockAudioDevice() is a spinlock — the mixer thread would
        // burn the entire load spinning on it. Nothing in sfxList is touched
        // until the buffer is complete, and the slot being filled is not yet
        // marked loaded, so no reader can reach it in the meantime.
        Sint16 *buffer = DC_LoadWAVStreamed((size_t)info.vFileSize, &samples, &rate, &channels, stageSfx);
        CloseFile();

        if (buffer) {
            LockAudioDevice();
            StrCopy(sfxList[sfxID].name, filePath);
            sfxList[sfxID].buffer   = buffer;
            sfxList[sfxID].length   = samples;
            sfxList[sfxID].rate     = rate;
            sfxList[sfxID].channels = (byte)channels;
            sfxList[sfxID].loaded   = true;
            UnlockAudioDevice();
        }
        else {
            PrintLog("Unable to read sfx: %s", info.fileName);
        }
    }
    return;
#endif

    if (LoadFile(fullPath, &info)) {
        byte *sfx = new byte[info.vFileSize];
        FileRead(sfx, info.vFileSize);
        CloseFile();

        LockAudioDevice();
#if RETRO_USING_SDL1 || RETRO_USING_SDL2
        SDL_RWops *src = SDL_RWFromMem(sfx, info.vFileSize);
        if (src == NULL) {
            PrintLog("Unable to open sfx: %s", info.fileName);
        }
        else {
            SDL_AudioSpec wav_spec;
            uint wav_length;
            byte *wav_buffer;
            SDL_AudioSpec *wav = SDL_LoadWAV_RW(src, 0, &wav_spec, &wav_buffer, &wav_length);

            SDL_RWclose(src);
            delete[] sfx;
            if (wav == NULL) {
                PrintLog("Unable to read sfx: %s", info.fileName);
            }
            else {
                SDL_AudioCVT convert;
                if (SDL_BuildAudioCVT(&convert, wav->format, wav->channels, wav->freq, audioDeviceFormat.format, audioDeviceFormat.channels,
                                      audioDeviceFormat.freq)
                    > 0) {
                    convert.buf = (byte *)malloc(wav_length * convert.len_mult);
                    convert.len = wav_length;
                    memcpy(convert.buf, wav_buffer, wav_length);
                    SDL_ConvertAudio(&convert);

                    StrCopy(sfxList[sfxID].name, filePath);
                    sfxList[sfxID].buffer = (Sint16 *)convert.buf;
                    sfxList[sfxID].length = convert.len_cvt / sizeof(Sint16);
                    sfxList[sfxID].loaded = true;
                    SDL_FreeWAV(wav_buffer);
                }
                else {
                    StrCopy(sfxList[sfxID].name, filePath);
                    sfxList[sfxID].buffer = (Sint16 *)wav_buffer;
                    sfxList[sfxID].length = wav_length / sizeof(Sint16);
                    sfxList[sfxID].loaded = true;
                }
            }
        }
#endif
        UnlockAudioDevice();
    }
}
#if RETRO_USING_KOS
// Surfaced on the perf overlay as "SFX rrrrr c": the rate and channel count the
// mixer is actually using for the most recent sound. A sound at the wrong pitch
// is either a source the loader mis-read or a source that is not the file you
// think it is, and this tells the two apart without guessing.
volatile int dcLastSfxRate     = 0;
volatile int dcLastSfxChannels = 0;
volatile int dcLastSfxID       = -1;

// How many times SetSfxAttributes has started a SECOND copy of a sound that was
// already playing on another channel.
//
// It looks for `sfxID == sfx || sfxID == -1` and takes the first match, so a
// free channel at a lower index wins over the channel the sound is actually on
// — and since it also rewinds samplePtr, the result is two copies of the same
// sample a few milliseconds apart. That is a comb filter, and a comb filter on
// a bright metallic sound is exactly what "higher pitched" describes.
//
// Rings and ring loss are the only two sfx in Sonic CD that call this, which is
// also the shortlist of sounds being complained about. If this counter climbs
// once per ring, that is the answer.
volatile int dcSfxDoubleStarts = 0;

// Pan of the most recent sound, as the mixer has it.
//
// Sonic CD's rings alternate between TWO sfx slots holding the SAME Ring.wav
// (GameConfig loads it as sfx 1 and sfx 2), hard-panned opposite ways: the Ring
// object's bytecode calls SetSfxAttributes(1, -1, -100) and
// SetSfxAttributes(2, -1, +100) on alternate rings. If that pan is not reaching
// the mixer, two copies of one sample play centred and comb against each other
// — which is what "the pitch is wrong but every partial measures right" is.
volatile int dcLastSfxPan = 0;

#endif

void PlaySfx(int sfx, bool loop)
{
#if RETRO_USING_KOS
    // Hardware first, and deliberately OUTSIDE the audio lock: an AICA sound
    // touches none of the state the mixer thread reads, so starting one no
    // longer has to wait for a callback in progress. That removes the last
    // source of "the sound arrives after the action" on this path.
    if (DC_AicaSfxPlay(sfx, loop)) {
        dcLastSfxID       = sfx;
        dcLastSfxRate     = sfxList[sfx].rate;
        dcLastSfxChannels = sfxList[sfx].channels;
        dcLastSfxPan      = 0;
        return;
    }
#endif
    LockAudioDevice();
#if RETRO_USING_KOS
    if (sfx >= 0 && sfx < SFX_COUNT) {
        dcLastSfxID       = sfx;
        dcLastSfxRate     = sfxList[sfx].rate;
        dcLastSfxChannels = sfxList[sfx].channels;
        dcLastSfxPan      = 0; // PlaySfx always starts centred

        // NO BUFFER WALK HERE.
        //
        // This used to checksum the whole sound to prove the loader was
        // producing the right bytes, and it did its job — Jump, Ring and
        // LoseRings all matched their files exactly. It was also a mistake to
        // leave in: PlaySfx runs UNDER THE AUDIO LOCK, so walking 38000 samples
        // with a multiply each held off the mixer callback every time a sound
        // started, which showed up as sound effects lagging the action.
        //
        // The cheap fields above (rate, channels, id) are three loads and stay.
    }
#endif
    int sfxChannelID = nextChannelPos++;
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID == sfx) {
            sfxChannelID = c;
            break;
        }
    }

    ChannelInfo *sfxInfo  = &sfxChannels[sfxChannelID];
    sfxInfo->sfxID        = sfx;
    sfxInfo->samplePtr    = sfxList[sfx].buffer;
    sfxInfo->sampleLength = sfxList[sfx].length;
    sfxInfo->loopSFX      = loop;
    sfxInfo->pan          = 0;
#if RETRO_USING_KOS
    sfxInfo->stepPos = 0; // restart the resampler's fractional position
#endif
    if (nextChannelPos == CHANNEL_COUNT)
        nextChannelPos = 0;
    UnlockAudioDevice();
}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan)
{
#if RETRO_USING_KOS
    // This, not PlaySfx, is what starts a ring — and the pan it carries is the
    // hard +/-100 alternation the two ring slots depend on. On the hardware path
    // that pan is one byte in the key-on rather than a multiply per sample.
    if (DC_AicaSfxSetAttr(sfx, loopCount, pan)) {
        dcLastSfxID       = sfx;
        dcLastSfxRate     = sfxList[sfx].rate;
        dcLastSfxChannels = sfxList[sfx].channels;
        dcLastSfxPan      = pan;
        return;
    }
#endif
    LockAudioDevice();
    int sfxChannel = -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx || sfxChannels[i].sfxID == -1) {
            sfxChannel = i;
            break;
        }
    }
    if (sfxChannel == -1) {
        // Upstream returns here WITHOUT unlocking. On SDL that is a counted
        // lock on the main thread and the damage is bounded; here it is the
        // mutex the mixer callback takes every few milliseconds, so leaking it
        // once stops all audio permanently and looks like a hang.
        UnlockAudioDevice();
        return; // wasn't found
    }

    // TODO: is this right? should it play an sfx here? without this rings dont play any sfx so I assume it must be?
#if RETRO_USING_KOS
    // Diagnostic only — nothing below is changed by it.
    if (sfxChannels[sfxChannel].sfxID != sfx) {
        for (int i = 0; i < CHANNEL_COUNT; ++i) {
            if (i != sfxChannel && sfxChannels[i].sfxID == sfx) {
                ++dcSfxDoubleStarts;
                break;
            }
        }
    }
#endif

    ChannelInfo *sfxInfo  = &sfxChannels[sfxChannel];
    sfxInfo->samplePtr    = sfxList[sfx].buffer;
    sfxInfo->sampleLength = sfxList[sfx].length;
    sfxInfo->loopSFX      = loopCount == -1 ? sfxInfo->loopSFX : loopCount;
    sfxInfo->pan          = pan;
    sfxInfo->sfxID        = sfx;
#if RETRO_USING_KOS
    // SetSfxAttributes, not PlaySfx, is what starts a ring — without this the
    // overlay never sees the sound the complaint is actually about.
    if (sfx >= 0 && sfx < SFX_COUNT) {
        dcLastSfxID       = sfx;
        dcLastSfxRate     = sfxList[sfx].rate;
        dcLastSfxChannels = sfxList[sfx].channels;
        dcLastSfxPan      = pan;
    }
#endif
#if RETRO_USING_KOS
    sfxInfo->stepPos = 0; // this rewinds samplePtr, so rewind the resampler too
#endif
    UnlockAudioDevice();
}
