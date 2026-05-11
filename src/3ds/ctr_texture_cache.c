// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "ctr_texture_cache.h"

#include "etc1_encoder.h"

#ifndef CTR_TEXTURE_CACHE_HOST
#include "ctr_renderer.h"
#endif
#include "image_decoder.h"
#include "utils.h"

#ifndef CTR_TEXTURE_CACHE_HOST
#include <3ds.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern char g_current_cache_dir[256];

#ifndef CTR_TEXTURE_CACHE_ENABLE_ETC1
#define CTR_TEXTURE_CACHE_ENABLE_ETC1 1
#endif

#ifdef CTR_TEXTURE_CACHE_HOST
static void *linearAlloc(size_t size) { return malloc(size); }
static void linearFree(void *ptr) { free(ptr); }
#endif

#ifdef _WIN32
#define FSEEK64(fp, offset, origin) _fseeki64((fp), (int64_t)(offset), (origin))
#define FTELL64(fp) _ftelli64((fp))
#else
#define FSEEK64(fp, offset, origin) fseeko((fp), (off_t)(offset), (origin))
#define FTELL64(fp) ftello((fp))
#endif

#define CACHE_ENTRY_VALID 1u
#define CTR_TEXTURE_CACHE_PATH_MAX 640

typedef struct {
    uint32_t flags;
    uint32_t segmentStart;
    uint32_t segmentCount;
} CacheItemDisk;

typedef struct {
    uint32_t atlasIndex;
    uint16_t sourceX, sourceY;
    uint16_t width, height;
    uint16_t atlasX, atlasY;
} CacheSegmentDisk;

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t format;
    uint32_t reserved;
} CacheAtlasDisk;

typedef struct {
    uint32_t tpagIndex;
    uint32_t groupId;
    uint32_t formatHint;
    int srcPage;
    int srcX, srcY;
    int localX, localY;
    int w, h;
    int packW, packH;
    int atlasId;
    int dstX, dstY;
} CacheImage;

typedef struct {
    uint32_t groupId;
    uint32_t formatHint;
    uint32_t start;
    uint32_t count;
    uint64_t area;
} CacheGroup;

typedef struct {
    int x, y, w, h;
} PackRect;

#define MAX_FREE_RECTS 512

typedef struct {
    PackRect rects[MAX_FREE_RECTS];
    int count;
} MaxRectsPacker;

typedef struct {
    MaxRectsPacker packer;
    uint32_t formatHint;
} CacheAtlas;

static CtrTextureCacheProgressFn g_progressCallback = NULL;
static void *g_progressUser = NULL;
static CacheImage *g_sortImages = NULL;
static bool g_cacheUseEtc1 = CTR_TEXTURE_CACHE_ENABLE_ETC1 != 0;

static uint32_t cache_build_flags(void) {
    return g_cacheUseEtc1 ? CTR_TEXTURE_CACHE_FLAG_ETC1 : 0u;
}

void CtrTextureCache_indexPath(char *out, size_t outSize) {
    snprintf(out, outSize, "%s/%s", g_current_cache_dir, CTR_TEXTURE_CACHE_FILE);
}

void CtrTextureCache_setProgressCallback(CtrTextureCacheProgressFn callback, void *user) {
    g_progressCallback = callback;
    g_progressUser = user;
}

#ifdef CTR_TEXTURE_CACHE_HOST
void CtrTextureCache_setUseEtc1(bool enabled) {
    g_cacheUseEtc1 = enabled;
}

bool CtrTextureCache_getUseEtc1(void) {
    return g_cacheUseEtc1;
}
#endif

static void ready_flag_path(char *out, size_t outSize) {
    snprintf(out, outSize, "%s/%s", g_current_cache_dir, CTR_TEXTURE_CACHE_READY_FLAG);
}

static void remove_if_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fclose(f);
    remove(path);
}

static void ensure_ready_flag(void) {
    char path[CTR_TEXTURE_CACHE_PATH_MAX];
    ready_flag_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("READY", f);
        fclose(f);
    }
}

static void remove_old_ready_flags(void) {
    static const char *names[] = {
        "cache_ready.flag",
        "cache_ready_v2.flag",
        "cache_ready_v3.flag",
        "cache_ready_v4.flag",
        "cache_ready_v5.flag",
        "cache_ready_v6.flag",
        "cache_ready_v7.flag",
        "cache_ready_v8.flag",
        "cache_ready_v9.flag",
        "cache_ready_v10.flag",
        "cache_ready_v11.flag",
        "cache_ready_v12.flag",
        "cache_ready_v13.flag",
        "cache_ready_v14.flag",
        "cache_ready_v15.flag",
        "cache_ready_v16.flag",
    };
    char path[CTR_TEXTURE_CACHE_PATH_MAX];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", g_current_cache_dir, names[i]);
        remove_if_exists(path);
    }
}

static void cleanup_legacy_page_files(void) {
    DIR *dir = opendir(g_current_cache_dir);
    if (!dir) return;

    struct dirent *ent;
    char path[CTR_TEXTURE_CACHE_PATH_MAX];
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "page_", 5) != 0) continue;
        if (!strstr(name, ".atlas") && !strstr(name, ".tmp")) continue;
        snprintf(path, sizeof(path), "%s/%s", g_current_cache_dir, name);
        remove(path);
    }
    closedir(dir);
}

static bool copy_file_contents(const char *srcPath, const char *dstPath) {
    FILE *src = fopen(srcPath, "rb");
    if (!src) return false;
    FILE *dst = fopen(dstPath, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }

    bool ok = true;
    uint8_t *buf = malloc(64 * 1024);
    if (!buf) ok = false;
    while (ok) {
        size_t n = fread(buf, 1, 64 * 1024, src);
        if (n > 0 && fwrite(buf, 1, n, dst) != n) ok = false;
        if (n < 64 * 1024) {
            if (ferror(src)) ok = false;
            break;
        }
    }
    free(buf);

    if (fclose(dst) != 0) ok = false;
    fclose(src);
    if (!ok) remove(dstPath);
    return ok;
}

static bool commit_tmp_file(const char *tmpPath, const char *finalPath) {
    remove(finalPath);
    if (rename(tmpPath, finalPath) == 0) return true;
    int renameErr = errno;
    if (!copy_file_contents(tmpPath, finalPath)) {
        fprintf(stderr,
                "CTR cache: commit rename failed (%s), copy fallback also failed (%s)\n",
                strerror(renameErr), strerror(errno));
        return false;
    }
    remove(tmpPath);
    return true;
}

static bool read_header(FILE *f, CtrTextureCacheHeader *hdr) {
    return f && hdr && fread(hdr, sizeof(*hdr), 1, f) == 1 &&
           hdr->magic == CTR_TEXTURE_CACHE_MAGIC &&
           hdr->version == CTR_TEXTURE_CACHE_VERSION &&
           hdr->atlasSize == CTR_TEXTURE_CACHE_ATLAS_SIZE &&
           hdr->atlasCount < 32768u &&
           hdr->basePageId + hdr->atlasCount <= 32767u &&
           hdr->itemsOffset >= sizeof(*hdr) &&
           hdr->segmentsOffset >= hdr->itemsOffset &&
           hdr->atlasInfosOffset >= hdr->segmentsOffset &&
           hdr->atlasDataOffset >= hdr->atlasInfosOffset;
}

typedef struct {
    uint64_t size;
    uint32_t sampleHash;
} SrcFingerprint;

static uint32_t fnv1a32_update(uint32_t h, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 0x01000193u;
    }
    return h;
}

