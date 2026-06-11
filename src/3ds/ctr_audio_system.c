// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "ctr_audio_system.h"
#include "ctr_audio_cache.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include <3ds.h>
#include <tremor/ivorbisfile.h>
#include "stb_ds.h"

#define FORMAT_PCM16 1
#define FORMAT_ADPCM 2
#define CTR_INSTANCE_ID_BASE 100000
#define CTR_STREAM_MAX_BYTES (24u * 1024u * 1024u)
#define CTR_STREAM_TARGET_RATE 22050u

static inline bool is_stream_id(int32_t id) {
    return id >= CTR_STREAM_ID_BASE && id < (CTR_STREAM_ID_BASE + MAX_CTR_STREAMS);
}

static inline int stream_slot(int32_t id) {
    return (int)(id - CTR_STREAM_ID_BASE);
}

static CtrStream* stream_for_id(CtrAudioSystem* sys, int32_t id) {
    if (!is_stream_id(id)) return NULL;
    CtrStream* s = &sys->streams[stream_slot(id)];
    return s->used ? s : NULL;
}

#ifdef LOG_ALL
#define CTR_AUDIO_LOG(...) do { fprintf(stderr, "[CTR_AUDIO] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)
#else
#define CTR_AUDIO_LOG(...) ((void)0)
#endif

static volatile unsigned g_ctr_audio_mix_lane;

static void update_channel_mix(CtrAudioSystem* sys, int ch) {
    if (sys->chans[ch].currentSoundId == -1) return;
    int id = sys->chans[ch].currentSoundId;
    float baseVol = 1.0f;
    if (!is_stream_id(id) && sys->base.dataWin != NULL &&
        (uint32_t) id < sys->base.dataWin->sond.count) {
        baseVol = sys->base.dataWin->sond.sounds[id].volume;
        if (baseVol <= 0.0f) baseVol = 1.0f;
    }

    float chGain = sys->chans[ch].gain;
    float master = sys->masterGain;
    float mixGain = baseVol * chGain * master;
    if (mixGain > 1.0f) mixGain = 1.0f;

    float mix[12] = { mixGain, mixGain, 0.0f, 0.0f, 0,0,0,0,0,0,0,0 };
    g_ctr_audio_mix_lane ^= ((unsigned) ch + 0x19u) * 0x2C1B3C6Du;
    ndspChnSetMix(ch, mix);
}

