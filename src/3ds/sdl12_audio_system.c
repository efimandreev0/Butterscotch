#include "sdl12_audio_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <malloc.h>
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#include "../vorbis/stb_vorbis.c"

static bool use_mixer = true;

#define MAX_CHANS 32
#define STREAM_THRES (512 * 1024)
#define MAX_CACHE 64

//evict oldest shit if ram gets full
static void evict_old(SysMixer *sm) {
    int cnt = 0;
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (sm->chunks[i]) cnt++;
    }
    if (cnt < MAX_CACHE) return;

    int victim = -1;
    uint32_t oldest = 0xFFFFFFFF;

    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (!sm->chunks[i]) continue;
        bool playing = false;

        for (int c = 0; c < MAX_CHANS; c++) {
            if (Mix_Playing(c) && Mix_GetChunk(c) == sm->chunks[i]) {
                playing = true;
                break;
            }
        }

        if (!playing && sm->lastUsed[i] < oldest) {
            oldest = sm->lastUsed[i];
            victim = i;
        }
    }

    if (victim >= 0) {
        Mix_FreeChunk(sm->chunks[victim]);
        sm->chunks[victim] = NULL;
        if (sm->sfxBuf[victim]) {
            linearFree(sm->sfxBuf[victim]);
            sm->sfxBuf[victim] = NULL;
        }
    }
}

static bool load_sfx(SysMixer *sm, int id, AudioEntry *ent) {
    evict_old(sm);
    uint8_t *raw = linearAlloc(ent->dataSize);
    if (!raw) return false;

    FILE *fp = fopen(sm->archivePath, "rb");
    if (!fp) {
        linearFree(raw);
        return false;
    }
    fseek(fp, ent->dataOffset, SEEK_SET);
    fread(raw, 1, ent->dataSize, fp);
    fclose(fp);

    if (ent->dataSize > 4 && !memcmp(raw, "OggS", 4)) {
        int chans, srate;
        short *pcm;
        int smp = stb_vorbis_decode_memory(raw, ent->dataSize, &chans, &srate, &pcm);

        if (smp > 0) {
            int f, mc;
            Uint16 fmt;
            Mix_QuerySpec(&f, &fmt, &mc);

            SDL_AudioCVT cvt;
            int len = smp * chans * sizeof(short);
            if (SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, chans, srate, fmt, mc, f) == 1) {
                cvt.len = len;
                cvt.buf = linearAlloc(cvt.len * cvt.len_mult);
                memcpy(cvt.buf, pcm, cvt.len);
                SDL_ConvertAudio(&cvt);

                sm->chunks[id] = Mix_QuickLoad_RAW(cvt.buf, cvt.len_cvt);
                sm->sfxBuf[id] = cvt.buf;
            } else {
                uint8_t *lp = linearAlloc(len);
                memcpy(lp, pcm, len);
                sm->chunks[id] = Mix_QuickLoad_RAW(lp, len);
                sm->sfxBuf[id] = lp;
            }
            free(pcm);
        }
    } else {
        SDL_RWops *rw = SDL_RWFromConstMem(raw, ent->dataSize);
        sm->chunks[id] = Mix_LoadWAV_RW(rw, 1);
    }

    linearFree(raw);
    return sm->chunks[id] != NULL;
}

//stahp the music
static void stop_other_music(SysMixer *sm, int keepId) {
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (i != keepId && sm->music[i]) {
            if (sm->curMusicId == i) {
                Mix_HaltMusic();
                sm->curMusicId = -1;
            }
            Mix_FreeMusic(sm->music[i]);
            sm->music[i] = NULL;
            if (sm->musicBuf[i]) {
                free(sm->musicBuf[i]);
                sm->musicBuf[i] = NULL;
            }
        }
    }
}

