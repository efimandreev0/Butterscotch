#include "sdl12_audio_system.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <malloc.h>
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>

#include "stb_ds.h"

// ===[ Globals ]===
// SDL_mixer + SDL audio subsystem are initialised once per app lifetime. Tear
// down is deferred to SdlMixer_globalShutdown at exit because Mix_OpenAudio /
// Mix_CloseAudio cycles inside the same process are not reliable on 3DS.
static bool s_mixerReady = false;

bool SdlMixer_globalInit(void) {
    if (s_mixerReady) return true;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "[AUDIO] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
    }
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        fprintf(stderr, "[AUDIO] Mix_OpenAudio failed: %s\n", Mix_GetError());
        return false;
    }
    Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    s_mixerReady = true;
    fprintf(stderr, "[AUDIO] SDL_mixer ready (44100/stereo/4096)\n");
    return true;
}

void SdlMixer_globalShutdown(void) {
    if (!s_mixerReady) return;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    Mix_CloseAudio();
    Mix_Quit();
    s_mixerReady = false;
}

#define STREAM_THRES (512 * 1024)
#define MAX_CACHE 64
#define MAX_MUSIC_CACHE 4
#define MUSIC_SILENCE_EPS 0.001f
#define UNDERTALE_DOMINANT_EPS 0.02f
#define UNDERTALE_RECENT_FADE_FRAMES 180u

// ===[ Helpers ]===

static float clamp_gain(float g) {
    if (g < 0.f) return 0.f;
    if (g > 8.f) return 8.f;
    return g;
}

static int gain_to_volume(float g) {
    int vol = (int) (clamp_gain(g) * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
    if (vol < 0) vol = 0;
    return vol;
}

static void tween_begin(SysMixerGainTween *tw, float current, float target, uint32_t timeMs) {
    tw->current = clamp_gain(current);
    tw->start = tw->current;
    tw->target = clamp_gain(target);
    tw->elapsed = 0.f;
    tw->duration = (float) timeMs / 1000.f;
    tw->active = timeMs > 0 && tw->duration > 0.f && tw->current != tw->target;
    if (!tw->active) tw->current = tw->target;
}

static bool tween_step(SysMixerGainTween *tw, float dt) {
    if (!tw->active) return false;
    tw->elapsed += dt;
    if (tw->elapsed >= tw->duration) {
        tw->current = tw->target;
        tw->active = false;
        return true;
    }
    float t = tw->elapsed / tw->duration;
    tw->current = tw->start + (tw->target - tw->start) * t;
    return true;
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t needleLen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needleLen && p[i]) {
            unsigned char a = (unsigned char) p[i];
            unsigned char b = (unsigned char) needle[i];
            if (tolower(a) != tolower(b)) break;
            i++;
        }
        if (i == needleLen) return true;
    }
    return false;
}

static SysMixerGameMode detect_game_mode(DataWin *dw) {
    const char *name = dw ? dw->gen8.name : NULL;
    const char *display = dw ? dw->gen8.displayName : NULL;
    if (contains_ci(name, "undertale") || contains_ci(display, "undertale")) {
        return SYS_MIXER_GAME_UNDERTALE;
    }
    if (contains_ci(name, "deltarune") || contains_ci(display, "deltarune")) {
        return SYS_MIXER_GAME_DELTARUNE;
    }
    return SYS_MIXER_GAME_GENERIC;
}

static const char *game_mode_name(SysMixerGameMode mode) {
    switch (mode) {
        case SYS_MIXER_GAME_UNDERTALE: return "UNDERTALE";
        case SYS_MIXER_GAME_DELTARUNE: return "DELTARUNE";
        default: return "GENERIC";
    }
}

static float music_audible_epsilon(SysMixer *sm) {
    return sm->gameMode == SYS_MIXER_GAME_UNDERTALE ? UNDERTALE_DOMINANT_EPS : MUSIC_SILENCE_EPS;
}

static bool has_extension(const char *path) {
    if (!path) return false;
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash);
}