static CtrCacheEntry* get_or_load_sound(CtrAudioSystem* sys, int32_t id) {
    if (id < 0) {
        CTR_AUDIO_LOG("load reject: id=%ld is negative", (long)id);
        return NULL;
    }
    if ((uint32_t)id >= sys->soundCount) {
        CTR_AUDIO_LOG("load reject: id=%ld outside soundCount=%lu", (long)id, (unsigned long)sys->soundCount);
        return NULL;
    }
    if (!sys->audioBin) {
        CTR_AUDIO_LOG("load reject: id=%ld has no audio.bin handle", (long)id);
        return NULL;
    }
    CtrSoundDef* def = &sys->soundBank[id];
    if (def->dataSize == 0) {
        CTR_AUDIO_LOG("load reject: id=%ld has empty data entry", (long)id);
        return NULL;
    }

    sys->frame++;

    for (int i = 0; i < MAX_LRU_CACHE; i++) {
        if (sys->cache[i].data != NULL && sys->cache[i].id == id) {
            sys->cache[i].lastAccess = sys->frame;
            CTR_AUDIO_LOG("cache hit: id=%ld slot=%d size=%lu", (long)id, i, (unsigned long)sys->cache[i].size);
            return &sys->cache[i];
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_LRU_CACHE; i++) {
        if (sys->cache[i].data == NULL) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        uint32_t oldestTime = 0xFFFFFFFF;
        for (int i = 0; i < MAX_LRU_CACHE; i++) {
            bool isPlaying = false;
            for (int ch = 0; ch < MAX_CTR_CHANNELS; ch++) {
                if (sys->chans[ch].currentSoundId == sys->cache[i].id) {
                    isPlaying = true;
                    break;
                }
            }
            if (!isPlaying && sys->cache[i].lastAccess < oldestTime) {
                oldestTime = sys->cache[i].lastAccess;
                slot = i;
            }
        }
    }

    if (slot == -1) {
        CTR_AUDIO_LOG("load reject: id=%ld no free LRU slot and all cached sounds are playing", (long)id);
        return NULL;
    }

    if (sys->cache[slot].data != NULL) {
        CTR_AUDIO_LOG("cache evict: slot=%d oldId=%ld oldSize=%lu", slot, (long)sys->cache[slot].id, (unsigned long)sys->cache[slot].size);
        linearFree(sys->cache[slot].data);
        sys->cache[slot].data = NULL;
    }

    void* buffer = linearMemAlign(def->dataSize, 0x80);
    if (!buffer) {
        CTR_AUDIO_LOG("load reject: id=%ld linearMemAlign failed size=%lu linearFree=%lu",
                      (long)id, (unsigned long)def->dataSize, (unsigned long)linearSpaceFree());
        return NULL;
    }

    if (fseek(sys->audioBin, (long)def->dataOffset, SEEK_SET) != 0) {
        CTR_AUDIO_LOG("load reject: id=%ld fseek offset=%lu failed errno=%d", (long)id, (unsigned long)def->dataOffset, errno);
        linearFree(buffer);
        return NULL;
    }
    size_t got = fread(buffer, 1, def->dataSize, sys->audioBin);
    if (got != def->dataSize) {
        CTR_AUDIO_LOG("load reject: id=%ld short read got=%lu expected=%lu offset=%lu errno=%d",
                      (long)id, (unsigned long)got, (unsigned long)def->dataSize, (unsigned long)def->dataOffset, errno);
        linearFree(buffer);
        return NULL;
    }
    DSP_FlushDataCache(buffer, def->dataSize);

    sys->cache[slot].id = id;
    sys->cache[slot].data = buffer;
    sys->cache[slot].size = def->dataSize;
    sys->cache[slot].lastAccess = sys->frame;
    CTR_AUDIO_LOG("cache load: id=%ld slot=%d offset=%lu size=%lu rate=%lu frames=%lu channels=%u format=%u stream=%d",
                  (long)id, slot, (unsigned long)def->dataOffset, (unsigned long)def->dataSize,
                  (unsigned long)def->sampleRate, (unsigned long)def->totalFrames,
                  (unsigned)def->channels, (unsigned)def->format, def->preferStream ? 1 : 0);

    return &sys->cache[slot];
}

static void ctr_init(AudioSystem *base, MAYBE_UNUSED DataWin *dw, FileSystem *fs) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    sys->base.dataWin = dw;
    sys->fs = fs;
    sys->masterGain = 1.0f;
    sys->nextInstanceId = CTR_INSTANCE_ID_BASE;
    CTR_AUDIO_LOG("init: dw=%p fs=%p", (void*)dw, (void*)fs);
    arrput(sys->base.audioGroups, dw);

    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        sys->chans[i].currentSoundId = -1;
        sys->chans[i].instanceId = -1;
    }
    for (int i = 0; i < MAX_LRU_CACHE; i++) sys->cache[i].data = NULL;

    char* path = NULL;
    if (fs && fs->vtable && fs->vtable->resolvePath) {
        path = fs->vtable->resolvePath(fs, "cache/audio.bin");
    }
    if (path) {
        CTR_AUDIO_LOG("opening audio cache: %s", path);
        sys->audioBin = fopen(path, "rb");
        if (!sys->audioBin) {
            CTR_AUDIO_LOG("audio cache open failed: %s errno=%d", path, errno);
        }
        free(path);
    } else {
        CTR_AUDIO_LOG("audio cache path resolve failed");
    }

    if (sys->audioBin) {
        CtrAudioCacheHeader hdr;
        size_t headerRead = fread(&hdr, 1, sizeof(hdr), sys->audioBin);
        if (headerRead != sizeof(hdr)) {
            CTR_AUDIO_LOG("audio cache header short read got=%lu expected=%lu errno=%d",
                          (unsigned long)headerRead, (unsigned long)sizeof(hdr), errno);
            fclose(sys->audioBin);
            sys->audioBin = NULL;
        } else {
            CTR_AUDIO_LOG("audio cache header: magic=0x%08lX version=%lu sounds=%lu index=%lu data=%lu dataSize=%lu srcSize=%llu hash=0x%08lX",
                          (unsigned long)hdr.magic, (unsigned long)hdr.version, (unsigned long)hdr.soundCount,
                          (unsigned long)hdr.indexOffset, (unsigned long)hdr.dataOffset, (unsigned long)hdr.dataSize,
                          (unsigned long long)hdr.srcSize, (unsigned long)hdr.srcSampleHash);
            if (hdr.magic != CTR_AUDIO_CACHE_MAGIC || hdr.version != CTR_AUDIO_CACHE_VERSION) {
                CTR_AUDIO_LOG("audio cache rejected: expected magic=0x%08lX version=%u",
                              (unsigned long)CTR_AUDIO_CACHE_MAGIC, CTR_AUDIO_CACHE_VERSION);
                fclose(sys->audioBin);
                sys->audioBin = NULL;
            } else {
                sys->soundCount = hdr.soundCount;
                sys->soundBank = calloc(sys->soundCount, sizeof(CtrSoundDef));
                if (!sys->soundBank) {
                    CTR_AUDIO_LOG("audio cache rejected: soundBank calloc failed count=%lu", (unsigned long)sys->soundCount);
                    fclose(sys->audioBin);
                    sys->audioBin = NULL;
                    sys->soundCount = 0;
                } else if (fseek(sys->audioBin, (long)hdr.indexOffset, SEEK_SET) != 0) {
                    CTR_AUDIO_LOG("audio cache index seek failed index=%lu errno=%d", (unsigned long)hdr.indexOffset, errno);
                    free(sys->soundBank);
                    sys->soundBank = NULL;
                    fclose(sys->audioBin);
                    sys->audioBin = NULL;
                    sys->soundCount = 0;
                } else {
                    uint32_t nonEmpty = 0;
                    for (uint32_t i = 0; i < sys->soundCount; i++) {
                        CtrAudioEntry entryRaw;
                        size_t entryRead = fread(&entryRaw, 1, sizeof(CtrAudioEntry), sys->audioBin);
                        if (entryRead != sizeof(CtrAudioEntry)) {
                            CTR_AUDIO_LOG("audio cache index short read at id=%lu got=%lu expected=%lu errno=%d",
                                          (unsigned long)i, (unsigned long)entryRead,
                                          (unsigned long)sizeof(CtrAudioEntry), errno);
                            sys->soundCount = i;
                            break;
                        }

                        sys->soundBank[i].dataOffset   = entryRaw.dataOffset;
                        sys->soundBank[i].dataSize     = entryRaw.dataSize;
                        sys->soundBank[i].sampleRate   = entryRaw.sampleRate;
                        sys->soundBank[i].totalFrames  = entryRaw.totalFrames;
                        sys->soundBank[i].channels     = entryRaw.channels;
                        sys->soundBank[i].format       = entryRaw.format;
                        sys->soundBank[i].preferStream = (entryRaw.flags & CTR_AUDIO_FLAG_PREFER_STREAM) != 0;
                        memcpy(sys->soundBank[i].adpcmCoefs, entryRaw.adpcmCoefs, sizeof(u16)*16);

                        if (entryRaw.dataSize > 0) {
                            nonEmpty++;
                            CTR_AUDIO_LOG("audio entry[%lu]: offset=%lu size=%lu rate=%lu frames=%lu channels=%u format=%u flags=0x%02X",
                                          (unsigned long)i, (unsigned long)entryRaw.dataOffset, (unsigned long)entryRaw.dataSize,
                                          (unsigned long)entryRaw.sampleRate, (unsigned long)entryRaw.totalFrames,
                                          (unsigned)entryRaw.channels, (unsigned)entryRaw.format, (unsigned)entryRaw.flags);
                        }
                    }
                    CTR_AUDIO_LOG("audio cache ready: sounds=%lu nonEmpty=%lu",
                                  (unsigned long)sys->soundCount, (unsigned long)nonEmpty);
                }
            }
        }
    } else {
        CTR_AUDIO_LOG("audio cache disabled: cache/audio.bin unavailable");
    }

    Result ndspRc = ndspInit();
    CTR_AUDIO_LOG("ndspInit rc=0x%08lX", (unsigned long)ndspRc);
    (void)ndspRc;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
}