static bool compute_src_fingerprint(const char *path, SrcFingerprint *out) {
    if (!path || !out) return false;
    out->size = 0;
    out->sampleHash = 0;

    struct stat st;
    if (stat(path, &st) == 0) out->size = (uint64_t) st.st_size;
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    FSEEK64(f, 0, SEEK_END);
    int64_t fileSize = FTELL64(f);
    if (fileSize <= 0) {
        fclose(f);
        return false;
    }
    if (out->size == 0) out->size = (uint64_t) fileSize;

    static const double offsets[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    uint8_t buf[1024];
    uint32_t hash = 0x811c9dc5u;
    uint8_t sizeBytes[8];
    for (int i = 0; i < 8; i++) sizeBytes[i] = (uint8_t) (((uint64_t) fileSize) >> (i * 8));
    hash = fnv1a32_update(hash, sizeBytes, sizeof(sizeBytes));

    for (int i = 0; i < 5; i++) {
        int64_t target = (int64_t) ((double) fileSize * offsets[i]);
        int64_t maxStart = fileSize > (int64_t) sizeof(buf) ? fileSize - (int64_t) sizeof(buf) : 0;
        if (target > maxStart) target = maxStart;
        if (target < 0) target = 0;
        if (FSEEK64(f, target, SEEK_SET) != 0) break;
        size_t got = fread(buf, 1, sizeof(buf), f);
        if (got == 0) break;
        hash = fnv1a32_update(hash, buf, got);
    }

    fclose(f);
    out->sampleHash = hash;
    return true;
}

bool CtrTextureCache_indexIsCurrentPath(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    CtrTextureCacheHeader hdr;
    bool ok = read_header(f, &hdr);
    fclose(f);

#ifdef CTR_TEXTURE_CACHE_HOST
    if (ok && hdr.reserved != cache_build_flags()) {
        fprintf(stderr,
                "CTR cache: atlas option mismatch in %s (cache flags=%08X, requested=%08X); rebuilding\n",
                path, (unsigned) hdr.reserved, (unsigned) cache_build_flags());
        ok = false;
    }
#endif

    if (ok) {
        char cacheDir[CTR_TEXTURE_CACHE_PATH_MAX];
        snprintf(cacheDir, sizeof(cacheDir), "%s", path);
        char *slash = strrchr(cacheDir, '/');
        char *backslash = strrchr(cacheDir, '\\');
        if (!slash || (backslash && backslash > slash)) slash = backslash;
        if (!slash) return ok;
        *slash = '\0';

        char gameDir[CTR_TEXTURE_CACHE_PATH_MAX];
        snprintf(gameDir, sizeof(gameDir), "%s", cacheDir);
        slash = strrchr(gameDir, '/');
        backslash = strrchr(gameDir, '\\');
        if (!slash || (backslash && backslash > slash)) slash = backslash;
        if (!slash) return ok;
        *slash = '\0';

        char dataPath[CTR_TEXTURE_CACHE_PATH_MAX];
        size_t gameDirLen = strlen(gameDir);
        static const char dataWinSuffix[] = "/data.win";
        if (gameDirLen + sizeof(dataWinSuffix) > sizeof(dataPath)) return ok;
        memcpy(dataPath, gameDir, gameDirLen);
        memcpy(dataPath + gameDirLen, dataWinSuffix, sizeof(dataWinSuffix));

        SrcFingerprint fp = {0};
        if (!compute_src_fingerprint(dataPath, &fp)) {
            fprintf(stderr, "CTR cache: cannot fingerprint '%s'; accepting header-only %s\n",
                    dataPath, path);
            return ok;
        }
        ok = (hdr.srcSize == fp.size && hdr.srcSampleHash == fp.sampleHash);
        if (!ok) {
            fprintf(stderr,
                    "CTR cache: stale atlas %s for '%s' (cache size=%llu hash=%08X, file size=%llu hash=%08X)\n",
                    path, dataPath,
                    (unsigned long long) hdr.srcSize, (unsigned) hdr.srcSampleHash,
                    (unsigned long long) fp.size, (unsigned) fp.sampleHash);
        }
    }
    return ok;
}

static bool index_is_valid_for(DataWin *dw) {
    if (!dw) return false;
    char path[CTR_TEXTURE_CACHE_PATH_MAX];
    CtrTextureCache_indexPath(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    CtrTextureCacheHeader hdr;
    bool headerOk = read_header(f, &hdr) &&
                    hdr.tpagCount <= dw->tpag.count &&
                    hdr.segmentCount < 65536u * 16u;
    fclose(f);
    if (!headerOk) return false;
#ifdef CTR_TEXTURE_CACHE_HOST
    if (hdr.reserved != cache_build_flags()) {
        fprintf(stderr,
                "CTR cache: atlas option mismatch (cache flags=%08X, requested=%08X); rebuilding\n",
                (unsigned) hdr.reserved, (unsigned) cache_build_flags());
        return false;
    }
#endif
    SrcFingerprint fp = {0};
    if (!compute_src_fingerprint(dw->filePath, &fp)) {
        fprintf(stderr,
                "CTR cache: cannot fingerprint '%s'; treating cache as stale.\n",
                dw->filePath ? dw->filePath : "(null)");
        return false;
    }

    if (hdr.srcSize != fp.size ||
        hdr.srcSampleHash != fp.sampleHash) {
        fprintf(stderr,
                "CTR cache: data.win fingerprint mismatch — cache (size=%llu hash=%08X) vs file (size=%llu hash=%08X); rebuilding\n",
                (unsigned long long) hdr.srcSize, (unsigned) hdr.srcSampleHash,
                (unsigned long long) fp.size, (unsigned) fp.sampleHash);
        return false;
    }

    return true;
}

static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static inline uint8_t cache_luma(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t) (((uint32_t) r * 77u + (uint32_t) g * 150u + (uint32_t) b * 29u + 128u) >> 8);
}

static size_t cache_format_bytes(uint32_t format, uint32_t w, uint32_t h) {
    size_t pixels = (size_t) w * (size_t) h;
    switch (format) {
        case CTR_TEXTURE_CACHE_FORMAT_A4: return (pixels + 1u) >> 1;
        case CTR_TEXTURE_CACHE_FORMAT_LA4: return pixels;
        case CTR_TEXTURE_CACHE_FORMAT_ETC1: return pixels / 2u;
        case CTR_TEXTURE_CACHE_FORMAT_ETC1A4: return pixels;
        case CTR_TEXTURE_CACHE_FORMAT_RGBA4:
        default: return pixels * 2u;
    }
}

static bool cache_format_valid(uint32_t format) {
    return format == CTR_TEXTURE_CACHE_FORMAT_RGBA4 ||
           format == CTR_TEXTURE_CACHE_FORMAT_LA4 ||
           format == CTR_TEXTURE_CACHE_FORMAT_A4 ||
           format == CTR_TEXTURE_CACHE_FORMAT_ETC1 ||
           format == CTR_TEXTURE_CACHE_FORMAT_ETC1A4;
}

static bool cache_format_is_etc(uint32_t format) {
    return format == CTR_TEXTURE_CACHE_FORMAT_ETC1 ||
           format == CTR_TEXTURE_CACHE_FORMAT_ETC1A4;
}

static uint32_t next_cache_dim(uint32_t v) {
    uint32_t out = 8;
    while (out < v && out < CTR_TEXTURE_CACHE_ATLAS_SIZE) out <<= 1;
    if (out > CTR_TEXTURE_CACHE_ATLAS_SIZE) out = CTR_TEXTURE_CACHE_ATLAS_SIZE;
    return out;
}

static uint32_t pack_atlas_dims(uint32_t w, uint32_t h) {
    if (w == 0) w = 8;
    if (h == 0) h = 8;
    if (w > CTR_TEXTURE_CACHE_ATLAS_SIZE) w = CTR_TEXTURE_CACHE_ATLAS_SIZE;
    if (h > CTR_TEXTURE_CACHE_ATLAS_SIZE) h = CTR_TEXTURE_CACHE_ATLAS_SIZE;
    return (w & 0xFFFFu) | ((h & 0xFFFFu) << 16);
}

static void unpack_atlas_dims(uint32_t packed, uint32_t *w, uint32_t *h) {
    uint32_t pw = packed & 0xFFFFu;
    uint32_t ph = (packed >> 16) & 0xFFFFu;
    if (pw == 0 || pw > CTR_TEXTURE_CACHE_ATLAS_SIZE) pw = CTR_TEXTURE_CACHE_ATLAS_SIZE;
    if (ph == 0 || ph > CTR_TEXTURE_CACHE_ATLAS_SIZE) ph = CTR_TEXTURE_CACHE_ATLAS_SIZE;
    if (w) *w = pw;
    if (h) *h = ph;
}

static void set_nibble(uint8_t *data, size_t pixelIndex, uint8_t value) {
    uint8_t v = value & 0x0F;
    uint8_t *byte = &data[pixelIndex >> 1];
    if ((pixelIndex & 1u) == 0) *byte = (uint8_t) ((*byte & 0xF0u) | v);
    else *byte = (uint8_t) ((*byte & 0x0Fu) | (uint8_t) (v << 4));
}

static uint8_t get_nibble(const uint8_t *data, size_t pixelIndex) {
    uint8_t byte = data[pixelIndex >> 1];
    return (pixelIndex & 1u) == 0 ? (byte & 0x0Fu) : (byte >> 4);
}

static void cache_set_pixel(uint8_t *linear, uint32_t format, size_t pixelIndex,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    switch (format) {
        case CTR_TEXTURE_CACHE_FORMAT_A4:
            set_nibble(linear, pixelIndex, (uint8_t) (a >> 4));
            break;
        case CTR_TEXTURE_CACHE_FORMAT_LA4: {
            uint8_t l = (uint8_t) (cache_luma(r, g, b) >> 4);
            linear[pixelIndex] = (uint8_t) ((l << 4) | (a >> 4));
            break;
        }
        case CTR_TEXTURE_CACHE_FORMAT_RGBA4:
        default:
            ((uint16_t *) linear)[pixelIndex] = pack_rgba4444(r, g, b, a);
            break;
    }
}

static uint16_t cache_get_pixel16(const uint8_t *linear, uint32_t format, size_t pixelIndex) {
    if (format == CTR_TEXTURE_CACHE_FORMAT_RGBA4) return ((const uint16_t *) linear)[pixelIndex];
    if (format == CTR_TEXTURE_CACHE_FORMAT_LA4) return linear[pixelIndex];
    return get_nibble(linear, pixelIndex);
}

static void cache_set_tiled_pixel(uint8_t *tiled, uint32_t format, size_t pixelIndex, uint16_t value) {
    if (format == CTR_TEXTURE_CACHE_FORMAT_RGBA4) {
        ((uint16_t *) tiled)[pixelIndex] = value;
    } else if (format == CTR_TEXTURE_CACHE_FORMAT_LA4) {
        tiled[pixelIndex] = (uint8_t) value;
    } else {
        set_nibble(tiled, pixelIndex, (uint8_t) value);
    }
}

static uint32_t classify_cache_image_format(const uint8_t *pixels, int srcW, const CacheImage *img) {
    bool anyOpaque = false;
    bool anyTransparent = false;
    bool whiteAlphaMask = true;
    bool luminanceOk = true;

    for (int y = 0; y < img->h; y++) {
        const uint8_t *row = pixels + (((size_t) (img->srcY + y) * (size_t) srcW + (size_t) img->srcX) * 4u);
        for (int x = 0; x < img->w; x++) {
            uint8_t r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2], a = row[x * 4 + 3];
            if (a == 0) {
                anyTransparent = true;
                continue;
            }
            if (a < 255) anyTransparent = true;
            anyOpaque = true;
            uint8_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
            uint8_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
            if (r < 240 || g < 240 || b < 240) whiteAlphaMask = false;
            if (max - min > 10 && max > 32) luminanceOk = false;
        }
    }
    if (!anyOpaque) return CTR_TEXTURE_CACHE_FORMAT_LA4;
    if (whiteAlphaMask || luminanceOk) return CTR_TEXTURE_CACHE_FORMAT_LA4;
    if ((uint32_t) img->w * (uint32_t) img->h <= 64u)
        return CTR_TEXTURE_CACHE_FORMAT_RGBA4;
    if (!g_cacheUseEtc1) return CTR_TEXTURE_CACHE_FORMAT_RGBA4;
    return anyTransparent
               ? CTR_TEXTURE_CACHE_FORMAT_ETC1A4
               : CTR_TEXTURE_CACHE_FORMAT_ETC1;
}

