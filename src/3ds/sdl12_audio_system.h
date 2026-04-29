#pragma once

#include "../audio_system.h"
#include <SDL/SDL_mixer.h>
#include <stdio.h>

#define SND_ID_BASE 100000
#define MUS_ID_BASE 200000

typedef struct {
    AudioSystem base;
    FileSystem *fs;

    Mix_Chunk **chunks;
    Mix_Music **music;
    uint8_t **musicBuf;
    void **sfxBuf;

    char *archivePath;

    int32_t curMusicId;
    uint32_t *lastUsed;
    uint32_t frame;
} SysMixer;

AudioSystem* SdlMixerAudioSystem_create(void);