static bool file_exists_full(const char *path) {
    if (!path || !path[0]) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

static char *resolve_existing_path(SysMixer *sm, const char *relativePath) {
    if (!relativePath || !relativePath[0]) return NULL;
    if (sm->fs->vtable->fileExists && sm->fs->vtable->fileExists(sm->fs, relativePath)) {
        return sm->fs->vtable->resolvePath(sm->fs, relativePath);
    }

    char *path = sm->fs->vtable->resolvePath(sm->fs, relativePath);
    if (path && file_exists_full(path)) return path;
    free(path);
    return NULL;
}

static bool try_audio_candidate(SysMixer *sm, const char *candidate, char **out) {
    if (!candidate || !candidate[0]) return false;
    *out = resolve_existing_path(sm, candidate);
    if (*out) return true;

    if (!has_extension(candidate)) {
        char withExt[768];
        snprintf(withExt, sizeof(withExt), "%s.ogg", candidate);
        *out = resolve_existing_path(sm, withExt);
        if (*out) return true;
    }
    return false;
}

static char *resolve_audio_path(SysMixer *sm, const char *filename) {
    if (!filename || !filename[0]) return NULL;

    char *path = NULL;
    if (try_audio_candidate(sm, filename, &path)) return path;

    if (strncmp(filename, "mus/", 4) == 0) {
        char parent[768];
        snprintf(parent, sizeof(parent), "../%s", filename);
        if (try_audio_candidate(sm, parent, &path)) return path;
    } else if (strncmp(filename, "../mus/", 7) != 0 && strchr(filename, '/') == NULL) {
        char mus[768];
        snprintf(mus, sizeof(mus), "mus/%s", filename);
        if (try_audio_candidate(sm, mus, &path)) return path;

        snprintf(mus, sizeof(mus), "../mus/%s", filename);
        if (try_audio_candidate(sm, mus, &path)) return path;
    }

    return NULL;
}

static bool sound_name_starts(const Sound *snd, const char *prefix) {
    const char *name = snd && snd->name ? snd->name : "";
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static bool sound_file_starts(const Sound *snd, const char *prefix) {
    const char *file = snd && snd->file ? snd->file : "";
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    return strncmp(base, prefix, strlen(prefix)) == 0;
}

static bool sound_name_equals(const Sound *snd, const char *name) {
    const char *soundName = snd && snd->name ? snd->name : "";
    return strcmp(soundName, name) == 0;
}

static bool sound_file_base_equals(const Sound *snd, const char *name) {
    const char *file = snd && snd->file ? snd->file : "";
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    size_t len = strlen(name);
    if (strncmp(base, name, len) != 0) return false;
    return base[len] == '\0' || base[len] == '.';
}

static bool sound_named_or_file_base_equals(const Sound *snd, const char *name) {
    return sound_name_equals(snd, name) || sound_file_base_equals(snd, name);
}

static bool sound_id_matches(SysMixer *sm, int32_t soundId, const char *name) {
    if (!sm || soundId < 0 || (uint32_t) soundId >= sm->base.dataWin->sond.count) return false;
    return sound_named_or_file_base_equals(&sm->base.dataWin->sond.sounds[soundId], name);
}

static bool undertale_sound_is_one_shot_music_effect(const Sound *snd) {
    // Undertale stores some very short SFX under music/*.ogg. If these use the
    // music channel they interrupt the current battle/map track.
    return sound_named_or_file_base_equals(snd, "mus_explosion") ||
           sound_named_or_file_base_equals(snd, "explosion") ||
           sound_named_or_file_base_equals(snd, "mus_create") ||
           sound_named_or_file_base_equals(snd, "create") ||
           sound_named_or_file_base_equals(snd, "mus_chime") ||
           sound_named_or_file_base_equals(snd, "chime") ||
           sound_named_or_file_base_equals(snd, "mus_cymbal") ||
           sound_named_or_file_base_equals(snd, "cymbal") ||
           sound_named_or_file_base_equals(snd, "mus_rimshot") ||
           sound_named_or_file_base_equals(snd, "rimshot") ||
           sound_named_or_file_base_equals(snd, "mus_sticksnap") ||
           sound_named_or_file_base_equals(snd, "sticksnap");
}

static bool sound_is_music_track(const Sound *snd, uint32_t dataSize) {
    // Undertale labels many one-shot effects as mus_sfx_*. Treat those as SFX
    // so they never steal SDL_mixer's single music channel.
    if (sound_name_starts(snd, "snd_") || sound_file_starts(snd, "snd_")) return false;
    if (sound_name_starts(snd, "mus_sfx") || sound_file_starts(snd, "mus_sfx")) return false;
    if (undertale_sound_is_one_shot_music_effect(snd)) return false;
    if (sound_name_starts(snd, "mus_") || sound_file_starts(snd, "mus_")) return true;
    if (sound_name_starts(snd, "bgm_") || sound_file_starts(snd, "bgm_")) return true;
    if (sound_name_starts(snd, "AUDIO_") || sound_file_starts(snd, "AUDIO_")) return true;
    return dataSize != 0xFFFFFFFFu && dataSize > STREAM_THRES;
}

static int stream_slot_from_id(SysMixer *sm, int32_t id) {
    if (id < STREAM_ID_BASE) return -1;
    int slot = id - STREAM_ID_BASE;
    if (slot < 0 || (uint32_t) slot >= sm->streamCapacity) return -1;
    if (!sm->streams[slot].active) return -1;
    return slot;
}

static int music_slot_from_handle(SysMixer *sm, int32_t id) {
    if (id <= MUS_ID_BASE || id >= STREAM_ID_BASE) return -1;
    int slot = id - MUS_ID_BASE - 1;
    if (slot < 0 || slot >= MAX_MUSIC_HANDLES) return -1;
    if (!sm->musicHandles[slot].active) return -1;
    return slot;
}

static int32_t music_handle_id(int slot) {
    return MUS_ID_BASE + 1 + slot;
}

static int alloc_music_handle(SysMixer *sm, int32_t soundId, float gain, bool loop) {
    int slot = -1;
    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        if (!sm->musicHandles[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
            uint32_t candidate = (sm->nextMusicHandleSlot + (uint32_t) i) % MAX_MUSIC_HANDLES;
            if (music_handle_id((int) candidate) != sm->curMusicHandle) {
                slot = (int) candidate;
                break;
            }
        }
    }
    if (slot < 0) slot = 0;

    sm->nextMusicHandleSlot = ((uint32_t) slot + 1u) % MAX_MUSIC_HANDLES;
    sm->musicHandles[slot] = (SysMixerMusicHandle) {
        .active = true,
        .playing = false,
        .paused = false,
        .loop = loop,
        .soundId = soundId,
        .gain = gain,
        .lastAudibleGain = gain > music_audible_epsilon(sm) ? gain : 1.f,
        .pitch = (soundId >= 0 && (uint32_t) soundId < sm->base.dataWin->sond.count && sm->pitches) ? sm->pitches[soundId] : 1.f,
        .createdFrame = sm->frame,
        .displacedFrame = sm->frame,
    };
    return slot;
}

static void free_stream(SysMixer *sm, uint32_t slot) {
    if (slot >= sm->streamCapacity) return;
    if (sm->curMusicId == (int32_t) (STREAM_ID_BASE + slot)) {
        Mix_HaltMusic();
        sm->curMusicId = -1;
    }
    if (sm->streams[slot].music) Mix_FreeMusic(sm->streams[slot].music);
    free(sm->streams[slot].path);
    memset(&sm->streams[slot], 0, sizeof(sm->streams[slot]));
}

static float current_music_base_volume(SysMixer *sm) {
    if (sm->curMusicId >= 0 && sm->curMusicId < (int32_t) sm->base.dataWin->sond.count) {
        return sm->base.dataWin->sond.sounds[sm->curMusicId].volume;
    }
    return 1.f;
}

static void apply_music_gain(SysMixer *sm) {
    Mix_VolumeMusic(gain_to_volume(sm->musicGain * current_music_base_volume(sm) * sm->masterGain));
}

static void apply_channel_gain(SysMixer *sm, int ch) {
    if (ch < 0 || ch >= MAX_CHANS) return;
    float base = 1.f;
    int32_t id = sm->channelSound[ch];
    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) {
        base = sm->base.dataWin->sond.sounds[id].volume;
    }
    Mix_Volume(ch, gain_to_volume(sm->channelGains[ch] * base * sm->masterGain));
}

static bool ensure_snd(SysMixer *sm, int id);

static void restart_current_music_if_silent(SysMixer *sm) {
    if (sm->curMusicId < 0 || Mix_PlayingMusic()) return;

    bool loop = true;
    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    if (currentSlot >= 0) loop = sm->musicHandles[currentSlot].loop;

    int streamSlot = stream_slot_from_id(sm, sm->curMusicId);
    if (streamSlot >= 0) {
        if (sm->streams[streamSlot].music) {
            apply_music_gain(sm);
            Mix_ResumeMusic();
            if (Mix_PlayMusic(sm->streams[streamSlot].music, loop ? -1 : 0) == 0 && currentSlot >= 0) {
                sm->musicHandles[currentSlot].playing = true;
                sm->musicHandles[currentSlot].paused = false;
            }
        }
        return;
    }

    if ((uint32_t) sm->curMusicId < sm->base.dataWin->sond.count && sm->music[sm->curMusicId]) {
        apply_music_gain(sm);
        Mix_ResumeMusic();
        if (Mix_PlayMusic(sm->music[sm->curMusicId], loop ? -1 : 0) == 0 && currentSlot >= 0) {
            sm->musicHandles[currentSlot].playing = true;
            sm->musicHandles[currentSlot].paused = false;
        }
    }
}

static bool play_music_for_handle(SysMixer *sm, int slot, float startGain, uint32_t fadeMs) {
    if (slot < 0 || slot >= MAX_MUSIC_HANDLES || !sm->musicHandles[slot].active) return false;

    SysMixerMusicHandle *mh = &sm->musicHandles[slot];
    int32_t soundId = mh->soundId;
    int streamSlot = stream_slot_from_id(sm, soundId);

    if (streamSlot >= 0) {
        if (!sm->streams[streamSlot].music) {
            sm->streams[streamSlot].music = Mix_LoadMUS(sm->streams[streamSlot].path);
            if (!sm->streams[streamSlot].music) {
                fprintf(stderr, "[AUDIO] Mix_LoadMUS lazy stream failed '%s': %s\n",
                        sm->streams[streamSlot].path ? sm->streams[streamSlot].path : "?", Mix_GetError());
                return false;
            }
        }
    } else {
        if (!ensure_snd(sm, soundId) || !sm->music[soundId]) return false;
    }

    if (sm->curMusicHandle > MUS_ID_BASE) {
        int oldSlot = music_slot_from_handle(sm, sm->curMusicHandle);
        if (oldSlot >= 0) {
            sm->musicHandles[oldSlot].playing = false;
            sm->musicHandles[oldSlot].displacedFrame = sm->frame;
            if (sm->musicHandles[oldSlot].gain > music_audible_epsilon(sm)) {
                sm->musicHandles[oldSlot].lastAudibleGain = sm->musicHandles[oldSlot].gain;
            }
        }
    }

    sm->curMusicId = soundId;
    sm->curMusicHandle = music_handle_id(slot);
    sm->musicGain = clamp_gain(startGain);
    tween_begin(&sm->musicTween, sm->musicGain, mh->gain, fadeMs);
    sm->musicGain = sm->musicTween.current;
    apply_music_gain(sm);

    Mix_Music *music = (streamSlot >= 0) ? sm->streams[streamSlot].music : sm->music[soundId];
    Mix_ResumeMusic();
    if (Mix_PlayMusic(music, mh->loop ? -1 : 0) < 0) {
        fprintf(stderr, "[AUDIO] Mix_PlayMusic failed for handle %ld sound %ld: %s\n",
                (long) sm->curMusicHandle, (long) soundId, Mix_GetError());
        mh->playing = false;
        sm->curMusicId = -1;
        sm->curMusicHandle = -1;
        return false;
    }

    Mix_ResumeMusic();
    mh->playing = true;
    mh->paused = false;
    mh->createdFrame = sm->frame;
    return true;
}

static bool undertale_music_can_auto_restore(SysMixer *sm, const SysMixerMusicHandle *mh) {
    if (!sm || !mh || sm->gameMode != SYS_MIXER_GAME_UNDERTALE) return true;
    if (sound_id_matches(sm, mh->soundId, "mus_gameover") ||
        sound_id_matches(sm, mh->soundId, "gameover") ||
        sound_id_matches(sm, mh->soundId, "mus_silence") ||
        sound_id_matches(sm, mh->soundId, "silence") ||
        sound_id_matches(sm, mh->soundId, "mus_f_newlaugh") ||
        sound_id_matches(sm, mh->soundId, "f_newlaugh") ||
        sound_id_matches(sm, mh->soundId, "mus_f_newlaugh_low") ||
        sound_id_matches(sm, mh->soundId, "f_newlaugh_low") ||
        sound_id_matches(sm, mh->soundId, "mus_f_part3") ||
        sound_id_matches(sm, mh->soundId, "f_part3")) {
        return false;
    }
    return true;
}

static bool undertale_sound_is_gameover(SysMixer *sm, int32_t soundId) {
    if (!sm || sm->gameMode != SYS_MIXER_GAME_UNDERTALE) return false;
    return sound_id_matches(sm, soundId, "mus_gameover") ||
           sound_id_matches(sm, soundId, "gameover");
}

static int32_t find_sound_id_matching(SysMixer *sm, const char *primary, const char *secondary) {
    if (!sm || !sm->base.dataWin) return -1;
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if ((primary && sound_id_matches(sm, (int32_t) i, primary)) ||
            (secondary && sound_id_matches(sm, (int32_t) i, secondary))) {
            return (int32_t) i;
        }
    }
    return -1;
}

