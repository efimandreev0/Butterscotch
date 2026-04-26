#include "sdl12_audio_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#include "../vorbis/stb_vorbis.c"

static bool use_mixer = true;

#define MAX_MIXER_CHANNELS 32
#define MUSIC_INSTANCE_ID_BASE 200000
#define SOUND_INSTANCE_ID_BASE 100000

#define STREAMING_SIZE_THRESHOLD (2 * 1024 * 1024)

// Кэш распакованных SFX: каждый чанк конвертирован в формат микшера
// (44.1 kHz stereo s16 = 176 KB/сек). 16 чанков по 2-3 сек = 5-8 MB heap,
// что слишком жирно для Old 3DS. 6 хватает (одновременно обычно играет 2-4),
// остальные подгрузятся из data.win по запросу.
#define MAX_CACHED_CHUNKS 16

// ============================================================================
// [1] MUSIC STREAMING (RWops)
// ============================================================================

typedef struct {
    FILE* fp;
    uint32_t base;
    uint32_t size;
    uint32_t pos;
} RWDataWinContext;

static int rw_datawin_seek(SDL_RWops *context, int offset, int whence) {
    RWDataWinContext *ctx = (RWDataWinContext *)context->hidden.unknown.data1;
    int32_t new_pos = (int32_t)ctx->pos;

    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos += offset;
    else if (whence == SEEK_END) new_pos = (int32_t)ctx->size + offset;

    if (new_pos < 0) new_pos = 0;
    if ((uint32_t)new_pos > ctx->size) new_pos = (int32_t)ctx->size;

    ctx->pos = (uint32_t)new_pos;
    return (int)ctx->pos;
}

static int rw_datawin_read(SDL_RWops *context, void *ptr, int size, int maxnum) {
    RWDataWinContext *ctx = (RWDataWinContext *)context->hidden.unknown.data1;

    if (ctx->pos >= ctx->size) return 0;

    uint32_t remaining = ctx->size - ctx->pos;
    uint32_t total_to_read = (uint32_t)size * maxnum;

    if (total_to_read > remaining) {
        maxnum = remaining / size;
    }
    if (maxnum <= 0) return 0;

    fseek(ctx->fp, (long)(ctx->base + ctx->pos), SEEK_SET);
    int read_items = fread(ptr, size, maxnum, ctx->fp);

    if (read_items > 0) {
        ctx->pos += (uint32_t)(read_items * size);
    }
    return read_items;
}

static int rw_datawin_close(SDL_RWops *context) {
    if (context) {
        RWDataWinContext *ctx = (RWDataWinContext *)context->hidden.unknown.data1;
        if (ctx) {
            if (ctx->fp) fclose(ctx->fp);
            free(ctx);
        }
        SDL_FreeRW(context);
    }
    return 0;
}

static SDL_RWops* createMusicRWops(const char* archivePath, uint32_t offset, uint32_t size) {
    FILE* fp = fopen(archivePath, "rb");
    if (!fp) return NULL;

    SDL_RWops *rw = SDL_AllocRW();
    if (!rw) {
        fclose(fp);
        return NULL;
    }

    RWDataWinContext *ctx = (RWDataWinContext *)safeMalloc(sizeof(RWDataWinContext));
    ctx->fp = fp;
    ctx->base = offset;
    ctx->size = size;
    ctx->pos = 0;

    rw->hidden.unknown.data1 = ctx;
    rw->read = rw_datawin_read;
    rw->seek = rw_datawin_seek;
    rw->write = NULL;
    rw->close = rw_datawin_close;
    rw->type = 0;

    return rw;
}

// ============================================================================
// [2] AUDIO CACHE & LOADING
// ============================================================================

