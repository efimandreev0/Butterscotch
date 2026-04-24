#include "ndsp_audio_system.h"
#include "data_win.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vorbis/stb_vorbis.c"
#include "stb_ds.h"

#define PCM_BYTES_PER_FRAME_STEREO 4
#define PCM_BYTES_PER_FRAME_MONO   2
#define NDSP_DEFAULT_SAMPLE_RATE 44100

// ===[ Helpers ]===

static void updateChannelMix(NdspAudioSystem* sys, NdspSoundInstance* inst) {
    if (inst->channelId < 0 || inst->channelId >= NDSP_MAX_CHANNELS) return;
    float mix[12] = {0.0f};
    float gain = sys->masterGain * inst->currentGain * inst->sondVolume;
    if (gain < 0.0f) gain = 0.0f;
    mix[0] = gain;
    mix[1] = gain;
    ndspChnSetMix((u32) inst->channelId, mix);
}

static void updateChannelRate(NdspSoundInstance* inst) {
    if (inst->channelId < 0 || inst->channelId >= NDSP_MAX_CHANNELS) return;
    float baseRate = (inst->sampleRate > 0) ? (float)inst->sampleRate : (float)NDSP_DEFAULT_SAMPLE_RATE;
    float rate = baseRate * inst->pitch * inst->sondPitch;
    if (rate < 1000.0f) rate = 1000.0f;
    if (rate > 96000.0f) rate = 96000.0f;
    ndspChnSetRate((u32) inst->channelId, rate);
}

static void stopInstance(NdspAudioSystem* sys, NdspSoundInstance* inst) {
    if (!inst || !inst->active) return;

    if (inst->channelId >= 0 && inst->channelId < NDSP_MAX_CHANNELS) {
        ndspChnWaveBufClear((u32) inst->channelId);
        ndspChnReset((u32) inst->channelId);
        sys->channelOwner[inst->channelId] = -1;
    }

    if (inst->vorbisStream) {
        stb_vorbis_close(inst->vorbisStream);
        inst->vorbisStream = NULL;
    }

    if (inst->streamFileData) {
        free(inst->streamFileData);
        inst->streamFileData = NULL;
    }

    // Сохраняем указатели на пул, чтобы не потерять их при memset
    short* b0 = inst->streamBuffersLinear[0];
    short* b1 = inst->streamBuffersLinear[1];

    memset(inst, 0, sizeof(*inst));

    inst->streamBuffersLinear[0] = b0;
    inst->streamBuffersLinear[1] = b1;
    inst->channelId = -1;
}

static int32_t selectVictimInstance(NdspAudioSystem* sys) {
    int32_t victim = -1;
    int32_t victimPriority = 0x7FFFFFFF;

    for (int32_t i = 0; i < NDSP_MAX_CHANNELS; i++) {
        NdspSoundInstance* inst = &sys->instances[i];
        if (!inst->active) continue;
        if (inst->priority < victimPriority) {
            victimPriority = inst->priority;
            victim = i;
        }
    }
    return victim;
}

static int32_t allocateChannel(NdspAudioSystem* sys) {
    for (int32_t ch = 0; ch < NDSP_MAX_CHANNELS; ch++) {
        if (sys->channelOwner[ch] < 0) return ch;
    }
    return -1;
}

static NdspSoundInstance* allocateInstanceSlot(NdspAudioSystem* sys, int32_t priority) {
    for (int32_t i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (!sys->instances[i].active) return &sys->instances[i];
    }

    int32_t victim = selectVictimInstance(sys);
    if (victim < 0) return NULL;

    if (sys->instances[victim].priority > priority) return NULL;

    stopInstance(sys, &sys->instances[victim]);
    return &sys->instances[victim];
}

static NdspSoundInstance* findInstanceById(NdspAudioSystem* sys, int32_t instanceId) {
    int32_t slot = instanceId - SOUND_INSTANCE_ID_BASE;
    if (slot < 0 || slot >= NDSP_MAX_CHANNELS) return NULL;
    NdspSoundInstance* inst = &sys->instances[slot];
    if (!inst->active || inst->instanceId != instanceId) return NULL;
    return inst;
}

// ===[ SFX & Streaming Logic ]===