static int32_t undertale_flowey_laugh_music_id(SysMixer *sm) {
    int32_t id = find_sound_id_matching(sm, "mus_f_part3", "f_part3");
    if (id >= 0) return id;
    id = find_sound_id_matching(sm, "mus_f_newlaugh_low", "f_newlaugh_low");
    if (id >= 0) return id;
    return find_sound_id_matching(sm, "mus_f_newlaugh", "f_newlaugh");
}

static bool switch_undertale_pitched_gameover_to_laugh_music(SysMixer *sm, int requestedSlot, float pitch) {
    if (!sm || sm->gameMode != SYS_MIXER_GAME_UNDERTALE || pitch < 2.0f) return false;
    if (requestedSlot < 0 || requestedSlot >= MAX_MUSIC_HANDLES) return false;
    if (!sm->musicHandles[requestedSlot].active ||
        !undertale_sound_is_gameover(sm, sm->musicHandles[requestedSlot].soundId)) {
        return false;
    }

    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    if (currentSlot < 0 || !sm->musicHandles[currentSlot].active ||
        !undertale_sound_is_gameover(sm, sm->musicHandles[currentSlot].soundId)) {
        return false;
    }

    int32_t laughMusic = undertale_flowey_laugh_music_id(sm);
    if (laughMusic < 0 || !ensure_snd(sm, laughMusic) || !sm->music[laughMusic]) return false;

    SysMixerMusicHandle *mh = &sm->musicHandles[currentSlot];
    mh->soundId = laughMusic;
    mh->pitch = 1.f;
    if (mh->gain <= music_audible_epsilon(sm)) mh->gain = 0.45f;
    if (mh->lastAudibleGain <= music_audible_epsilon(sm)) mh->lastAudibleGain = mh->gain;

    float startGain = sm->musicGain > music_audible_epsilon(sm) ? sm->musicGain : mh->gain;
    return play_music_for_handle(sm, currentSlot, startGain, 0);
}

static bool restore_previous_music(SysMixer *sm, int stoppedSlot) {
    int best = -1;
    uint32_t newest = 0;
    float bestGain = 0.f;
    float eps = music_audible_epsilon(sm);

    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        SysMixerMusicHandle *mh = &sm->musicHandles[i];
        if (i == stoppedSlot || !mh->active || mh->playing) continue;
        if (!mh->loop) continue;
        if (!undertale_music_can_auto_restore(sm, mh)) continue;
        float candidateGain = mh->gain;
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE && mh->paused && candidateGain <= eps) {
            candidateGain = mh->lastAudibleGain;
        }
        if (sm->gameMode != SYS_MIXER_GAME_UNDERTALE && mh->paused) continue;
        if (candidateGain <= eps) continue;
        uint32_t orderFrame = sm->gameMode == SYS_MIXER_GAME_UNDERTALE ? mh->displacedFrame : mh->createdFrame;
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            if (orderFrame < newest) continue;
            if (orderFrame == newest && candidateGain < bestGain) continue;
        } else if (orderFrame < newest) {
            continue;
        }
        if (orderFrame >= newest || candidateGain >= bestGain) {
            newest = orderFrame;
            bestGain = candidateGain;
            best = i;
        }
    }

    if (best < 0) return false;
    if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE && sm->musicHandles[best].gain <= eps) {
        sm->musicHandles[best].gain = sm->musicHandles[best].lastAudibleGain;
        int32_t soundId = sm->musicHandles[best].soundId;
        if (soundId >= 0 && (uint32_t) soundId < sm->base.dataWin->sond.count) {
            sm->soundGains[soundId] = sm->musicHandles[best].gain;
        }
    }
    sm->musicHandles[best].paused = false;

    // Restart the displaced music from the beginning. This is intentional:
    // SDL_mixer only has one music channel, so we cannot preserve the exact
    // paused position of an older map track after battle/event music replaces it.
    return play_music_for_handle(sm, best, 0.f, 250);
}

static bool restore_undertale_paused_music(SysMixer *sm, int stoppedSlot) {
    if (sm->gameMode != SYS_MIXER_GAME_UNDERTALE) return false;

    int best = -1;
    uint32_t newest = 0;
    float eps = music_audible_epsilon(sm);

    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        SysMixerMusicHandle *mh = &sm->musicHandles[i];
        if (i == stoppedSlot || !mh->active || mh->playing || !mh->loop) continue;
        if (!undertale_music_can_auto_restore(sm, mh)) continue;
        if (!mh->paused || mh->gain > eps || mh->lastAudibleGain <= eps) continue;
        if (mh->displacedFrame < newest) continue;
        newest = mh->displacedFrame;
        best = i;
    }

    if (best < 0) return false;
    sm->musicHandles[best].gain = sm->musicHandles[best].lastAudibleGain;
    int32_t soundId = sm->musicHandles[best].soundId;
    if (soundId >= 0 && (uint32_t) soundId < sm->base.dataWin->sond.count) {
        sm->soundGains[soundId] = sm->musicHandles[best].gain;
    }
    sm->musicHandles[best].paused = false;
    return play_music_for_handle(sm, best, 0.f, 250);
}