static uint8_t *read_blob(FILE *fp, uint32_t off, uint32_t size) {
    if (!fp || !size) return NULL;
    if (FSEEK64(fp, off, SEEK_SET) != 0) return NULL;
    uint8_t *buf = malloc(size);
    if (buf && fread(buf, 1, size, fp) != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

static inline uint32_t morton_pos(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    r |= (x & 1u) << 0;
    r |= (y & 1u) << 1;
    r |= (x & 2u) << 1;
    r |= (y & 2u) << 2;
    r |= (x & 4u) << 2;
    r |= (y & 4u) << 3;
    return r;
}

static void tile_cache(const uint8_t *linear, uint8_t *tiled, uint32_t format,
                       int linW, int linH, int potW, int potH) {
    int blocksX = potW >> 3;
    int blocksY = potH >> 3;
    memset(tiled, 0, cache_format_bytes(format, (uint32_t) potW, (uint32_t) potH));
    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            size_t blockBase = (size_t) (by * blocksX + bx) * 64u;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    int sx = bx * 8 + x;
                    int syTop = (potH - 1) - (by * 8 + y);
                    uint16_t px = 0;
                    if (sx < linW && syTop >= 0 && syTop < linH)
                        px = cache_get_pixel16(linear, format, (size_t) syTop * (size_t) linW + (size_t) sx);
                    cache_set_tiled_pixel(tiled, format,
                                          blockBase + morton_pos((uint32_t) x, (uint32_t) y),
                                          px);
                }
            }
        }
    }
}

static int cmp_image_group(const void *a, const void *b) {
    const CacheImage *ia = &g_sortImages[*(const uint32_t *) a];
    const CacheImage *ib = &g_sortImages[*(const uint32_t *) b];
    if (ia->groupId < ib->groupId) return -1;
    if (ia->groupId > ib->groupId) return 1;
    return 0;
}

static int cmp_group_area_desc(const void *a, const void *b) {
    const CacheGroup *ga = (const CacheGroup *) a;
    const CacheGroup *gb = (const CacheGroup *) b;
    if (ga->area < gb->area) return 1;
    if (ga->area > gb->area) return -1;
    return 0;
}

static int cmp_image_size_desc(const void *a, const void *b) {
    const CacheImage *ia = &g_sortImages[*(const uint32_t *) a];
    const CacheImage *ib = &g_sortImages[*(const uint32_t *) b];
    int ma = ia->w > ia->h ? ia->w : ia->h;
    int mb = ib->w > ib->h ? ib->w : ib->h;
    if (ma != mb) return mb - ma;
    return (ib->w * ib->h) - (ia->w * ia->h);
}

static int cmp_image_atlas_src(const void *a, const void *b) {
    const CacheImage *ia = &g_sortImages[*(const uint32_t *) a];
    const CacheImage *ib = &g_sortImages[*(const uint32_t *) b];
    if (ia->atlasId < ib->atlasId) return -1;
    if (ia->atlasId > ib->atlasId) return 1;
    if (ia->srcPage < ib->srcPage) return -1;
    if (ia->srcPage > ib->srcPage) return 1;
    if (ia->tpagIndex < ib->tpagIndex) return -1;
    if (ia->tpagIndex > ib->tpagIndex) return 1;
    return 0;
}

static void packer_init(MaxRectsPacker *p) {
    p->count = 1;
    p->rects[0] = (PackRect){
        0, 0, (int) CTR_TEXTURE_CACHE_ATLAS_SIZE,
        (int) CTR_TEXTURE_CACHE_ATLAS_SIZE
    };
}