static NdspPcmCache* getOrDecodeSfx(NdspAudioSystem* sys, int32_t soundIndex, AudioEntry* entry, int audioGroup) {
    // Защита от выхода за пределы массива
    if (soundIndex < 0 || (uint32_t)soundIndex >= sys->pcmCacheCount) return NULL;

    NdspPcmCache* cacheObj = &sys->pcmCache[soundIndex];

    // O(1) проверка кэша: Если мы уже пытались загрузить звук (успешно или нет),
    // возвращаем результат, чтобы не грузить SD-карту заново.
    if (cacheObj->isLoaded) {
        return cacheObj->pcmData ? cacheObj : NULL;
    }

    void* oggData = entry->data;
    bool freeOggData = false;

    if (!oggData && entry->dataSize > 0) {
        char buf[64];
        if (audioGroup == 0) snprintf(buf, sizeof(buf), "data.win");
        else snprintf(buf, sizeof(buf), "audiogroup%d.dat", audioGroup);

        char* path = sys->fileSystem->vtable->resolvePath(sys->fileSystem, buf);
        if (path) {
            FILE* f = fopen(path, "rb");
            if (f) {
                setvbuf(f, NULL, _IOFBF, 128 * 1024);
                fseek(f, entry->dataOffset, SEEK_SET);
                oggData = malloc(entry->dataSize);
                fread(oggData, 1, entry->dataSize, f);
                fclose(f);
                freeOggData = true;
            }
            free(path);
        }
    }

    if (!oggData) {
        cacheObj->isLoaded = true; // Помечаем как обработанный, чтобы не долбить диск
        return NULL;
    }

    int channels = 1, sampleRate = 44100, samples = 0, bitsPerSample = 16;
    size_t byteSize = 0;
    void* linearMem = NULL;

    // Проверяем, является ли файл WAV-ом (заголовок RIFF WAVE)
    bool isWav = false;
    if (entry->dataSize > 12) {
        uint8_t* head = (uint8_t*)oggData;
        if (head[0]=='R' && head[1]=='I' && head[2]=='F' && head[3]=='F' &&
            head[8]=='W' && head[9]=='A' && head[10]=='V' && head[11]=='E') {
            isWav = true;
        }
    }

    if (isWav) {
        // Парсим WAV напрямую в память
        uint8_t* ptr = (uint8_t*)oggData + 12;
        uint8_t* end = (uint8_t*)oggData + entry->dataSize;
        void* pcmWavData = NULL;

        while (ptr + 8 <= end) {
            uint32_t cId = *(uint32_t*)ptr;
            uint32_t cSize = *(uint32_t*)(ptr + 4);
            ptr += 8;

            if (cId == 0x20746D66) { // чанк "fmt "
                channels = *(uint16_t*)(ptr + 2);
                sampleRate = *(uint32_t*)(ptr + 4);
                bitsPerSample = *(uint16_t*)(ptr + 14);
            } else if (cId == 0x61746164) { // чанк "data"
                byteSize = cSize;
                pcmWavData = ptr;
                break;
            }
            ptr += cSize;
        }

        if (pcmWavData && (bitsPerSample == 8 || bitsPerSample == 16)) {
            samples = byteSize / (channels * (bitsPerSample / 8));
            linearMem = linearAlloc(byteSize);
            if (linearMem) {
                memcpy(linearMem, pcmWavData, byteSize);
                DSP_FlushDataCache(linearMem, byteSize);
            }
        }
    } else {
        // Это OGG, используем stb_vorbis
        short* decodedPcm;
        samples = stb_vorbis_decode_memory(oggData, entry->dataSize, &channels, &sampleRate, &decodedPcm);

        if (samples > 0) {
            byteSize = samples * channels * sizeof(short);
            linearMem = linearAlloc(byteSize);
            if (linearMem) {
                memcpy(linearMem, decodedPcm, byteSize);
                DSP_FlushDataCache(linearMem, byteSize);
            }
            free(decodedPcm);
        }
    }

    if (freeOggData) free(oggData);

    // Кэшируем результат раз и навсегда
    cacheObj->isLoaded = true;
    cacheObj->soundIdx = soundIndex;

    if (!linearMem) return NULL;

    cacheObj->pcmData = linearMem;
    cacheObj->dataSize = byteSize;
    cacheObj->channels = channels;
    cacheObj->sampleRate = sampleRate;
    cacheObj->sampleCount = samples;
    cacheObj->bitsPerSample = bitsPerSample;

    return cacheObj;
}