static void ctr_destroy(AudioSystem *base) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    CTR_AUDIO_LOG("destroy");
    ndspExit();
    for (int i = 0; i < MAX_LRU_CACHE; i++) if (sys->cache[i].data) linearFree(sys->cache[i].data);
    for (int i = 0; i < MAX_CTR_STREAMS; i++) {
        if (sys->streams[i].used && sys->streams[i].data) {
            linearFree(sys->streams[i].data);
            sys->streams[i].used = false;
        }
    }
    if (sys->base.audioGroups != NULL) {
        for (int32_t i = 1; i < (int32_t) arrlen(sys->base.audioGroups); i++) {
            if (sys->base.audioGroups[i] != NULL) {
                DataWin_free(sys->base.audioGroups[i]);
            }
        }
        arrfree(sys->base.audioGroups);
    }
    if (sys->audioBin) fclose(sys->audioBin);
    if (sys->soundBank) free(sys->soundBank);
    free(sys);
}

static void ctr_update(AudioSystem* sys_base, MAYBE_UNUSED float dt) {
    CtrAudioSystem *sys = (CtrAudioSystem *) sys_base;
    sys->frame++;

    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId != -1) {
            bool isDone = (sys->chans[i].waveBuf.status == NDSP_WBUF_DONE);

            if (isDone && !sys->chans[i].loop) {
                CTR_AUDIO_LOG("channel done: ch=%d sound=%ld instance=%ld",
                              i, (long)sys->chans[i].currentSoundId, (long)sys->chans[i].instanceId);
                sys->chans[i].currentSoundId = -1;
                sys->chans[i].instanceId = -1;
            }
        }
    }
}

static int32_t ctr_play(AudioSystem *base, int32_t id, int32_t prio, bool loop) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    CTR_AUDIO_LOG("play request: id=%ld prio=%ld loop=%d soundCount=%lu audioBin=%p",
                  (long)id, (long)prio, loop ? 1 : 0, (unsigned long)sys->soundCount, (void*)sys->audioBin);
    CtrCacheEntry* entry = NULL;
    CtrSoundDef* def = NULL;
    CtrSoundDef streamDef;
    CtrCacheEntry streamEntry;
    if (is_stream_id(id)) {
        CtrStream* st = stream_for_id(sys, id);
        if (!st) {
            CTR_AUDIO_LOG("play failed: stream id=%ld not bound", (long) id);
            return -1;
        }
        memset(&streamDef, 0, sizeof(streamDef));
        streamDef.dataOffset   = 0;
        streamDef.dataSize     = st->dataSize;
        streamDef.sampleRate   = st->sampleRate;
        streamDef.totalFrames  = st->totalFrames;
        streamDef.channels     = st->channels;
        streamDef.format       = st->format;
        streamDef.preferStream = true;
        def = &streamDef;

        streamEntry.id         = id;
        streamEntry.data       = st->data;
        streamEntry.size       = st->dataSize;
        streamEntry.lastAccess = sys->frame;
        entry = &streamEntry;
    } else {
        entry = get_or_load_sound(sys, id);
        if (!entry) {
            CTR_AUDIO_LOG("play failed: id=%ld cache/load rejected", (long)id);
            return -1;
        }
        def = &sys->soundBank[id];
    }

    float lengthSec = (def->sampleRate > 0) ? ((float)def->totalFrames / def->sampleRate) : 0.0f;
    bool isMusic = def->preferStream || (lengthSec > 5.0f);
    int score_prio = prio + (loop ? 1000 : 0) + (isMusic ? 50000 : 0);

    int ch = -1;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == -1) { ch = i; break; }
    }

    if (ch == -1) {
        int victim = -1;
        int lowestScore = 0x7FFFFFFF;
        uint32_t oldestVictimTime = 0xFFFFFFFF;

        for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
            int candScore = sys->chans[i].priority;
            uint32_t candTime = sys->chans[i].playFrame;

            if (candScore < lowestScore || (candScore == lowestScore && candTime < oldestVictimTime)) {
                lowestScore = candScore;
                oldestVictimTime = candTime;
                victim = i;
            }
        }

        if (victim != -1 && score_prio >= lowestScore) {
            CTR_AUDIO_LOG("play steal: id=%ld victim=%d oldSound=%ld oldPrio=%d newPrio=%d",
                          (long)id, victim, (long)sys->chans[victim].currentSoundId, lowestScore, score_prio);
            ndspChnWaveBufClear(victim);
            ch = victim;
        }
    }

    if (ch != -1) {
        CtrChannelState *chan = &sys->chans[ch];
        ndspChnReset(ch);

        uint16_t ndspFormat = NDSP_FORMAT_MONO_PCM16;
        if (def->format == FORMAT_ADPCM) {
            ndspFormat = NDSP_FORMAT_MONO_ADPCM;
        } else if (def->channels == 2) {
            ndspFormat = NDSP_FORMAT_STEREO_PCM16;
        }
        ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
        ndspChnSetRate(ch, (float)def->sampleRate);
        ndspChnSetFormat(ch, ndspFormat);
        if (def->format == FORMAT_ADPCM) {
            ndspChnSetAdpcmCoefs(ch, def->adpcmCoefs);
        }

        memset(&chan->waveBuf, 0, sizeof(ndspWaveBuf));
        chan->waveBuf.data_vaddr = entry->data;
        chan->waveBuf.nsamples = def->totalFrames;
        if (def->format != FORMAT_ADPCM && def->channels == 2) {
            chan->waveBuf.nsamples = def->totalFrames * 2u;
        }
        chan->waveBuf.looping = loop;

        if (def->format == FORMAT_ADPCM) {
            memset(&chan->adpcmState, 0, sizeof(ndspAdpcmData));
            chan->waveBuf.adpcm_data = &chan->adpcmState;
        }

        chan->currentSoundId = id;
        chan->instanceId     = sys->nextInstanceId++;
        chan->priority       = score_prio;
        chan->loop           = loop;
        chan->gain           = 1.0f;
        chan->pitch          = 1.0f;
        chan->playFrame      = sys->frame;

        update_channel_mix(sys, ch);
        DSP_FlushDataCache(entry->data, entry->size);
        ndspChnSetPaused(ch, false);
        ndspChnWaveBufAdd(ch, &chan->waveBuf);
        CTR_AUDIO_LOG("play ok: id=%ld instance=%ld ch=%d format=0x%04X rate=%lu frames=%lu samples=%lu gain=%.3f playing=%d",
                      (long)id, (long)chan->instanceId, ch, (unsigned)ndspFormat,
                      (unsigned long)def->sampleRate, (unsigned long)def->totalFrames,
                      (unsigned long)chan->waveBuf.nsamples, chan->gain,
                      ndspChnIsPlaying(ch) ? 1 : 0);

        return chan->instanceId;
    }
    CTR_AUDIO_LOG("play failed: id=%ld no channel accepted newPrio=%d", (long)id, score_prio);
    return -1;
}
static void ctr_stop(AudioSystem *base, int32_t soundOrInstance) {
    if (soundOrInstance < 0) return;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == soundOrInstance || sys->chans[i].instanceId == soundOrInstance) {
            CTR_AUDIO_LOG("stop: target=%ld ch=%d sound=%ld instance=%ld",
                          (long)soundOrInstance, i, (long)sys->chans[i].currentSoundId, (long)sys->chans[i].instanceId);
            ndspChnWaveBufClear(i);
            sys->chans[i].currentSoundId = -1;
            sys->chans[i].instanceId = -1;
        }
    }
}

