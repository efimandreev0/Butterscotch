#include "ctr_audio_system.h"
#include "ctr_audio_cache.h"

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <3ds.h>

#define FORMAT_PCM16 1
#define FORMAT_ADPCM 2
#define CTR_INSTANCE_ID_BASE 100000 // Как на PS2, чтобы не пересекаться с Resource ID

static void update_channel_mix(CtrAudioSystem* sys, int ch) {
    if (sys->chans[ch].currentSoundId == -1) return;
    int id = sys->chans[ch].currentSoundId;

    float baseVol = sys->base.dataWin->sond.sounds[id].volume;
    float chGain = sys->chans[ch].gain;
    float master = sys->masterGain;
    float mixGain = baseVol * chGain * master;

    float mix[12] = { mixGain, mixGain, 0.0f, 0.0f, 0,0,0,0,0,0,0,0 };
    ndspChnSetMix(ch, mix);
}

static CtrCacheEntry* get_or_load_sound(CtrAudioSystem* sys, int32_t id) {
    if (id < 0 || (uint32_t)id >= sys->soundCount || !sys->audioBin) return NULL;
    CtrSoundDef* def = &sys->soundBank[id];
    if (def->dataSize == 0) return NULL;

    sys->frame++;

    for (int i = 0; i < MAX_LRU_CACHE; i++) {
        if (sys->cache[i].data != NULL && sys->cache[i].id == id) {
            sys->cache[i].lastAccess = sys->frame;
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

    if (slot == -1) return NULL;

    if (sys->cache[slot].data != NULL) {
        linearFree(sys->cache[slot].data);
        sys->cache[slot].data = NULL;
    }

    void* buffer = linearMemAlign(def->dataSize, 0x80);
    if (!buffer) return NULL;

    fseek(sys->audioBin, (long)def->dataOffset, SEEK_SET);
    fread(buffer, 1, def->dataSize, sys->audioBin);
    DSP_FlushDataCache(buffer, def->dataSize);

    sys->cache[slot].id = id;
    sys->cache[slot].data = buffer;
    sys->cache[slot].size = def->dataSize;
    sys->cache[slot].lastAccess = sys->frame;

    return &sys->cache[slot];
}

static void ctr_init(AudioSystem *base, MAYBE_UNUSED DataWin *dw, FileSystem *fs) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    sys->base.dataWin = dw;
    sys->fs = fs;
    sys->masterGain = 1.0f;
    sys->nextInstanceId = CTR_INSTANCE_ID_BASE;

    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        sys->chans[i].currentSoundId = -1;
        sys->chans[i].instanceId = -1;
    }
    for (int i = 0; i < MAX_LRU_CACHE; i++) sys->cache[i].data = NULL;

    char* path = fs->vtable->resolvePath(fs, "cache/audio.bin");
    if (path) {
        sys->audioBin = fopen(path, "rb");
        free(path);
    }

    if (sys->audioBin) {
        uint32_t buf[8];
        fread(buf, 1, 32, sys->audioBin);
        sys->soundCount = buf[2];
        uint32_t indexOffset = buf[3];

        sys->soundBank = calloc(sys->soundCount, sizeof(CtrSoundDef));
        fseek(sys->audioBin, indexOffset, SEEK_SET);

        for (uint32_t i = 0; i < sys->soundCount; i++) {
            CtrAudioEntry entryRaw;
            fread(&entryRaw, sizeof(CtrAudioEntry), 1, sys->audioBin);

            sys->soundBank[i].dataOffset   = entryRaw.dataOffset;
            sys->soundBank[i].dataSize     = entryRaw.dataSize;
            sys->soundBank[i].sampleRate   = entryRaw.sampleRate;
            sys->soundBank[i].totalFrames  = entryRaw.totalFrames;
            sys->soundBank[i].channels     = entryRaw.channels;
            sys->soundBank[i].format       = entryRaw.format;
            sys->soundBank[i].preferStream = (entryRaw.flags & CTR_AUDIO_FLAG_PREFER_STREAM) != 0;
            memcpy(sys->soundBank[i].adpcmCoefs, entryRaw.adpcmCoefs, sizeof(u16)*16);
        }
    }

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
}

static void ctr_destroy(AudioSystem *base) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    ndspExit();
    for (int i = 0; i < MAX_LRU_CACHE; i++) if (sys->cache[i].data) linearFree(sys->cache[i].data);
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
                sys->chans[i].currentSoundId = -1;
                sys->chans[i].instanceId = -1;
            }
        }
    }
}