static bool ensure_snd(SysMixer *sm, int id) {
    if (!use_mixer || id < 0 || id >= sm->base.dataWin->sond.count) return false;
    sm->lastUsed[id] = sm->frame;
    if (sm->chunks[id] || sm->music[id]) return true;

    Sound *snd = &sm->base.dataWin->sond.sounds[id];

    if (snd->flags & 1) {
        AudioEntry *ent = &sm->base.dataWin->audo.entries[snd->audioFile];
        if (!ent->dataSize) return false;

        if (ent->dataSize > STREAM_THRES) {
            stop_other_music(sm, id);

            uint8_t *mb = malloc(ent->dataSize);
            FILE *fp = fopen(sm->archivePath, "rb");
            if (fp) {
                fseek(fp, ent->dataOffset, SEEK_SET);
                fread(mb, 1, ent->dataSize, fp);
                fclose(fp);

                SDL_RWops *rw = SDL_RWFromConstMem(mb, ent->dataSize);
                sm->music[id] = Mix_LoadMUS_RW(rw);
                if (sm->music[id]) sm->musicBuf[id] = mb;
                else free(mb);
            } else free(mb);
        } else {
            load_sfx(sm, id, ent);
        }
    } else {
        if (!snd->file || !snd->file[0]) return false;
        char name[512];
        snprintf(name, sizeof(name), "%s%s", snd->file, strchr(snd->file, '.') ? "" : ".ogg");
        char *path = sm->fs->vtable->resolvePath(sm->fs, name);

        if (path) {
            if (strstr(path, ".wav")) sm->chunks[id] = Mix_LoadWAV(path);
            else {
                stop_other_music(sm, id);
                sm->music[id] = Mix_LoadMUS(path);
            }
            free(path);
        }
    }
    return sm->chunks[id] || sm->music[id];
}

static void sys_init(AudioSystem *sys, DataWin *dw, FileSystem *fs) {
    SysMixer *sm = (SysMixer *) sys;
    sm->base.dataWin = dw;
    sm->fs = fs;

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        fprintf(stderr, "SDL_mixer init failed: %s\n", Mix_GetError());
        use_mixer = false;
    }

    if (use_mixer) {
        Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
        Mix_AllocateChannels(MAX_CHANS);

        int cnt = dw->sond.count;
        sm->chunks = calloc(cnt, sizeof(Mix_Chunk *));
        sm->music = calloc(cnt, sizeof(Mix_Music *));
        sm->musicBuf = calloc(cnt, sizeof(uint8_t *));
        sm->sfxBuf = calloc(cnt, sizeof(void *));
        sm->lastUsed = calloc(cnt, sizeof(uint32_t));

        sm->curMusicId = -1;
        sm->frame = 1;

        sm->archivePath = fs->vtable->resolvePath(fs, "data.win");
        if (!sm->archivePath) sm->archivePath = fs->vtable->resolvePath(fs, "game.unx");
    }
}

static void sys_destroy(AudioSystem *sys) {
    if (!use_mixer) return;
    SysMixer *sm = (SysMixer *) sys;

    Mix_HaltChannel(-1);
    Mix_HaltMusic();

    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (sm->chunks[i]) Mix_FreeChunk(sm->chunks[i]);
        if (sm->music[i]) Mix_FreeMusic(sm->music[i]);
        if (sm->musicBuf[i]) free(sm->musicBuf[i]);
        if (sm->sfxBuf[i]) linearFree(sm->sfxBuf[i]);
    }

    free(sm->chunks);
    free(sm->music);
    free(sm->musicBuf);
    free(sm->sfxBuf);
    free(sm->lastUsed);
    if (sm->archivePath) free(sm->archivePath);

    Mix_CloseAudio();
    Mix_Quit();
    free(sm);
}

static void sys_update(AudioSystem *sys, float dt) {
    if (use_mixer) ((SysMixer *) sys)->frame++;
}

static int32_t sys_play(AudioSystem *sys, int32_t id, int32_t prio, bool loop) {
    if (!use_mixer) return 0;
    SysMixer *sm = (SysMixer *) sys;
    if (!ensure_snd(sm, id)) return -1;

    int vol = sm->base.dataWin->sond.sounds[id].volume * MIX_MAX_VOLUME;

    if (sm->music[id]) {
        Mix_VolumeMusic(vol);
        Mix_PlayMusic(sm->music[id], loop ? -1 : 0);
        sm->curMusicId = id;
        return MUS_ID_BASE;
    }

    if (sm->chunks[id]) {
        int ch = Mix_PlayChannel(-1, sm->chunks[id], loop ? -1 : 0);
        if (ch >= 0) {
            Mix_Volume(ch, vol);
            return SND_ID_BASE + ch;
        }
    }
    return -1;
}

static void sys_stop(AudioSystem *sys, int32_t id) {
    if (!use_mixer) return;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) {
        Mix_HaltMusic();
        sm->curMusicId = -1;
    } else if (id >= SND_ID_BASE) {
        Mix_HaltChannel(id - SND_ID_BASE);
    } else if (sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) {
            if (Mix_GetChunk(i) == sm->chunks[id]) Mix_HaltChannel(i);
        }
    }
}