static void ctr_stop_all(AudioSystem *base) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    CTR_AUDIO_LOG("stop all");
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        ndspChnWaveBufClear(i);
        sys->chans[i].currentSoundId = -1;
        sys->chans[i].instanceId = -1;
    }
}

static bool ctr_is_playing(AudioSystem *base, int32_t id) {
    if (id < 0) return false;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            return true;
        }
    }
    return false;
}

static void ctr_pause(AudioSystem *base, int32_t id) {
    if (id < 0) return;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            CTR_AUDIO_LOG("pause: target=%ld ch=%d", (long)id, i);
            ndspChnSetPaused(i, true);
        }
    }
}

static void ctr_resume(AudioSystem *base, int32_t id) {
    if (id < 0) return;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            CTR_AUDIO_LOG("resume: target=%ld ch=%d", (long)id, i);
            ndspChnSetPaused(i, false);
        }
    }
}

static void ctr_pause_all(MAYBE_UNUSED AudioSystem *base) {
    CTR_AUDIO_LOG("pause all");
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) ndspChnSetPaused(i, true);
}

static void ctr_resume_all(MAYBE_UNUSED AudioSystem *base) {
    CTR_AUDIO_LOG("resume all");
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) ndspChnSetPaused(i, false);
}

static void ctr_set_gain(AudioSystem *base, int32_t id, float g, MAYBE_UNUSED uint32_t t) {
    if (id < 0) return;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            CTR_AUDIO_LOG("gain: target=%ld ch=%d gain=%.3f tween=%lu", (long)id, i, g, (unsigned long)t);
            sys->chans[i].gain = g;
            update_channel_mix(sys, i);
        }
    }
}

static float ctr_get_gain(AudioSystem *base, int32_t id) {
    if (id < 0) return 0.0f;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) return sys->chans[i].gain;
    }
    return 0.0f;
}

static uint32_t base_sample_rate_for(CtrAudioSystem* sys, int32_t soundOrInstance) {
    int32_t target = soundOrInstance;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].instanceId == target) {
            target = sys->chans[i].currentSoundId;
            break;
        }
    }
    if (is_stream_id(target)) {
        CtrStream* st = stream_for_id(sys, target);
        return st ? st->sampleRate : 0;
    }
    if (target >= 0 && (uint32_t) target < sys->soundCount) {
        return sys->soundBank[target].sampleRate;
    }
    return 0;
}

static uint32_t total_frames_for(CtrAudioSystem* sys, int32_t soundOrInstance) {
    int32_t target = soundOrInstance;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].instanceId == target) {
            target = sys->chans[i].currentSoundId;
            break;
        }
    }
    if (is_stream_id(target)) {
        CtrStream* st = stream_for_id(sys, target);
        return st ? st->totalFrames : 0;
    }
    if (target >= 0 && (uint32_t) target < sys->soundCount) {
        return sys->soundBank[target].totalFrames;
    }
    return 0;
}

static void ctr_set_pitch(AudioSystem *base, int32_t id, float p) {
    if (id < 0) return;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            CTR_AUDIO_LOG("pitch: target=%ld ch=%d pitch=%.3f", (long)id, i, p);
            sys->chans[i].pitch = p;
            uint32_t hardwareBaseRate = base_sample_rate_for(sys, sys->chans[i].currentSoundId);
            if (hardwareBaseRate > 0) {
                ndspChnSetRate(i, (float) hardwareBaseRate * p);
            }
        }
    }
}

static float ctr_get_pitch(AudioSystem *base, int32_t id) {
    if (id < 0) return 1.0f;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) return sys->chans[i].pitch;
    }
    return 1.0f;
}