static int32_t ctr_play(AudioSystem *base, int32_t id, int32_t prio, bool loop) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;

    CtrCacheEntry* entry = get_or_load_sound(sys, id);
    if (!entry) return -1;
    CtrSoundDef* def = &sys->soundBank[id];

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
            ndspChnSetAdpcmCoefs(ch, def->adpcmCoefs);
        } else if (def->channels == 2) {
            ndspFormat = NDSP_FORMAT_STEREO_PCM16;
        }

        ndspChnSetFormat(ch, ndspFormat);
        ndspChnSetRate(ch, (float)def->sampleRate);

        memset(&chan->waveBuf, 0, sizeof(ndspWaveBuf));
        chan->waveBuf.data_vaddr = entry->data;
        chan->waveBuf.nsamples   = def->totalFrames;
        chan->waveBuf.looping    = loop;

        if (def->format == FORMAT_ADPCM) {
            memset(&chan->adpcmState, 0, sizeof(ndspAdpcmData));
            chan->waveBuf.adpcm_data = &chan->adpcmState;
        }

        chan->currentSoundId = id;
        chan->instanceId     = sys->nextInstanceId++; // ВЫДАЕМ БЕЗОПАСНЫЙ УНИКАЛЬНЫЙ ID
        chan->priority       = score_prio;
        chan->loop           = loop;
        chan->gain           = 1.0f;
        chan->pitch          = 1.0f;
        chan->playFrame      = sys->frame;

        update_channel_mix(sys, ch);
        DSP_FlushDataCache(entry->data, entry->size);
        ndspChnWaveBufAdd(ch, &chan->waveBuf);

        return chan->instanceId; // ВОЗВРАЩАЕМ УНИКАЛЬНЫЙ ID ВМЕСТО НОМЕРА КАНАЛА
    }
    return -1;
}

static void ctr_stop(AudioSystem *base, int32_t soundOrInstance) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        // ФИКС КРОСС-ФАЙРА: Проверяем безопасно!
        if (sys->chans[i].currentSoundId == soundOrInstance || sys->chans[i].instanceId == soundOrInstance) {
            ndspChnWaveBufClear(i);
            sys->chans[i].currentSoundId = -1;
            sys->chans[i].instanceId = -1;
        }
    }
}

static void ctr_stop_all(AudioSystem *base) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        ndspChnWaveBufClear(i);
        sys->chans[i].currentSoundId = -1;
        sys->chans[i].instanceId = -1;
    }
}

static bool ctr_is_playing(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            return true;
        }
    }
    return false;
}

static void ctr_pause(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            ndspChnSetPaused(i, true);
        }
    }
}

static void ctr_resume(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            ndspChnSetPaused(i, false);
        }
    }
}

static void ctr_pause_all(AudioSystem *base) {
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) ndspChnSetPaused(i, true);
}

static void ctr_resume_all(AudioSystem *base) {
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) ndspChnSetPaused(i, false);
}

static void ctr_set_gain(AudioSystem *base, int32_t id, float g, MAYBE_UNUSED uint32_t t) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            sys->chans[i].gain = g;
            update_channel_mix(sys, i);
        }
    }
}

static float ctr_get_gain(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) return sys->chans[i].gain;
    }
    return 0.0f;
}

static void ctr_set_pitch(AudioSystem *base, int32_t id, float p) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            sys->chans[i].pitch = p;
            int soundIdx = sys->chans[i].currentSoundId;
            float hardwareBaseRate = (float)sys->soundBank[soundIdx].sampleRate;
            ndspChnSetRate(i, hardwareBaseRate * p);
        }
    }
}

static float ctr_get_pitch(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) return sys->chans[i].pitch;
    }
    return 1.0f;
}

static float ctr_get_pos(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId == id || sys->chans[i].instanceId == id) {
            int soundIdx = sys->chans[i].currentSoundId;
            return (float)ndspChnGetSamplePos(i) / (float)sys->soundBank[soundIdx].sampleRate;
        }
    }
    return 0.0f;
}

static void ctr_set_pos(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t id, MAYBE_UNUSED float pos) {}

static float ctr_get_length(AudioSystem *base, int32_t id) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    if (id >= 0 && id < (int32_t)sys->soundCount) {
        float fsRate = (float)sys->soundBank[id].sampleRate;
        if (fsRate > 0.0f) return (float)sys->soundBank[id].totalFrames / fsRate;
    }
    return 0.0f;
}

static void ctr_set_master(AudioSystem *base, float g) {
    CtrAudioSystem *sys = (CtrAudioSystem *) base;
    sys->masterGain = g;
    for (int i = 0; i < MAX_CTR_CHANNELS; i++) {
        if (sys->chans[i].currentSoundId != -1) update_channel_mix(sys, i);
    }
}

static void ctr_set_chans(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t cnt) {}
static void ctr_grp_load(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t grp) {}
static bool ctr_grp_loaded(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t grp) { return true; }
static int32_t ctr_create_stream(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED const char *filename) { return -1; }
static bool ctr_destroy_stream(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t streamIndex) { return false; }

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
};

CtrAudioSystem *CtrAudioSystem_create(void) {
    CtrAudioSystem *sys = calloc(1, sizeof(CtrAudioSystem));
    sys->base.vtable = &vtable;
    return sys;
}