static bool current_music_audible(SysMixer *sm) {
    if (sm->curMusicId < 0 || !Mix_PlayingMusic()) return false;
    if (sm->musicGain <= music_audible_epsilon(sm)) return false;

    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    if (currentSlot >= 0) {
        SysMixerMusicHandle *mh = &sm->musicHandles[currentSlot];
        if (!mh->playing || mh->paused || mh->gain <= music_audible_epsilon(sm)) return false;
    }

    return true;
}

static void set_current_music_paused(SysMixer *sm, bool paused) {
    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    if (currentSlot < 0) return;
    sm->musicHandles[currentSlot].paused = paused;
    sm->musicHandles[currentSlot].playing = !paused;
}

static void mark_music_handles_for_sound_paused(SysMixer *sm, int32_t soundId, bool paused) {
    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        SysMixerMusicHandle *mh = &sm->musicHandles[i];
        if (!mh->active || mh->soundId != soundId) continue;
        mh->paused = paused;
        if (paused) mh->playing = false;
    }
}

static void set_music_handle_gains_for_sound(SysMixer *sm, int32_t soundId, float gain) {
    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        SysMixerMusicHandle *mh = &sm->musicHandles[i];
        if (!mh->active || mh->soundId != soundId) continue;
        mh->gain = gain;
        if (gain > music_audible_epsilon(sm)) mh->lastAudibleGain = gain;
    }
}

static int find_best_music_for_sound(SysMixer *sm, int32_t soundId, bool allowPaused) {
    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    int best = -1;
    float bestGain = 0.f;
    uint32_t newest = 0;
    float eps = music_audible_epsilon(sm);

    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        if (i == currentSlot) continue;
        SysMixerMusicHandle *mh = &sm->musicHandles[i];
        if (!mh->active || mh->soundId != soundId || !mh->loop) continue;
        if (!undertale_music_can_auto_restore(sm, mh)) continue;
        if (!allowPaused && mh->paused) continue;
        if (mh->gain <= eps) continue;
        if (mh->gain < bestGain) continue;
        if (mh->gain == bestGain && mh->createdFrame < newest) continue;
        bestGain = mh->gain;
        newest = mh->createdFrame;
        best = i;
    }

    return best;
}

static int find_recent_dominant_music(SysMixer *sm) {
    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    int best = -1;
    float bestGain = 0.f;
    uint32_t newest = 0;
    float eps = music_audible_epsilon(sm);

    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        if (i == currentSlot) continue;
        SysMixerMusicHandle *mh = &sm->musicHandles[i];
        if (!mh->active || mh->playing || mh->paused || !mh->loop) continue;
        if (!undertale_music_can_auto_restore(sm, mh)) continue;
        if (mh->gain <= eps) continue;
        if (sm->frame - mh->createdFrame > UNDERTALE_RECENT_FADE_FRAMES) continue;
        if (mh->gain < bestGain) continue;
        if (mh->gain == bestGain && mh->createdFrame < newest) continue;
        bestGain = mh->gain;
        newest = mh->createdFrame;
        best = i;
    }

    return best;
}

static bool undertale_play_requested_music(SysMixer *sm, int32_t soundId, uint32_t fadeMs, bool allowPaused) {
    if (sm->gameMode != SYS_MIXER_GAME_UNDERTALE) return false;

    int best = find_best_music_for_sound(sm, soundId, allowPaused);
    if (best < 0) return false;

    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    float currentGain = currentSlot >= 0 ? sm->musicHandles[currentSlot].gain : sm->musicGain;
    if (currentSlot >= 0 && currentGain > music_audible_epsilon(sm) &&
        sm->musicHandles[best].gain <= currentGain + 0.01f && current_music_audible(sm)) {
        return false;
    }

    return play_music_for_handle(sm, best, 0.f, fadeMs);
}

static bool undertale_consider_dominant_music(SysMixer *sm, int32_t changedSoundId, uint32_t fadeMs) {
    if (sm->gameMode != SYS_MIXER_GAME_UNDERTALE) return false;

    if (undertale_play_requested_music(sm, changedSoundId, fadeMs, false)) {
        return true;
    }

    int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
    float currentGain = currentSlot >= 0 ? sm->musicHandles[currentSlot].gain : sm->musicGain;
    if (current_music_audible(sm) && currentGain > music_audible_epsilon(sm)) {
        return false;
    }

    int best = find_recent_dominant_music(sm);
    if (best < 0) return false;
    return play_music_for_handle(sm, best, 0.f, fadeMs);
}

static DataWin *groupOf(SysMixer *sm, int32_t groupIndex) {
    if (groupIndex < 0) return NULL;
    if ((size_t) groupIndex >= arrlenu(sm->base.audioGroups)) return NULL;
    return sm->base.audioGroups[groupIndex];
}

static const char *archiveOf(SysMixer *sm, int32_t groupIndex) {
    if (groupIndex < 0) return NULL;
    if ((size_t) groupIndex >= arrlenu(sm->archivePaths)) return NULL;
    return sm->archivePaths[groupIndex];
}

static bool readEntryBytes(const char *archive, uint32_t offset, uint32_t size, uint8_t *out) {
    if (!archive || !size || !out) return false;
    FILE *fp = fopen(archive, "rb");
    if (!fp) {
        fprintf(stderr, "[AUDIO] open archive failed: %s\n", archive);
        return false;
    }
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    size_t got = fread(out, 1, size, fp);
    fclose(fp);
    if (got != size) {
        fprintf(stderr, "[AUDIO] short read %u/%u from %s @%u\n",
                (unsigned) got, (unsigned) size, archive, (unsigned) offset);
        return false;
    }
    return true;
}

// Drop the oldest finished cached chunk so we don't blow heap on big games.
static void evict_old(SysMixer *sm) {
    int cnt = 0;
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (sm->chunks[i]) cnt++;
    }
    if (cnt < MAX_CACHE) return;

    int victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;

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
            victim = (int) i;
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

static void evict_old_music(SysMixer *sm, int keepId) {
    int cnt = 0;
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (sm->music[i]) cnt++;
    }
    if (cnt <= MAX_MUSIC_CACHE) return;

    while (cnt > MAX_MUSIC_CACHE) {
        int victim = -1;
        uint32_t oldest = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
            if (!sm->music[i]) continue;
            if ((int) i == keepId || sm->curMusicId == (int32_t) i) continue;
            if (sm->lastUsed[i] < oldest) {
                oldest = sm->lastUsed[i];
                victim = (int) i;
            }
        }
        if (victim < 0) return;

        Mix_FreeMusic(sm->music[victim]);
        sm->music[victim] = NULL;
        if (sm->musicBuf[victim]) {
            free(sm->musicBuf[victim]);
            sm->musicBuf[victim] = NULL;
        }
        cnt--;
    }
}

// Read a SFX entry from its audiogroup archive into a Mix_Chunk.
static bool load_sfx(SysMixer *sm, int id, AudioEntry *ent, const char *archive) {
    evict_old(sm);

    uint8_t *raw = linearAlloc(ent->dataSize);
    if (!raw) {
        fprintf(stderr, "[AUDIO] linearAlloc(%u) failed for sfx %d\n",
                (unsigned) ent->dataSize, id);
        return false;
    }
    if (!readEntryBytes(archive, ent->dataOffset, ent->dataSize, raw)) {
        linearFree(raw);
        return false;
    }

    SDL_RWops *rw = SDL_RWFromConstMem(raw, ent->dataSize);
    sm->chunks[id] = Mix_LoadWAV_RW(rw, 1);
    linearFree(raw);
    return sm->chunks[id] != NULL;
}

