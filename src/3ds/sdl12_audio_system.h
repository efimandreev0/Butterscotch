#pragma once

#include "../audio_system.h"
#include <SDL/SDL_mixer.h>
#include <stdio.h>

#define SND_ID_BASE 100000
#define MUS_ID_BASE 200000
#define STREAM_ID_BASE 300000
#define MAX_CHANS 32
#define MAX_MUSIC_HANDLES 128

typedef struct {
    float current;
    float start;
    float target;
    float elapsed;
    float duration;
    bool active;
} SysMixerGainTween;

typedef struct {
    bool active;
    Mix_Music *music;
    char *path;
    float gain;
    SysMixerGainTween tween;
} SysMixerStream;

typedef struct {
    bool active;
    bool playing;
    bool paused;
    bool loop;
    int32_t soundId;
    float gain;
    float lastAudibleGain;
    float pitch;
    uint32_t createdFrame;
    uint32_t displacedFrame;
} SysMixerMusicHandle;

typedef enum {
    SYS_MIXER_GAME_GENERIC = 0,
    SYS_MIXER_GAME_UNDERTALE,
    SYS_MIXER_GAME_DELTARUNE,
} SysMixerGameMode;

typedef struct {
    AudioSystem base;
    FileSystem *fs;

    // Per-sound caches indexed by SOND id (base.dataWin->sond.count entries).
    Mix_Chunk **chunks;
    Mix_Music **music;
    uint8_t   **musicBuf;   // raw ogg/mp3 bytes backing a Mix_Music
    void      **sfxBuf;     // linearAlloc'd PCM backing a Mix_QuickLoad_RAW chunk
    float      *soundGains;
    float      *pitches;
    uint32_t   *lastUsed;

    int32_t channelSound[MAX_CHANS];
    float channelGains[MAX_CHANS];
    SysMixerGainTween channelTweens[MAX_CHANS];

    float musicGain;
    SysMixerGainTween musicTween;
    float masterGain;

    // Per-audiogroup archive paths. archivePaths[N] is the file the AUDO offsets
    // for audioGroups[N] resolve into. archivePaths[0] = data.win/game.unx.
    // Grown lazily by groupLoad. Stored as a stb_ds dynamic array.
    char **archivePaths;

    int32_t  curMusicId;
    int32_t  curMusicHandle;
    uint32_t nextMusicHandleSlot;
    uint32_t frame;
    SysMixerGameMode gameMode;
    uint32_t undertaleGameoverSuppressUntil;

    SysMixerMusicHandle musicHandles[MAX_MUSIC_HANDLES];

    SysMixerStream *streams;
    uint32_t streamCount;
    uint32_t streamCapacity;
} SysMixer;

AudioSystem* SdlMixerAudioSystem_create(void);

// One-time audio backend init (SDL audio + Mix_OpenAudio + Mix_Init). Safe to
// call multiple times; subsequent calls are no-ops. Tearing the mixer down
// between games is unreliable on 3DS so we keep it alive for the whole app.
bool SdlMixer_globalInit(void);
void SdlMixer_globalShutdown(void);
