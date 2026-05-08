#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CTR_AUDIO_CACHE_MAGIC      0x55424143u   // 'CABU' le
#define CTR_AUDIO_CACHE_VERSION    3u
#define CTR_AUDIO_CACHE_FILE       "audio.bin"

#define CTR_AUDIO_FLAG_LOOP            (1u << 0)
#define CTR_AUDIO_FLAG_PREFER_STREAM   (1u << 1)

#define CTR_AUDIO_FORMAT_PCM8   0u   // 8-bit signed (mono only). NDSP_FORMAT_*_PCM8.
#define CTR_AUDIO_FORMAT_PCM16  1u   // 16-bit signed. NDSP_FORMAT_*_PCM16.
#define CTR_AUDIO_FORMAT_ADPCM  2u   // GC DSP-ADPCM (mono). NDSP_FORMAT_MONO_ADPCM. 4× smaller than PCM16.

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