static bool fillAndQueueBuffer(NdspSoundInstance* inst, int32_t bufferIndex) {
    if (!inst || !inst->vorbisStream) return false;

    ndspWaveBuf* wb = &inst->waveBufsStream[bufferIndex];
    if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) return true;

    short* dst = inst->streamBuffersLinear[bufferIndex];
    int samplesToRead = STREAM_BUFFER_SAMPLES;
    int samplesRead = stb_vorbis_get_samples_short_interleaved(inst->vorbisStream, inst->channels, dst, samplesToRead * inst->channels);

    // Бесшовный цикл
    if (samplesRead < samplesToRead && inst->isLooping) {
        stb_vorbis_seek_start(inst->vorbisStream);
        int remaining = samplesToRead - samplesRead;
        int extra = stb_vorbis_get_samples_short_interleaved(inst->vorbisStream, inst->channels, dst + (samplesRead * inst->channels), remaining * inst->channels);
        samplesRead += extra;
    }

    if (samplesRead == 0) {
        inst->endOfStream = true;
        return false;
    }

    memset(wb, 0, sizeof(*wb));
    wb->data_vaddr = dst;
    wb->nsamples = samplesRead;
    DSP_FlushDataCache(dst, samplesRead * inst->channels * sizeof(short));

    ndspChnWaveBufAdd((u32) inst->channelId, wb);
    return true;
}

// ===[ Vtable Implementations ]===

static void ndspSysInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    arrput(sys->base.audioGroups, dataWin);
    sys->fileSystem = fileSystem;
    sys->masterGain = 1.0f;

    if (R_FAILED(ndspInit())) {
        sys->isNdspReady = false;
        return;
    }
    sys->isNdspReady = true;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    for (int32_t i = 0; i < NDSP_MAX_CHANNELS; i++) {
        sys->channelOwner[i] = -1;
        sys->instances[i].channelId = -1;
        ndspChnReset((u32) i);
    }

    sys->nextInstanceCounter = 0;

    // Выделяем единый пул памяти без фрагментации (~768 KB Linear RAM)
    size_t poolBytes = NDSP_MAX_CHANNELS * STREAM_BUFFERS * STREAM_BUFFER_SAMPLES * 2 * sizeof(short);
    sys->streamBufPool = linearAlloc(poolBytes);

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        sys->instances[i].streamBuffersLinear[0] = (short*)((uint8_t*)sys->streamBufPool + (i * 2 + 0) * STREAM_BUFFER_SAMPLES * 4);
        sys->instances[i].streamBuffersLinear[1] = (short*)((uint8_t*)sys->streamBufPool + (i * 2 + 1) * STREAM_BUFFER_SAMPLES * 4);
    }

    sys->pcmCacheCount = dataWin->sond.count;
    sys->pcmCache = safeCalloc(sys->pcmCacheCount, sizeof(NdspPcmCache));
}

static void ndspSysDestroy(AudioSystem* audio) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (!sys->isNdspReady) return;

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        stopInstance(sys, &sys->instances[i]);
    }

    if (sys->streamBufPool) {
        linearFree(sys->streamBufPool);
        sys->streamBufPool = NULL;
    }

    if (sys->pcmCache) {
        for (uint32_t i = 0; i < sys->pcmCacheCount; i++) {
            if (sys->pcmCache[i].isLoaded && sys->pcmCache[i].pcmData) {
                linearFree(sys->pcmCache[i].pcmData);
            }
        }
        free(sys->pcmCache);
    }

    ndspExit();
    free(sys);
}

