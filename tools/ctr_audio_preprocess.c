#include "ctr_audio_preprocess.h"
#include "ctr_adpcm_encode.h"

#include "data_win.h"
#include "ctr_audio_cache.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern char g_current_cache_dir[256];

static int16_t *decode_wav(const uint8_t *src, uint32_t srcSize,
                           uint32_t *outSampleRate, uint8_t *outChannels,
                           uint32_t *outFrames) {
    if (srcSize < 44) return NULL;
    if (memcmp(src, "RIFF", 4) != 0 || memcmp(src + 8, "WAVE", 4) != 0) return NULL;

    uint32_t off = 12;
    uint16_t fmtTag = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t *dataPtr = NULL;
    uint32_t dataLen = 0;

    while (off + 8 <= srcSize) {
        const char *id = (const char *)(src + off);
        uint32_t sz = (uint32_t)src[off + 4]
                    | ((uint32_t)src[off + 5] << 8)
                    | ((uint32_t)src[off + 6] << 16)
                    | ((uint32_t)src[off + 7] << 24);
        off += 8;
        if (off + sz > srcSize) break;

        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            fmtTag        = (uint16_t)(src[off + 0] | (src[off + 1] << 8));
            channels      = (uint16_t)(src[off + 2] | (src[off + 3] << 8));
            sampleRate    = (uint32_t)(src[off + 4] | (src[off + 5] << 8)
                                     | (src[off + 6] << 16) | (src[off + 7] << 24));
            bitsPerSample = (uint16_t)(src[off + 14] | (src[off + 15] << 8));
        } else if (memcmp(id, "data", 4) == 0) {
            dataPtr = src + off;
            dataLen = sz;
            break;
        }
        off += sz + (sz & 1);
    }

    if (!dataPtr || fmtTag != 1 || channels < 1 || channels > 2) return NULL;
    if (bitsPerSample != 8 && bitsPerSample != 16) return NULL;

    uint32_t bytesPerSrcFrame = (bitsPerSample / 8) * channels;
    if (bytesPerSrcFrame == 0) return NULL;
    uint32_t frames = dataLen / bytesPerSrcFrame;
    int16_t *pcm = (int16_t *) malloc((size_t) frames * channels * sizeof(int16_t));
    if (!pcm) return NULL;

    if (bitsPerSample == 16) {
        memcpy(pcm, dataPtr, (size_t) frames * bytesPerSrcFrame);
    } else {
        for (uint32_t i = 0; i < frames * channels; i++) {
            int16_t s = (int16_t)((int32_t)dataPtr[i] - 128);
            pcm[i] = (int16_t)(s << 8);
        }
    }

    *outSampleRate = sampleRate;
    *outChannels   = (uint8_t) channels;
    *outFrames     = frames;
    return pcm;
}

static int16_t *decode_ogg(const uint8_t *src, uint32_t srcSize,
                           uint32_t *outSampleRate, uint8_t *outChannels,
                           uint32_t *outFrames) {
    int chans = 0, rate = 0;
    short *out = NULL;
    int totalSamples = stb_vorbis_decode_memory(src, (int) srcSize, &chans, &rate, &out);
    if (totalSamples <= 0 || !out) return NULL;
    if (chans < 1 || chans > 2 || rate < 8000 || rate > 96000) {
        free(out);
        return NULL;
    }
    *outSampleRate = (uint32_t) rate;
    *outChannels   = (uint8_t) chans;
    *outFrames     = (uint32_t) totalSamples;
    return out;
}

static int16_t *decode_blob(const uint8_t *src, uint32_t srcSize,
                            uint32_t *sr, uint8_t *ch, uint32_t *frames) {
    if (srcSize >= 4 && memcmp(src, "OggS", 4) == 0) return decode_ogg(src, srcSize, sr, ch, frames);
    if (srcSize >= 12 && memcmp(src, "RIFF", 4) == 0) return decode_wav(src, srcSize, sr, ch, frames);
    return NULL;
}

#ifndef CTR_AUDIO_TARGET_RATE
#define CTR_AUDIO_TARGET_RATE 22050u
#endif

#ifndef CTR_AUDIO_KEEP_STEREO
#define CTR_AUDIO_KEEP_STEREO 0
#endif