static float ctr_get_pos(AudioSystem *base, int32_t id) {
    if (id < 0) return 0.0f;
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            uint32_t rate = base_sample_rate_for(sys, sys->chans[i].currentSoundId);
            if (rate == 0) return 0.0f;
            return (float) ndspChnGetSamplePos(i) / (float) rate;
        }
    }
    return 0.0f;
}
static bool resolve_channel_payload(CtrAudioSystem* sys, int ch,
                                    const void** outData, uint32_t* outDataSize,
                                    uint32_t* outSampleRate, uint32_t* outTotalFrames,
                                    uint8_t* outChannels, uint8_t* outFormat) {
    int32_t id = sys->chans[ch].currentSoundId;
    if (id < 0) return false;

    if (is_stream_id(id)) {
        CtrStream* st = stream_for_id(sys, id);
        if (!st) return false;
        *outData         = st->data;
        *outDataSize     = st->dataSize;
        *outSampleRate   = st->sampleRate;
        *outTotalFrames  = st->totalFrames;
        *outChannels     = st->channels;
        *outFormat       = st->format;
        return true;
    }

    if ((uint32_t) id >= sys->soundCount || sys->soundBank == NULL) return false;
    void* data = NULL;
    uint32_t size = 0;
    for (int i = 0; i < MAX_LRU_CACHE; i++) {
        if (sys->cache[i].data != NULL && sys->cache[i].id == id) {
            data = sys->cache[i].data;
            size = sys->cache[i].size;
            break;
        }
    }
    if (!data) return false;

    CtrSoundDef* def = &sys->soundBank[id];
    *outData         = data;
    *outDataSize     = size;
    *outSampleRate   = def->sampleRate;
    *outTotalFrames  = def->totalFrames;
    *outChannels     = def->channels;
    *outFormat       = def->format;
    return true;
}
static void ctr_set_pos(AudioSystem* base, int32_t id, float pos) {
    if (id < 0) return;
    CtrAudioSystem* sys = (CtrAudioSystem*) base;
    if (pos < 0.0f) pos = 0.0f;

    int ch = -1;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            ch = i;
            break;
        }
    }
    if (ch < 0) {
        CTR_AUDIO_LOG("set_pos: no channel for target=%ld pos=%.3f", (long) id, pos);
        return;
    }

    const void* data = NULL;
    uint32_t dataSize = 0, sampleRate = 0, totalFrames = 0;
    uint8_t channels = 0, format = 0;
    if (!resolve_channel_payload(sys, ch,
                                 &data, &dataSize, &sampleRate, &totalFrames,
                                 &channels, &format)) {
        CTR_AUDIO_LOG("set_pos: cannot resolve payload ch=%d target=%ld", ch, (long) id);
        return;
    }
    if (sampleRate == 0 || totalFrames == 0 || data == NULL) {
        CTR_AUDIO_LOG("set_pos: invalid payload (rate=%lu frames=%lu data=%p)",
                      (unsigned long) sampleRate, (unsigned long) totalFrames, data);
        return;
    }
    uint64_t target64 = (uint64_t) (pos * (double) sampleRate);
    if (totalFrames > 0 && target64 >= totalFrames) target64 = totalFrames - 1;
    uint32_t targetFrame = (uint32_t) target64;
    uint32_t byteOffset   = 0;
    uint32_t remainingFrm = totalFrames - targetFrame;
    if (format == FORMAT_ADPCM) {
        uint32_t blockIdx = targetFrame / 14u;
        targetFrame       = blockIdx * 14u;
        byteOffset        = blockIdx * 8u;
        remainingFrm      = totalFrames - targetFrame;
    } else {
        byteOffset = targetFrame * (uint32_t) channels * 2u;
    }
    if (byteOffset >= dataSize) {
        CTR_AUDIO_LOG("set_pos: byteOffset=%lu past buffer size=%lu, ignoring",
                      (unsigned long) byteOffset, (unsigned long) dataSize);
        return;
    }

    CtrChannelState* chan = &sys->chans[ch];
    bool wasLooping = chan->loop;
    ndspChnWaveBufClear(ch);
    memset(&chan->waveBuf, 0, sizeof(chan->waveBuf));
    chan->waveBuf.data_vaddr = (const uint8_t*) data + byteOffset;
    if (format == FORMAT_ADPCM) {
        chan->waveBuf.nsamples = remainingFrm;
        memset(&chan->adpcmState, 0, sizeof(chan->adpcmState));
        chan->waveBuf.adpcm_data = &chan->adpcmState;
    } else if (channels == 2) {
        chan->waveBuf.nsamples = remainingFrm * 2u;
    } else {
        chan->waveBuf.nsamples = remainingFrm;
    }
    chan->waveBuf.looping = false;
    if (wasLooping) {
        memset(&chan->loopWaveBuf, 0, sizeof(chan->loopWaveBuf));
        chan->loopWaveBuf.data_vaddr = data;
        if (format == FORMAT_ADPCM) {
            chan->loopWaveBuf.nsamples   = totalFrames;
            memset(&chan->loopAdpcmState, 0, sizeof(chan->loopAdpcmState));
            chan->loopWaveBuf.adpcm_data = &chan->loopAdpcmState;
        } else if (channels == 2) {
            chan->loopWaveBuf.nsamples = totalFrames * 2u;
        } else {
            chan->loopWaveBuf.nsamples = totalFrames;
        }
        chan->loopWaveBuf.looping = true;
        chan->hasLoopTail = true;
    } else {
        chan->hasLoopTail = false;
        memset(&chan->loopWaveBuf, 0, sizeof(chan->loopWaveBuf));
    }
    DSP_FlushDataCache((void*) data, dataSize);
    ndspChnSetPaused(ch, false);
    ndspChnWaveBufAdd(ch, &chan->waveBuf);
    if (chan->hasLoopTail) {
        ndspChnWaveBufAdd(ch, &chan->loopWaveBuf);
    }

    chan->playFrame = sys->frame;
    update_channel_mix(sys, ch);

    CTR_AUDIO_LOG("set_pos: target=%ld ch=%d pos=%.3fs frame=%u/%u offset=%uB remaining=%u%s playing=%d",
                  (long) id, ch, pos,
                  (unsigned) targetFrame, (unsigned) totalFrames,
                  (unsigned) byteOffset, (unsigned) remainingFrm,
                  chan->hasLoopTail ? " (+loop tail)" : "",
                  ndspChnIsPlaying(ch) ? 1 : 0);
}