static char* resolveExternalPath(SdlMixerAudioSystem* sys, Sound* sound) {
    if (!sound->file || sound->file[0] == '\0') return NULL;

    char filename[512];
    if (strchr(sound->file, '.') != NULL) {
        snprintf(filename, sizeof(filename), "%s", sound->file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", sound->file);
    }
    return sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
}

static void evictOldestSfx(SdlMixerAudioSystem* sys) {
    int loadedChunks = 0;
    uint32_t soundCount = sys->base.dataWin->sond.count;

    for (uint32_t i = 0; i < soundCount; i++) {
        if (sys->chunks[i]) loadedChunks++;
    }

    if (loadedChunks >= MAX_CACHED_CHUNKS) {
        int oldestId = -1;
        uint32_t oldestTime = 0xFFFFFFFF;

        for (uint32_t i = 0; i < soundCount; i++) {
            if (sys->chunks[i]) {
                bool isPlaying = false;
                for (int ch = 0; ch < MAX_MIXER_CHANNELS; ch++) {
                    if (Mix_Playing(ch) && Mix_GetChunk(ch) == sys->chunks[i]) {
                        isPlaying = true;
                        break;
                    }
                }

                if (!isPlaying && sys->chunkLastUsed[i] < oldestTime) {
                    oldestTime = sys->chunkLastUsed[i];
                    oldestId = (int)i;
                }
            }
        }

        if (oldestId != -1) {
            Mix_FreeChunk(sys->chunks[oldestId]);
            sys->chunks[oldestId] = NULL;

            if (sys->decodedSfxBufs[oldestId]) {
                free(sys->decodedSfxBufs[oldestId]);
                sys->decodedSfxBufs[oldestId] = NULL;
            }
        }
    }
}

static bool loadSfxIntoRAM(SdlMixerAudioSystem* sys, int32_t soundIndex, AudioEntry* entry) {
    evictOldestSfx(sys);

    uint8_t* rawBuf = safeMalloc(entry->dataSize);
    FILE* fp = fopen(sys->archivePath, "rb");
    if (!fp) {
        free(rawBuf);
        return false;
    }

    fseek(fp, (long)entry->dataOffset, SEEK_SET);
    fread(rawBuf, 1, entry->dataSize, fp);
    fclose(fp);

    bool isOgg = (entry->dataSize > 4 && memcmp(rawBuf, "OggS", 4) == 0);

    if (isOgg) {
        int ogg_channels, ogg_sample_rate;
        short *decodedPcm;
        int samples = stb_vorbis_decode_memory(rawBuf, entry->dataSize, &ogg_channels, &ogg_sample_rate, &decodedPcm);

        if (samples > 0) {
            int freq, mix_channels;
            Uint16 format;
            Mix_QuerySpec(&freq, &format, &mix_channels);

            SDL_AudioCVT cvt;
            int build_ret = SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, ogg_channels, ogg_sample_rate, format, mix_channels, freq);
            int original_len = samples * ogg_channels * sizeof(short);

            // Если формат отличается (например, OGG моно, а микшер стерео), конвертируем аудио "на лету"
            if (build_ret == 1) {
                cvt.len = original_len;
                cvt.buf = safeMalloc(cvt.len * cvt.len_mult);
                memcpy(cvt.buf, decodedPcm, cvt.len);
                SDL_ConvertAudio(&cvt);

                sys->chunks[soundIndex] = Mix_QuickLoad_RAW(cvt.buf, cvt.len_cvt);
                sys->decodedSfxBufs[soundIndex] = cvt.buf; // Сохраняем конвертированный буфер
                free(decodedPcm); // Исходный моно-сигнал больше не нужен
            } else {
                // Если звук уже соответствует настройкам микшера
                sys->chunks[soundIndex] = Mix_QuickLoad_RAW((uint8_t*)decodedPcm, original_len);
                sys->decodedSfxBufs[soundIndex] = decodedPcm;
            }
        }
    } else {
        SDL_RWops* rw = SDL_RWFromConstMem(rawBuf, entry->dataSize);
        sys->chunks[soundIndex] = Mix_LoadWAV_RW(rw, 1);
    }

    free(rawBuf);

    if (!sys->chunks[soundIndex]) {
        fprintf(stderr, "Audio Error: Failed to load SFX '%s'\n", sys->base.dataWin->sond.sounds[soundIndex].name);
        return false;
    }
    return true;
}

static bool ensureSoundLoaded(SdlMixerAudioSystem* sys, int32_t soundIndex) {
    if (!use_mixer) return true;
    if (soundIndex < 0 || (uint32_t)soundIndex >= sys->base.dataWin->sond.count) return false;

    sys->chunkLastUsed[soundIndex] = sys->audioFrameCounter;
    if (sys->chunks[soundIndex] || sys->music[soundIndex]) return true;

    Sound* sound = &sys->base.dataWin->sond.sounds[soundIndex];
    bool isEmbedded = (sound->flags & 0x01) != 0;

    if (isEmbedded) {
        AudioEntry* entry = &sys->base.dataWin->audo.entries[sound->audioFile];
        if (entry->dataSize == 0) return false;

        // Здесь файл распределяется: в BGM или в SFX
        bool isMusic = (entry->dataSize > STREAMING_SIZE_THRESHOLD);

        if (isMusic) {
            SDL_RWops* rw = createMusicRWops(sys->archivePath, entry->dataOffset, entry->dataSize);
            if (rw) sys->music[soundIndex] = Mix_LoadMUS_RW(rw);
        } else {
            loadSfxIntoRAM(sys, soundIndex, entry);
        }
    } else {
        char* path = resolveExternalPath(sys, sound);
        if (path) {
            if (strstr(path, ".wav")) sys->chunks[soundIndex] = Mix_LoadWAV(path);
            else sys->music[soundIndex] = Mix_LoadMUS(path);
            free(path);
        }
    }

    return (sys->chunks[soundIndex] || sys->music[soundIndex]);
}