static bool sound_keeps_stereo(const char *name) {
#if CTR_AUDIO_KEEP_STEREO
    if (!name || !*name) return false;
    static const char *prefixes[] = { "mus_", "music_", "song_", "bgm_", "ost_" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], n) == 0) return true;
    }
#else
    (void) name;
#endif
    return false;
}

static int16_t *postprocess_pcm(const int16_t *srcPcm, uint32_t srcFrames,
                                uint32_t srcRate, uint8_t srcCh,
                                bool keepStereo,
                                uint32_t *outFrames, uint8_t *outCh) {
    if (srcCh < 1 || srcCh > 2) return NULL;
    uint8_t  tgtCh   = (srcCh == 2 && keepStereo) ? 2 : 1;
    uint32_t tgtRate = CTR_AUDIO_TARGET_RATE;

    const int16_t *mid       = srcPcm;
    int16_t       *midOwned  = NULL;
    uint8_t        midCh     = srcCh;
    if (srcCh == 2 && tgtCh == 1) {
        midOwned = (int16_t *) malloc((size_t) srcFrames * sizeof(int16_t));
        if (!midOwned) return NULL;
        for (uint32_t i = 0; i < srcFrames; i++) {
            int32_t lhs = srcPcm[i * 2 + 0];
            int32_t rhs = srcPcm[i * 2 + 1];
            midOwned[i] = (int16_t)((lhs + rhs) >> 1);
        }
        mid   = midOwned;
        midCh = 1;
    }

    int16_t *out = NULL;
    uint32_t outF = 0;

    if (srcRate == tgtRate) {
        outF = srcFrames;
        size_t bytes = (size_t) outF * tgtCh * sizeof(int16_t);
        out = (int16_t *) malloc(bytes);
        if (!out) { free(midOwned); return NULL; }
        memcpy(out, mid, bytes);
    } else {
        uint64_t cnt = ((uint64_t) srcFrames * tgtRate) / (uint64_t) srcRate;
        if (cnt == 0) cnt = 1;
        outF = (uint32_t) cnt;
        out = (int16_t *) malloc((size_t) outF * tgtCh * sizeof(int16_t));
        if (!out) { free(midOwned); return NULL; }

        for (uint32_t i = 0; i < outF; i++) {
            uint64_t srcPos = (uint64_t) i * srcRate;
            uint32_t idx0   = (uint32_t)(srcPos / tgtRate);
            uint32_t frac   = (uint32_t)(srcPos % tgtRate);
            if (idx0 >= srcFrames) idx0 = srcFrames - 1;
            uint32_t idx1 = (idx0 + 1 < srcFrames) ? idx0 + 1 : idx0;

            for (uint8_t c = 0; c < tgtCh; c++) {
                int32_t a = mid[idx0 * midCh + c];
                int32_t b = mid[idx1 * midCh + c];
                int32_t mix = a + (int32_t)(((int64_t)(b - a) * frac) / (int64_t) tgtRate);
                if (mix >  32767) mix =  32767;
                if (mix < -32768) mix = -32768;
                out[i * tgtCh + c] = (int16_t) mix;
            }
        }
    }

    free(midOwned);
    *outFrames = outF;
    *outCh     = tgtCh;
    return out;
}

static uint32_t fnv1a_update(uint32_t h, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) { h ^= buf[i]; h *= 0x01000193u; }
    return h;
}

static bool fingerprint(const char *path, uint64_t *outSize, uint32_t *outHash) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return false; }
    *outSize = (uint64_t) sz;

    uint32_t h = 0x811c9dc5u;
    uint8_t szb[8];
    for (int i = 0; i < 8; i++) szb[i] = (uint8_t)((uint64_t)sz >> (i * 8));
    h = fnv1a_update(h, szb, 8);

    static const double pts[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    uint8_t buf[1024];
    for (int i = 0; i < 5; i++) {
        long target = (long)((double)sz * pts[i]);
        long maxStart = sz > (long)sizeof(buf) ? sz - (long)sizeof(buf) : 0;
        if (target > maxStart) target = maxStart;
        if (target < 0) target = 0;
        fseek(f, target, SEEK_SET);
        size_t got = fread(buf, 1, sizeof(buf), f);
        if (got == 0) break;
        h = fnv1a_update(h, buf, got);
    }
    fclose(f);
    *outHash = h;
    return true;
}

