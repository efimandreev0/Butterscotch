#pragma once

#include "../audio_system.h"
#include <3ds.h>
#include <stdio.h>

#define MAX_CTR_CHANNELS 24
#define MAX_LRU_CACHE 64

typedef struct {
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t sampleRate;
    uint32_t totalFrames;
    uint8_t  channels;
    uint8_t  format;
    u16      adpcmCoefs[16];
    bool     preferStream;
} CtrSoundDef;

typedef struct {
    int32_t id;
    void*   data;
    uint32_t size;
    uint32_t lastAccess;
} CtrCacheEntry;

typedef struct {
    int32_t currentSoundId;
    int32_t instanceId;    // <--- ДОБАВЛЕН УНИКАЛЬНЫЙ ID ИНСТАНСА
    int32_t priority;
    bool    loop;
    float   gain;
    float   pitch;

    ndspWaveBuf   waveBuf;
    ndspAdpcmData adpcmState;

    uint32_t playFrame;
} CtrChannelState;

typedef struct {
    AudioSystem base;
    FileSystem *fs;

    FILE* audioBin;
    CtrSoundDef* soundBank;
    uint32_t soundCount;

    CtrCacheEntry cache[MAX_LRU_CACHE];
    CtrChannelState chans[MAX_CTR_CHANNELS];

    float masterGain;
    uint32_t frame;
    int32_t nextInstanceId;
} CtrAudioSystem;

CtrAudioSystem *CtrAudioSystem_create(void);