// ============================================================================
// [3] VTABLE IMPLEMENTATIONS
// ============================================================================

static void sdlmInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    sys->base.dataWin = dataWin;
    sys->fileSystem = fileSystem;

    // Буфер 4096 samples @ 44100 Hz = ~93 мс латентности. Раньше стояло 2048
    // (~46 мс): звук успевал начаться ДО того, как кадр успел свопнуться на
    // экран (при 30 fps сам кадр рендерится ~25-33 мс), из-за чего в битве
    // с Азгором sfx опережали анимацию. 4096 ещё и переживает редкие просадки
    // главного потока без статтеров в колбэке.
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        fprintf(stderr, "Audio: Failed to init SDL_mixer: %s\n", Mix_GetError());
        use_mixer = false;
    }

    if (use_mixer) {
        Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
        Mix_AllocateChannels(MAX_MIXER_CHANNELS);

        uint32_t soundCount = dataWin->sond.count;
        sys->chunks = safeCalloc(soundCount, sizeof(Mix_Chunk*));
        sys->music = safeCalloc(soundCount, sizeof(Mix_Music*));
        sys->decodedSfxBufs = safeCalloc(soundCount, sizeof(void*));
        sys->chunkLastUsed = safeCalloc(soundCount, sizeof(uint32_t));

        sys->currentMusicSoundIndex = -1;
        sys->audioFrameCounter = 1;

        sys->archivePath = fileSystem->vtable->resolvePath(fileSystem, "data.win");
        if (!sys->archivePath) {
            sys->archivePath = fileSystem->vtable->resolvePath(fileSystem, "game.unx");
        }
    }
}

static void sdlmDestroy(AudioSystem* audio) {
    if (use_mixer) {
        SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

        Mix_HaltChannel(-1);
        Mix_HaltMusic();

        uint32_t soundCount = sys->base.dataWin->sond.count;
        for (uint32_t i = 0; i < soundCount; i++) {
            if (sys->chunks[i]) Mix_FreeChunk(sys->chunks[i]);
            if (sys->music[i]) Mix_FreeMusic(sys->music[i]);
            if (sys->decodedSfxBufs[i]) free(sys->decodedSfxBufs[i]);
        }

        free(sys->chunks);
        free(sys->music);
        free(sys->decodedSfxBufs);
        free(sys->chunkLastUsed);
        if (sys->archivePath) free(sys->archivePath);

        Mix_CloseAudio();
        Mix_Quit();
        free(sys);
    }
}

static void sdlmUpdate(AudioSystem* audio, float deltaTime) {
    if (use_mixer) {
        ((SdlMixerAudioSystem*)audio)->audioFrameCounter++;
        (void)deltaTime;
    }
}

static int32_t sdlmPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    if (!use_mixer) return 0;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (!ensureSoundLoaded(sys, soundIndex)) return -1;

    Sound* sound = &sys->base.dataWin->sond.sounds[soundIndex];
    int volume = (int)(sound->volume * MIX_MAX_VOLUME);

    if (sys->music[soundIndex] != NULL) {
        Mix_VolumeMusic(volume);
        if (Mix_PlayMusic(sys->music[soundIndex], loop ? -1 : 0) == -1) {
            fprintf(stderr, "Audio Error: Mix_PlayMusic failed: %s\n", Mix_GetError());
        }
        sys->currentMusicSoundIndex = soundIndex;
        return MUSIC_INSTANCE_ID_BASE;
    }

    if (sys->chunks[soundIndex] != NULL) {
        int channel = Mix_PlayChannel(-1, sys->chunks[soundIndex], loop ? -1 : 0);
        if (channel >= 0) {
            Mix_Volume(channel, volume);
            return SOUND_INSTANCE_ID_BASE + channel;
        }
    }
    return -1;
}