static bool load_music_from_entry(SysMixer *sm, int id, AudioEntry *ent, const char *archive, const Sound *snd) {
    uint8_t *mb = malloc(ent->dataSize);
    if (!mb) {
        fprintf(stderr, "[AUDIO] malloc(%u) failed for music %d\n",
                (unsigned) ent->dataSize, id);
        return false;
    }
    if (!readEntryBytes(archive, ent->dataOffset, ent->dataSize, mb)) {
        free(mb);
        return false;
    }

    SDL_RWops *rw = SDL_RWFromConstMem(mb, ent->dataSize);
    Mix_Music *music = Mix_LoadMUS_RW(rw);
    if (!music) {
        fprintf(stderr, "[AUDIO] Mix_LoadMUS_RW failed for '%s': %s\n",
                snd && snd->name ? snd->name : "?", Mix_GetError());
        free(mb);
        return false;
    }

    sm->music[id] = music;
    sm->musicBuf[id] = mb;
    evict_old_music(sm, id);
    return true;
}

static bool load_external_sound(SysMixer *sm, int id, Sound *snd) {
    char *path = resolve_audio_path(sm, snd->file);
    if (!path) {
        fprintf(stderr, "[AUDIO] cannot resolve external sound '%s' (%s)\n",
                snd->name ? snd->name : "?", snd->file ? snd->file : "?");
        return false;
    }

    bool asMusic = sound_is_music_track(snd, 0xFFFFFFFFu);
    bool loaded = false;
    if (!asMusic || strstr(path, ".wav") || strstr(path, ".WAV")) {
        sm->chunks[id] = Mix_LoadWAV(path);
        loaded = sm->chunks[id] != NULL;
        if (!loaded) {
            fprintf(stderr, "[AUDIO] Mix_LoadWAV failed for '%s' at '%s': %s\n",
                    snd->name ? snd->name : "?", path, Mix_GetError());
        }
    } else {
        Mix_Music *music = Mix_LoadMUS(path);
        if (music) {
            sm->music[id] = music;
            evict_old_music(sm, id);
            loaded = true;
        } else {
            fprintf(stderr, "[AUDIO] Mix_LoadMUS failed for '%s' at '%s': %s\n",
                    snd->name ? snd->name : "?", path, Mix_GetError());
        }
    }

    free(path);
    return loaded;
}

static bool ensure_snd(SysMixer *sm, int id) {
    if (!s_mixerReady || id < 0 || (uint32_t) id >= sm->base.dataWin->sond.count) return false;
    sm->lastUsed[id] = sm->frame;
    if (sm->chunks[id] || sm->music[id]) return true;

    Sound *snd = &sm->base.dataWin->sond.sounds[id];

    // Embedded/compressed sounds live in the AUDO chunk of their audiogroup.
    if ((snd->flags & 0x01) || (snd->flags & 0x02)) {
        DataWin *gw = groupOf(sm, snd->audioGroup);
        const char *archive = archiveOf(sm, snd->audioGroup);
        if (!gw || !archive) {
            static int warned[16] = {0};
            int idx = (snd->audioGroup >= 0 && snd->audioGroup < 16) ? snd->audioGroup : 0;
            if (!warned[idx]) {
                warned[idx] = 1;
                fprintf(stderr, "[AUDIO] sound '%s' wants group %d but it isn't loaded\n",
                        snd->name ? snd->name : "?", (int) snd->audioGroup);
            }
            return false;
        }
        if (snd->audioFile < 0 || (uint32_t) snd->audioFile >= gw->audo.count) {
            fprintf(stderr, "[AUDIO] sound '%s' bad audioFile %d (group %d has %u)\n",
                    snd->name ? snd->name : "?", (int) snd->audioFile, (int) snd->audioGroup,
                    (unsigned) gw->audo.count);
            return false;
        }

        AudioEntry *ent = &gw->audo.entries[snd->audioFile];
        if (!ent->dataSize) return false;

        if (sound_is_music_track(snd, ent->dataSize)) {
            load_music_from_entry(sm, id, ent, archive, snd);
        } else if (!load_sfx(sm, id, ent, archive)) {
            fprintf(stderr, "[AUDIO] load_sfx failed for '%s' (group %d, file %d)\n",
                    snd->name ? snd->name : "?", (int) snd->audioGroup, (int) snd->audioFile);
        }
    } else {
        if (!snd->file || !snd->file[0]) return false;
        load_external_sound(sm, id, snd);
    }
    return sm->chunks[id] || sm->music[id];
}

static void reset_channel_state(SysMixer *sm, int ch) {
    if (ch < 0 || ch >= MAX_CHANS) return;
    sm->channelSound[ch] = -1;
    sm->channelGains[ch] = 1.f;
    memset(&sm->channelTweens[ch], 0, sizeof(sm->channelTweens[ch]));
    sm->channelTweens[ch].current = 1.f;
}

// ===[ Vtable ]===

static void sys_init(AudioSystem *sys, DataWin *dw, FileSystem *fs) {
    SysMixer *sm = (SysMixer *) sys;
    sm->base.dataWin = dw;
    sm->fs = fs;

    if (!SdlMixer_globalInit()) return;

    Mix_AllocateChannels(MAX_CHANS);

    int cnt = (int) dw->sond.count;
    sm->chunks     = calloc(cnt, sizeof(Mix_Chunk *));
    sm->music      = calloc(cnt, sizeof(Mix_Music *));
    sm->musicBuf   = calloc(cnt, sizeof(uint8_t *));
    sm->sfxBuf     = calloc(cnt, sizeof(void *));
    sm->soundGains = calloc(cnt, sizeof(float));
    sm->lastUsed   = calloc(cnt, sizeof(uint32_t));
    sm->pitches    = calloc(cnt, sizeof(float));
    for (int i = 0; i < cnt; i++) {
        sm->pitches[i] = 1.0f;
        sm->soundGains[i] = 1.0f;
    }
    for (int ch = 0; ch < MAX_CHANS; ch++) reset_channel_state(sm, ch);

    sm->curMusicId = -1;
    sm->curMusicHandle = -1;
    sm->musicGain = 1.f;
    sm->musicTween.current = 1.f;
    sm->masterGain = 1.f;
    sm->frame = 1;
    sm->gameMode = detect_game_mode(dw);
    fprintf(stderr, "[AUDIO] game mode: %s (%s / %s)\n",
            game_mode_name(sm->gameMode),
            dw->gen8.name ? dw->gen8.name : "?",
            dw->gen8.displayName ? dw->gen8.displayName : "?");

    // Group 0: the main data.win itself. Resolve its on-disk path so AUDO
    // offsets can be turned into byte ranges later.
    char *primary = fs->vtable->resolvePath(fs, "data.win");
    if (!primary) primary = fs->vtable->resolvePath(fs, "game.unx");
    arrput(sm->base.audioGroups, dw);
    arrput(sm->archivePaths, primary);
}

static void sys_destroy(AudioSystem *sys) {
    SysMixer *sm = (SysMixer *) sys;

    if (s_mixerReady) {
        Mix_HaltChannel(-1);
        Mix_HaltMusic();

        for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
            if (sm->chunks[i])   Mix_FreeChunk(sm->chunks[i]);
            if (sm->music[i])    Mix_FreeMusic(sm->music[i]);
            if (sm->musicBuf[i]) free(sm->musicBuf[i]);
            if (sm->sfxBuf[i])   linearFree(sm->sfxBuf[i]);
        }
        for (uint32_t i = 0; i < sm->streamCapacity; i++) {
            if (sm->streams[i].active) free_stream(sm, i);
        }
    }

    free(sm->chunks);
    free(sm->music);
    free(sm->musicBuf);
    free(sm->sfxBuf);
    free(sm->soundGains);
    free(sm->lastUsed);
    free(sm->pitches);
    free(sm->streams);

    // Skip group 0 (owned by main); free everything we loaded ourselves.
    if (arrlen(sm->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(sm->base.audioGroups); i++) {
            if (sm->base.audioGroups[i]) DataWin_free(sm->base.audioGroups[i]);
        }
    }
    for (int32_t i = 0; i < (int32_t) arrlen(sm->archivePaths); i++) {
        if (sm->archivePaths[i]) free(sm->archivePaths[i]);
    }
    arrfree(sm->base.audioGroups);
    arrfree(sm->archivePaths);

    free(sm);
}