static bool pack_rects_overlap(PackRect a, PackRect b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

static bool pack_rect_contains(PackRect outer, PackRect inner) {
    return outer.x <= inner.x && outer.y <= inner.y &&
           outer.x + outer.w >= inner.x + inner.w &&
           outer.y + outer.h >= inner.y + inner.h;
}

static void packer_add_free(MaxRectsPacker *p, PackRect r) {
    if (r.w <= 0 || r.h <= 0 || p->count >= MAX_FREE_RECTS) return;
    p->rects[p->count++] = r;
}

static void packer_split(MaxRectsPacker *p, PackRect used) {
    for (int i = 0; i < p->count;) {
        PackRect freeR = p->rects[i];
        if (!pack_rects_overlap(freeR, used)) {
            i++;
            continue;
        }

        p->rects[i] = p->rects[--p->count];
        if (freeR.x < used.x)
            packer_add_free(p, (PackRect){freeR.x, freeR.y, used.x - freeR.x, freeR.h});
        if (freeR.x + freeR.w > used.x + used.w)
            packer_add_free(p, (PackRect){
                                used.x + used.w, freeR.y,
                                freeR.x + freeR.w - used.x - used.w, freeR.h
                            });
        if (freeR.y < used.y)
            packer_add_free(p, (PackRect){freeR.x, freeR.y, freeR.w, used.y - freeR.y});
        if (freeR.y + freeR.h > used.y + used.h)
            packer_add_free(p, (PackRect){
                                freeR.x, used.y + used.h, freeR.w,
                                freeR.y + freeR.h - used.y - used.h
                            });
    }
}

static void packer_prune(MaxRectsPacker *p) {
    for (int i = 0; i < p->count; i++) {
        for (int j = 0; j < p->count; j++) {
            if (i == j) continue;
            if (pack_rect_contains(p->rects[j], p->rects[i])) {
                p->rects[i] = p->rects[--p->count];
                i--;
                break;
            }
        }
    }
}

static bool packer_insert(MaxRectsPacker *p, int w, int h, int *outX, int *outY) {
    int best = -1;
    int bestShort = 0x7fffffff;
    int bestLong = 0x7fffffff;
    for (int i = 0; i < p->count; i++) {
        PackRect r = p->rects[i];
        if (r.w < w || r.h < h) continue;
        int leftoverH = r.w - w;
        int leftoverV = r.h - h;
        int shortSide = leftoverH < leftoverV ? leftoverH : leftoverV;
        int longSide = leftoverH > leftoverV ? leftoverH : leftoverV;
        if (shortSide < bestShort || (shortSide == bestShort && longSide < bestLong)) {
            best = i;
            bestShort = shortSide;
            bestLong = longSide;
        }
    }
    if (best < 0) return false;

    PackRect placed = {p->rects[best].x, p->rects[best].y, w, h};
    *outX = placed.x;
    *outY = placed.y;
    packer_split(p, placed);
    packer_prune(p);
    return true;
}

static bool add_cache_image(CacheImage **images, uint32_t *count, uint32_t *cap,
                            CacheImage img) {
    if (*count >= *cap) {
        uint32_t next = *cap ? (*cap * 2u) : 512u;
        CacheImage *grown = realloc(*images, (size_t) next * sizeof(CacheImage));
        if (!grown) return false;
        *images = grown;
        *cap = next;
    }
    (*images)[(*count)++] = img;
    return true;
}

static uint32_t pack_cache_images(CacheImage *images, uint32_t imageCount) {
    if (!images || imageCount == 0) return 0;

    uint32_t *order = malloc((size_t) imageCount * sizeof(uint32_t));
    if (!order) return 0;
    for (uint32_t i = 0; i < imageCount; i++) order[i] = i;

    g_sortImages = images;
    qsort(order, imageCount, sizeof(uint32_t), cmp_image_group);

    CacheGroup *groups = NULL;
    uint32_t groupCount = 0, groupCap = 0;
    for (uint32_t at = 0; at < imageCount;) {
        uint32_t start = at;
        uint32_t groupId = images[order[at]].groupId;
        uint64_t area = 0;
        uint32_t formatHint = images[order[at]].formatHint;
        while (at < imageCount && images[order[at]].groupId == groupId) {
            CacheImage *img = &images[order[at]];
            area += (uint64_t) img->w * (uint64_t) img->h;
            at++;
        }
        if (groupCount >= groupCap) {
            uint32_t next = groupCap ? groupCap * 2u : 128u;
            CacheGroup *grown = realloc(groups, (size_t) next * sizeof(CacheGroup));
            if (!grown) {
                free(groups);
                free(order);
                g_sortImages = NULL;
                return 0;
            }
            groups = grown;
            groupCap = next;
        }
        groups[groupCount++] = (CacheGroup){groupId, formatHint, start, at - start, area};
    }
    qsort(groups, groupCount, sizeof(CacheGroup), cmp_group_area_desc);

    CacheAtlas *atlases = NULL;
    uint32_t atlasCount = 0, atlasCap = 0;

    for (uint32_t g = 0; g < groupCount; g++) {
        CacheGroup *grp = &groups[g];
        uint32_t *idx = malloc((size_t) grp->count * sizeof(uint32_t));
        int *tryX = malloc((size_t) grp->count * sizeof(int));
        int *tryY = malloc((size_t) grp->count * sizeof(int));
        if (!idx || !tryX || !tryY) {
            free(idx);
            free(tryX);
            free(tryY);
            continue;
        }
        for (uint32_t i = 0; i < grp->count; i++) idx[i] = order[grp->start + i];
        qsort(idx, grp->count, sizeof(uint32_t), cmp_image_size_desc);

        bool placedGroup = false;
        for (uint32_t a = 0; a < atlasCount && !placedGroup; a++) {
            if (atlases[a].formatHint != grp->formatHint) continue;
            MaxRectsPacker clone = atlases[a].packer;
            bool allFit = true;
            for (uint32_t i = 0; i < grp->count; i++) {
                CacheImage *img = &images[idx[i]];
                if (!packer_insert(&clone, img->w, img->h, &tryX[i], &tryY[i])) {
                    allFit = false;
                    break;
                }
            }
            if (allFit) {
                atlases[a].packer = clone;
                for (uint32_t i = 0; i < grp->count; i++) {
                    CacheImage *img = &images[idx[i]];
                    img->atlasId = (int) a;
                    img->dstX = tryX[i];
                    img->dstY = tryY[i];
                }
                placedGroup = true;
            }
        }

        if (!placedGroup) {
            bool *remaining = calloc(grp->count, sizeof(bool));
            if (remaining) {
                for (uint32_t i = 0; i < grp->count; i++) remaining[i] = true;
                uint32_t left = grp->count;
                while (left > 0) {
                    if (atlasCount >= atlasCap) {
                        uint32_t next = atlasCap ? atlasCap * 2u : 32u;
                        CacheAtlas *grown = realloc(atlases, (size_t) next * sizeof(CacheAtlas));
                        if (!grown) break;
                        atlases = grown;
                        atlasCap = next;
                    }
                    uint32_t atlasId = atlasCount++;
                    packer_init(&atlases[atlasId].packer);
                    atlases[atlasId].formatHint = grp->formatHint;
                    bool any = false;
                    for (uint32_t i = 0; i < grp->count; i++) {
                        if (!remaining[i]) continue;
                        CacheImage *img = &images[idx[i]];
                        int px, py;
                        if (packer_insert(&atlases[atlasId].packer, img->w, img->h, &px, &py)) {
                            img->atlasId = (int) atlasId;
                            img->dstX = px;
                            img->dstY = py;
                            remaining[i] = false;
                            left--;
                            any = true;
                        }
                    }
                    if (!any) break;
                }
                free(remaining);
            }
        }

        free(idx);
        free(tryX);
        free(tryY);
    }

    free(atlases);
    free(groups);
    free(order);
    g_sortImages = NULL;
    return atlasCount;
}

static void assign_asset_groups(DataWin *dw, uint32_t *groupIds, uint32_t *nextGroup) {
    for (uint32_t s = 0; s < dw->sprt.count; s++) {
        Sprite *spr = &dw->sprt.sprites[s];
        uint32_t group = (*nextGroup)++;
        for (uint32_t f = 0; f < spr->textureCount; f++) {
            int32_t tpag = spr->tpagIndices ? spr->tpagIndices[f] : -1;
            if (tpag >= 0 && (uint32_t) tpag < dw->tpag.count && groupIds[tpag] == 0)
                groupIds[tpag] = group;
        }
    }
    for (uint32_t b = 0; b < dw->bgnd.count; b++) {
        int32_t tpag = dw->bgnd.backgrounds[b].tpagIndex;
        if (tpag >= 0 && (uint32_t) tpag < dw->tpag.count && groupIds[tpag] == 0)
            groupIds[tpag] = (*nextGroup)++;
    }
    for (uint32_t f = 0; f < dw->font.count; f++) {
        int32_t tpag = dw->font.fonts[f].tpagIndex;
        if (tpag >= 0 && (uint32_t) tpag < dw->tpag.count && groupIds[tpag] == 0)
            groupIds[tpag] = (*nextGroup)++;
    }
}

static void trim_cache_images(DataWin *dw, FILE *dwFile, CacheImage *images,
                              uint32_t *imageCount, bool gm2022) {
    if (!dw || !dwFile || !images || !imageCount || *imageCount == 0) return;

    uint32_t removed = 0;
    uint64_t oldArea = 0;
    uint64_t newArea = 0;

    for (uint32_t p = 0; p < dw->txtr.count; p++) {
        bool needed = false;
        for (uint32_t i = 0; i < *imageCount; i++) {
            if (images[i].srcPage == (int) p && images[i].w > 0 && images[i].h > 0) {
                needed = true;
                break;
            }
        }
        if (!needed) continue;

        Texture *t = &dw->txtr.textures[p];
        if (!t->blobSize) continue;
        uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
        if (!blob) continue;

        int srcW = 0, srcH = 0;
        uint8_t *pixels = ImageDecoder_decodeToRgba(blob, t->blobSize, gm2022, &srcW, &srcH);
        free(blob);
        if (!pixels) {
            fprintf(stderr, "CTR cache: trim skipped TXTR page %lu (decode failed)\n",
                    (unsigned long) p);
            continue;
        }

        for (uint32_t i = 0; i < *imageCount; i++) {
            CacheImage *img = &images[i];
            if (img->srcPage != (int) p || img->w <= 0 || img->h <= 0) continue;
            oldArea += (uint64_t) img->w * (uint64_t) img->h;

            if (img->srcX < 0 || img->srcY < 0 ||
                img->srcX + img->w > srcW || img->srcY + img->h > srcH) {
                newArea += (uint64_t) img->w * (uint64_t) img->h;
                continue;
            }

            int minX = img->w, minY = img->h;
            int maxX = -1, maxY = -1;
            for (int y = 0; y < img->h; y++) {
                const uint8_t *row = pixels + (((size_t) (img->srcY + y) * (size_t) srcW +
                                                (size_t) img->srcX) * 4u);
                for (int x = 0; x < img->w; x++) {
                    if (row[x * 4 + 3] == 0) continue;
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                }
            }

            if (maxX < 0 || maxY < 0) {
                img->w = 0;
                img->h = 0;
                removed++;
                continue;
            }

            img->srcX += minX;
            img->srcY += minY;
            img->localX += minX;
            img->localY += minY;
            img->w = maxX - minX + 1;
            img->h = maxY - minY + 1;
            img->formatHint = classify_cache_image_format(pixels, srcW, img);
            newArea += (uint64_t) img->w * (uint64_t) img->h;
        }

        ImageDecoder_freeRgba(pixels);
    }

    uint32_t out = 0;
    for (uint32_t i = 0; i < *imageCount; i++) {
        if (images[i].w <= 0 || images[i].h <= 0) continue;
        if (!cache_format_valid(images[i].formatHint))
            images[i].formatHint = CTR_TEXTURE_CACHE_FORMAT_RGBA4;
        images[i].groupId = (images[i].groupId << 4) | (images[i].formatHint & 0x0Fu);
        if (out != i) images[out] = images[i];
        out++;
    }
    *imageCount = out;

    if (oldArea > 0) {
        uint64_t saved = oldArea > newArea ? oldArea - newArea : 0;
        fprintf(stderr,
                "CTR cache: transparent trim removed %lu empty rects, saved %.2f MB of atlas input\n",
                (unsigned long) removed, (double) saved * 2.0 / (1024.0 * 1024.0));
    }
}

static inline void blit_cache_image_rgba8(uint8_t *atlasRgba,
                                          int atlasStride,
                                          const CacheImage *img,
                                          const uint8_t *srcPixels, int srcW) {
    for (int y = 0; y < img->h; y++) {
        const uint8_t *src = srcPixels + (((size_t) (img->srcY + y) * (size_t) srcW +
                                           (size_t) img->srcX) * 4u);
        uint8_t *dst = atlasRgba + (((size_t) (img->dstY + y) * (size_t) atlasStride +
                                     (size_t) img->dstX) * 4u);
        memcpy(dst, src, (size_t) img->w * 4u);
    }
}

static void convert_rgba8_to_cache_linear(const uint8_t *rgba, uint8_t *linear,
                                          uint32_t format, int w, int h) {
    size_t bytes = cache_format_bytes(format, (uint32_t) w, (uint32_t) h);
    memset(linear, 0, bytes);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = rgba + ((size_t) y * (size_t) w * 4u);
        for (int x = 0; x < w; x++) {
            cache_set_pixel(linear, format, (size_t) y * (size_t) w + (size_t) x,
                            row[x * 4 + 0], row[x * 4 + 1],
                            row[x * 4 + 2], row[x * 4 + 3]);
        }
    }
}

static bool write_final_atlas_from_rgba8(FILE *f, uint32_t format,
                                         const uint8_t *atlasRgba,
                                         uint8_t *linearBuf,
                                         uint8_t *tiledBuf,
                                         int atlasW, int atlasH) {
    const size_t outBytes = cache_format_bytes(format,
                                               (uint32_t) atlasW,
                                               (uint32_t) atlasH);
    if (cache_format_is_etc(format)) {
        bool withAlpha = (format == CTR_TEXTURE_CACHE_FORMAT_ETC1A4);
        if (!Etc1_encodeImageTiled(atlasRgba, atlasW, atlasH,
                                   withAlpha, tiledBuf)) {
            return false;
        }
    } else {
        convert_rgba8_to_cache_linear(atlasRgba, linearBuf, format,
                                      atlasW, atlasH);
        tile_cache(linearBuf, tiledBuf, format, atlasW, atlasH,
                   atlasW, atlasH);
    }
    return fwrite(tiledBuf, 1, outBytes, f) == outBytes;
}

