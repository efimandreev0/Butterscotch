#pragma once

#include "data_win.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTR_TEXTURE_CACHE_MAGIC      0x4B415052u
// Bumped when the cache layout or fingerprint scheme changes. v12 adds a
// data.win source fingerprint (size + mtime + GEN8 hash) to the header so
// stale caches from a different game / older mod version are rejected
// instead of rendering as garbage (e.g. Chinese glyph atlas leaking into
// Deltarune sprite slots after a game switch).
#define CTR_TEXTURE_CACHE_VERSION    17u
#define CTR_TEXTURE_CACHE_ATLAS_SIZE 256u
#define CTR_TEXTURE_CACHE_FILE       "atlas.bin"
#define CTR_TEXTURE_CACHE_READY_FLAG "cache_ready_v18.flag"

#define CTR_TEXTURE_CACHE_FORMAT_RGBA4 4u
#define CTR_TEXTURE_CACHE_FORMAT_LA4   9u
#define CTR_TEXTURE_CACHE_FORMAT_A4    11u

typedef struct CtrRenderer CtrRenderer;

typedef void (*CtrTextureCacheProgressFn)(uint32_t pageIndex, uint32_t pageCount,
                                          const char *path, void *user);

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t tpagCount;
    uint32_t basePageId;
    uint32_t atlasCount;
    uint32_t atlasSize;
    uint32_t segmentCount;
    uint32_t itemsOffset;
    uint32_t segmentsOffset;
    uint32_t atlasInfosOffset;
    uint32_t atlasDataOffset;
    // Source-file fingerprint of the data.win this cache was built for.
    // If any of these mismatches when we go to apply, the cache is rebuilt.
    uint64_t srcSize;       // bytes of data.win
    uint64_t srcMtime;      // unix mtime (or 0 if stat failed)
    uint32_t srcSampleHash; // FNV-1a of a few stable byte ranges of data.win
    uint32_t reserved;      // padding / future use
} CtrTextureCacheHeader;

void CtrTextureCache_indexPath(char *out, size_t outSize);
bool CtrTextureCache_indexIsCurrentPath(const char *path);
void CtrTextureCache_setProgressCallback(CtrTextureCacheProgressFn callback, void *user);
void CtrTextureCache_prepare(DataWin *dw);
bool CtrTextureCache_apply(CtrRenderer *ctx, DataWin *dw);