static void sys_update(AudioSystem *sys, float dt) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;
    sm->frame++;

    if (tween_step(&sm->musicTween, dt)) {
        sm->musicGain = sm->musicTween.current;
        apply_music_gain(sm);
        if (!sm->musicTween.active && sm->gameMode == SYS_MIXER_GAME_UNDERTALE && sm->curMusicId >= 0) {
            (void) undertale_consider_dominant_music(sm, sm->curMusicId, 0);
        }
    }

    int streamSlot = stream_slot_from_id(sm, sm->curMusicId);
    if (streamSlot >= 0 && tween_step(&sm->streams[streamSlot].tween, dt)) {
        sm->streams[streamSlot].gain = sm->streams[streamSlot].tween.current;
        sm->musicGain = sm->streams[streamSlot].gain;
        apply_music_gain(sm);
        if (!sm->streams[streamSlot].tween.active && sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            (void) undertale_consider_dominant_music(sm, sm->curMusicId, 0);
        }
    }

    for (int ch = 0; ch < MAX_CHANS; ch++) {
        if (sm->channelSound[ch] >= 0 && !Mix_Playing(ch)) {
            reset_channel_state(sm, ch);
            continue;
        }
        if (tween_step(&sm->channelTweens[ch], dt)) {
            sm->channelGains[ch] = sm->channelTweens[ch].current;
            apply_channel_gain(sm, ch);
        }
    }
}

static int32_t sys_play(AudioSystem *sys, int32_t id, MAYBE_UNUSED int32_t prio, bool loop) {
    if (!s_mixerReady) return -1;
    SysMixer *sm = (SysMixer *) sys;

    if (undertale_sound_is_gameover(sm, id) &&
        sm->undertaleGameoverSuppressUntil != 0 &&
        sm->frame < sm->undertaleGameoverSuppressUntil) {
        return -1;
    }

    int streamSlot = stream_slot_from_id(sm, id);
    if (streamSlot >= 0) {
        int slot = alloc_music_handle(sm, id, sm->streams[streamSlot].gain, loop);
        return play_music_for_handle(sm, slot, sm->musicHandles[slot].gain, 0) ? music_handle_id(slot) : -1;
    }

    if (!ensure_snd(sm, id)) return -1;

    if (sm->music[id]) {
        int slot = alloc_music_handle(sm, id, sm->soundGains[id], loop);
        return play_music_for_handle(sm, slot, sm->musicHandles[slot].gain, 0) ? music_handle_id(slot) : -1;
    }

    if (sm->chunks[id]) {
        int ch = Mix_PlayChannel(-1, sm->chunks[id], loop ? -1 : 0);
        if (ch >= 0 && ch < MAX_CHANS) {
            sm->channelSound[ch] = id;
            sm->channelGains[ch] = sm->soundGains[id];
            tween_begin(&sm->channelTweens[ch], sm->channelGains[ch], sm->channelGains[ch], 0);
            apply_channel_gain(sm, ch);
            return SND_ID_BASE + ch;
        }
    }
    return -1;
}

static void sys_stop(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        SysMixerMusicHandle *mh = &sm->musicHandles[musicSlot];
        bool wasCurrent = sm->curMusicHandle == id;
        if (sm->curMusicHandle == id) {
            Mix_HaltMusic();
            sm->curMusicId = -1;
            sm->curMusicHandle = -1;
        }
        mh->playing = false;
        mh->paused = false;
        mh->active = false;
        if (wasCurrent) restore_previous_music(sm, musicSlot);
        return;
    }

    if (id == MUS_ID_BASE || sm->curMusicId == id) {
        int stoppedSlot = music_slot_from_handle(sm, sm->curMusicHandle);
        Mix_HaltMusic();
        if (sm->curMusicHandle > MUS_ID_BASE) {
            int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
            if (currentSlot >= 0) {
                sm->musicHandles[currentSlot].playing = false;
                sm->musicHandles[currentSlot].paused = false;
                sm->musicHandles[currentSlot].active = false;
            }
        }
        sm->curMusicId = -1;
        sm->curMusicHandle = -1;
        sm->musicGain = 0.f;
        sm->musicTween.active = false;
        if (id != MUS_ID_BASE && sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            (void) restore_undertale_paused_music(sm, stoppedSlot);
        }
    } else if (id >= SND_ID_BASE && id < SND_ID_BASE + MAX_CHANS) {
        int ch = id - SND_ID_BASE;
        Mix_HaltChannel(ch);
        reset_channel_state(sm, ch);
    } else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) {
        if (sm->curMusicId == id) {
            int stoppedSlot = music_slot_from_handle(sm, sm->curMusicHandle);
            Mix_HaltMusic();
            if (sm->curMusicHandle > MUS_ID_BASE) {
                int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
                if (currentSlot >= 0) {
                    sm->musicHandles[currentSlot].playing = false;
                    sm->musicHandles[currentSlot].paused = false;
                    sm->musicHandles[currentSlot].active = false;
                }
            }
            sm->curMusicId = -1;
            sm->curMusicHandle = -1;
            sm->musicGain = 0.f;
            sm->musicTween.active = false;
            if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
                (void) restore_undertale_paused_music(sm, stoppedSlot);
            }
        }
        if (sm->chunks[id]) {
            for (int i = 0; i < MAX_CHANS; i++) {
                if (Mix_GetChunk(i) == sm->chunks[id]) {
                    Mix_HaltChannel(i);
                    reset_channel_state(sm, i);
                }
            }
        }
    }
}

static void sys_stop_all(AudioSystem *sys) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;
    bool suppressGameoverRestart = undertale_sound_is_gameover(sm, sm->curMusicId);
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    sm->curMusicId = -1;
    sm->curMusicHandle = -1;
    for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
        sm->musicHandles[i].active = false;
        sm->musicHandles[i].playing = false;
        sm->musicHandles[i].paused = false;
    }
    for (int ch = 0; ch < MAX_CHANS; ch++) reset_channel_state(sm, ch);
    if (suppressGameoverRestart) {
        // Flowey's death sequence stops gameover.ogg, waits 150 frames, then
        // explicitly replays it for the laugh section. Only block accidental
        // immediate restarts; do not block that scripted replay.
        sm->undertaleGameoverSuppressUntil = sm->frame + 120u;
    }
}

static bool sys_is_playing(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return false;
    SysMixer *sm = (SysMixer *) sys;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            return sm->curMusicHandle == id && current_music_audible(sm);
        }
        return sm->musicHandles[musicSlot].playing && sm->curMusicHandle == id && Mix_PlayingMusic();
    }

    if (id == MUS_ID_BASE) {
        return sm->gameMode == SYS_MIXER_GAME_UNDERTALE ? current_music_audible(sm) : Mix_PlayingMusic();
    }
    if (stream_slot_from_id(sm, id) >= 0) {
        return sm->gameMode == SYS_MIXER_GAME_UNDERTALE
            ? (sm->curMusicId == id && current_music_audible(sm))
            : (sm->curMusicId == id && Mix_PlayingMusic());
    }
    if (id >= SND_ID_BASE && id < SND_ID_BASE + MAX_CHANS) return Mix_Playing(id - SND_ID_BASE);

    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) {
        if (sm->curMusicId == id) {
            return sm->gameMode == SYS_MIXER_GAME_UNDERTALE ? current_music_audible(sm) : Mix_PlayingMusic();
        }
        if (sm->chunks[id]) {
            for (int i = 0; i < MAX_CHANS; i++) {
                if (Mix_Playing(i) && Mix_GetChunk(i) == sm->chunks[id]) return true;
            }
        }
    }
    return false;
}