static float ctr_get_length(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    if (is_stream_id(id)) {
        CtrStream* st = stream_for_id(sys, id);
        if (!st || st->sampleRate == 0) return 0.0f;
        return (float) st->totalFrames / (float) st->sampleRate;
    }
    uint32_t rate   = base_sample_rate_for(sys, id);
    uint32_t frames = total_frames_for(sys, id);
    if (rate > 0 && frames > 0) {
        return (float) frames / (float) rate;
    }
    if (id >= 0 && id < (int32_t)sys->soundCount) {
        float fsRate = (float)sys->soundBank[id].sampleRate;
        if (fsRate > 0.0f) return (float)sys->soundBank[id].totalFrames / fsRate;
    }
    return 0.0f;
}

static void ctr_set_master(AudioSystem *base, float g) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    if (g <= 0.0f) {
        fprintf(stderr, "[CTR_AUDIO] WARNING: master gain set to %.3f -- all audio is now MUTED. "
                        "If this isn't intentional, check the game's options ini (dr.ini, config.ini, ...).\n", g);
        fflush(stderr);
    }
    CTR_AUDIO_LOG("master gain: %.3f", g);
    sys->masterGain = g;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId != -1) update_channel_mix(sys, i);
    }
}

static void ctr_set_chans(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t cnt) {
    CTR_AUDIO_LOG("set channel count ignored: %ld", (long)cnt);
}
static void ctr_grp_load(AudioSystem *base, int32_t grp) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    if (grp < 0) {
        CTR_AUDIO_LOG("group load: invalid index %ld", (long) grp);
        return;
    }
    if (grp == 0) {
        CTR_AUDIO_LOG("group load: %ld is the main data.win, already loaded", (long) grp);
        return;
    }
    if (sys->base.audioGroups != NULL && (int32_t) arrlen(sys->base.audioGroups) > grp &&
        sys->base.audioGroups[grp] != NULL) {
        CTR_AUDIO_LOG("group load: %ld already loaded (audo count=%lu)",
                      (long) grp,
                      (unsigned long) sys->base.audioGroups[grp]->audo.count);
        return;
    }
    while ((int32_t) arrlen(sys->base.audioGroups) <= grp) {
        arrput(sys->base.audioGroups, NULL);
    }

    DataWin *parsed = NULL;
    if (sys->fs && sys->fs->vtable && sys->fs->vtable->resolvePath) {
        char relPath[32];
        snprintf(relPath, sizeof(relPath), "audiogroup%ld.dat", (long) grp);
        char* absPath = sys->fs->vtable->resolvePath(sys->fs, relPath);
        if (absPath) {
            DataWinParserOptions opts = {
                .parseGen8 = true,
                .parseAudo = true,
                .skipTextureBlobData = true,
                .skipAudioBlobData = true,
            };
            parsed = DataWin_parse(absPath, opts);
            if (parsed != NULL) {
                CTR_AUDIO_LOG("group load %ld: parsed '%s' audo=%lu",
                              (long) grp, absPath, (unsigned long) parsed->audo.count);
            } else {
                CTR_AUDIO_LOG("group load %ld: '%s' missing or unparseable (offline cache supplies samples)",
                              (long) grp, absPath);
            }
            free(absPath);
        }
    }

    sys->base.audioGroups[grp] = parsed;
}

static bool ctr_grp_loaded(AudioSystem *base, int32_t grp) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    if (grp < 0) return false;
    bool loaded = sys->base.audioGroups != NULL &&
                  (int32_t) arrlen(sys->base.audioGroups) > grp;
    CTR_AUDIO_LOG("group loaded query: %ld -> %s",
                  (long) grp, loaded ? "true" : "false");
    return loaded;
}

static int16_t* decode_wav_to_pcm16(FILE* f, uint32_t* outRate, uint8_t* outCh, uint32_t* outFrames) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 44) return NULL;
    fseek(f, 0, SEEK_SET);

    uint8_t* raw = malloc((size_t) sz);
    if (!raw) return NULL;
    if (fread(raw, 1, (size_t) sz, f) != (size_t) sz) {
        free(raw);
        return NULL;
    }

    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) {
        free(raw);
        return NULL;
    }

    uint16_t fmtTag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* dataPtr = NULL;
    uint32_t dataLen = 0;
    uint32_t off = 12;
    while (off + 8 <= (uint32_t) sz) {
        const char* id = (const char*) (raw + off);
        uint32_t csz = (uint32_t) raw[off + 4] | ((uint32_t) raw[off + 5] << 8) |
                       ((uint32_t) raw[off + 6] << 16) | ((uint32_t) raw[off + 7] << 24);
        off += 8;
        if (off + csz > (uint32_t) sz) break;
        if (memcmp(id, "fmt ", 4) == 0 && csz >= 16) {
            fmtTag   = (uint16_t)(raw[off + 0] | (raw[off + 1] << 8));
            channels = (uint16_t)(raw[off + 2] | (raw[off + 3] << 8));
            rate     = (uint32_t)(raw[off + 4] | (raw[off + 5] << 8) | (raw[off + 6] << 16) | (raw[off + 7] << 24));
            bits     = (uint16_t)(raw[off + 14] | (raw[off + 15] << 8));
        } else if (memcmp(id, "data", 4) == 0) {
            dataPtr = raw + off;
            dataLen = csz;
            break;
        }
        off += csz + (csz & 1);
    }

    if (!dataPtr || fmtTag != 1 || channels < 1 || channels > 2 || (bits != 8 && bits != 16)) {
        free(raw);
        return NULL;
    }

    uint32_t bytesPerFrame = (bits / 8) * channels;
    if (bytesPerFrame == 0) {
        free(raw);
        return NULL;
    }
    uint32_t frames = dataLen / bytesPerFrame;
    int16_t* pcm = malloc((size_t) frames * channels * sizeof(int16_t));
    if (!pcm) {
        free(raw);
        return NULL;
    }
    if (bits == 16) {
        memcpy(pcm, dataPtr, (size_t) frames * bytesPerFrame);
    } else {
        for (uint32_t i = 0; i < frames * channels; i++) {
            int16_t s = (int16_t)((int32_t) dataPtr[i] - 128);
            pcm[i] = (int16_t)(s << 8);
        }
    }
    free(raw);

    *outRate   = rate;
    *outCh     = (uint8_t) channels;
    *outFrames = frames;
    return pcm;
}