static void ndspSysUpdate(AudioSystem* audio, float deltaTime) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (!sys->isNdspReady) return;

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        NdspSoundInstance* inst = &sys->instances[i];
        if (!inst->active) continue;

        // Фейд громкости
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            updateChannelMix(sys, inst);
        }

        if (inst->isStreaming) {
            for (int b = 0; b < STREAM_BUFFERS; b++) {
                if (inst->waveBufsStream[b].status == NDSP_WBUF_DONE) {
                    fillAndQueueBuffer(inst, b);
                }
            }

            bool anyQueued = (inst->waveBufsStream[0].status == NDSP_WBUF_QUEUED || inst->waveBufsStream[0].status == NDSP_WBUF_PLAYING) ||
                             (inst->waveBufsStream[1].status == NDSP_WBUF_QUEUED || inst->waveBufsStream[1].status == NDSP_WBUF_PLAYING);

            if (inst->endOfStream && !anyQueued) {
                stopInstance(sys, inst);
            }
        } else {
            // Для обычных SFX: если не зациклен и проиграл
            if (!inst->isLooping && inst->waveBufSfx.status == NDSP_WBUF_DONE) {
                stopInstance(sys, inst);
            }
        }
    }
}

static int32_t ndspPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (!sys->isNdspReady || sys->masterGain <= 0.0001f) return -1;

    DataWin* dw = sys->base.audioGroups[0];
    if (soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) return -1;
    Sound* sound = &dw->sond.sounds[soundIndex];

    bool isEmbedded = (sound->flags & 0x01) != 0;
    AudioEntry* entry = NULL;

    if (isEmbedded) {
        int ag = sound->audioGroup;
        if (ag >= 0 && ag < arrlen(sys->base.audioGroups) && sys->base.audioGroups[ag] != NULL) {
            if (sound->audioFile >= 0 && sound->audioFile < (int32_t)sys->base.audioGroups[ag]->audo.count) {
                entry = &sys->base.audioGroups[ag]->audo.entries[sound->audioFile];
            }
        }
        if (!entry) return -1;
    }

    bool useStreaming = (!isEmbedded || (entry && entry->dataSize >= STREAMING_SIZE_THRESHOLD));

    int32_t channel = allocateChannel(sys);
    if (channel < 0) {
        int32_t victimIdx = selectVictimInstance(sys);
        if (victimIdx < 0 || sys->instances[victimIdx].priority > priority) return -1;
        stopInstance(sys, &sys->instances[victimIdx]);
        channel = allocateChannel(sys);
        if (channel < 0) return -1;
    }

    NdspSoundInstance* inst = allocateInstanceSlot(sys, priority);
    if (!inst) return -1;

    inst->active = true;
    inst->channelId = channel;
    inst->soundIndex = soundIndex;
    inst->instanceId = SOUND_INSTANCE_ID_BASE + sys->nextInstanceCounter++;
    inst->priority = priority;

    inst->sondVolume = sound->volume;
    inst->sondPitch = sound->pitch;
    if (inst->sondPitch <= 0.0f) inst->sondPitch = 1.0f;
    inst->currentGain = sound->volume;
    inst->targetGain = sound->volume;
    inst->pitch = 1.0f;

    inst->isLooping = loop;
    inst->isStreaming = useStreaming;
    inst->endOfStream = false;

    sys->channelOwner[channel] = (int32_t)(inst - sys->instances);

    ndspChnReset((u32)channel);
    ndspChnSetInterp((u32)channel, NDSP_INTERP_LINEAR); // Защита от краша Polyphase

    if (useStreaming) {
        int err = 0;
        if (isEmbedded) {
            void* streamData = entry->data;
            if (!streamData && entry->dataSize > 0) {
                char buf[64];
                if (sound->audioGroup == 0) snprintf(buf, sizeof(buf), "data.win");
                else snprintf(buf, sizeof(buf), "audiogroup%d.dat", sound->audioGroup);

                char* path = sys->fileSystem->vtable->resolvePath(sys->fileSystem, buf);
                if (path) {
                    FILE* f = fopen(path, "rb");
                    if (f) {
                        setvbuf(f, NULL, _IOFBF, 128 * 1024);
                        fseek(f, entry->dataOffset, SEEK_SET);
                        streamData = malloc(entry->dataSize);
                        fread(streamData, 1, entry->dataSize, f);
                        fclose(f);
                        inst->streamFileData = streamData; // Привязываем для очистки
                    }
                    free(path);
                }
            }
            if (streamData) inst->vorbisStream = stb_vorbis_open_memory(streamData, entry->dataSize, &err, NULL);
        } else {
            char filename[512];
            snprintf(filename, sizeof(filename), "%s%s", sound->file, strchr(sound->file, '.') ? "" : ".ogg");
            char* resolvedPath = sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
            if (resolvedPath) {
                inst->vorbisStream = stb_vorbis_open_filename(resolvedPath, &err, NULL);
                free(resolvedPath);
            }
        }

        if (!inst->vorbisStream) {
            stopInstance(sys, inst);
            return -1;
        }

        stb_vorbis_info info = stb_vorbis_get_info(inst->vorbisStream);
        inst->channels = info.channels;
        inst->sampleRate = info.sample_rate;

        ndspChnSetFormat((u32)channel, (inst->channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        updateChannelRate(inst);
        updateChannelMix(sys, inst);

        fillAndQueueBuffer(inst, 0);
        fillAndQueueBuffer(inst, 1);

    } else {
        NdspPcmCache* cache = getOrDecodeSfx(sys, soundIndex, entry, sound->audioGroup);
        if (!cache) {
            stopInstance(sys, inst);
            return -1;
        }

        inst->channels = cache->channels;
        inst->sampleRate = cache->sampleRate;

        u32 format;
        if (cache->bitsPerSample == 8) {
            format = (cache->channels == 2) ? NDSP_FORMAT_STEREO_PCM8 : NDSP_FORMAT_MONO_PCM8;
        } else {
            format = (cache->channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
        }
        ndspChnSetFormat((u32)channel, format);

        updateChannelRate(inst);
        updateChannelMix(sys, inst);

        memset(&inst->waveBufSfx, 0, sizeof(ndspWaveBuf));
        inst->waveBufSfx.data_vaddr = cache->pcmData;
        inst->waveBufSfx.nsamples = cache->sampleCount;
        inst->waveBufSfx.looping = loop;
        inst->waveBufSfx.status = NDSP_WBUF_FREE;

        ndspChnWaveBufAdd((u32)channel, &inst->waveBufSfx);
    }

    ndspChnSetPaused((u32)channel, false);
    return inst->instanceId;
}

static void ndspStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (!sys->isNdspReady) return;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        NdspSoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst) stopInstance(sys, inst);
    } else {
        for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
            if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) {
                stopInstance(sys, &sys->instances[i]);
            }
        }
    }
}