static void sys_pause(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        SysMixerMusicHandle *mh = &sm->musicHandles[musicSlot];
        mh->paused = true;
        mh->playing = false;
        if (sm->curMusicHandle == id) Mix_PauseMusic();
        return;
    }

    if (id == MUS_ID_BASE || sm->curMusicId == id) {
        set_current_music_paused(sm, true);
        Mix_PauseMusic();
    } else if (id >= SND_ID_BASE && id < SND_ID_BASE + MAX_CHANS) Mix_Pause(id - SND_ID_BASE);
    else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) {
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) mark_music_handles_for_sound_paused(sm, id, true);
        if (sm->chunks[id]) {
            for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Pause(i);
        }
    }
}

static void sys_resume(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        SysMixerMusicHandle *mh = &sm->musicHandles[musicSlot];
        mh->paused = false;
        if (sm->curMusicHandle == id) {
            Mix_ResumeMusic();
            mh->playing = true;
        } else {
            play_music_for_handle(sm, musicSlot, sm->musicHandles[musicSlot].gain, 0);
        }
        return;
    }

    if (id == MUS_ID_BASE || sm->curMusicId == id) {
        set_current_music_paused(sm, false);
        Mix_ResumeMusic();
    } else if (id >= SND_ID_BASE && id < SND_ID_BASE + MAX_CHANS) Mix_Resume(id - SND_ID_BASE);
    else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) {
        bool resumedMusic = false;
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            mark_music_handles_for_sound_paused(sm, id, false);
            resumedMusic = undertale_play_requested_music(sm, id, 0, true);
        }
        if (!resumedMusic && sm->chunks[id]) {
            for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Resume(i);
        }
    }
}

static void sys_pause_all(MAYBE_UNUSED AudioSystem *sys) {
    if (s_mixerReady) {
        Mix_Pause(-1);
        Mix_PauseMusic();
    }
}

static void sys_resume_all(MAYBE_UNUSED AudioSystem *sys) {
    if (s_mixerReady) {
        Mix_Resume(-1);
        Mix_ResumeMusic();
    }
}

static void sys_set_gain(AudioSystem *sys, int32_t id, float g, uint32_t t) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;
    g = clamp_gain(g);

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        SysMixerMusicHandle *mh = &sm->musicHandles[musicSlot];
        float current = mh->gain;
        mh->gain = g;
        if (g > music_audible_epsilon(sm)) mh->lastAudibleGain = g;
        if (sm->curMusicHandle == id) {
            tween_begin(&sm->musicTween, sm->musicGain, g, t);
            sm->musicGain = sm->musicTween.current;
            apply_music_gain(sm);
            if (current <= 0.001f && g > 0.001f) restart_current_music_if_silent(sm);
            if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
                (void) undertale_consider_dominant_music(sm, mh->soundId, t);
            }
        } else if (g > 0.001f) {
            play_music_for_handle(sm, musicSlot, 0.f, t);
        }
        return;
    }

    int streamSlot = stream_slot_from_id(sm, id);
    if (streamSlot >= 0) {
        float current = sm->streams[streamSlot].gain;
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) set_music_handle_gains_for_sound(sm, id, g);
        tween_begin(&sm->streams[streamSlot].tween, current, g, t);
        sm->streams[streamSlot].gain = sm->streams[streamSlot].tween.current;
        if (sm->curMusicId == id) {
            int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
            if (currentSlot >= 0) {
                sm->musicHandles[currentSlot].gain = g;
                if (g > music_audible_epsilon(sm)) sm->musicHandles[currentSlot].lastAudibleGain = g;
            }
            tween_begin(&sm->musicTween, sm->musicGain, g, t);
            sm->musicGain = sm->musicTween.current;
            apply_music_gain(sm);
            if (current <= 0.001f && g > 0.001f) restart_current_music_if_silent(sm);
        }
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            (void) undertale_consider_dominant_music(sm, id, t);
        }
        return;
    }

    if (id == MUS_ID_BASE) {
        float current = sm->musicGain;
        int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
        if (currentSlot >= 0) {
            sm->musicHandles[currentSlot].gain = g;
            if (g > music_audible_epsilon(sm)) sm->musicHandles[currentSlot].lastAudibleGain = g;
        }
        tween_begin(&sm->musicTween, sm->musicGain, g, t);
        sm->musicGain = sm->musicTween.current;
        apply_music_gain(sm);
        if (current <= 0.001f && g > 0.001f) restart_current_music_if_silent(sm);
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE && sm->curMusicId >= 0) {
            (void) undertale_consider_dominant_music(sm, sm->curMusicId, t);
        }
        return;
    }

    if (id >= SND_ID_BASE && id < SND_ID_BASE + MAX_CHANS) {
        int ch = id - SND_ID_BASE;
        tween_begin(&sm->channelTweens[ch], sm->channelGains[ch], g, t);
        sm->channelGains[ch] = sm->channelTweens[ch].current;
        apply_channel_gain(sm, ch);
        return;
    }

    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) {
        sm->soundGains[id] = g;
        if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
            set_music_handle_gains_for_sound(sm, id, g);
        }

        if (sm->music[id]) {
            if (sm->curMusicId == id) {
                float current = sm->musicGain;
                int currentSlot = music_slot_from_handle(sm, sm->curMusicHandle);
                if (currentSlot >= 0) {
                    sm->musicHandles[currentSlot].gain = g;
                    if (g > music_audible_epsilon(sm)) sm->musicHandles[currentSlot].lastAudibleGain = g;
                }
                tween_begin(&sm->musicTween, sm->musicGain, g, t);
                sm->musicGain = sm->musicTween.current;
                apply_music_gain(sm);
                if (current <= 0.001f && g > 0.001f) restart_current_music_if_silent(sm);
            }
            if (sm->gameMode == SYS_MIXER_GAME_UNDERTALE) {
                (void) undertale_consider_dominant_music(sm, id, t);
            }
        }

        if (sm->chunks[id]) {
            for (int ch = 0; ch < MAX_CHANS; ch++) {
                if (sm->channelSound[ch] == id || Mix_GetChunk(ch) == sm->chunks[id]) {
                    tween_begin(&sm->channelTweens[ch], sm->channelGains[ch], g, t);
                    sm->channelGains[ch] = sm->channelTweens[ch].current;
                    apply_channel_gain(sm, ch);
                }
            }
        }
    }
}

static float sys_get_gain(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return 0.f;
    SysMixer *sm = (SysMixer *) sys;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) return sm->musicHandles[musicSlot].gain;

    int streamSlot = stream_slot_from_id(sm, id);
    if (streamSlot >= 0) return sm->streams[streamSlot].gain;
    if (id == MUS_ID_BASE) return sm->musicGain;
    if (id >= SND_ID_BASE && id < SND_ID_BASE + MAX_CHANS) return sm->channelGains[id - SND_ID_BASE];
    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count) return sm->soundGains[id];
    return 0.f;
}

static void sys_set_pitch(AudioSystem *sys, int32_t id, float p) {
    SysMixer *sm = (SysMixer *) sys;
    if (p <= 0.f) p = 1.f;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        sm->musicHandles[musicSlot].pitch = p;
        int32_t soundId = sm->musicHandles[musicSlot].soundId;
        if (soundId >= 0 && (uint32_t) soundId < sys->dataWin->sond.count && sm->pitches) {
            sm->pitches[soundId] = p;
        }
        (void) switch_undertale_pitched_gameover_to_laugh_music(sm, musicSlot, p);
        return;
    }

    if (s_mixerReady && id >= 0 && (uint32_t) id < sys->dataWin->sond.count && sm->pitches) {
        sm->pitches[id] = p;
        for (int i = 0; i < MAX_MUSIC_HANDLES; i++) {
            if (sm->musicHandles[i].active && sm->musicHandles[i].soundId == id) {
                sm->musicHandles[i].pitch = p;
            }
        }
    }
}