static void sys_stop_all(AudioSystem *sys) {
    if (!use_mixer) return;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    ((SysMixer *) sys)->curMusicId = -1;
}

static bool sys_is_playing(AudioSystem *sys, int32_t id) {
    if (!use_mixer) return false;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || (sm->curMusicId == id && Mix_PlayingMusic())) return Mix_PlayingMusic();
    if (id >= SND_ID_BASE) return Mix_Playing(id - SND_ID_BASE);

    if (sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) {
            if (Mix_Playing(i) && Mix_GetChunk(i) == sm->chunks[id]) return true;
        }
    }
    return false;
}

static void sys_pause(AudioSystem *sys, int32_t id) {
    if (!use_mixer) return;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_PauseMusic();
    else if (id >= SND_ID_BASE) Mix_Pause(id - SND_ID_BASE);
    else if (sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Pause(i);
    }
}

static void sys_resume(AudioSystem *sys, int32_t id) {
    if (!use_mixer) return;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_ResumeMusic();
    else if (id >= SND_ID_BASE) Mix_Resume(id - SND_ID_BASE);
    else if (sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Resume(i);
    }
}

static void sys_pause_all(AudioSystem *sys) {
    if (use_mixer) {
        Mix_Pause(-1);
        Mix_PauseMusic();
    }
}

static void sys_resume_all(AudioSystem *sys) {
    if (use_mixer) {
        Mix_Resume(-1);
        Mix_ResumeMusic();
    }
}

static void sys_set_gain(AudioSystem *sys, int32_t id, float g, uint32_t t) {
    if (!use_mixer) return;
    SysMixer *sm = (SysMixer *) sys;
    int vol = fminf(g * MIX_MAX_VOLUME, MIX_MAX_VOLUME);

    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_VolumeMusic(vol);
    else if (id >= SND_ID_BASE) Mix_Volume(id - SND_ID_BASE, vol);
    else if (sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Volume(i, vol);
    }
}

static float sys_get_gain(AudioSystem *sys, int32_t id) {
    if (!use_mixer) return 0.f;
    SysMixer *sm = (SysMixer *) sys;
    if (id == MUS_ID_BASE || sm->curMusicId == id) return (float) Mix_VolumeMusic(-1) / MIX_MAX_VOLUME;
    if (id >= SND_ID_BASE) return (float) Mix_Volume(id - SND_ID_BASE, -1) / MIX_MAX_VOLUME;
    return 0.f;
}

//stubs
static void sys_set_pitch(AudioSystem *sys, int32_t id, float p) {
}

static float sys_get_pitch(AudioSystem *sys, int32_t id) { return 1.f; }
static float sys_get_pos(AudioSystem *sys, int32_t id) { return 0.f; }

static void sys_set_pos(AudioSystem *sys, int32_t id, float pos) {
    if (use_mixer && (id == MUS_ID_BASE || ((SysMixer *) sys)->curMusicId == id)) Mix_SetMusicPosition(pos);
}

static void sys_set_master(AudioSystem *sys, float g) {
    if (use_mixer) {
        int v = g * MIX_MAX_VOLUME;
        Mix_Volume(-1, v);
        Mix_VolumeMusic(v);
    }
}

static void sys_set_chans(AudioSystem *sys, int32_t cnt) { if (use_mixer) Mix_AllocateChannels(cnt); }

static void sys_grp_load(AudioSystem *sys, int32_t grp) {
}

static bool sys_grp_loaded(AudioSystem *sys, int32_t grp) { return true; }

static AudioSystemVtable vtable = {
    .init = sys_init, .destroy = sys_destroy, .update = sys_update,
    .playSound = sys_play, .stopSound = sys_stop, .stopAll = sys_stop_all,
    .isPlaying = sys_is_playing, .pauseSound = sys_pause, .resumeSound = sys_resume,
    .pauseAll = sys_pause_all, .resumeAll = sys_resume_all, .setSoundGain = sys_set_gain,
    .getSoundGain = sys_get_gain, .setSoundPitch = sys_set_pitch, .getSoundPitch = sys_get_pitch,
    .getTrackPosition = sys_get_pos, .setTrackPosition = sys_set_pos,
    .setMasterGain = sys_set_master, .setChannelCount = sys_set_chans,
    .groupLoad = sys_grp_load, .groupIsLoaded = sys_grp_loaded,
};

AudioSystem *SdlMixerAudioSystem_create(void) {
    SysMixer *sm = calloc(1, sizeof(SysMixer));
    sm->base.vtable = &vtable;
    return (AudioSystem *) sm;
}