static void ndspStopAll(AudioSystem* audio) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (!sys->isNdspReady) return;
    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) stopInstance(sys, &sys->instances[i]);
}

static bool ndspIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (!sys->isNdspReady) return false;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        return findInstanceById(sys, soundOrInstance) != NULL;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) return true;
    }
    return false;
}

static void ndspPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        NdspSoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst) {
            inst->paused = true;
            ndspChnSetPaused((u32) inst->channelId, true);
        }
        return;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) {
            sys->instances[i].paused = true;
            ndspChnSetPaused((u32) sys->instances[i].channelId, true);
        }
    }
}

static void ndspResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        NdspSoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst) {
            inst->paused = false;
            ndspChnSetPaused((u32) inst->channelId, false);
        }
        return;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) {
            sys->instances[i].paused = false;
            ndspChnSetPaused((u32) sys->instances[i].channelId, false);
        }
    }
}

static void ndspPauseAll(AudioSystem* audio) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active) {
            sys->instances[i].paused = true;
            ndspChnSetPaused((u32) sys->instances[i].channelId, true);
        }
    }
}

static void ndspResumeAll(AudioSystem* audio) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active) {
            sys->instances[i].paused = false;
            ndspChnSetPaused((u32) sys->instances[i].channelId, false);
        }
    }
}

static void ndspSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (gain < 0.0f) gain = 0.0f;

    NdspSoundInstance* target = NULL;
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        target = findInstanceById(sys, soundOrInstance);
        if (!target) return;

        target->targetGain = gain;
        if (timeMs == 0) {
            target->currentGain = gain;
            target->fadeTimeRemaining = 0.0f;
            target->fadeTotalTime = 0.0f;
            updateChannelMix(sys, target);
        } else {
            target->startGain = target->currentGain;
            target->fadeTotalTime = (float) timeMs / 1000.0f;
            target->fadeTimeRemaining = target->fadeTotalTime;
        }
        return;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (!sys->instances[i].active || sys->instances[i].soundIndex != soundOrInstance) continue;

        sys->instances[i].targetGain = gain;
        if (timeMs == 0) {
            sys->instances[i].currentGain = gain;
            sys->instances[i].fadeTimeRemaining = 0.0f;
            sys->instances[i].fadeTotalTime = 0.0f;
            updateChannelMix(sys, &sys->instances[i]);
        } else {
            sys->instances[i].startGain = sys->instances[i].currentGain;
            sys->instances[i].fadeTotalTime = (float) timeMs / 1000.0f;
            sys->instances[i].fadeTimeRemaining = sys->instances[i].fadeTotalTime;
        }
    }
}