static float sys_get_pitch(AudioSystem *sys, int32_t id) {
    SysMixer *sm = (SysMixer *) sys;
    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) return sm->musicHandles[musicSlot].pitch;
    if (s_mixerReady && id >= 0 && (uint32_t) id < sys->dataWin->sond.count && sm->pitches) {
        return sm->pitches[id];
    }
    return 1.f;
}

static float sys_get_pos(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t id) {
    return 0.f;
}

static void sys_set_pos(AudioSystem *sys, int32_t id, float pos) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;
    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) {
        if (sm->curMusicHandle == id) Mix_SetMusicPosition(pos);
        return;
    }
    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_SetMusicPosition(pos);
}

static float sys_get_length(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return 0.f;
    SysMixer *sm = (SysMixer *) sys;

    int musicSlot = music_slot_from_handle(sm, id);
    if (musicSlot >= 0) return 0.f;

    if (id == MUS_ID_BASE || sm->curMusicId == id || stream_slot_from_id(sm, id) >= 0) return 0.f;
    if (id >= SND_ID_BASE) return 0.f;

    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        int freq, chans;
        Uint16 fmt;
        Mix_QuerySpec(&freq, &fmt, &chans);
        int bytes_per_sample = (fmt & 0xFF) / 8;
        if (freq > 0 && chans > 0 && bytes_per_sample > 0) {
            return (float) sm->chunks[id]->alen / (freq * chans * bytes_per_sample);
        }
    }
    return 0.f;
}

static void sys_set_master(AudioSystem *sys, float g) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;
    sm->masterGain = clamp_gain(g);
    apply_music_gain(sm);
    for (int ch = 0; ch < MAX_CHANS; ch++) {
        if (sm->channelSound[ch] >= 0 || Mix_GetChunk(ch)) apply_channel_gain(sm, ch);
    }
}

static void sys_set_chans(MAYBE_UNUSED AudioSystem *sys, int32_t cnt) {
    if (!s_mixerReady) return;
    if (cnt < 1) cnt = 1;
    if (cnt > MAX_CHANS) cnt = MAX_CHANS;
    Mix_AllocateChannels((int) cnt);
}

// Lazy-load audiogroupN.dat: parse its AUDO chunk (offsets only — we read
// the actual bytes on demand from disk) and stash the file path.
static void sys_grp_load(AudioSystem *sys, int32_t grp) {
    SysMixer *sm = (SysMixer *) sys;
    if (grp <= 0) return; // group 0 is the main data.win, set up in sys_init

    // Already loaded?
    if ((size_t) grp < arrlenu(sm->base.audioGroups) && sm->base.audioGroups[grp]) return;

    char rel[64];
    snprintf(rel, sizeof(rel), "audiogroup%d.dat", (int) grp);
    char *path = sm->fs->vtable->resolvePath(sm->fs, rel);
    if (!path) {
        fprintf(stderr, "[AUDIO] cannot resolve %s\n", rel);
        return;
    }

    DataWinParserOptions opt = (DataWinParserOptions) {
        .parseAudo = 1,
        .skipAudioBlobData = 1,
    };
    DataWin *gw = DataWin_parse(path, opt);
    if (!gw) {
        fprintf(stderr, "[AUDIO] failed to parse %s\n", path);
        free(path);
        return;
    }

    // Pad both arrays up to (grp+1) entries with NULLs so we can index by grp.
    while ((int32_t) arrlen(sm->base.audioGroups) <= grp) arrput(sm->base.audioGroups, NULL);
    while ((int32_t) arrlen(sm->archivePaths)    <= grp) arrput(sm->archivePaths, NULL);
    sm->base.audioGroups[grp] = gw;
    sm->archivePaths[grp]     = path;
    fprintf(stderr, "[AUDIO] loaded audiogroup %d (%s, %u entries)\n",
            (int) grp, path, (unsigned) gw->audo.count);
}

static bool sys_grp_loaded(AudioSystem *sys, int32_t grp) {
    SysMixer *sm = (SysMixer *) sys;
    if (grp < 0) return false;
    if ((size_t) grp >= arrlenu(sm->base.audioGroups)) return false;
    return sm->base.audioGroups[grp] != NULL;
}

static int32_t sys_create_stream(AudioSystem *sys, const char *filename) {
    if (!s_mixerReady || !filename || !filename[0]) return -1;
    SysMixer *sm = (SysMixer *) sys;

    char *path = resolve_audio_path(sm, filename);
    if (!path) {
        fprintf(stderr, "[AUDIO] cannot resolve stream '%s'\n", filename);
        return -1;
    }

    uint32_t slot = 0;
    for (; slot < sm->streamCapacity; slot++) {
        if (!sm->streams[slot].active) break;
    }
    if (slot == sm->streamCapacity) {
        uint32_t newCap = sm->streamCapacity ? sm->streamCapacity * 2 : 8;
        SysMixerStream *newStreams = realloc(sm->streams, newCap * sizeof(SysMixerStream));
        if (!newStreams) {
            free(path);
            return -1;
        }
        memset(newStreams + sm->streamCapacity, 0, (newCap - sm->streamCapacity) * sizeof(SysMixerStream));
        sm->streams = newStreams;
        sm->streamCapacity = newCap;
    }

    sm->streams[slot].active = true;
    sm->streams[slot].music = NULL;
    sm->streams[slot].path = path;
    sm->streams[slot].gain = 1.f;
    tween_begin(&sm->streams[slot].tween, 1.f, 1.f, 0);
    if (slot + 1 > sm->streamCount) sm->streamCount = slot + 1;

    fprintf(stderr, "[AUDIO] created lazy stream %d for '%s' -> '%s'\n",
            (int) (STREAM_ID_BASE + (int32_t) slot), filename, path);
    return STREAM_ID_BASE + (int32_t) slot;
}

static bool sys_destroy_stream(AudioSystem *sys, int32_t streamIndex) {
    if (!s_mixerReady) return false;
    SysMixer *sm = (SysMixer *) sys;
    int slot = stream_slot_from_id(sm, streamIndex);
    if (slot < 0) return false;
    free_stream(sm, (uint32_t) slot);
    return true;
}

static AudioSystemVtable vtable = {
    .init = sys_init, .destroy = sys_destroy, .update = sys_update,
    .playSound = sys_play, .stopSound = sys_stop, .stopAll = sys_stop_all,
    .isPlaying = sys_is_playing, .pauseSound = sys_pause, .resumeSound = sys_resume,
    .pauseAll = sys_pause_all, .resumeAll = sys_resume_all, .setSoundGain = sys_set_gain,
    .getSoundGain = sys_get_gain, .setSoundPitch = sys_set_pitch, .getSoundPitch = sys_get_pitch,
    .getTrackPosition = sys_get_pos, .setTrackPosition = sys_set_pos,
    .getSoundLength = sys_get_length,
    .setMasterGain = sys_set_master, .setChannelCount = sys_set_chans,
    .groupLoad = sys_grp_load, .groupIsLoaded = sys_grp_loaded,
    .createStream = sys_create_stream, .destroyStream = sys_destroy_stream
};

AudioSystem *SdlMixerAudioSystem_create(void) {
    SysMixer *sm = calloc(1, sizeof(SysMixer));
    sm->base.vtable = &vtable;
    return (AudioSystem *) sm;
}