static int16_t* decode_ogg_to_pcm16(FILE* f, uint32_t* outRate, uint8_t* outCh, uint32_t* outFrames) {
    OggVorbis_File vf;
    if (ov_open(f, &vf, NULL, 0) < 0) return NULL;

    vorbis_info* vi = ov_info(&vf, -1);
    if (!vi || vi->channels < 1 || vi->channels > 2) {
        ov_clear(&vf);
        return NULL;
    }

    ogg_int64_t total = ov_pcm_total(&vf, -1);
    if (total <= 0 || total > 0x40000000) {
        ov_clear(&vf);
        return NULL;
    }

    size_t totalBytes = (size_t) total * (size_t) vi->channels * sizeof(int16_t);
    int16_t* pcm = malloc(totalBytes);
    if (!pcm) {
        ov_clear(&vf);
        return NULL;
    }

    uint8_t* cursor = (uint8_t*) pcm;
    size_t remaining = totalBytes;
    int currentSection = 0;
    while (remaining > 0) {
        long got = ov_read(&vf, (char*) cursor, (int) (remaining > 4096 ? 4096 : remaining), &currentSection);
        if (got <= 0) break;
        cursor   += got;
        remaining -= (size_t) got;
    }

    *outRate   = (uint32_t) vi->rate;
    *outCh     = (uint8_t) vi->channels;
    *outFrames = (uint32_t)((cursor - (uint8_t*) pcm) / (vi->channels * sizeof(int16_t)));

    ov_clear(&vf);
    return pcm;
}
static int16_t* decode_external_audio(FILE* f, uint32_t* outRate, uint8_t* outCh, uint32_t* outFrames) {
    uint8_t magic[4] = {0};
    if (fread(magic, 1, 4, f) != 4) return NULL;
    rewind(f);
    if (memcmp(magic, "OggS", 4) == 0) {
        return decode_ogg_to_pcm16(f, outRate, outCh, outFrames);
    }
    if (memcmp(magic, "RIFF", 4) == 0) {
        return decode_wav_to_pcm16(f, outRate, outCh, outFrames);
    }
    return NULL;
}