static float ndspGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        NdspSoundInstance* inst = findInstanceById(sys, soundOrInstance);
        return inst ? inst->currentGain : 0.0f;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) {
            return sys->instances[i].currentGain;
        }
    }
    return 0.0f;
}

static void ndspSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (pitch <= 0.0f) pitch = 0.01f;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        NdspSoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst) {
            inst->pitch = pitch;
            updateChannelRate(inst);
        }
        return;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) {
            sys->instances[i].pitch = pitch;
            updateChannelRate(&sys->instances[i]);
        }
    }
}

static float ndspGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        NdspSoundInstance* inst = findInstanceById(sys, soundOrInstance);
        return inst ? inst->pitch : 1.0f;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active && sys->instances[i].soundIndex == soundOrInstance) {
            return sys->instances[i].pitch;
        }
    }
    return 1.0f;
}

static float ndspGetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 0.0f;
}

static void ndspSetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float positionSeconds) {
}

static void ndspSetMasterGain(AudioSystem* audio, float gain) {
    NdspAudioSystem* sys = (NdspAudioSystem*) audio;
    if (gain < 0.0f) gain = 0.0f;
    sys->masterGain = gain;

    if (gain <= 0.0001f) {
        ndspStopAll(audio);
        return;
    }

    for (int i = 0; i < NDSP_MAX_CHANNELS; i++) {
        if (sys->instances[i].active) updateChannelMix(sys, &sys->instances[i]);
    }
}

static void ndspSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) { }

static void ndspGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);

        char* path = ((NdspAudioSystem*)audio)->fileSystem->vtable->resolvePath(((NdspAudioSystem*)audio)->fileSystem, buf);
        if (path) {
            DataWin *audioGroup = DataWin_parse(path, (DataWinParserOptions) {
                .parseAudo = true,
                .skipAudioBlobData = true
            });
            free(path);

            while (arrlen(audio->audioGroups) <= groupIndex) {
                arrput(audio->audioGroups, NULL);
            }
            audio->audioGroups[groupIndex] = audioGroup;
        }
    }
}

static bool ndspGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

static int32_t ndspCreateStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED const char* filename) { return -1; }
static bool ndspDestroyStream(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t streamIndex) { return false; }

static AudioSystemVtable ndspAudioSystemVtable = {
    .init = ndspSysInit,
    .destroy = ndspSysDestroy,
    .update = ndspSysUpdate,
    .playSound = ndspPlaySound,
    .stopSound = ndspStopSound,
    .stopAll = ndspStopAll,
    .isPlaying = ndspIsPlaying,
    .pauseSound = ndspPauseSound,
    .resumeSound = ndspResumeSound,
    .pauseAll = ndspPauseAll,
    .resumeAll = ndspResumeAll,
    .setSoundGain = ndspSetSoundGain,
    .getSoundGain = ndspGetSoundGain,
    .setSoundPitch = ndspSetSoundPitch,
    .getSoundPitch = ndspGetSoundPitch,
    .getTrackPosition = ndspGetTrackPosition,
    .setTrackPosition = ndspSetTrackPosition,
    .setMasterGain = ndspSetMasterGain,
    .setChannelCount = ndspSetChannelCount,
    .groupLoad = ndspGroupLoad,
    .groupIsLoaded = ndspGroupIsLoaded,
    .createStream = ndspCreateStream,
    .destroyStream = ndspDestroyStream,
};

NdspAudioSystem* NdspAudioSystem_create(void) {
    NdspAudioSystem* sys = safeCalloc(1, sizeof(NdspAudioSystem));
    sys->base.vtable = &ndspAudioSystemVtable;
    return sys;
}