static bool create_disk_tables(DataWin *dw, CacheImage *images, uint32_t imageCount,
                               CacheItemDisk **outItems, CacheSegmentDisk **outSegments,
                               uint32_t *outSegmentCount) {
    CacheItemDisk *items = calloc(dw->tpag.count ? dw->tpag.count : 1, sizeof(CacheItemDisk));
    uint32_t *counts = calloc(dw->tpag.count ? dw->tpag.count : 1, sizeof(uint32_t));
    if (!items || !counts) {
        free(items);
        free(counts);
        return false;
    }

    uint32_t segmentCount = 0;
    for (uint32_t i = 0; i < imageCount; i++) {
        if (images[i].atlasId < 0) continue;
        if (images[i].tpagIndex >= dw->tpag.count) continue;
        counts[images[i].tpagIndex]++;
        segmentCount++;
    }

    uint32_t at = 0;
    for (uint32_t i = 0; i < dw->tpag.count; i++) {
        if (counts[i] == 0) continue;
        items[i].flags = CACHE_ENTRY_VALID;
        items[i].segmentStart = at;
        items[i].segmentCount = counts[i];
        at += counts[i];
    }

    CacheSegmentDisk *segments = calloc(segmentCount ? segmentCount : 1, sizeof(CacheSegmentDisk));
    uint32_t *cursor = calloc(dw->tpag.count ? dw->tpag.count : 1, sizeof(uint32_t));
    if (!segments || !cursor) {
        free(items);
        free(counts);
        free(segments);
        free(cursor);
        return false;
    }
    for (uint32_t i = 0; i < dw->tpag.count; i++) cursor[i] = items[i].segmentStart;

    for (uint32_t i = 0; i < imageCount; i++) {
        CacheImage *img = &images[i];
        if (img->atlasId < 0 || img->tpagIndex >= dw->tpag.count) continue;
        uint32_t slot = cursor[img->tpagIndex]++;
        segments[slot] = (CacheSegmentDisk){
            .atlasIndex = (uint32_t) img->atlasId,
            .sourceX = (uint16_t) img->localX,
            .sourceY = (uint16_t) img->localY,
            .width = (uint16_t) img->w,
            .height = (uint16_t) img->h,
            .atlasX = (uint16_t) img->dstX,
            .atlasY = (uint16_t) img->dstY,
        };
    }

    free(counts);
    free(cursor);
    *outItems = items;
    *outSegments = segments;
    *outSegmentCount = segmentCount;
    return true;
}

typedef struct {
    uint8_t *data;
    bool isLinear;
} AtlasMem;

static AtlasMem alloc_atlas_mem(size_t bytes) {
    AtlasMem m = {NULL, false};
    m.data = linearAlloc(bytes);
    if (m.data) {
        m.isLinear = true;
        return m;
    }
    m.data = malloc(bytes);
    return m;
}

static void free_atlas_mem(AtlasMem *m) {
    if (!m || !m->data) return;
    if (m->isLinear) linearFree(m->data);
    else free(m->data);
    m->data = NULL;
}