static int32_t ctr_create_stream(AudioSystem* base, const char* filename) {
    CtrAudioSystem* sys = (CtrAudioSystem*) base;
    if (!filename || !*filename) {
        CTR_AUDIO_LOG("create_stream: empty filename");
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_CTR_STREAMS; i++) {
        if (!sys->streams[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        CTR_AUDIO_LOG("create_stream: no free slot for '%s' (max=%d)", filename, MAX_CTR_STREAMS);
        return -1;
    }

    char* resolvedPath = NULL;
    if (sys->fs && sys->fs->vtable && sys->fs->vtable->resolvePath) {
        resolvedPath = sys->fs->vtable->resolvePath(sys->fs, filename);
    }
    const char* openPath = resolvedPath ? resolvedPath : filename;

    FILE* f = fopen(openPath, "rb");
    if (!f) {
        CTR_AUDIO_LOG("create_stream: fopen('%s') failed errno=%d", openPath, errno);
        free(resolvedPath);
        return -1;
    }

    uint32_t rate = 0, frames = 0;
    uint8_t channels = 0;
    int16_t* pcm = decode_external_audio(f, &rate, &channels, &frames);
    if (!pcm) {
        fclose(f);
        CTR_AUDIO_LOG("create_stream: decode failed for '%s'", openPath);
        free(resolvedPath);
        return -1;
    }
    {
        uint8_t magic[4] = {0};
        (void) magic;
    }
    if (ftell(f) >= 0) {
        fclose(f);
    }

    if (channels < 1 || channels > 2 || frames == 0 || rate == 0) {
        free(pcm);
        free(resolvedPath);
        CTR_AUDIO_LOG("create_stream: invalid decoded params (ch=%u rate=%u frames=%u)",
                      (unsigned) channels, (unsigned) rate, (unsigned) frames);
        return -1;
    }
    {
        uint32_t srcRate = rate;
        uint8_t  srcCh   = channels;
        uint32_t srcFrames = frames;
        int16_t* mono = pcm;
        bool monoOwned = false;
        if (srcCh == 2) {
            mono = malloc((size_t) srcFrames * sizeof(int16_t));
            if (!mono) {
                free(pcm);
                free(resolvedPath);
                CTR_AUDIO_LOG("create_stream: stereo->mono alloc failed (%lu frames)",
                              (unsigned long) srcFrames);
                return -1;
            }
            for (uint32_t i = 0; i < srcFrames; i++) {
                int32_t lhs = pcm[i * 2 + 0];
                int32_t rhs = pcm[i * 2 + 1];
                mono[i] = (int16_t)((lhs + rhs) >> 1);
            }
            monoOwned = true;
        }
        int16_t* finalPcm = mono;
        uint32_t finalFrames = srcFrames;
        bool finalOwned = monoOwned;
        if (srcRate != CTR_STREAM_TARGET_RATE) {
            uint64_t outCount = ((uint64_t) srcFrames * CTR_STREAM_TARGET_RATE) / (uint64_t) srcRate;
            if (outCount == 0) outCount = 1;
            int16_t* resampled = malloc((size_t) outCount * sizeof(int16_t));
            if (!resampled) {
                if (monoOwned) free(mono);
                free(pcm);
                free(resolvedPath);
                CTR_AUDIO_LOG("create_stream: resample alloc failed (%lu frames)",
                              (unsigned long) outCount);
                return -1;
            }
            for (uint32_t i = 0; i < outCount; i++) {
                uint64_t srcPos = (uint64_t) i * srcRate;
                uint32_t idx0   = (uint32_t)(srcPos / CTR_STREAM_TARGET_RATE);
                uint32_t frac   = (uint32_t)(srcPos % CTR_STREAM_TARGET_RATE);
                if (idx0 >= srcFrames) idx0 = srcFrames - 1;
                uint32_t idx1   = (idx0 + 1 < srcFrames) ? idx0 + 1 : idx0;
                int32_t a = mono[idx0];
                int32_t b = mono[idx1];
                int32_t mix = a + (int32_t)(((int64_t)(b - a) * frac) / (int64_t) CTR_STREAM_TARGET_RATE);
                if (mix >  32767) mix =  32767;
                if (mix < -32768) mix = -32768;
                resampled[i] = (int16_t) mix;
            }
            if (monoOwned) free(mono);
            finalPcm    = resampled;
            finalFrames = (uint32_t) outCount;
            finalOwned  = true;
        }

        if (finalPcm != pcm) {
            free(pcm);
            pcm = finalPcm;
        }
        (void) finalOwned;
        rate     = CTR_STREAM_TARGET_RATE;
        channels = 1;
        frames   = finalFrames;
    }

    size_t bytes = (size_t) frames * channels * sizeof(int16_t);
    if (bytes > CTR_STREAM_MAX_BYTES) {
        CTR_AUDIO_LOG("create_stream: '%s' too large (%lu bytes > cap %u). Use the offline ADPCM cache for tracks this big.",
                      openPath, (unsigned long) bytes, (unsigned) CTR_STREAM_MAX_BYTES);
        free(pcm);
        free(resolvedPath);
        return -1;
    }

    void* linbuf = linearMemAlign(bytes, 0x80);
    if (!linbuf) {
        CTR_AUDIO_LOG("create_stream: linearMemAlign(%lu) failed linearFree=%lu",
                      (unsigned long) bytes, (unsigned long) linearSpaceFree());
        free(pcm);
        free(resolvedPath);
        return -1;
    }
    memcpy(linbuf, pcm, bytes);
    free(pcm);
    DSP_FlushDataCache(linbuf, bytes);

    CtrStream* st = &sys->streams[slot];
    st->used        = true;
    st->data        = linbuf;
    st->dataSize    = (uint32_t) bytes;
    st->sampleRate  = rate;
    st->totalFrames = frames;
    st->channels    = channels;
    st->format      = FORMAT_PCM16;
    snprintf(st->path, sizeof(st->path), "%s", openPath);

    int32_t streamId = CTR_STREAM_ID_BASE + slot;
    CTR_AUDIO_LOG("create_stream ok: id=%ld slot=%d path='%s' rate=%u ch=%u frames=%u bytes=%lu",
                  (long) streamId, slot, st->path,
                  (unsigned) rate, (unsigned) channels, (unsigned) frames,
                  (unsigned long) bytes);

    free(resolvedPath);
    return streamId;
}

static bool ctr_destroy_stream(AudioSystem* base, int32_t streamIndex) {
    CtrAudioSystem* sys = (CtrAudioSystem*) base;
    CtrStream* st = stream_for_id(sys, streamIndex);
    if (!st) {
        CTR_AUDIO_LOG("destroy_stream: invalid id=%ld", (long) streamIndex);
        return false;
    }
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == streamIndex) {
            ndspChnWaveBufClear(i);
            sys->chans[i].currentSoundId = -1;
            sys->chans[i].instanceId     = -1;
        }
    }

    CTR_AUDIO_LOG("destroy_stream: id=%ld path='%s' bytes=%lu",
                  (long) streamIndex, st->path, (unsigned long) st->dataSize);

    if (st->data) linearFree(st->data);
    memset(st, 0, sizeof(*st));
    return true;
}

static void ctr_on_room_changed(AudioSystem *base, int32_t roomIndex) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    if (!sys || !sys->base.dataWin) return;
    sys->frame++;
    CTR_AUDIO_LOG("room changed: room=%ld frame=%lu", (long)roomIndex, (unsigned long)sys->frame);
}

static AudioSystemVtable vtable = {
    .init             = ctr_init,
    .destroy          = ctr_destroy,
    .update           = ctr_update,
    .playSound        = ctr_play,
    .stopSound        = ctr_stop,
    .stopAll          = ctr_stop_all,
    .isPlaying        = ctr_is_playing,
    .pauseSound       = ctr_pause,
    .resumeSound      = ctr_resume,
    .pauseAll         = ctr_pause_all,
    .resumeAll        = ctr_resume_all,
    .setSoundGain     = ctr_set_gain,
    .getSoundGain     = ctr_get_gain,
    .setSoundPitch    = ctr_set_pitch,
    .getSoundPitch    = ctr_get_pitch,
    .getTrackPosition = ctr_get_pos,
    .setTrackPosition = ctr_set_pos,
    .getSoundLength   = ctr_get_length,
    .setMasterGain    = ctr_set_master,
    .setChannelCount  = ctr_set_chans,
    .groupLoad        = ctr_grp_load,
    .groupIsLoaded    = ctr_grp_loaded,
    .createStream     = ctr_create_stream,
    .destroyStream    = ctr_destroy_stream
    //.onRoomChanged    = ctr_on_room_changed
};

CtrAudioSystem *CtrAudioSystem_create(void) {
    CtrAudioSystem *sys = calloc(1, sizeof(CtrAudioSystem));
    sys->base.vtable = &vtable;
    CTR_AUDIO_LOG("create: %p", (void*)sys);
    return sys;
}