static DataWin *parse_audiogroup_file(const char *path) {
    DataWinParserOptions opt = {
        .parseGen8 = true,
        .parseAudo = true,
        .skipTextureBlobData = true,
        .skipAudioBlobData = true,
    };
    return DataWin_parse(path, opt);
}

static void parent_dir_of(const char *path, char *out, size_t outSize) {
    if (!path || !out || outSize == 0) return;
    const char *fwd = strrchr(path, '/');
    const char *bwd = strrchr(path, '\\');
    if (!fwd || (bwd && bwd > fwd)) fwd = bwd;
    size_t n = fwd ? (size_t)(fwd - path) : 0;
    if (n == 0) { snprintf(out, outSize, "."); return; }
    if (n >= outSize) n = outSize - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static bool file_exists_at(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

typedef struct {
    DataWin *dw;
    char    *path;
} GroupArchive;

int CtrAudioPreprocess_run(const char *dataWinPath, const char *cacheDir, const char *label) {
    snprintf(g_current_cache_dir, sizeof(g_current_cache_dir), "%s", cacheDir);

    DataWinParserOptions opt = {
        .parseGen8 = true,
        .parseSond = true,
        .parseAgrp = true,
        .parseAudo = true,
        .parseStrg = true,
        .skipTextureBlobData = true,
        .skipAudioBlobData = true,
    };
    fprintf(stderr, "[%s] audio: parsing %s\n", label, dataWinPath);
    DataWin *dw = DataWin_parse(dataWinPath, opt);
    if (!dw) {
        fprintf(stderr, "[%s] audio: DataWin_parse failed\n", label);
        return 1;
    }

    char gameDir[512];
    parent_dir_of(dataWinPath, gameDir, sizeof(gameDir));

    uint32_t groupCount = (dw->agrp.count > 0) ? dw->agrp.count : 1;
    GroupArchive *groups = (GroupArchive *) calloc(groupCount, sizeof(GroupArchive));
    groups[0].dw   = dw;
    groups[0].path = strdup(dataWinPath);
    for (uint32_t g = 1; g < groupCount; g++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/audiogroup%u.dat", gameDir, g);
        if (!file_exists_at(p)) {
            fprintf(stderr, "[%s] audio: audiogroup%u.dat missing — sounds in that group will be empty entries\n",
                    label, g);
            groups[g].dw = NULL;
            groups[g].path = NULL;
            continue;
        }
        groups[g].dw   = parse_audiogroup_file(p);
        groups[g].path = strdup(p);
        if (!groups[g].dw) {
            fprintf(stderr, "[%s] audio: audiogroup%u.dat parse failed\n", label, g);
        }
    }

    char outPath[512];
    snprintf(outPath, sizeof(outPath), "%s/%s", cacheDir, CTR_AUDIO_CACHE_FILE);
    char tmpPath[512];
    snprintf(tmpPath, sizeof(tmpPath), "%s/audio.tmp", cacheDir);
    remove(tmpPath);

    FILE *out = fopen(tmpPath, "w+b");
    if (!out) {
        fprintf(stderr, "[%s] audio: cannot create %s (%s)\n", label, tmpPath, strerror(errno));
        DataWin_free(dw);
        free(groups[0].path);
        free(groups);
        return 1;
    }

    CtrAudioCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = CTR_AUDIO_CACHE_MAGIC;
    hdr.version = CTR_AUDIO_CACHE_VERSION;
    hdr.soundCount  = dw->sond.count;
    hdr.indexOffset = sizeof(hdr);
    hdr.dataOffset  = hdr.indexOffset + dw->sond.count * sizeof(CtrAudioEntry);

    CtrAudioEntry *entries = (CtrAudioEntry *) calloc(dw->sond.count, sizeof(CtrAudioEntry));

    fseek(out, (long) hdr.dataOffset, SEEK_SET);

    uint64_t cursor = hdr.dataOffset;
    uint32_t embOk = 0, embSkip = 0;
    uint32_t extOk = 0, extSkip = 0;

    uint64_t bytesByFmt[3] = {0, 0, 0};
    uint32_t countByFmt[3] = {0, 0, 0};

    for (uint32_t i = 0; i < dw->sond.count; i++) {
        Sound *snd = &dw->sond.sounds[i];

        if (snd->flags & 1) {
            int32_t grp = snd->audioGroup;
            if (grp < 0 || (uint32_t) grp >= groupCount || !groups[grp].dw) {
                embSkip++; continue;
            }
            DataWin *gdw = groups[grp].dw;
            if (snd->audioFile < 0 || (uint32_t) snd->audioFile >= gdw->audo.count) {
                embSkip++; continue;
            }
            AudioEntry *ent = &gdw->audo.entries[snd->audioFile];
            if (ent->dataSize == 0) { embSkip++; continue; }

            FILE *src = fopen(groups[grp].path, "rb");
            if (!src) { embSkip++; continue; }
            uint8_t *raw = (uint8_t *) malloc(ent->dataSize);
            if (!raw) { fclose(src); embSkip++; continue; }
            if (fseek(src, (long) ent->dataOffset, SEEK_SET) != 0 ||
                fread(raw, 1, ent->dataSize, src) != ent->dataSize) {
                fclose(src);
                free(raw);
                embSkip++;
                continue;
            }
            fclose(src);

            uint32_t sr = 0, framesDec = 0;
            uint8_t  ch = 0;
            int16_t *decoded = decode_blob(raw, ent->dataSize, &sr, &ch, &framesDec);
            free(raw);
            if (!decoded) {
                fprintf(stderr, "[%s] audio: sound '%s' (group %d, audoIdx %d) — unsupported format, skipping\n",
                        label, snd->name ? snd->name : "?", grp, snd->audioFile);
                embSkip++;
                continue;
            }

            bool keepStereo = sound_keeps_stereo(snd->name);
            uint32_t framesOut = 0;
            uint8_t  chOut     = 0;
            int16_t *pcm = postprocess_pcm(decoded, framesDec, sr, ch,
                                            keepStereo,
                                            &framesOut, &chOut);
            free(decoded);
            if (!pcm) {
                fprintf(stderr, "[%s] audio: postprocess failed for '%s'\n",
                        label, snd->name ? snd->name : "?");
                embSkip++;
                continue;
            }

            uint8_t  outFormat   = keepStereo ? CTR_AUDIO_FORMAT_PCM16 : CTR_AUDIO_FORMAT_ADPCM;
            uint32_t payloadSize = 0;
            void    *writeBuf    = NULL;
            uint8_t *adpcmOwned  = NULL;

            memset(entries[i].adpcmCoefs, 0, sizeof(entries[i].adpcmCoefs));

            if (outFormat == CTR_AUDIO_FORMAT_ADPCM) {
                payloadSize = (uint32_t) CtrAdpcm_byteCount(framesOut);
                adpcmOwned  = (uint8_t *) malloc(payloadSize);
                if (!adpcmOwned) {
                    free(pcm);
                    embSkip++;
                    continue;
                }
                CtrAdpcm_encode(pcm, framesOut, adpcmOwned, entries[i].adpcmCoefs);
                writeBuf = adpcmOwned;
            } else {
                payloadSize = framesOut * chOut * 2u;
                writeBuf    = pcm;
            }

            entries[i].dataOffset  = (uint32_t) cursor;
            entries[i].dataSize    = payloadSize;
            entries[i].sampleRate  = CTR_AUDIO_TARGET_RATE;
            entries[i].totalFrames = framesOut;
            entries[i].channels    = chOut;
            entries[i].loopStart   = 0;
            entries[i].flags       = 0;
            entries[i].format      = outFormat;
            if (payloadSize > 256u * 1024u) entries[i].flags |= CTR_AUDIO_FLAG_PREFER_STREAM;

            if (fwrite(writeBuf, 1, payloadSize, out) != payloadSize) {
                fprintf(stderr, "[%s] audio: write failed at sound %u\n", label, i);
                free(adpcmOwned);
                free(pcm);
                free(entries);
                fclose(out);
                for (uint32_t g = 1; g < groupCount; g++) {
                    if (groups[g].dw)   DataWin_free(groups[g].dw);
                    if (groups[g].path) free(groups[g].path);
                }
                free(groups[0].path);
                free(groups);
                DataWin_free(dw);
                return 1;
            }
            cursor += payloadSize;
            bytesByFmt[outFormat] += payloadSize;
            countByFmt[outFormat]++;
            if (outFormat == CTR_AUDIO_FORMAT_PCM16 && countByFmt[outFormat] <= 50) {
                fprintf(stderr,
                    "[%s] audio: PCM16 #%u name='%s' ch=%u rate=%u dur=%.2fs size=%uKB\n",
                    label, countByFmt[outFormat],
                    snd->name ? snd->name : "?",
                    chOut, CTR_AUDIO_TARGET_RATE,
                    (double) framesOut / (double) CTR_AUDIO_TARGET_RATE,
                    payloadSize / 1024u);
            }
            free(adpcmOwned);
            free(pcm);
            embOk++;
            continue;
        }

        if (!snd->file || !snd->file[0]) { extSkip++; continue; }

        char extPath[640];
        snprintf(extPath, sizeof(extPath), "%s/%s", gameDir, snd->file);
        FILE *src = fopen(extPath, "rb");
        if (!src) {
            snprintf(extPath, sizeof(extPath), "%s/%s.ogg", gameDir, snd->file);
            src = fopen(extPath, "rb");
        }
        if (!src) {
            snprintf(extPath, sizeof(extPath), "%s/%s.wav", gameDir, snd->file);
            src = fopen(extPath, "rb");
        }
        if (!src) {
            fprintf(stderr, "[%s] audio: external '%s' not found (tried %s/{file,.ogg,.wav})\n",
                    label, snd->name ? snd->name : "?", snd->file);
            extSkip++;
            continue;
        }

        fseek(src, 0, SEEK_END);
        long sz = ftell(src);
        fseek(src, 0, SEEK_SET);
        if (sz <= 0) { fclose(src); extSkip++; continue; }
        uint8_t *raw = (uint8_t *) malloc((size_t) sz);
        if (!raw) { fclose(src); extSkip++; continue; }
        if (fread(raw, 1, (size_t) sz, src) != (size_t) sz) {
            fclose(src);
            free(raw);
            extSkip++;
            continue;
        }
        fclose(src);

        uint32_t sr = 0, framesDec = 0;
        uint8_t  ch = 0;
        int16_t *decoded = decode_blob(raw, (uint32_t) sz, &sr, &ch, &framesDec);
        free(raw);
        if (!decoded) {
            fprintf(stderr, "[%s] audio: external '%s' (%s) — unsupported format, skipping\n",
                    label, snd->name ? snd->name : "?", extPath);
            extSkip++;
            continue;
        }

        bool keepStereoExt = sound_keeps_stereo(snd->name);
        uint32_t framesOut = 0;
        uint8_t  chOut     = 0;
        int16_t *pcm = postprocess_pcm(decoded, framesDec, sr, ch,
                                        keepStereoExt,
                                        &framesOut, &chOut);
        free(decoded);
        if (!pcm) {
            fprintf(stderr, "[%s] audio: postprocess failed for external '%s'\n",
                    label, snd->name ? snd->name : "?");
            extSkip++;
            continue;
        }

        uint8_t  outFormatExt = keepStereoExt ? CTR_AUDIO_FORMAT_PCM16 : CTR_AUDIO_FORMAT_ADPCM;
        uint32_t payloadSize  = 0;
        void    *writeBufExt  = NULL;
        uint8_t *adpcmOwnedExt = NULL;

        memset(entries[i].adpcmCoefs, 0, sizeof(entries[i].adpcmCoefs));

        if (outFormatExt == CTR_AUDIO_FORMAT_ADPCM) {
            payloadSize  = (uint32_t) CtrAdpcm_byteCount(framesOut);
            adpcmOwnedExt = (uint8_t *) malloc(payloadSize);
            if (!adpcmOwnedExt) { free(pcm); extSkip++; continue; }
            CtrAdpcm_encode(pcm, framesOut, adpcmOwnedExt, entries[i].adpcmCoefs);
            writeBufExt = adpcmOwnedExt;
        } else {
            payloadSize = framesOut * chOut * 2u;
            writeBufExt = pcm;
        }

        entries[i].dataOffset  = (uint32_t) cursor;
        entries[i].dataSize    = payloadSize;
        entries[i].sampleRate  = CTR_AUDIO_TARGET_RATE;
        entries[i].totalFrames = framesOut;
        entries[i].channels    = chOut;
        entries[i].loopStart   = 0;
        entries[i].flags       = 0;
        entries[i].format      = outFormatExt;
        if (payloadSize > 256u * 1024u) entries[i].flags |= CTR_AUDIO_FLAG_PREFER_STREAM;

        if (fwrite(writeBufExt, 1, payloadSize, out) != payloadSize) {
            fprintf(stderr, "[%s] audio: write failed at external sound %u\n", label, i);
            free(adpcmOwnedExt);
            free(pcm);
            free(entries);
            fclose(out);
            for (uint32_t g = 1; g < groupCount; g++) {
                if (groups[g].dw)   DataWin_free(groups[g].dw);
                if (groups[g].path) free(groups[g].path);
            }
            free(groups[0].path);
            free(groups);
            DataWin_free(dw);
            return 1;
        }
        cursor += payloadSize;
        bytesByFmt[outFormatExt] += payloadSize;
        countByFmt[outFormatExt]++;
        if (outFormatExt == CTR_AUDIO_FORMAT_PCM16 && countByFmt[outFormatExt] <= 50) {
            fprintf(stderr,
                "[%s] audio: PCM16 #%u (ext) name='%s' file='%s' ch=%u dur=%.2fs size=%uKB\n",
                label, countByFmt[outFormatExt],
                snd->name ? snd->name : "?",
                snd->file ? snd->file : "?",
                chOut,
                (double) framesOut / (double) CTR_AUDIO_TARGET_RATE,
                payloadSize / 1024u);
        }
        free(adpcmOwnedExt);
        free(pcm);
        extOk++;
    }

    hdr.dataSize = (uint32_t)(cursor - hdr.dataOffset);
    fingerprint(dataWinPath, &hdr.srcSize, &hdr.srcSampleHash);

    fseek(out, 0, SEEK_SET);
    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1) {
        fprintf(stderr, "[%s] audio: header write failed\n", label);
        fclose(out);
        free(entries);
        for (uint32_t g = 1; g < groupCount; g++) {
            if (groups[g].dw)   DataWin_free(groups[g].dw);
            if (groups[g].path) free(groups[g].path);
        }
        free(groups[0].path);
        free(groups);
        DataWin_free(dw);
        return 1;
    }
    fseek(out, (long) hdr.indexOffset, SEEK_SET);
    if (fwrite(entries, sizeof(CtrAudioEntry), dw->sond.count, out) != dw->sond.count) {
        fprintf(stderr, "[%s] audio: index write failed\n", label);
        fclose(out);
        free(entries);
        for (uint32_t g = 1; g < groupCount; g++) {
            if (groups[g].dw)   DataWin_free(groups[g].dw);
            if (groups[g].path) free(groups[g].path);
        }
        free(groups[0].path);
        free(groups);
        DataWin_free(dw);
        return 1;
    }
    fclose(out);

    remove(outPath);
    if (rename(tmpPath, outPath) != 0) {
        FILE *src = fopen(tmpPath, "rb");
        FILE *dst = fopen(outPath, "wb");
        if (src && dst) {
            uint8_t buf[64 * 1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
        remove(tmpPath);
    }

    fprintf(stderr,
            "[%s] audio: embedded %u ok / %u skip, external %u ok / %u skip, total %u/%u\n",
            label, embOk, embSkip, extOk, extSkip,
            embOk + extOk, dw->sond.count);
    fprintf(stderr,
            "[%s] audio: PCM8=%u sounds (%u KB)  PCM16=%u sounds (%u KB)  ADPCM=%u sounds (%u KB)  -> %u KB total -> %s\n",
            label,
            countByFmt[CTR_AUDIO_FORMAT_PCM8],  (unsigned)(bytesByFmt[CTR_AUDIO_FORMAT_PCM8]  / 1024),
            countByFmt[CTR_AUDIO_FORMAT_PCM16], (unsigned)(bytesByFmt[CTR_AUDIO_FORMAT_PCM16] / 1024),
            countByFmt[CTR_AUDIO_FORMAT_ADPCM], (unsigned)(bytesByFmt[CTR_AUDIO_FORMAT_ADPCM] / 1024),
            (unsigned)(hdr.dataSize / 1024), outPath);

    free(entries);
    for (uint32_t g = 1; g < groupCount; g++) {
        if (groups[g].dw)   DataWin_free(groups[g].dw);
        if (groups[g].path) free(groups[g].path);
    }
    free(groups[0].path);
    free(groups);
    DataWin_free(dw);
    return 0;
}