void CtrTextureCache_prepare(DataWin *dw) {
    if (!dw) return;
    if (index_is_valid_for(dw)) {
        ensure_ready_flag();
        remove_old_ready_flags();
        return;
    }
    if (dw->txtr.count == 0 || !dw->txtr.textures) {
        fprintf(stderr,
                "CTR cache: cannot build atlas.bin without TXTR metadata; "
                "preprocess this game or parse data.win with parseTxtr=1 for cache generation.\n");
        return;
    }

    char atlasPath[CTR_TEXTURE_CACHE_PATH_MAX], tmpPath[CTR_TEXTURE_CACHE_PATH_MAX];
    const char *failReason = "unknown";
    CtrTextureCache_indexPath(atlasPath, sizeof(atlasPath));
    snprintf(tmpPath, sizeof(tmpPath), "%s/atlas.tmp", g_current_cache_dir);
    remove_if_exists(tmpPath);

    bool ok = true;
    FILE *dwFile = dw->filePath ? fopen(dw->filePath, "rb") : NULL;
    if (dwFile) setvbuf(dwFile, NULL, _IOFBF, 256 * 1024);
    if (!dwFile) {
        ok = false;
        failReason = "cannot open data.win";
    }

    uint32_t *groupIds = calloc(dw->tpag.count ? dw->tpag.count : 1, sizeof(uint32_t));
    CacheImage *images = NULL;
    uint32_t imageCount = 0, imageCap = 0;
    if (!groupIds) {
        ok = false;
        failReason = "group id allocation failed";
    }

    uint32_t nextGroup = 1;
    if (ok) assign_asset_groups(dw, groupIds, &nextGroup);
    bool gm2022 = ok && DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);

    if (ok) {
        for (uint32_t i = 0; i < dw->tpag.count; i++) {
            TexturePageItem *item = &dw->tpag.items[i];
            if (item->texturePageId < 0 || (uint32_t) item->texturePageId >= dw->txtr.count) continue;
            int w = item->sourceWidth;
            int h = item->sourceHeight;
            if (w <= 0 || h <= 0) continue;
            uint32_t group = groupIds[i] ? groupIds[i] : nextGroup++;
            for (int y = 0; y < h; y += (int) CTR_TEXTURE_CACHE_ATLAS_SIZE) {
                int segH = h - y;
                if (segH > (int) CTR_TEXTURE_CACHE_ATLAS_SIZE) segH = (int) CTR_TEXTURE_CACHE_ATLAS_SIZE;
                for (int x = 0; x < w; x += (int) CTR_TEXTURE_CACHE_ATLAS_SIZE) {
                    int segW = w - x;
                    if (segW > (int) CTR_TEXTURE_CACHE_ATLAS_SIZE) segW = (int) CTR_TEXTURE_CACHE_ATLAS_SIZE;
                    CacheImage img = {
                        .tpagIndex = i,
                        .groupId = group,
                        .srcPage = item->texturePageId,
                        .srcX = item->sourceX + x,
                        .srcY = item->sourceY + y,
                        .localX = x,
                        .localY = y,
                        .w = segW,
                        .h = segH,
                        .atlasId = -1,
                        .dstX = 0,
                        .dstY = 0,
                    };
                    if (!add_cache_image(&images, &imageCount, &imageCap, img)) {
                        ok = false;
                        failReason = "cache image allocation failed";
                        break;
                    }
                }
                if (!ok) break;
            }
            if (!ok) break;
        }
    }
    if (ok) trim_cache_images(dw, dwFile, images, &imageCount, gm2022);
    uint32_t atlasCount = ok ? pack_cache_images(images, imageCount) : 0;
    if (ok && imageCount > 0 && atlasCount == 0) {
        ok = false;
        failReason = "MaxRects produced no atlases";
    }
    if (ok && dw->txtr.count + atlasCount > 32767u) {
        ok = false;
        failReason = "too many texture pages";
    }

    CacheItemDisk *items = NULL;
    CacheSegmentDisk *segments = NULL;
    uint32_t segmentCount = 0;
    if (ok && !create_disk_tables(dw, images, imageCount, &items, &segments, &segmentCount)) {
        ok = false;
        failReason = "disk table allocation failed";
    }

    CacheAtlasDisk *atlasInfos = NULL;
    uint32_t rgba4Count = 0, la4Count = 0, a4Count = 0, etc1Count = 0, etc1a4Count = 0;
    if (ok) {
        atlasInfos = calloc(atlasCount ? atlasCount : 1, sizeof(CacheAtlasDisk));
        if (!atlasInfos) {
            ok = false;
            failReason = "atlas info allocation failed";
        }
    }
    if (ok) {
        for (uint32_t a = 0; a < atlasCount; a++) atlasInfos[a].format = 0;
        for (uint32_t i = 0; i < imageCount; i++) {
            CacheImage *img = &images[i];
            if (img->atlasId < 0 || (uint32_t) img->atlasId >= atlasCount) continue;
            uint32_t fmt = cache_format_valid(img->formatHint)
                               ? img->formatHint
                               : CTR_TEXTURE_CACHE_FORMAT_RGBA4;
            uint32_t *atlasFmt = &atlasInfos[img->atlasId].format;
            uint32_t usedW = (uint32_t) (img->dstX + img->w);
            uint32_t usedH = (uint32_t) (img->dstY + img->h);
            uint32_t curW = atlasInfos[img->atlasId].reserved & 0xFFFFu;
            uint32_t curH = (atlasInfos[img->atlasId].reserved >> 16) & 0xFFFFu;
            if (usedW > curW) curW = usedW;
            if (usedH > curH) curH = usedH;
            atlasInfos[img->atlasId].reserved = pack_atlas_dims(curW, curH);
            if (*atlasFmt == 0 || *atlasFmt == fmt) {
                *atlasFmt = fmt;
            } else if (fmt == CTR_TEXTURE_CACHE_FORMAT_RGBA4 ||
                       *atlasFmt == CTR_TEXTURE_CACHE_FORMAT_RGBA4) {
                *atlasFmt = CTR_TEXTURE_CACHE_FORMAT_RGBA4;
            } else if (fmt == CTR_TEXTURE_CACHE_FORMAT_LA4 ||
                       fmt == CTR_TEXTURE_CACHE_FORMAT_A4 ||
                       *atlasFmt == CTR_TEXTURE_CACHE_FORMAT_LA4 ||
                       *atlasFmt == CTR_TEXTURE_CACHE_FORMAT_A4) {
                *atlasFmt = CTR_TEXTURE_CACHE_FORMAT_RGBA4;
            } else if (fmt == CTR_TEXTURE_CACHE_FORMAT_ETC1A4 ||
                       *atlasFmt == CTR_TEXTURE_CACHE_FORMAT_ETC1A4) {
                *atlasFmt = CTR_TEXTURE_CACHE_FORMAT_ETC1A4;
            } else {
                *atlasFmt = CTR_TEXTURE_CACHE_FORMAT_ETC1;
            }
        }
        for (uint32_t a = 0; a < atlasCount; a++) {
            if (!cache_format_valid(atlasInfos[a].format))
                atlasInfos[a].format = CTR_TEXTURE_CACHE_FORMAT_RGBA4;
            uint32_t atlasW = 0, atlasH = 0;
            unpack_atlas_dims(atlasInfos[a].reserved, &atlasW, &atlasH);
            atlasW = next_cache_dim(atlasW);
            atlasH = next_cache_dim(atlasH);
            atlasInfos[a].reserved = pack_atlas_dims(atlasW, atlasH);
            atlasInfos[a].size = (uint32_t) cache_format_bytes(
                atlasInfos[a].format,
                atlasW,
                atlasH);
            if (atlasInfos[a].format == CTR_TEXTURE_CACHE_FORMAT_RGBA4) rgba4Count++;
            else if (atlasInfos[a].format == CTR_TEXTURE_CACHE_FORMAT_LA4) la4Count++;
            else if (atlasInfos[a].format == CTR_TEXTURE_CACHE_FORMAT_A4) a4Count++;
            else if (atlasInfos[a].format == CTR_TEXTURE_CACHE_FORMAT_ETC1) etc1Count++;
            else if (atlasInfos[a].format == CTR_TEXTURE_CACHE_FORMAT_ETC1A4) etc1a4Count++;
        }
    }
    const size_t atlasRgbaBytes = (size_t) CTR_TEXTURE_CACHE_ATLAS_SIZE *
                                  (size_t) CTR_TEXTURE_CACHE_ATLAS_SIZE * 4u;
    const size_t maxAtlasBytes = cache_format_bytes(CTR_TEXTURE_CACHE_FORMAT_RGBA4,
                                                    CTR_TEXTURE_CACHE_ATLAS_SIZE,
                                                    CTR_TEXTURE_CACHE_ATLAS_SIZE);

    FILE *atlasFile = NULL;
    CtrTextureCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    uint64_t atlasDataBytes = 0;
    if (ok) {
        hdr.magic = CTR_TEXTURE_CACHE_MAGIC;
        hdr.version = CTR_TEXTURE_CACHE_VERSION;
        hdr.tpagCount = dw->tpag.count;
        hdr.basePageId = dw->txtr.count;
        hdr.atlasCount = atlasCount;
        hdr.atlasSize = CTR_TEXTURE_CACHE_ATLAS_SIZE;
        hdr.segmentCount = segmentCount;
        hdr.itemsOffset = sizeof(CtrTextureCacheHeader);
        hdr.segmentsOffset = hdr.itemsOffset + (uint32_t) (dw->tpag.count * sizeof(CacheItemDisk));
        hdr.atlasInfosOffset = hdr.segmentsOffset + (uint32_t) (segmentCount * sizeof(CacheSegmentDisk));
        hdr.atlasDataOffset = hdr.atlasInfosOffset + (uint32_t) (atlasCount * sizeof(CacheAtlasDisk));

        SrcFingerprint fp = {0};
        compute_src_fingerprint(dw->filePath, &fp);
        hdr.srcSize = fp.size;
        hdr.srcMtime = 0;
        hdr.srcSampleHash = fp.sampleHash;
        hdr.reserved = cache_build_flags();

        uint64_t off = hdr.atlasDataOffset;
        for (uint32_t a = 0; a < atlasCount; a++) {
            atlasInfos[a].offset = (uint32_t) off;
            off += atlasInfos[a].size;
            atlasDataBytes += atlasInfos[a].size;
        }
        if (off > 0xFFFFFFFFull) {
            ok = false;
            failReason = "atlas.bin would exceed 4GB";
        } else {
            fprintf(stderr,
                    "CTR cache: packing %lu images into %lu atlases, final atlas data %.2f MB "
                    "(RGBA4=%lu LA4=%lu A4=%lu ETC1=%lu ETC1A4=%lu)\n",
                    (unsigned long) imageCount, (unsigned long) atlasCount,
                    (double) atlasDataBytes / (1024.0 * 1024.0),
                    (unsigned long) rgba4Count, (unsigned long) la4Count,
                    (unsigned long) a4Count, (unsigned long) etc1Count,
                    (unsigned long) etc1a4Count);
        }
    }

    if (ok) {
        atlasFile = fopen(tmpPath, "wb");
        if (!atlasFile) {
            ok = false;
            failReason = "cannot create atlas.tmp";
        }
    }
    if (ok) {
        setvbuf(atlasFile, NULL, _IOFBF, 512 * 1024);
        if (!(fwrite(&hdr, sizeof(hdr), 1, atlasFile) == 1 &&
              fwrite(items, sizeof(CacheItemDisk), dw->tpag.count, atlasFile) == dw->tpag.count &&
              fwrite(segments, sizeof(CacheSegmentDisk), segmentCount, atlasFile) == segmentCount &&
              fwrite(atlasInfos, sizeof(CacheAtlasDisk), atlasCount, atlasFile) == atlasCount)) {
            ok = false;
            failReason = "failed writing atlas tables";
        }
    }

    uint32_t *imgOrder = NULL;
    AtlasMem atlasRgba = {NULL, false};
    AtlasMem linearBuf = {NULL, false};
    AtlasMem tiledBuf = {NULL, false};
    if (ok && imageCount > 0) {
        imgOrder = malloc((size_t) imageCount * sizeof(uint32_t));
        if (!imgOrder) {
            ok = false;
            failReason = "image order alloc";
        }
    }
    if (ok && imgOrder) {
        for (uint32_t i = 0; i < imageCount; i++) imgOrder[i] = i;
        g_sortImages = images;
        qsort(imgOrder, imageCount, sizeof(uint32_t), cmp_image_atlas_src);
        g_sortImages = NULL;
    }
    if (ok && atlasCount > 0) {
        atlasRgba = alloc_atlas_mem(atlasRgbaBytes);
        linearBuf = alloc_atlas_mem(maxAtlasBytes);
        tiledBuf = alloc_atlas_mem(maxAtlasBytes);
        if (!atlasRgba.data || !linearBuf.data || !tiledBuf.data) {
            ok = false;
            failReason = "atlas encode buffer alloc";
        }
    }

    uint32_t orderPos = 0;
    for (uint32_t a = 0; ok && a < atlasCount; a++) {
        if (g_progressCallback) g_progressCallback(a, atlasCount, atlasPath, g_progressUser);
        uint32_t atlasW = 0, atlasH = 0;
        unpack_atlas_dims(atlasInfos[a].reserved, &atlasW, &atlasH);
        memset(atlasRgba.data, 0, (size_t) atlasW * (size_t) atlasH * 4u);

        while (orderPos < imageCount) {
            CacheImage *peek = &images[imgOrder[orderPos]];
            if (peek->atlasId < 0 || (uint32_t) peek->atlasId < a) {
                orderPos++;
                continue;
            }
            break;
        }

        uint32_t atlasStart = orderPos;
        while (orderPos < imageCount &&
               images[imgOrder[orderPos]].atlasId == (int) a) {
            orderPos++;
        }
        uint32_t atlasEnd = orderPos;

        for (uint32_t i = atlasStart; ok && i < atlasEnd;) {
            int srcPage = images[imgOrder[i]].srcPage;
            uint32_t groupStart = i;
            while (i < atlasEnd && images[imgOrder[i]].srcPage == srcPage) i++;
            uint32_t groupEnd = i;

            if (srcPage < 0 || (uint32_t) srcPage >= dw->txtr.count) continue;
            Texture *t = &dw->txtr.textures[srcPage];
            uint8_t *pixels = NULL;
            int w = 0, h = 0;
            if (t->blobSize) {
                uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
                if (blob) {
                    pixels = ImageDecoder_decodeToRgba(blob, t->blobSize, gm2022, &w, &h);
                    free(blob);
                } else {
                    fprintf(stderr, "CTR cache: failed to read TXTR page %d blob\n", srcPage);
                }
            } else {
                fprintf(stderr, "CTR cache: TXTR page %d has no embedded blob\n", srcPage);
            }
            if (!pixels) {
                fprintf(stderr,
                        "CTR cache: failed to decode TXTR page %d; atlas entries stay blank\n",
                        srcPage);
                continue;
            }

            for (uint32_t k = groupStart; k < groupEnd; k++) {
                CacheImage *img = &images[imgOrder[k]];
                if (img->srcX < 0 || img->srcY < 0 ||
                    img->srcX + img->w > w || img->srcY + img->h > h) {
                    fprintf(stderr,
                            "CTR cache: skipping out-of-bounds TPAG %lu on TXTR page %d (%d,%d %dx%d over %dx%d)\n",
                            (unsigned long) img->tpagIndex, srcPage,
                            img->srcX, img->srcY, img->w, img->h, w, h);
                    continue;
                }
                if (img->dstX + img->w > (int) CTR_TEXTURE_CACHE_ATLAS_SIZE ||
                    img->dstY + img->h > (int) CTR_TEXTURE_CACHE_ATLAS_SIZE) {
                    fprintf(stderr,
                            "CTR cache: TPAG %lu dst out of atlas (%d,%d %dx%d, atlas %d)\n",
                            (unsigned long) img->tpagIndex,
                            img->dstX, img->dstY, img->w, img->h, img->atlasId);
                    continue;
                }
                blit_cache_image_rgba8(atlasRgba.data, (int) atlasW, img, pixels, w);
            }
            ImageDecoder_freeRgba(pixels);
        }

        if (!write_final_atlas_from_rgba8(atlasFile, atlasInfos[a].format,
                                          atlasRgba.data, linearBuf.data,
                                          tiledBuf.data,
                                          (int) atlasW, (int) atlasH)) {
            ok = false;
            failReason = "atlas encode/write failed";
        }
    }

    free_atlas_mem(&atlasRgba);
    free_atlas_mem(&linearBuf);
    free_atlas_mem(&tiledBuf);
    free(imgOrder);

