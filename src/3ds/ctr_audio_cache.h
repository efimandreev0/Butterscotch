// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CTR_AUDIO_CACHE_MAGIC      0x55424143u   
#define CTR_AUDIO_CACHE_VERSION    3u
#define CTR_AUDIO_CACHE_FILE       "audio.bin"

#define CTR_AUDIO_FLAG_LOOP            (1u << 0)
#define CTR_AUDIO_FLAG_PREFER_STREAM   (1u << 1)

#define CTR_AUDIO_FORMAT_PCM8   0u   
#define CTR_AUDIO_FORMAT_PCM16  1u   
#define CTR_AUDIO_FORMAT_ADPCM  2u   

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t soundCount;
    uint32_t indexOffset;
    uint32_t dataOffset;
    uint32_t dataSize;
    uint64_t srcSize;
    uint32_t srcSampleHash;
    uint32_t reserved;
} CtrAudioCacheHeader;

typedef struct {
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t sampleRate;
    uint32_t totalFrames;
    uint32_t loopStart;
    uint8_t  channels;
    uint8_t  flags;
    uint8_t  format;
    uint8_t  reserved;
    int16_t  adpcmCoefs[16];

} CtrAudioEntry;