static void sdlmStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_HaltMusic();
        sys->currentMusicSoundIndex = -1;
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_HaltChannel(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) {
            Mix_HaltMusic();
            sys->currentMusicSoundIndex = -1;
        }
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_HaltChannel(i);
            }
        }
    }
}

static void sdlmStopAll(AudioSystem* audio) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    sys->currentMusicSoundIndex = -1;
}

static bool sdlmIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return false;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        return Mix_PlayingMusic() != 0;
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) return Mix_Playing(channel) != 0;
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance && Mix_PlayingMusic()) return true;
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_Playing(i) && Mix_GetChunk(i) == targetChunk) return true;
            }
        }
    }
    return false;
}

static void sdlmPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) Mix_PauseMusic();
    else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Pause(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_PauseMusic();
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_Pause(i);
            }
        }
    }
}

static void sdlmResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) Mix_ResumeMusic();
    else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Resume(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_ResumeMusic();
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_Resume(i);
            }
        }
    }
}

static void sdlmPauseAll(AudioSystem* audio) {
    (void)audio;
    if (use_mixer) { Mix_Pause(-1); Mix_PauseMusic(); }
}

static void sdlmResumeAll(AudioSystem* audio) {
    (void)audio;
    if (use_mixer) { Mix_Resume(-1); Mix_ResumeMusic(); }
}

static void sdlmSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    if (!use_mixer) return;
    (void)timeMs;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    int vol = (int)(gain * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) Mix_VolumeMusic(vol);
    else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Volume(channel, vol);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_VolumeMusic(vol);
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_Volume(i, vol);
            }
        }
    }
}

static float sdlmGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return 0.0f;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE || sys->currentMusicSoundIndex == soundOrInstance) {
        return (float)Mix_VolumeMusic(-1) / (float)MIX_MAX_VOLUME;
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) return (float)Mix_Volume(channel, -1) / (float)MIX_MAX_VOLUME;
    }
    return 0.0f;
}

static void sdlmSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) { (void)audio; (void)soundOrInstance; (void)pitch; }
static float sdlmGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) { (void)audio; (void)soundOrInstance; return 1.0f; }
static float sdlmGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) { (void)audio; (void)soundOrInstance; return 0.0f; }

static void sdlmSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE || sys->currentMusicSoundIndex == soundOrInstance) {
        Mix_SetMusicPosition((double)positionSeconds);
    }
}

static void sdlmSetMasterGain(AudioSystem* audio, float gain) {
    if (!use_mixer) return;
    (void)audio;
    int vol = (int)(gain * MIX_MAX_VOLUME);
    Mix_Volume(-1, vol);
    Mix_VolumeMusic(vol);
}

static void sdlmSetChannelCount(AudioSystem* audio, int32_t count) {
    if (!use_mixer) return;
    (void)audio;
    Mix_AllocateChannels(count);
}

static void sdlmGroupLoad(AudioSystem* audio, int32_t groupIndex) { (void)audio; (void)groupIndex; }
static bool sdlmGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) { (void)audio; (void)groupIndex; return true; }

static AudioSystemVtable sdlmAudioSystemVtable = {
    .init = sdlmInit, .destroy = sdlmDestroy, .update = sdlmUpdate,
    .playSound = sdlmPlaySound, .stopSound = sdlmStopSound, .stopAll = sdlmStopAll,
    .isPlaying = sdlmIsPlaying, .pauseSound = sdlmPauseSound, .resumeSound = sdlmResumeSound,
    .pauseAll = sdlmPauseAll, .resumeAll = sdlmResumeAll, .setSoundGain = sdlmSetSoundGain,
    .getSoundGain = sdlmGetSoundGain, .setSoundPitch = sdlmSetSoundPitch, .getSoundPitch = sdlmGetSoundPitch,
    .getTrackPosition = sdlmGetTrackPosition, .setTrackPosition = sdlmSetTrackPosition,
    .setMasterGain = sdlmSetMasterGain, .setChannelCount = sdlmSetChannelCount,
    .groupLoad = sdlmGroupLoad, .groupIsLoaded = sdlmGroupIsLoaded,
};

AudioSystem* SdlMixerAudioSystem_create(void) {
    SdlMixerAudioSystem* sys = safeCalloc(1, sizeof(SdlMixerAudioSystem));
    sys->base.vtable = &sdlmAudioSystemVtable;
    return (AudioSystem*) sys;
}