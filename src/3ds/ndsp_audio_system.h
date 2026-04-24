#pragma once

#include "common.h"
#include "audio_system.h"
#include <3ds.h>

#define NDSP_MAX_CHANNELS 24
#define SOUND_INSTANCE_ID_BASE 100000
#define AUDIO_STREAM_INDEX_BASE 300000

#define STREAMING_SIZE_THRESHOLD (128 * 1024)

// Увеличиваем буфер до 16384 (примерно 0.37 сек на буфер).
// Это спасет слабенький процессор 3DS от "голодания" и прерывания музыки.
#define STREAM_BUFFERS 2
#define STREAM_BUFFER_SAMPLES 16384

typedef struct stb_vorbis stb_vorbis;

typedef struct {
    bool isLoaded;
    int32_t soundIdx;
    void* pcmData;
    size_t dataSize;
    int channels;
    int sampleRate;
    int sampleCount;
    int bitsPerSample;
} NdspPcmCache;

typedef struct {
    bool active;
    bool paused;
    int32_t soundIndex;
    int32_t instanceId;
    int channelId;

    // Громкость и фейды
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;

    // Базовые параметры звука из data.win
    float sondVolume;
    float sondPitch;
    float pitch;

    int32_t priority;
    bool isLooping;
    bool isStreaming;
    bool endOfStream;

    ndspWaveBuf waveBufSfx;

    ndspWaveBuf waveBufsStream[STREAM_BUFFERS];
    short* streamBuffersLinear[STREAM_BUFFERS];
    stb_vorbis* vorbisStream;

    int channels;
    int sampleRate;

    void* streamFileData;
} NdspSoundInstance;

typedef struct {
    AudioSystem base;
    bool isNdspReady;

    float masterGain; // Мастер-громкость

    NdspSoundInstance instances[NDSP_MAX_CHANNELS];
    int32_t channelOwner[NDSP_MAX_CHANNELS]; // Отслеживание владельца аппаратного канала
    void* streamBufPool; // Единый Linear-пул для стриминговых буферов

    int32_t nextInstanceCounter;
    FileSystem* fileSystem;

    NdspPcmCache* pcmCache;
    uint32_t pcmCacheCount;
} NdspAudioSystem;

NdspAudioSystem* NdspAudioSystem_create(void);