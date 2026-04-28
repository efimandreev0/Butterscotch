#pragma once

#include "../audio_system.h"
#include <SDL/SDL_mixer.h>
#include <stdio.h>

#define SOUND_INSTANCE_ID_BASE 100000

typedef struct {
    AudioSystem base;
    FileSystem* fileSystem;

    FILE* dataWinFile;
    FILE* sfxDataWinFile;
    Mix_Chunk** chunks;
    Mix_Music** music;
    uint8_t** compressedMusicBuf;
    void** decodedSfxBufs;

    char* archivePath;

    int32_t* channelToSoundIndex;
    int32_t currentMusicSoundIndex;

    uint32_t* chunkLastUsed;
    uint32_t audioFrameCounter;

} SdlMixerAudioSystem;

AudioSystem* SdlMixerAudioSystem_create(void);