#if 0
    const size_t maxAtlasBytes = cache_format_bytes(CTR_TEXTURE_CACHE_FORMAT_RGBA4,
                                                    CTR_TEXTURE_CACHE_ATLAS_SIZE,
                                                    CTR_TEXTURE_CACHE_ATLAS_SIZE);

    FILE *atlasFile = NULL;
    CtrTextureCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    AtlasMem atlasBuf = {NULL, false};
    AtlasMem zeroBuf = {NULL, false};

    if (ok) {
        atlasFile = fopen(tmpPath, "w+b");
        if (!atlasFile) {
            ok = false;
            failReason = "cannot create atlas.tmp";
        }
    }
    if (ok) {
        setvbuf(atlasFile, NULL, _IOFBF, 512 * 1024);
        hdr.magic = CTR_TEXTURE_CACHE_MAGIC;
        hdr.version = CTR_TEXTURE_CACHE_VERSION;
        hdr.tpagCount = dw->tpag.count;
        hdr.basePageId = dw->txtr.count;
        hdr.atlasCount = atlasCount;
        hdr.atlasSize = CTR_TEXTURE_CACHE_ATLAS_SIZE;
        hdr.segmentCount = segmentCount;
        hdr.itemsOffset = sizeof(CtrTextureCacheHeader);
        hdr.segmentsOffset = hdr.itemsOffset + (uint32_t) (dw->tpag.count * sizeof(CacheItemDisk));
        hdr.atlasInfosOffset = hdr.segmentsOffset + (uint32_t) (segmentCount * sizeof(CacheSegmentDisk));
        hdr.atlasDataOffset = hdr.atlasInfosOffset + (uint32_t) (atlasCount * sizeof(CacheAtlasDisk));
        SrcFingerprint fp = {0};
        compute_src_fingerprint(dw->filePath, &fp);
        hdr.srcSize = fp.size;
        hdr.srcMtime = 0;
        hdr.srcSampleHash = fp.sampleHash;
        hdr.reserved = cache_build_flags();
        uint32_t off = hdr.atlasDataOffset;
        for (uint32_t a = 0; a < atlasCount; a++) {
            atlasInfos[a].offset = off;
            off += atlasInfos[a].size;
        }

        if (!(fwrite(&hdr, sizeof(hdr), 1, atlasFile) == 1 &&
              fwrite(items, sizeof(CacheItemDisk), dw->tpag.count, atlasFile) == dw->tpag.count &&
              fwrite(segments, sizeof(CacheSegmentDisk), segmentCount, atlasFile) == segmentCount &&
              fwrite(atlasInfos, sizeof(CacheAtlasDisk), atlasCount, atlasFile) == atlasCount)) {
            ok = false;
            failReason = "failed writing atlas tables";
        }
    }
    if (ok && atlasCount > 0) {
        zeroBuf = alloc_atlas_mem(maxAtlasBytes);
        if (!zeroBuf.data) {
            ok = false;
            failReason = "zero buffer alloc";
        } else {
            memset(zeroBuf.data, 0, maxAtlasBytes);
            for (uint32_t a = 0; ok && a < atlasCount; a++) {
                if (fwrite(zeroBuf.data, 1, atlasInfos[a].size, atlasFile) != atlasInfos[a].size) {
                    ok = false;
                    failReason = "failed reserving atlas bytes";
                }
            }
        }
        free_atlas_mem(&zeroBuf);
    }
    if (ok && atlasCount > 0) {
        atlasBuf = alloc_atlas_mem(maxAtlasBytes);
        if (!atlasBuf.data) {
            ok = false;
            failReason = "atlas RMW buffer alloc";
        }
    }
    uint32_t *imgOrder = NULL;
    if (ok && imageCount > 0) {
        imgOrder = malloc(imageCount * sizeof(uint32_t));
        if (!imgOrder) {
            ok = false;
            failReason = "image order alloc";
        }
    }
    if (ok && imgOrder) {
        for (uint32_t i = 0; i < imageCount; i++) imgOrder[i] = i;
        for (uint32_t i = 1; i < imageCount; i++) {
            uint32_t key = imgOrder[i];
            CacheImage *kImg = &images[key];
            int32_t kp = kImg->srcPage, ka = kImg->atlasId;
            int32_t j = (int32_t) i - 1;
            while (j >= 0) {
                CacheImage *jImg = &images[imgOrder[j]];
                if (jImg->srcPage < kp || (jImg->srcPage == kp && jImg->atlasId <= ka)) break;
                imgOrder[j + 1] = imgOrder[j];
                j--;
            }
            imgOrder[j + 1] = key;
        }
    }

    if (ok) {
        for (uint32_t p = 0; ok && p < dw->txtr.count; p++) {
            if (g_progressCallback) g_progressCallback(p, dw->txtr.count, atlasPath, g_progressUser);
            uint32_t rangeStart = 0, rangeEnd = 0;
            bool hasAny = false;
            for (uint32_t i = 0; imgOrder && i < imageCount; i++) {
                CacheImage *img = &images[imgOrder[i]];
                if (img->srcPage == (int) p && img->atlasId >= 0) {
                    if (!hasAny) {
                        rangeStart = i;
                        hasAny = true;
                    }
                    rangeEnd = i + 1;
                }
            }
            if (!hasAny) continue;
            Texture *t = &dw->txtr.textures[p];
            uint8_t *pixels = NULL;
            int w = 0, h = 0;
            if (t->blobSize) {
                uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
                if (blob) {
                    pixels = ImageDecoder_decodeToRgba(blob, t->blobSize, gm2022, &w, &h);
                    free(blob);
                } else {
                    fprintf(stderr, "CTR cache: failed to read TXTR page %lu blob\n",
                            (unsigned long) p);
                }
            } else {
                fprintf(stderr, "CTR cache: TXTR page %lu has no embedded blob\n",
                        (unsigned long) p);
            }
            if (!pixels) {
                fprintf(stderr, "CTR cache: failed to decode TXTR page %lu; atlas entries stay blank\n",
                        (unsigned long) p);
                continue;
            }
            uint32_t i = rangeStart;
            while (ok && i < rangeEnd) {
                int32_t curAtlas = images[imgOrder[i]].atlasId;
                uint32_t groupStart = i;
                while (i < rangeEnd && images[imgOrder[i]].atlasId == curAtlas) i++;
                uint32_t groupEnd = i;
                size_t atlasBytes = atlasInfos[curAtlas].size;
                uint32_t atlasFormat = atlasInfos[curAtlas].format;
                if (fseek(atlasFile, (long) atlasInfos[curAtlas].offset, SEEK_SET) != 0 ||
                    fread(atlasBuf.data, 1, atlasBytes, atlasFile) != atlasBytes) {
                    ok = false;
                    failReason = "atlas RMW read failed";
                    break;
                }
                for (uint32_t k = groupStart; k < groupEnd; k++) {
                    CacheImage *img = &images[imgOrder[k]];
                    if (img->srcX < 0 || img->srcY < 0 ||
                        img->srcX + img->w > w || img->srcY + img->h > h) {
                        fprintf(stderr,
                                "CTR cache: skipping out-of-bounds TPAG %lu on TXTR page %lu (%d,%d %dx%d over %dx%d)\n",
                                (unsigned long) img->tpagIndex, (unsigned long) p,
                                img->srcX, img->srcY, img->w, img->h, w, h);
                        continue;
                    }
                    if (img->dstX + img->w > (int) CTR_TEXTURE_CACHE_ATLAS_SIZE ||
                        img->dstY + img->h > (int) CTR_TEXTURE_CACHE_ATLAS_SIZE) {
                        fprintf(stderr,
                                "CTR cache: TPAG %lu dst out of atlas (%d,%d %dx%d, atlas %d)\n",
                                (unsigned long) img->tpagIndex,
                                img->dstX, img->dstY, img->w, img->h, img->atlasId);
                        continue;
                    }
                    blit_cache_image_mem(atlasBuf.data, atlasFormat, img, pixels, w);
                }
                if (fseek(atlasFile, (long) atlasInfos[curAtlas].offset, SEEK_SET) != 0 ||
                    fwrite(atlasBuf.data, 1, atlasBytes, atlasFile) != atlasBytes) {
                    ok = false;
                    failReason = "atlas RMW write failed";
                    break;
                }
            }

            ImageDecoder_freeRgba(pixels);
        }
    }
    if (ok && atlasCount > 0) {
        AtlasMem tiledBuf = alloc_atlas_mem(maxAtlasBytes);
        if (!tiledBuf.data) {
            ok = false;
            failReason = "swizzle buffer alloc";
        }
        for (uint32_t a = 0; ok && a < atlasCount; a++) {
            size_t atlasBytes = atlasInfos[a].size;
            uint32_t atlasFormat = atlasInfos[a].format;
            if (fseek(atlasFile, (long) atlasInfos[a].offset, SEEK_SET) != 0 ||
                fread(atlasBuf.data, 1, atlasBytes, atlasFile) != atlasBytes) {
                ok = false;
                failReason = "swizzle read failed";
                break;
            }
            tile_cache(atlasBuf.data, tiledBuf.data, atlasFormat,
                       (int) CTR_TEXTURE_CACHE_ATLAS_SIZE,
                       (int) CTR_TEXTURE_CACHE_ATLAS_SIZE,
                       (int) CTR_TEXTURE_CACHE_ATLAS_SIZE,
                       (int) CTR_TEXTURE_CACHE_ATLAS_SIZE);
            if (fseek(atlasFile, (long) atlasInfos[a].offset, SEEK_SET) != 0 ||
                fwrite(tiledBuf.data, 1, atlasBytes, atlasFile) != atlasBytes) {
                ok = false;
                failReason = "swizzle write failed";
                break;
            }
        }
        free_atlas_mem(&tiledBuf);
    }

    free_atlas_mem(&atlasBuf);
    free(imgOrder);
#endif

    if (atlasFile) {
        if (fclose(atlasFile) != 0 && ok) {
            ok = false;
            failReason = "failed flushing atlas.tmp";
        }
        atlasFile = NULL;
    }
    if (dwFile) fclose(dwFile);

    if (ok && g_progressCallback) g_progressCallback(atlasCount, atlasCount, atlasPath, g_progressUser);

    if (ok) {
        if (!commit_tmp_file(tmpPath, atlasPath)) {
            ok = false;
            failReason = "failed committing atlas.tmp to atlas.bin";
        }
    }
    if (ok && CtrTextureCache_indexIsCurrentPath(atlasPath)) {
        ensure_ready_flag();
        remove_old_ready_flags();
        cleanup_legacy_page_files();
        fprintf(stderr,
                "CTR cache: committed %s (%lu atlases, %lu segments, max %upx pages; "
                "RGBA4=%lu LA4=%lu A4=%lu ETC1=%lu ETC1A4=%lu)\n",
                CTR_TEXTURE_CACHE_FILE, (unsigned long) atlasCount, (unsigned long) segmentCount,
                (unsigned) CTR_TEXTURE_CACHE_ATLAS_SIZE,
                (unsigned long) rgba4Count, (unsigned long) la4Count, (unsigned long) a4Count,
                (unsigned long) etc1Count, (unsigned long) etc1a4Count);
    } else {
        if (ok) failReason = "committed atlas.bin failed header validation";
        remove_if_exists(tmpPath);
        fprintf(stderr, "CTR cache: atlas.bin build failed before index commit: %s\n", failReason);
    }

    free(groupIds);
    free(images);
    free(items);
    free(segments);
    free(atlasInfos);
}

#ifndef CTR_TEXTURE_CACHE_HOST
static void log_apply_index_failure(DataWin *dw, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "CTR cache: apply failed: cannot open %s\n", path);
        return;
    }

    CtrTextureCacheHeader hdr = {0};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "CTR cache: apply failed: short header in %s\n", path);
        fclose(f);
        return;
    }
    fclose(f);

    if (hdr.magic != CTR_TEXTURE_CACHE_MAGIC) {
        fprintf(stderr, "CTR cache: apply failed: bad atlas magic %08X in %s\n",
                (unsigned) hdr.magic, path);
    } else if (hdr.version != CTR_TEXTURE_CACHE_VERSION) {
        fprintf(stderr, "CTR cache: apply failed: atlas version %u, runner expects %u (%s)\n",
                (unsigned) hdr.version, (unsigned) CTR_TEXTURE_CACHE_VERSION, path);
    } else if (hdr.atlasSize != CTR_TEXTURE_CACHE_ATLAS_SIZE) {
        fprintf(stderr, "CTR cache: apply failed: atlas page size %u, runner expects %u (%s)\n",
                (unsigned) hdr.atlasSize, (unsigned) CTR_TEXTURE_CACHE_ATLAS_SIZE, path);
    } else if (dw && hdr.tpagCount > dw->tpag.count) {
        fprintf(stderr, "CTR cache: apply failed: atlas has %u TPAGs but data.win parsed %u (%s)\n",
                (unsigned) hdr.tpagCount, (unsigned) dw->tpag.count, path);
    } else if (hdr.segmentCount >= 65536u * 16u) {
        fprintf(stderr, "CTR cache: apply failed: atlas segment table is too large (%u)\n",
                (unsigned) hdr.segmentCount);
    } else {
        fprintf(stderr, "CTR cache: apply failed: stale atlas fingerprint or invalid offsets (%s)\n",
                path);
    }
}

bool CtrTextureCache_apply(CtrRenderer *ctx, DataWin *dw) {
    if (!ctx || !dw) return false;
    char path[CTR_TEXTURE_CACHE_PATH_MAX];
    CtrTextureCache_indexPath(path, sizeof(path));
    if (!index_is_valid_for(dw)) {
        log_apply_index_failure(dw, path);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "CTR cache: apply failed: cannot open %s\n", path);
        return false;
    }

    CtrTextureCacheHeader hdr;
    bool ok = false;
    if (read_header(f, &hdr) &&
        hdr.tpagCount <= dw->tpag.count &&
        hdr.basePageId + hdr.atlasCount <= 32767u) {
        CacheItemDisk *items = malloc((size_t) hdr.tpagCount * sizeof(CacheItemDisk));
        CacheSegmentDisk *segments = malloc((size_t) hdr.segmentCount * sizeof(CacheSegmentDisk));
        CacheAtlasDisk *atlasInfos = malloc((size_t) hdr.atlasCount * sizeof(CacheAtlasDisk));
        if (items && segments && atlasInfos &&
            fseek(f, (long) hdr.itemsOffset, SEEK_SET) == 0 &&
            fread(items, sizeof(CacheItemDisk), hdr.tpagCount, f) == hdr.tpagCount &&
            fseek(f, (long) hdr.segmentsOffset, SEEK_SET) == 0 &&
            fread(segments, sizeof(CacheSegmentDisk), hdr.segmentCount, f) == hdr.segmentCount &&
            fseek(f, (long) hdr.atlasInfosOffset, SEEK_SET) == 0 &&
            fread(atlasInfos, sizeof(CacheAtlasDisk), hdr.atlasCount, f) == hdr.atlasCount) {
            free(ctx->sourcePages);
            free(ctx->cacheItems);
            free(ctx->cacheSegments);
            ctx->sourcePages = calloc(hdr.atlasCount ? hdr.atlasCount : 1, sizeof(CtrSourcePage));
            ctx->cacheItems = calloc(hdr.tpagCount ? hdr.tpagCount : 1, sizeof(CtrCachedTpag));
            ctx->cacheSegments = calloc(hdr.segmentCount ? hdr.segmentCount : 1, sizeof(CtrCachedSegment));

            ok = (ctx->sourcePages || hdr.atlasCount == 0) &&
                 (ctx->cacheItems || hdr.tpagCount == 0) &&
                 (ctx->cacheSegments || hdr.segmentCount == 0);
            if (ok) {
                ctx->repackBasePageId = hdr.basePageId;
                ctx->repackPageCount = hdr.atlasCount;
                ctx->sourcePageCount = hdr.atlasCount;
                ctx->cacheItemCount = hdr.tpagCount;
                ctx->cacheSegmentCount = hdr.segmentCount;

                for (uint32_t i = 0; i < hdr.atlasCount; i++) {
                    CtrSourcePage *page = &ctx->sourcePages[i];
                    uint32_t atlasW = 0, atlasH = 0;
                    unpack_atlas_dims(atlasInfos[i].reserved, &atlasW, &atlasH);
                    page->fileOffset = atlasInfos[i].offset;
                    page->dataSize = atlasInfos[i].size;
                    page->format = cache_format_valid(atlasInfos[i].format)
                                       ? atlasInfos[i].format
                                       : CTR_TEXTURE_CACHE_FORMAT_RGBA4;
                    page->width = (int) atlasW;
                    page->height = (int) atlasH;
                    page->potW = (int) atlasW;
                    page->potH = (int) atlasH;
                }

                for (uint32_t i = 0; i < hdr.tpagCount; i++) {
                    CacheItemDisk *src = &items[i];
                    if ((src->flags & CACHE_ENTRY_VALID) == 0) continue;
                    if (src->segmentStart + src->segmentCount > hdr.segmentCount) continue;
                    ctx->cacheItems[i] = (CtrCachedTpag){
                        .valid = true,
                        .segmentStart = src->segmentStart,
                        .segmentCount = src->segmentCount,
                    };
                }

                for (uint32_t i = 0; i < hdr.segmentCount; i++) {
                    CacheSegmentDisk *src = &segments[i];
                    if (src->atlasIndex >= hdr.atlasCount) continue;
                    ctx->cacheSegments[i] = (CtrCachedSegment){
                        .atlasIndex = src->atlasIndex,
                        .sourceX = src->sourceX,
                        .sourceY = src->sourceY,
                        .width = src->width,
                        .height = src->height,
                        .atlasX = src->atlasX,
                        .atlasY = src->atlasY,
                    };
                }
            }
        }
        free(items);
        free(segments);
        free(atlasInfos);
    }

    fclose(f);
    if (!ok) {
        fprintf(stderr,
                "CTR cache: apply failed while reading atlas index tables from %s\n",
                path);
    }
    return ok;
}
#endif
