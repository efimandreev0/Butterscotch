#include "ctr_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>

#include "stb_image.h"
#include "image_decoder.h"
#include "ctr_texture_cache.h"
#include "utils.h"

#include "render2d_shader_shbin.h"

extern char g_current_cache_dir[256];
extern char g_current_data_path[256];

extern DVLB_s *g_vshaderDvlb;
extern shaderProgram_s g_shaderProg;

#define BATCH_QUAD_CAP        2048
#define BATCH_VERT_CAP        (BATCH_QUAD_CAP * 6)
#define ATLAS_MAGIC           0x534C5441u  // 'ATLS'
#define ATLAS_TILED_MAGIC     0x544C5441u  // 'ATLT'
#define REPACK_MAGIC          0x4B415052u  // 'RPAK'
#define REPACK_VERSION        2u
#define CACHE_READY_FLAG      "cache_ready_v5.flag"
#define REPACK_INDEX_FILE     "atlas.bin"
#define MAX_RR_SEGMENTS       64
#define MAX_RR_POINTS         (MAX_RR_SEGMENTS * 4 + 1)
#define LINEAR_LOW            (6u * 1024u * 1024u)
#define LINEAR_SAFE           (8u * 1024u * 1024u)
#define CTR_PREFETCH_MIN_FREE (10u * 1024u * 1024u)
#define CTR_PREFETCH_ROOM_BUDGET 4u
#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#ifndef CTR_OPT_DIAGNOSTICS
#define CTR_OPT_DIAGNOSTICS 0
#endif

#if CTR_OPT_DIAGNOSTICS
#define CTR_DIAG(...) fprintf(stderr, __VA_ARGS__)
#else
#define CTR_DIAG(...) ((void)0)
#endif

#define MAX_GC_TARGETS 64
static C3D_RenderTarget* g_gc_targets[MAX_GC_TARGETS];
static int g_gc_target_count = 0;

// ---- Live theme + screen layout (driven by launcher) -----------------------

static CtrGameScreen g_ctr_game_screen = CTR_GAME_SCREEN_TOP;
static CtrBackdropMode g_ctr_backdrop_mode = CTR_BACKDROP_GRADIENT;
static CtrDisplayMode g_ctr_display_mode = CTR_DISPLAY_ORIGINAL;
static CtrAppFilterMode g_ctr_app_filter_mode = CTR_APP_FILTER_LINEAR;

static struct {
    float bgTop[3];
    float bgBot[3];
    float accent[3];
    float blurAlpha;
    float particleAlpha;
} g_letterbox = {
    .bgTop = {0.18f, 0.10f, 0.26f},
    .bgBot = {0.02f, 0.03f, 0.08f},
    .accent = {1.0f, 0.74f, 0.24f},
    .blurAlpha = 0.35f,
    .particleAlpha = 1.0f,
};

void CtrRenderer_setGameScreen(CtrGameScreen which) {
    if (which != CTR_GAME_SCREEN_TOP && which != CTR_GAME_SCREEN_BOTTOM) return;
    g_ctr_game_screen = which;
}

CtrGameScreen CtrRenderer_getGameScreen(void) { return g_ctr_game_screen; }

void CtrRenderer_setBackdropMode(CtrBackdropMode mode) {
    int value = (int)mode;
    if (value < (int)CTR_BACKDROP_GRADIENT || value > (int)CTR_BACKDROP_STRETCH)
        mode = CTR_BACKDROP_GRADIENT;
    g_ctr_backdrop_mode = mode;
}

CtrBackdropMode CtrRenderer_getBackdropMode(void) { return g_ctr_backdrop_mode; }

void CtrRenderer_setDisplayMode(CtrDisplayMode mode) {
    int value = (int)mode;
    if (value < 0 || value >= (int)CTR_DISPLAY_MODE_COUNT)
        mode = CTR_DISPLAY_ORIGINAL;
    g_ctr_display_mode = mode;
}

CtrDisplayMode CtrRenderer_getDisplayMode(void) { return g_ctr_display_mode; }

void CtrRenderer_setAppFilterMode(CtrAppFilterMode mode) {
    int value = (int)mode;
    if (value < (int)CTR_APP_FILTER_LINEAR || value >= (int)CTR_APP_FILTER_COUNT)
        mode = CTR_APP_FILTER_LINEAR;
    g_ctr_app_filter_mode = mode;
}

CtrAppFilterMode CtrRenderer_getAppFilterMode(void) { return g_ctr_app_filter_mode; }

static void apply_app_filter(CtrRenderer *ctx) {
    if (!ctx || !ctx->appTex.data) return;
    GPU_TEXTURE_FILTER_PARAM filter =
        (g_ctr_app_filter_mode == CTR_APP_FILTER_NEAREST) ? GPU_NEAREST : GPU_LINEAR;
    C3D_TexSetFilter(&ctx->appTex, filter, filter);
}

void CtrRenderer_setLetterboxTheme(float topR, float topG, float topB,
                                   float botR, float botG, float botB,
                                   float accentR, float accentG, float accentB,
                                   float blurAlpha, float particleAlpha) {
    g_letterbox.bgTop[0] = topR; g_letterbox.bgTop[1] = topG; g_letterbox.bgTop[2] = topB;
    g_letterbox.bgBot[0] = botR; g_letterbox.bgBot[1] = botG; g_letterbox.bgBot[2] = botB;
    g_letterbox.accent[0] = accentR; g_letterbox.accent[1] = accentG; g_letterbox.accent[2] = accentB;
    if (blurAlpha     < 0.f) blurAlpha     = 0.f;
    if (particleAlpha < 0.f) particleAlpha = 0.f;
    g_letterbox.blurAlpha = blurAlpha;
    g_letterbox.particleAlpha = particleAlpha;
}

C3D_RenderTarget *CtrRenderer_getTopTarget(Renderer *ren) {
    return ren ? ((CtrRenderer *)ren)->topTarget : NULL;
}

C3D_RenderTarget *CtrRenderer_getBottomTarget(Renderer *ren) {
    return ren ? ((CtrRenderer *)ren)->bottomTarget : NULL;
}

static void safe_delete_target(CtrRenderer *ctx, C3D_RenderTarget *target) {
    if (!target) return;
    if (ctx->inFrame && g_gc_target_count < MAX_GC_TARGETS) {
        g_gc_targets[g_gc_target_count++] = target;
    } else {
        C3D_RenderTargetDelete(target);
    }
}

static void gc_clear_targets() {
    for (int i = 0; i < g_gc_target_count; i++) {
        if (g_gc_targets[i]) {
            C3D_RenderTargetDelete(g_gc_targets[i]);
        }
    }
    g_gc_target_count = 0;
}

static void gc_add_target(C3D_RenderTarget* tgt) {
    if (!tgt) return;
    if (g_gc_target_count < MAX_GC_TARGETS) {
        g_gc_targets[g_gc_target_count++] = tgt;
    } else {
        C3D_RenderTargetDelete(tgt);
    }
}

// Utilities

typedef struct {
    uint32_t magic;
    uint32_t w, h;
} AtlasHeader;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t tpagCount;
    uint32_t basePageId;
    uint32_t atlasCount;
    uint32_t atlasSize;
} RepackHeader;

typedef struct {
    uint32_t flags;
    uint32_t pageId;
    uint16_t sourceX, sourceY;
    uint16_t sourceWidth, sourceHeight;
    uint16_t targetX, targetY;
    uint16_t targetWidth, targetHeight;
    uint16_t boundingWidth, boundingHeight;
} RepackMapEntry;

#define REPACK_ENTRY_VALID 1u

typedef struct {
    float    x, y, z;
    float    u, v;
    float    r, g, b, a;
} CtrVertex;

static uint32_t g_frame = 0;
static CtrRendererCacheProgressFn g_cacheProgressCallback = NULL;
static void *g_cacheProgressUser = NULL;

void CtrRenderer_setCacheProgressCallback(CtrRendererCacheProgressFn callback, void *user) {
    g_cacheProgressCallback = callback;
    g_cacheProgressUser = user;
    CtrTextureCache_setProgressCallback((CtrTextureCacheProgressFn)callback, user);
}

void CtrRenderer_resetSessionState(void) {
    CtrRenderer_setCacheProgressCallback(NULL, NULL);
    g_frame = 0;
}

static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static int next_pow2(int x) {
    if (x < 8) return 8;
    x--;
    x |= x >> 1;  x |= x >> 2;  x |= x >> 4;  x |= x >> 8;  x |= x >> 16;
    return x + 1;
}

static inline uint8_t clamp_u8(float a) {
    if (a <= 0.f) return 0;
    if (a >= 1.f) return 255;
    return (uint8_t)(a * 255.f);
}

static void col2fv(uint32_t col, float a, float out[4]) {
    out[0] = BGR_R(col) / 255.f;
    out[1] = BGR_G(col) / 255.f;
    out[2] = BGR_B(col) / 255.f;
    out[3] = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
}

static uint8_t *read_blob(FILE *fp, uint32_t off, uint32_t size) {
    if (!fp || !size) return NULL;
    fseek(fp, off, SEEK_SET);
    uint8_t *buf = malloc(size);
    if (buf && fread(buf, 1, size, fp) != size) { free(buf); return NULL; }
    return buf;
}

// Texture tiling

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

static void tile_rgba4(const uint16_t *linear, uint16_t *tiled,
                       int linW, int linH, int potW, int potH) {
    int blocksX = potW >> 3;
    int blocksY = potH >> 3;
    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            uint16_t *block = &tiled[(by * blocksX + bx) * 64];
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    int sx = bx * 8 + x;
                    int sy_top = (potH - 1) - (by * 8 + y);
                    uint16_t px = 0;
                    if (sx < linW && sy_top >= 0 && sy_top < linH) {
                        px = linear[sy_top * linW + sx];
                    }
                    block[morton_pos(x, y)] = px;
                }
            }
        }
    }
}

// Texture cache / local repack.
//
// data.win is never modified. First launch creates:
//   atlas.bin           - TPAG remap table (old TPAG index -> new page/x/y)
//   page_<id>.atlas     - linear RGBA4444 atlas pages, ATLS header + pixels
//
// Repacked pages start at dw->txtr.count, so old source page ids remain usable
// as a fallback for oversized TPAGs that cannot fit into a 512x512 atlas.

static void page_meta_path(char *out, size_t outSize, int pageId) {
    snprintf(out, outSize, "%s/page_%d.atlas", g_current_cache_dir, pageId);
}

static void repack_index_path(char *out, size_t outSize) {
    snprintf(out, outSize, "%s/%s", g_current_cache_dir, REPACK_INDEX_FILE);
}

static bool repack_index_is_valid(DataWin *dw) {
    if (!dw) return false;
    char path[256];
    repack_index_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    RepackHeader hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1 &&
              hdr.magic == REPACK_MAGIC &&
              hdr.version == REPACK_VERSION &&
              hdr.atlasSize == CTR_REPACK_ATLAS_SIZE &&
              hdr.tpagCount <= dw->tpag.count &&
              hdr.atlasCount < 32768u &&
              hdr.basePageId + hdr.atlasCount <= 32767u;
    fclose(f);

    if (ok && hdr.atlasCount > 0) {
        char pagePath[256];
        page_meta_path(pagePath, sizeof(pagePath), (int)hdr.basePageId);
        FILE *p = fopen(pagePath, "rb");
        ok = (p != NULL);
        if (p) fclose(p);
    }
    return ok;
}

static void ensure_cache_ready_flag(void) {
    char flagFile[256];
    snprintf(flagFile, sizeof(flagFile), "%s/%s", g_current_cache_dir, CACHE_READY_FLAG);
    FILE *f = fopen(flagFile, "w");
    if (f) { fputs("READY", f); fclose(f); }
}

static void remove_if_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fclose(f);
    remove(path);
}

static void remove_legacy_tile_files(int pageId) {
    char path[256];
    for (int cy = 0; cy < CTR_MAX_CHUNKS_Y; cy++) {
        for (int cx = 0; cx < CTR_MAX_CHUNKS_X; cx++) {
            snprintf(path, sizeof(path), "%s/page_%d_%d_%d.atlas",
                     g_current_cache_dir, pageId, cx, cy);
            remove_if_exists(path);
        }
    }
}

static bool write_one_page_legacy(int pageId, const uint8_t *pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return false;

    char path[256], tmpPath[256];
    page_meta_path(path, sizeof(path), pageId);
    snprintf(tmpPath, sizeof(tmpPath), "%s/page_%d.tmp", g_current_cache_dir, pageId);
    remove_if_exists(tmpPath);
    remove_legacy_tile_files(pageId);

    FILE *outF = fopen(tmpPath, "wb");
    if (!outF) return false;
    setvbuf(outF, NULL, _IOFBF, 256 * 1024);

    bool ok = true;
    AtlasHeader hdr = { ATLAS_MAGIC, (uint32_t)w, (uint32_t)h };
    if (fwrite(&hdr, sizeof(hdr), 1, outF) != 1) ok = false;

    uint16_t *row = malloc((size_t)w * sizeof(uint16_t));
    if (!row) ok = false;
    for (int y = 0; ok && y < h; y++) {
        const uint8_t *src = pixels + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            //if (a == 0) { r = 0; g = 0; b = 0; }
            row[x] = pack_rgba4444(r, g, b, a);
        }
        if (fwrite(row, sizeof(uint16_t), (size_t)w, outF) != (size_t)w) ok = false;
    }
    free(row);
    fclose(outF);

    if (ok && rename(tmpPath, path) != 0) {
        remove_if_exists(path);
        ok = (rename(tmpPath, path) == 0);
    }
    if (!ok) remove_if_exists(tmpPath);
    return ok;
}

typedef struct {
    uint32_t tpagIndex;
    uint32_t groupId;
    int srcPage;
    int srcX, srcY;
    int w, h;
    int atlasId;
    int dstX, dstY;
} RepackImage;

typedef struct {
    uint32_t groupId;
    uint32_t start;
    uint32_t count;
    uint64_t area;
} RepackGroup;

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
} RepackAtlas;

static RepackImage *g_sort_images = NULL;

static int cmp_image_group(const void *a, const void *b) {
    const RepackImage *ia = &g_sort_images[*(const uint32_t *)a];
    const RepackImage *ib = &g_sort_images[*(const uint32_t *)b];
    if (ia->groupId < ib->groupId) return -1;
    if (ia->groupId > ib->groupId) return 1;
    return 0;
}

static int cmp_group_area_desc(const void *a, const void *b) {
    const RepackGroup *ga = (const RepackGroup *)a;
    const RepackGroup *gb = (const RepackGroup *)b;
    if (ga->area < gb->area) return 1;
    if (ga->area > gb->area) return -1;
    return 0;
}

static int cmp_image_size_desc(const void *a, const void *b) {
    const RepackImage *ia = &g_sort_images[*(const uint32_t *)a];
    const RepackImage *ib = &g_sort_images[*(const uint32_t *)b];
    int ma = ia->w > ia->h ? ia->w : ia->h;
    int mb = ib->w > ib->h ? ib->w : ib->h;
    if (ma != mb) return mb - ma;
    return (ib->w * ib->h) - (ia->w * ia->h);
}

static void packer_init(MaxRectsPacker *p) {
    p->count = 1;
    p->rects[0] = (PackRect){0, 0, CTR_REPACK_ATLAS_SIZE, CTR_REPACK_ATLAS_SIZE};
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
        if (freeR.x < used.x) {
            packer_add_free(p, (PackRect){freeR.x, freeR.y, used.x - freeR.x, freeR.h});
        }
        if (freeR.x + freeR.w > used.x + used.w) {
            packer_add_free(p, (PackRect){used.x + used.w, freeR.y,
                                          freeR.x + freeR.w - used.x - used.w, freeR.h});
        }
        if (freeR.y < used.y) {
            packer_add_free(p, (PackRect){freeR.x, freeR.y, freeR.w, used.y - freeR.y});
        }
        if (freeR.y + freeR.h > used.y + used.h) {
            packer_add_free(p, (PackRect){freeR.x, used.y + used.h, freeR.w,
                                          freeR.y + freeR.h - used.y - used.h});
        }
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
        int longSide  = leftoverH > leftoverV ? leftoverH : leftoverV;
        if (shortSide < bestShort || (shortSide == bestShort && longSide < bestLong)) {
            best = i;
            bestShort = shortSide;
            bestLong = longSide;
        }
    }
    if (best < 0) return false;

    PackRect placed = { p->rects[best].x, p->rects[best].y, w, h };
    *outX = placed.x;
    *outY = placed.y;
    packer_split(p, placed);
    packer_prune(p);
    return true;
}

static bool add_repack_image(RepackImage **images, uint32_t *count, uint32_t *cap,
                             RepackImage img) {
    if (*count >= *cap) {
        uint32_t next = *cap ? (*cap * 2u) : 256u;
        RepackImage *grown = realloc(*images, (size_t)next * sizeof(RepackImage));
        if (!grown) return false;
        *images = grown;
        *cap = next;
    }
    (*images)[(*count)++] = img;
    return true;
}

static uint32_t pack_repack_images(RepackImage *images, uint32_t imageCount) {
    if (!images || imageCount == 0) return 0;

    uint32_t *order = malloc((size_t)imageCount * sizeof(uint32_t));
    if (!order) return 0;
    for (uint32_t i = 0; i < imageCount; i++) order[i] = i;

    g_sort_images = images;
    qsort(order, imageCount, sizeof(uint32_t), cmp_image_group);

    RepackGroup *groups = NULL;
    uint32_t groupCount = 0;
    uint32_t groupCap = 0;
    for (uint32_t at = 0; at < imageCount;) {
        uint32_t start = at;
        uint32_t groupId = images[order[at]].groupId;
        uint64_t area = 0;
        while (at < imageCount && images[order[at]].groupId == groupId) {
            RepackImage *img = &images[order[at]];
            area += (uint64_t)img->w * (uint64_t)img->h;
            at++;
        }
        if (groupCount >= groupCap) {
            uint32_t next = groupCap ? groupCap * 2u : 128u;
            RepackGroup *grown = realloc(groups, (size_t)next * sizeof(RepackGroup));
            if (!grown) { free(groups); free(order); return 0; }
            groups = grown;
            groupCap = next;
        }
        groups[groupCount++] = (RepackGroup){ groupId, start, at - start, area };
    }
    qsort(groups, groupCount, sizeof(RepackGroup), cmp_group_area_desc);

    RepackAtlas *atlases = NULL;
    uint32_t atlasCount = 0;
    uint32_t atlasCap = 0;

    for (uint32_t g = 0; g < groupCount; g++) {
        RepackGroup *grp = &groups[g];
        uint32_t *idx = malloc((size_t)grp->count * sizeof(uint32_t));
        int *tryX = malloc((size_t)grp->count * sizeof(int));
        int *tryY = malloc((size_t)grp->count * sizeof(int));
        if (!idx || !tryX || !tryY) {
            free(idx); free(tryX); free(tryY);
            continue;
        }
        for (uint32_t i = 0; i < grp->count; i++) idx[i] = order[grp->start + i];
        qsort(idx, grp->count, sizeof(uint32_t), cmp_image_size_desc);

        bool placedGroup = false;
        for (uint32_t a = 0; a < atlasCount && !placedGroup; a++) {
            MaxRectsPacker clone = atlases[a].packer;
            bool allFit = true;
            for (uint32_t i = 0; i < grp->count; i++) {
                RepackImage *img = &images[idx[i]];
                if (!packer_insert(&clone, img->w, img->h, &tryX[i], &tryY[i])) {
                    allFit = false;
                    break;
                }
            }
            if (allFit) {
                atlases[a].packer = clone;
                for (uint32_t i = 0; i < grp->count; i++) {
                    RepackImage *img = &images[idx[i]];
                    img->atlasId = (int)a;
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
                        RepackAtlas *grown = realloc(atlases, (size_t)next * sizeof(RepackAtlas));
                        if (!grown) break;
                        atlases = grown;
                        atlasCap = next;
                    }
                    uint32_t atlasId = atlasCount++;
                    packer_init(&atlases[atlasId].packer);
                    bool any = false;
                    for (uint32_t i = 0; i < grp->count; i++) {
                        if (!remaining[i]) continue;
                        RepackImage *img = &images[idx[i]];
                        int px, py;
                        if (packer_insert(&atlases[atlasId].packer, img->w, img->h, &px, &py)) {
                            img->atlasId = (int)atlasId;
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
    g_sort_images = NULL;
    return atlasCount;
}

static bool init_empty_repack_page(uint32_t pageId) {
    char path[256], tmpPath[256];
    page_meta_path(path, sizeof(path), (int)pageId);
    snprintf(tmpPath, sizeof(tmpPath), "%s/page_%lu.tmp",
             g_current_cache_dir, (unsigned long)pageId);
    remove_if_exists(tmpPath);

    FILE *f = fopen(tmpPath, "wb");
    if (!f) return false;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);

    bool ok = true;
    AtlasHeader hdr = { ATLAS_MAGIC, CTR_REPACK_ATLAS_SIZE, CTR_REPACK_ATLAS_SIZE };
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) ok = false;

    uint16_t *zero = calloc(CTR_REPACK_ATLAS_SIZE, sizeof(uint16_t));
    if (!zero) ok = false;
    for (int y = 0; ok && y < CTR_REPACK_ATLAS_SIZE; y++) {
        if (fwrite(zero, sizeof(uint16_t), CTR_REPACK_ATLAS_SIZE, f) != CTR_REPACK_ATLAS_SIZE) {
            ok = false;
        }
    }
    free(zero);
    fclose(f);

    if (ok && rename(tmpPath, path) != 0) {
        remove_if_exists(path);
        ok = (rename(tmpPath, path) == 0);
    }
    if (!ok) remove_if_exists(tmpPath);
    return ok;
}

static bool finalize_repack_page_tiled(uint32_t pageId) {
    char path[256], tmpPath[256];
    page_meta_path(path, sizeof(path), (int)pageId);
    snprintf(tmpPath, sizeof(tmpPath), "%s/page_%lu.tiled.tmp",
             g_current_cache_dir, (unsigned long)pageId);

    FILE *in = fopen(path, "rb");
    if (!in) return false;

    AtlasHeader hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, in) == 1 &&
              hdr.magic == ATLAS_MAGIC &&
              hdr.w > 0 && hdr.h > 0 &&
              hdr.w <= CTR_REPACK_ATLAS_SIZE &&
              hdr.h <= CTR_REPACK_ATLAS_SIZE;
    int w = ok ? (int)hdr.w : 0;
    int h = ok ? (int)hdr.h : 0;
    uint16_t *linear = NULL;
    uint16_t *tiled = NULL;
    if (ok) {
        linear = calloc((size_t)w * (size_t)h, sizeof(uint16_t));
        tiled = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
        ok = (linear != NULL && tiled != NULL);
    }
    if (ok) {
        ok = fread(linear, sizeof(uint16_t), (size_t)w * (size_t)h, in) ==
             (size_t)w * (size_t)h;
    }
    fclose(in);

    if (ok) {
        tile_rgba4(linear, tiled, w, h, w, h);
        remove_if_exists(tmpPath);
        FILE *out = fopen(tmpPath, "wb");
        ok = (out != NULL);
        if (ok) {
            AtlasHeader outHdr = { ATLAS_TILED_MAGIC, (uint32_t)w, (uint32_t)h };
            ok = fwrite(&outHdr, sizeof(outHdr), 1, out) == 1 &&
                 fwrite(tiled, sizeof(uint16_t), (size_t)w * (size_t)h, out) ==
                 (size_t)w * (size_t)h;
            fclose(out);
        }
        if (ok && rename(tmpPath, path) != 0) {
            remove_if_exists(path);
            ok = (rename(tmpPath, path) == 0);
        }
    }

    if (!ok) remove_if_exists(tmpPath);
    free(linear);
    free(tiled);
    return ok;
}

static bool blit_repack_image(const RepackImage *img, const uint8_t *srcPixels,
                              int srcW, int srcH, uint32_t basePageId) {
    if (!img || !srcPixels) return false;
    if (img->srcX < 0 || img->srcY < 0 || img->dstX < 0 || img->dstY < 0) return false;
    if (img->srcX + img->w > srcW || img->srcY + img->h > srcH) return false;
    if (img->dstX + img->w > CTR_REPACK_ATLAS_SIZE || img->dstY + img->h > CTR_REPACK_ATLAS_SIZE) return false;

    char path[256];
    page_meta_path(path, sizeof(path), (int)(basePageId + (uint32_t)img->atlasId));
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    uint16_t *row = malloc((size_t)img->w * sizeof(uint16_t));
    bool ok = (row != NULL);
    for (int y = 0; ok && y < img->h; y++) {
        const uint8_t *src = srcPixels + (((size_t)(img->srcY + y) * (size_t)srcW + (size_t)img->srcX) * 4u);
        for (int x = 0; x < img->w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            //if (a == 0) { r = 0; g = 0; b = 0; }
            row[x] = pack_rgba4444(r, g, b, a);
        }
        long off = (long)sizeof(AtlasHeader) +
                   (((long)(img->dstY + y) * CTR_REPACK_ATLAS_SIZE + img->dstX) * 2L);
        if (fseek(f, off, SEEK_SET) != 0 ||
            fwrite(row, sizeof(uint16_t), (size_t)img->w, f) != (size_t)img->w) {
            ok = false;
        }
    }
    free(row);
    fclose(f);
    return ok;
}

static bool write_repack_index(const RepackMapEntry *entries, uint32_t tpagCount,
                               uint32_t basePageId, uint32_t atlasCount) {
    char path[256], tmpPath[256];
    repack_index_path(path, sizeof(path));
    snprintf(tmpPath, sizeof(tmpPath), "%s/atlas.tmp", g_current_cache_dir);
    remove_if_exists(tmpPath);

    FILE *f = fopen(tmpPath, "wb");
    RepackHeader hdr = {
        REPACK_MAGIC, REPACK_VERSION, tpagCount, basePageId,
        atlasCount, CTR_REPACK_ATLAS_SIZE
    };
    bool ok = false;
    if (f) {
        ok = true;
        if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) ok = false;
        if (ok && fwrite(entries, 1, (size_t)tpagCount * sizeof(RepackMapEntry), f) !=
                  (size_t)tpagCount * sizeof(RepackMapEntry)) ok = false;
        fclose(f);
    }

    if (ok && rename(tmpPath, path) != 0) {
        remove_if_exists(path);
        ok = (rename(tmpPath, path) == 0);
    }
    if (!ok) remove_if_exists(tmpPath);
    if (!ok) {
        FILE *direct = fopen(path, "wb");
        if (!direct) return false;
        RepackHeader hdr = {
            REPACK_MAGIC, REPACK_VERSION, tpagCount, basePageId,
            atlasCount, CTR_REPACK_ATLAS_SIZE
        };
        size_t bytes = (size_t)tpagCount * sizeof(RepackMapEntry);
        ok = fwrite(&hdr, sizeof(hdr), 1, direct) == 1 &&
             fwrite(entries, 1, bytes, direct) == bytes;
        fclose(direct);
        if (!ok) remove_if_exists(path);
    }
    return ok;
}

static void assign_asset_groups(DataWin *dw, uint32_t *groupIds, uint32_t *nextGroup) {
    for (uint32_t s = 0; s < dw->sprt.count; s++) {
        Sprite *spr = &dw->sprt.sprites[s];
        uint32_t group = (*nextGroup)++;
        for (uint32_t f = 0; f < spr->textureCount; f++) {
            int32_t tpag = spr->tpagIndices ? spr->tpagIndices[f] : -1;
            if (tpag >= 0 && (uint32_t)tpag < dw->tpag.count && groupIds[tpag] == 0) {
                groupIds[tpag] = group;
            }
        }
    }
    for (uint32_t b = 0; b < dw->bgnd.count; b++) {
        int32_t tpag = dw->bgnd.backgrounds[b].tpagIndex;
        if (tpag >= 0 && (uint32_t)tpag < dw->tpag.count && groupIds[tpag] == 0) {
            groupIds[tpag] = (*nextGroup)++;
        }
    }
    for (uint32_t f = 0; f < dw->font.count; f++) {
        int32_t tpag = dw->font.fonts[f].tpagIndex;
        if (tpag >= 0 && (uint32_t)tpag < dw->tpag.count && groupIds[tpag] == 0) {
            groupIds[tpag] = (*nextGroup)++;
        }
    }
}

static void build_texture_cache(DataWin *dw) {
    if (!dw) return;
    char flagFile[256];
    snprintf(flagFile, sizeof(flagFile), "%s/%s", g_current_cache_dir, CACHE_READY_FLAG);

    if (repack_index_is_valid(dw)) {
        FILE *ready = fopen(flagFile, "r");
        if (ready) fclose(ready);
        else ensure_cache_ready_flag();
        return;
    }

    FILE *f = fopen(flagFile, "r");
    if (f) {
        fclose(f);
        remove_if_exists(flagFile);
    }

    FILE *dwFile = dw->filePath ? fopen(dw->filePath, "rb") : NULL;
    if (dwFile) setvbuf(dwFile, NULL, _IOFBF, 256 * 1024);

    bool ok = (dwFile != NULL);
    uint32_t basePageId = dw->txtr.count;
    RepackMapEntry *entries = calloc(dw->tpag.count, sizeof(RepackMapEntry));
    uint32_t *groupIds = calloc(dw->tpag.count ? dw->tpag.count : 1, sizeof(uint32_t));
    bool *fallbackPages = calloc(dw->txtr.count ? dw->txtr.count : 1, sizeof(bool));
    RepackImage *images = NULL;
    uint32_t imageCount = 0;
    uint32_t imageCap = 0;
    if (!entries || !groupIds || !fallbackPages) ok = false;

    uint32_t nextGroup = 1;
    if (ok) assign_asset_groups(dw, groupIds, &nextGroup);

    if (ok) {
        for (uint32_t i = 0; i < dw->tpag.count; i++) {
            TexturePageItem *item = &dw->tpag.items[i];
            if (item->texturePageId < 0 || (uint32_t)item->texturePageId >= dw->txtr.count) continue;
            int w = item->sourceWidth;
            int h = item->sourceHeight;
            if (w <= 0 || h <= 0) continue;
            if (w > CTR_REPACK_ATLAS_SIZE || h > CTR_REPACK_ATLAS_SIZE) {
                fallbackPages[item->texturePageId] = true;
                continue;
            }
            uint32_t group = groupIds[i] ? groupIds[i] : nextGroup++;
            RepackImage img = {
                .tpagIndex = i,
                .groupId = group,
                .srcPage = item->texturePageId,
                .srcX = item->sourceX,
                .srcY = item->sourceY,
                .w = w,
                .h = h,
                .atlasId = -1,
                .dstX = 0,
                .dstY = 0,
            };
            if (!add_repack_image(&images, &imageCount, &imageCap, img)) {
                ok = false;
                break;
            }
        }
    }

    uint32_t atlasCount = ok ? pack_repack_images(images, imageCount) : 0;
    if (ok && imageCount > 0 && atlasCount == 0) ok = false;
    if (ok && basePageId + atlasCount > 32767u) ok = false;
    if (ok) {
        for (uint32_t i = 0; i < imageCount; i++) {
            if (images[i].atlasId < 0 && images[i].srcPage >= 0 &&
                (uint32_t)images[i].srcPage < dw->txtr.count) {
                fallbackPages[images[i].srcPage] = true;
            }
        }
    }

    if (ok) {
        for (uint32_t a = 0; a < atlasCount; a++) {
            if (!init_empty_repack_page(basePageId + a)) ok = false;
        }
    }

    if (ok) {
        for (uint32_t i = 0; i < imageCount; i++) {
            RepackImage *img = &images[i];
            if (img->atlasId < 0) continue;
            TexturePageItem *old = &dw->tpag.items[img->tpagIndex];
            entries[img->tpagIndex] = (RepackMapEntry){
                .flags = REPACK_ENTRY_VALID,
                .pageId = basePageId + (uint32_t)img->atlasId,
                .sourceX = (uint16_t)img->dstX,
                .sourceY = (uint16_t)img->dstY,
                .sourceWidth = old->sourceWidth,
                .sourceHeight = old->sourceHeight,
                .targetX = old->targetX,
                .targetY = old->targetY,
                .targetWidth = old->targetWidth,
                .targetHeight = old->targetHeight,
                .boundingWidth = old->boundingWidth,
                .boundingHeight = old->boundingHeight,
            };
        }
    }

    for (uint32_t p = 0; ok && p < dw->txtr.count; p++) {
        char progressPath[256];
        page_meta_path(progressPath, sizeof(progressPath), (int)p);
        if (g_cacheProgressCallback) {
            g_cacheProgressCallback(p, dw->txtr.count, progressPath, g_cacheProgressUser);
        }

        bool needed = fallbackPages[p];
        for (uint32_t i = 0; !needed && i < imageCount; i++) {
            if (images[i].srcPage == (int)p) needed = true;
        }
        if (!needed) {
            char oldPath[256];
            page_meta_path(oldPath, sizeof(oldPath), (int)p);
            remove_if_exists(oldPath);
            remove_legacy_tile_files((int)p);
            continue;
        }

        Texture *t = &dw->txtr.textures[p];
        if (!t->blobSize) {
            fprintf(stderr, "CTR cache: TXTR page %lu has no embedded blob\n", (unsigned long)p);
            ok = false;
            continue;
        }
        uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
        if (!blob) {
            fprintf(stderr, "CTR cache: failed to read TXTR page %lu blob\n", (unsigned long)p);
            ok = false;
            continue;
        }

        int w, h;
        uint8_t *pixels = ImageDecoder_decodeToRgba(
            blob, t->blobSize, DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), &w, &h);
        free(blob);
        if (!pixels) {
            fprintf(stderr, "CTR cache: failed to decode TXTR page %lu\n", (unsigned long)p);
            ok = false;
            continue;
        }

        if (fallbackPages[p] && !write_one_page_legacy((int)p, pixels, w, h)) {
            fprintf(stderr, "CTR cache: failed to write legacy page %lu\n", (unsigned long)p);
            ok = false;
        }
        for (uint32_t i = 0; ok && i < imageCount; i++) {
            if (images[i].srcPage != (int)p) continue;
            if (images[i].atlasId < 0) continue;
            if (!blit_repack_image(&images[i], pixels, w, h, basePageId)) {
                fprintf(stderr, "CTR cache: failed to blit TPAG %lu from source page %lu\n",
                        (unsigned long)images[i].tpagIndex, (unsigned long)p);
                ok = false;
            }
        }
        ImageDecoder_freeRgba(pixels);
        if (ok && !fallbackPages[p]) {
            char oldPath[256];
            page_meta_path(oldPath, sizeof(oldPath), (int)p);
            remove_if_exists(oldPath);
            remove_legacy_tile_files((int)p);
        }
    }

    if (ok) {
        for (uint32_t a = 0; a < atlasCount; a++) {
            if (!finalize_repack_page_tiled(basePageId + a)) {
                fprintf(stderr, "CTR cache: failed to finalize repack atlas %lu\n",
                        (unsigned long)(basePageId + a));
                ok = false;
                break;
            }
        }
    }

    if (dwFile) fclose(dwFile);

    if (g_cacheProgressCallback) {
        g_cacheProgressCallback(dw->txtr.count, dw->txtr.count, NULL, g_cacheProgressUser);
    }

    if (ok) {
        if (write_repack_index(entries, dw->tpag.count, basePageId, atlasCount) &&
            repack_index_is_valid(dw)) {
            ensure_cache_ready_flag();
        } else {
            fprintf(stderr, "CTR cache: failed to write %s\n", REPACK_INDEX_FILE);
        }
    } else {
        fprintf(stderr, "CTR cache: build failed before index commit\n");
    }

    free(images);
    free(entries);
    free(groupIds);
    free(fallbackPages);

    char oldFlag[256];
    snprintf(oldFlag, sizeof(oldFlag), "%s/cache_ready.flag", g_current_cache_dir);
    remove_if_exists(oldFlag);
    snprintf(oldFlag, sizeof(oldFlag), "%s/cache_ready_v2.flag", g_current_cache_dir);
    remove_if_exists(oldFlag);
    snprintf(oldFlag, sizeof(oldFlag), "%s/cache_ready_v3.flag", g_current_cache_dir);
    remove_if_exists(oldFlag);
    snprintf(oldFlag, sizeof(oldFlag), "%s/cache_ready_v4.flag", g_current_cache_dir);
    remove_if_exists(oldFlag);
}

void CtrRenderer_prepareTextureCache(DataWin *dw) {
    CtrTextureCache_prepare(dw);
}

static bool apply_repack_index(CtrRenderer *ctx, DataWin *dw) {
    if (!ctx || !dw) return false;

    char path[256];
    repack_index_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    RepackHeader hdr;
    bool ok = false;
    if (fread(&hdr, sizeof(hdr), 1, f) == 1 &&
        hdr.magic == REPACK_MAGIC &&
        hdr.version == REPACK_VERSION &&
        hdr.atlasSize == CTR_REPACK_ATLAS_SIZE &&
        hdr.tpagCount <= dw->tpag.count &&
        hdr.atlasCount < 32768u &&
        hdr.basePageId + hdr.atlasCount <= 32767u) {
        RepackMapEntry *entries = malloc((size_t)hdr.tpagCount * sizeof(RepackMapEntry));
        if (entries && fread(entries, sizeof(RepackMapEntry), hdr.tpagCount, f) == hdr.tpagCount) {
            ctx->repackBasePageId = hdr.basePageId;
            ctx->repackPageCount = hdr.atlasCount;
            ctx->sourcePageCount = hdr.atlasCount;
            ctx->sourcePages = calloc(ctx->sourcePageCount ? ctx->sourcePageCount : 1,
                                      sizeof(CtrSourcePage));
            ok = (ctx->sourcePages != NULL || ctx->sourcePageCount == 0);

            if (ok) {
                for (uint32_t i = 0; i < hdr.tpagCount; i++) {
                    RepackMapEntry *e = &entries[i];
                    if ((e->flags & REPACK_ENTRY_VALID) == 0) continue;
                    if (e->pageId < hdr.basePageId ||
                        e->pageId >= hdr.basePageId + hdr.atlasCount ||
                        e->pageId > 32767u) {
                        continue;
                    }
                    TexturePageItem *item = &dw->tpag.items[i];
                    item->texturePageId = (int16_t)e->pageId;
                    item->sourceX = e->sourceX;
                    item->sourceY = e->sourceY;
                    item->sourceWidth = e->sourceWidth;
                    item->sourceHeight = e->sourceHeight;
                    item->targetX = e->targetX;
                    item->targetY = e->targetY;
                    item->targetWidth = e->targetWidth;
                    item->targetHeight = e->targetHeight;
                    item->boundingWidth = e->boundingWidth;
                    item->boundingHeight = e->boundingHeight;
                }
            }
        }
        free(entries);
    }

    fclose(f);
    return ok;
}

// Vertex batching

// Forward decls — these live further down the file.
static void rebind_state(CtrRenderer *ctx);
static void apply_projection(CtrRenderer *ctx, const C3D_Mtx *m);
static void apply_blend(CtrRenderer *ctx, int mode);
static void emit_blend_state(CtrRenderer *ctx);
static void apply_depth_write_mask(CtrRenderer *ctx);
static void apply_alpha_test_state(CtrRenderer *ctx);
static void free_all_tilemap_caches(CtrRenderer *ctx);
static bool cache_item_available(CtrRenderer *ctx, uint32_t id);

// C3D_FrameSplit invalidates BufInfo / AttrInfo / TexEnv / AlphaBlend / projection
// uniforms — bind_target already knows this (calls rebind_state+apply_projection
// after every split). Anywhere else that splits mid-frame must do the same, or
// the next DrawArrays reads vertices from a stale buffer binding and renders
// garbage (UNDERTALE undynebridge / DELTARUNE cooking minigame). Use this
// helper instead of calling C3D_FrameSplit(0) directly.
static void ctr_safe_frame_split(CtrRenderer *ctx) {
    C3D_FrameSplit(0);
    if (ctx->inFrame && ctx->activeTarget) {
        rebind_state(ctx);
        apply_projection(ctx, &ctx->currentProjection);
        // Re-emit C3D_AlphaBlend — citro3d state cache doesn't always restore it
        // after a split, and stale blend state mangles font edges + UI overlays
        // for the rest of the frame. Safe to call: batchVerts is 0 at this point
        // (caller flushed before splitting), so the inner flush_batch is a no-op.
        emit_blend_state(ctx);
    }
}

static void flush_batch(CtrRenderer *ctx) {
    if (!ctx->batchVerts || !ctx->batchTex || !ctx->inFrame) {
        ctx->batchVerts = 0;
        ctx->batchTex   = NULL;
        return;
    }

    GSPGPU_FlushDataCache(ctx->vbuf + ctx->batchStart, ctx->batchVerts * sizeof(CtrVertex));

    C3D_TexBind(0, ctx->batchTex);
    C3D_DrawArrays(GPU_TRIANGLES, ctx->batchStart, ctx->batchVerts);
    ctx->batchStart += ctx->batchVerts;
    ctx->batchVerts  = 0;
    ctx->batchTex    = NULL;
}

static inline CtrVertex *vbuf_reserve(CtrRenderer *ctx, uint32_t count, C3D_Tex *tex) {
    if (ctx->batchTex && ctx->batchTex != tex) flush_batch(ctx);
    if (ctx->vbufHead + count > ctx->vbufCap) {
        flush_batch(ctx);
        ctr_safe_frame_split(ctx);
        ctx->vbufHead   = 0;
        ctx->batchStart = 0;
    }
    ctx->batchTex = tex;
    CtrVertex *v = (CtrVertex *)ctx->vbuf + ctx->vbufHead;
    ctx->vbufHead   += count;
    ctx->batchVerts += count;
    return v;
}

static void push_quad_uvgrad(CtrRenderer *ctx, C3D_Tex *tex,
                             const float x[4], const float y[4],
                             float u0, float v0, float u1, float v1,
                             const float c[4][4]) {
    if (ctx->surfaceDrawSuppressed) return;
    CtrVertex *v = vbuf_reserve(ctx, 6, tex);
    #define EMIT(idx, ix, uu, vv, cc) \
        do { \
            v[idx].x = x[ix]; v[idx].y = y[ix]; v[idx].z = 0.f; \
            v[idx].u = uu;    v[idx].v = vv; \
            v[idx].r = c[cc][0]; v[idx].g = c[cc][1]; v[idx].b = c[cc][2]; v[idx].a = c[cc][3]; \
        } while (0)
    EMIT(0, 0, u0, v0, 0);
    EMIT(1, 1, u1, v0, 1);
    EMIT(2, 2, u1, v1, 2);
    EMIT(3, 0, u0, v0, 0);
    EMIT(4, 2, u1, v1, 2);
    EMIT(5, 3, u0, v1, 3);
    #undef EMIT
}

static void push_quad(CtrRenderer *ctx, C3D_Tex *tex,
                      float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3,
                      float u0, float v0, float u1, float v1,
                      const float col[4]) {
    if (ctx->surfaceDrawSuppressed) return;
    float xs[4] = {x0, x1, x2, x3};
    float ys[4] = {y0, y1, y2, y3};
    float cs[4][4] = {
        {col[0], col[1], col[2], col[3]},
        {col[0], col[1], col[2], col[3]},
        {col[0], col[1], col[2], col[3]},
        {col[0], col[1], col[2], col[3]},
    };
    CtrVertex *v = vbuf_reserve(ctx, 6, tex);
    #define VV(idx, ix, iy, uu, vv, cc) \
        v[idx] = (CtrVertex){xs[ix], ys[iy], 0.f, uu, vv, cs[cc][0], cs[cc][1], cs[cc][2], cs[cc][3]}
    VV(0, 0, 0, u0, v0, 0);
    VV(1, 1, 1, u1, v0, 1);
    VV(2, 2, 2, u1, v1, 2);
    VV(3, 0, 0, u0, v0, 0);
    VV(4, 2, 2, u1, v1, 2);
    VV(5, 3, 3, u0, v1, 3);
    #undef VV
}

static void push_solid_tri(CtrRenderer *ctx, float x1, float y1, float x2, float y2,
                           float x3, float y3, const float c1[4], const float c2[4], const float c3[4]) {
    if (ctx->surfaceDrawSuppressed) return;
    CtrVertex *v = vbuf_reserve(ctx, 3, &ctx->whiteTex);
    v[0] = (CtrVertex){x1, y1, 0, .5f, .5f, c1[0], c1[1], c1[2], c1[3]};
    v[1] = (CtrVertex){x2, y2, 0, .5f, .5f, c2[0], c2[1], c2[2], c2[3]};
    v[2] = (CtrVertex){x3, y3, 0, .5f, .5f, c3[0], c3[1], c3[2], c3[3]};
}

static void draw_letterbox_backdrop(CtrRenderer *ctx) {
    if (g_ctr_backdrop_mode == CTR_BACKDROP_BLACK) {
        float x[4] = {0.f, (float)ctx->winW, (float)ctx->winW, 0.f};
        float y[4] = {0.f, 0.f, (float)ctx->winH, (float)ctx->winH};
        float bg[4][4] = {
            {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f},
            {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, x, y, .5f, .5f, .5f, .5f, bg);
        return;
    }

    float u0 = 0.f;
    float u1 = (float)ctx->appLogicW / (float)ctx->appPotW;
    float v0 = (float)(ctx->appPotH - ctx->appLogicH) / (float)ctx->appPotH;
    float v1 = 1.f;

    float zoomU = (u1 - u0) * 0.15f;
    float zoomV = (v1 - v0) * 0.15f;
    float texU0 = u0 + zoomU;
    float texU1 = u1 - zoomU;
    float texVTop = v1 - zoomV;
    float texVBot = v0 + zoomV;

    float x[4] = {0.f, (float)ctx->winW, (float)ctx->winW, 0.f};
    float y[4] = {0.f, 0.f, (float)ctx->winH, (float)ctx->winH};
    float bgTopR = g_letterbox.bgTop[0];
    float bgTopG = g_letterbox.bgTop[1];
    float bgTopB = g_letterbox.bgTop[2];
    float bgBotR = g_letterbox.bgBot[0];
    float bgBotG = g_letterbox.bgBot[1];
    float bgBotB = g_letterbox.bgBot[2];
    float bg[4][4] = {
        {bgTopR, bgTopG, bgTopB, 1.f}, {bgTopR, bgTopG, bgTopB, 1.f},
        {bgBotR, bgBotG, bgBotB, 1.f}, {bgBotR, bgBotG, bgBotB, 1.f}
    };
    push_quad_uvgrad(ctx, &ctx->whiteTex, x, y, .5f, .5f, .5f, .5f, bg);

    if (g_ctr_backdrop_mode == CTR_BACKDROP_BLUR && g_letterbox.blurAlpha > 0.001f) {
        float blurAlpha = g_letterbox.blurAlpha;
        float offset = 14.0f;

        float offsets_x[4] = {-offset, offset, -offset, offset};
        float offsets_y[4] = {-offset, -offset, offset, offset};

        for (int j = 0; j < 4; j++) {
            float ox = offsets_x[j];
            float oy = offsets_y[j];

            float px[4] = {ox, (float)ctx->winW + ox, (float)ctx->winW + ox, ox};
            float py[4] = {oy, oy, (float)ctx->winH + oy, (float)ctx->winH + oy};

            float c[4][4] = {
                {0.45f, 0.45f, 0.45f, blurAlpha}, {0.45f, 0.45f, 0.45f, blurAlpha},
                {0.15f, 0.15f, 0.15f, blurAlpha}, {0.15f, 0.15f, 0.15f, blurAlpha}
            };

            push_quad_uvgrad(ctx, &ctx->appTex, px, py, texU0, texVTop, texU1, texVBot, c);
        }
    }

    if (g_letterbox.particleAlpha > 0.001f) {
        float t = (float)g_frame * 0.025f;
        float aR = g_letterbox.accent[0];
        float aG = g_letterbox.accent[1];
        float aB = g_letterbox.accent[2];
        for (int i = 0; i < 18; i++) {
            float seed = (float)i * 15.37f;
            float px = fmodf(seed * 19.1f + t * (18.f + (float)(i % 4) * 7.f), (float)ctx->winW + 48.f) - 24.f;
            float py = fmodf(seed * 11.3f + sinf(t + seed) * 16.f, (float)ctx->winH + 32.f) - 16.f;
            float s = 1.2f + (float)(i % 3) * 0.8f;

            float alpha = g_letterbox.particleAlpha *
                          (0.12f + 0.10f * (sinf(t * 1.7f + seed) * 0.5f + 0.5f));
            float pc[4] = {aR, aG, aB, alpha};
            push_quad(ctx, &ctx->whiteTex, px, py, px + s, py, px + s, py + s, px, py + s,
                      .5f, .5f, .5f, .5f, pc);
        }
    }
}

// Atlases

static void free_old_pages(CtrRenderer *ctx) {
    if (linearSpaceFree() >= LINEAR_LOW) return;
    bool flushed = false;
    int evicted = 0;

    while (evicted < 32 && linearSpaceFree() < LINEAR_SAFE) {
        uint32_t oldest = UINT32_MAX;
        int      victim = -1;
        for (uint32_t i = 0; i < ctx->pageCount; i++) {
            if (!ctx->pages[i].loaded || ctx->pages[i].keepResident ||
                ctx->pages[i].lastFrame >= g_frame) continue;
            if (ctx->pages[i].lastFrame < oldest) {
                oldest = ctx->pages[i].lastFrame;
                victim = (int)i;
            }
        }
        if (victim < 0) {
            for (uint32_t i = 0; i < ctx->pageCount; i++) {
                if (!ctx->pages[i].loaded || ctx->pages[i].keepResident) continue;
                victim = (int)i;
                break;
            }
        }
        if (victim < 0) break;

        if (!flushed) { flush_batch(ctx); flushed = true; }

        for (int cx = 0; cx < ctx->pages[victim].chunksX; cx++) {
            for (int cy = 0; cy < ctx->pages[victim].chunksY; cy++) {
                CtrAtlasChunk *ch = &ctx->pages[victim].chunks[cx][cy];
                if (ch->valid) {
                    C3D_TexDelete(&ch->tex);
                    ch->valid = false;
                }
            }
        }
        ctx->pages[victim].loaded = false;
        evicted++;
    }
}

static void extract_legacy_page_file(CtrRenderer *ctx, DataWin *dw, uint32_t id,
                                     FILE *f, int aw, int ah) {
    enum { LEGACY_CHUNK_SIZE = 1024 };
    TexturePageItem *item = &dw->tpag.items[id];
    int w = item->sourceWidth  > 0 ? item->sourceWidth  : 1;
    int h = item->sourceHeight > 0 ? item->sourceHeight : 1;

    CtrPage *page = &ctx->pages[id];
    page->origW = w;
    page->origH = h;
    page->chunksX = (int)fminf((float)((w + LEGACY_CHUNK_SIZE - 1) / LEGACY_CHUNK_SIZE),
                               (float)CTR_MAX_CHUNKS_X);
    page->chunksY = (int)fminf((float)((h + LEGACY_CHUNK_SIZE - 1) / LEGACY_CHUNK_SIZE),
                               (float)CTR_MAX_CHUNKS_Y);
    bool complete = true;

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            CtrAtlasChunk *chunk = &page->chunks[cx][cy];
            memset(chunk, 0, sizeof(*chunk));
            chunk->srcX   = cx * LEGACY_CHUNK_SIZE;
            chunk->srcY   = cy * LEGACY_CHUNK_SIZE;
            chunk->width  = (int)fminf((float)(w - chunk->srcX), (float)LEGACY_CHUNK_SIZE);
            chunk->height = (int)fminf((float)(h - chunk->srcY), (float)LEGACY_CHUNK_SIZE);
            chunk->potW   = next_pow2(chunk->width);
            chunk->potH   = next_pow2(chunk->height);

            uint16_t *linear = calloc((size_t)chunk->potW * chunk->potH, sizeof(uint16_t));
            if (!linear) { complete = false; continue; }

            for (int y = 0; y < chunk->height; y++) {
                int sy = item->sourceY + chunk->srcY + y;
                int sx = item->sourceX + chunk->srcX;
                if (sy < 0 || sy >= ah || sx < 0 || sx >= aw) continue;
                int readable = chunk->width;
                if (sx + readable > aw) readable = aw - sx;
                if (readable <= 0) continue;
                fseek(f, sizeof(AtlasHeader) + ((long)sy * aw + sx) * 2, SEEK_SET);
                fread(&linear[y * chunk->potW], sizeof(uint16_t), (size_t)readable, f);
            }

            if (!C3D_TexInit(&chunk->tex, (u16)chunk->potW, (u16)chunk->potH, GPU_RGBA4)) {
                free(linear);
                complete = false;
                continue;
            }
            C3D_TexSetFilter(&chunk->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&chunk->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

            uint32_t tiledSize = (uint32_t)chunk->potW * (uint32_t)chunk->potH * 2u;
            uint16_t *tiled = linearAlloc(tiledSize);
            if (!tiled) {
                free(linear);
                C3D_TexDelete(&chunk->tex);
                complete = false;
                continue;
            }
            tile_rgba4(linear, tiled, chunk->potW, chunk->potH, chunk->potW, chunk->potH);
            free(linear);

            C3D_TexLoadImage(&chunk->tex, tiled, GPU_TEXFACE_2D, 0);
            C3D_TexFlush(&chunk->tex);
            linearFree(tiled);
            chunk->valid = true;
        }
    }

    if (!complete) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            for (int cy = 0; cy < page->chunksY; cy++) {
                CtrAtlasChunk *chunk = &page->chunks[cx][cy];
                if (chunk->valid) C3D_TexDelete(&chunk->tex);
                memset(chunk, 0, sizeof(*chunk));
            }
        }
        page->loaded = false;
        page->chunksX = 0;
        page->chunksY = 0;
        return;
    }
    page->loaded = true;
}

static __attribute__((aligned(8))) char dyn_buf[64 * 1024];

static bool is_repacked_page(const CtrRenderer *ctx, int pageId) {
    return ctx->repackPageCount > 0 &&
           pageId >= (int)ctx->repackBasePageId &&
           pageId < (int)(ctx->repackBasePageId + ctx->repackPageCount);
}

static CtrSourcePage *get_source_page(CtrRenderer *ctx, int pageId) {
    if (!is_repacked_page(ctx, pageId)) return NULL;
    uint32_t idx = (uint32_t)pageId - ctx->repackBasePageId;
    if (idx >= ctx->sourcePageCount) return NULL;
    return &ctx->sourcePages[idx];
}

static void free_old_source_pages(CtrRenderer *ctx) {
    if (!ctx->sourcePages || linearSpaceFree() >= LINEAR_LOW) return;
    bool flushed = false;
    int evicted = 0;

    while (evicted < 16 && linearSpaceFree() < LINEAR_SAFE) {
        uint32_t oldest = UINT32_MAX;
        int victim = -1;
        for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
            CtrSourcePage *p = &ctx->sourcePages[i];
            if (!p->loaded || p->keepResident || p->lastFrame >= g_frame) continue;
            if (p->lastFrame < oldest) {
                oldest = p->lastFrame;
                victim = (int)i;
            }
        }
        if (victim < 0) break;
        if (!flushed) { flush_batch(ctx); flushed = true; }
        C3D_TexDelete(&ctx->sourcePages[victim].tex);
        memset(&ctx->sourcePages[victim].tex, 0, sizeof(ctx->sourcePages[victim].tex));
        ctx->sourcePages[victim].loaded = false;
        ctx->sourcePages[victim].loadFailed = false;
        evicted++;
    }
}

static void unload_nonresident_source_pages(CtrRenderer *ctx) {
    if (!ctx || !ctx->sourcePages) return;
    bool flushed = false;
    uint32_t evicted = 0;
    for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
        CtrSourcePage *p = &ctx->sourcePages[i];
        if (!p->loaded || p->keepResident) continue;
        if (!flushed) { flush_batch(ctx); flushed = true; }
        C3D_TexDelete(&p->tex);
        memset(&p->tex, 0, sizeof(p->tex));
        p->loaded = false;
        p->loadFailed = false;
        evicted++;
    }
    if (evicted > 0) {
        CTR_DIAG("CTR cache: evicted %lu stale atlas textures before room preload\n",
                 (unsigned long)evicted);
    }
}

static bool load_source_page_dyn(CtrRenderer *ctx, int pageId) {
    CtrSourcePage *page = get_source_page(ctx, pageId);
    if (!page) return false;
    if (page->loaded) {
        page->lastFrame = g_frame;
        return true;
    }
    // loadFailed used to stick until the next room change, which made one transient
    // OOM during a peak (mid-frame eviction churn) permanently kill that atlas for
    // the rest of the room. The Undyne bridge has many big tilesets and thrashes
    // the eviction policy hardest, so a bridge atlas would lose its load forever
    // and tiles from it would never render. Retry once per frame instead — if
    // memory has freed up since (eviction, frame end), the second pass succeeds.
    if (page->loadFailed) {
        if (page->lastFrame == g_frame) return false;
        page->loadFailed = false;
    }
    if (page->fileOffset == 0 || !ctx->atlasFile) return false;

    free_old_source_pages(ctx);

    uint32_t tiledSize = page->dataSize;
    if (tiledSize == 0) {
        tiledSize = CTR_TEXTURE_CACHE_ATLAS_SIZE * CTR_TEXTURE_CACHE_ATLAS_SIZE * 2u;
    }
    GPU_TEXCOLOR texFormat = GPU_RGBA4;
    if (page->format == CTR_TEXTURE_CACHE_FORMAT_LA4) texFormat = GPU_LA4;
    else if (page->format == CTR_TEXTURE_CACHE_FORMAT_A4) texFormat = GPU_A4;

    bool texOk = C3D_TexInit(&page->tex,
                             (u16)CTR_TEXTURE_CACHE_ATLAS_SIZE,
                             (u16)CTR_TEXTURE_CACHE_ATLAS_SIZE,
                             texFormat);

    if (!texOk) {
        fprintf(stderr, "CTR: Failed to alloc C3D_Tex for atlas %d\n", pageId);
        page->loadFailed = true;
        page->lastFrame = g_frame; // stamp so retry waits until next frame
        return false;
    }
    if (tiledSize > page->tex.size) tiledSize = (uint32_t)page->tex.size;

    fseek(ctx->atlasFile, (long)page->fileOffset, SEEK_SET);
    if (fread(page->tex.data, 1, tiledSize, ctx->atlasFile) != tiledSize) {
        C3D_TexDelete(&page->tex);
        page->loadFailed = true;
        page->lastFrame = g_frame;
        return false;
    }

    C3D_TexFlush(&page->tex);
    C3D_TexSetFilter(&page->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&page->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    page->width = (int)CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->height = (int)CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->potW = (int)CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->potH = (int)CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->loaded = true;
    page->loadFailed = false;
    page->lastFrame = g_frame;

    if (!ctx->preloadingAtlases) {
        ctx->lazyLoadsThisRoom++;
        if (ctx->lazyLoadsThisRoom == 1) {
            CTR_DIAG("CTR cache: first lazy atlas in room %d '%s': page %d; linear free %.2f MB\n",
                     ctx->currentRoomIndex,
                     ctx->currentRoomName[0] ? ctx->currentRoomName : "?",
                     pageId,
                     (double)linearSpaceFree() / (1024.0 * 1024.0));
        }
        CTR_DIAG("CTR cache: lazy atlas load during draw: page %d; linear free %.2f MB\n",
                 pageId, (double)linearSpaceFree() / (1024.0 * 1024.0));
    }

    return true;
}

static bool queue_source_page_prefetch(CtrRenderer *ctx, int32_t pageId) {
    if (!ctx || ctx->prefetchQueueCount >= CTR_PREFETCH_QUEUE_CAP) return false;
    CtrSourcePage *page = get_source_page(ctx, pageId);
    if (!page || page->loaded) {
        if (page) page->lastFrame = g_frame;
        return false;
    }
    for (uint32_t i = 0; i < ctx->prefetchQueueCount; i++) {
        if (ctx->prefetchQueue[i] == pageId) return false;
    }
    ctx->prefetchQueue[ctx->prefetchQueueCount++] = pageId;
    return true;
}

static uint32_t process_prefetch_queue(CtrRenderer *ctx, uint32_t budget) {
    if (!ctx || !ctx->atlasFile || budget == 0) return 0;
    if (linearSpaceFree() < CTR_PREFETCH_MIN_FREE) return 0;

    uint32_t loaded = 0;
    uint32_t attempts = 0;
    while (ctx->prefetchQueueCount > 0 && loaded < budget && attempts < CTR_PREFETCH_QUEUE_CAP) {
        int32_t pageId = ctx->prefetchQueue[0];
        memmove(&ctx->prefetchQueue[0], &ctx->prefetchQueue[1],
                (ctx->prefetchQueueCount - 1) * sizeof(ctx->prefetchQueue[0]));
        ctx->prefetchQueueCount--;
        attempts++;

        CtrSourcePage *page = get_source_page(ctx, pageId);
        if (!page || page->loaded) continue;
        if (linearSpaceFree() < CTR_PREFETCH_MIN_FREE) break;

        bool oldPreload = ctx->preloadingAtlases;
        ctx->preloadingAtlases = true;
        bool ok = load_source_page_dyn(ctx, pageId);
        ctx->preloadingAtlases = oldPreload;
        if (ok && page->loaded) loaded++;
    }
    return loaded;
}

static void load_page_dyn(CtrRenderer *ctx, DataWin *dw, int32_t idx) {
    if (idx < 0 || (uint32_t)idx >= ctx->pageCount) return;

    int pageIdSigned = dw->tpag.items[idx].texturePageId;
    if (is_repacked_page(ctx, pageIdSigned)) {
        load_source_page_dyn(ctx, pageIdSigned);
        return;
    }

    CtrPage *page = &ctx->pages[idx];
    if (page->loaded) return;

    free_old_pages(ctx);

    if (pageIdSigned < 0) return;
    uint32_t pageId = (uint32_t)pageIdSigned;
    char path[256];
    page_meta_path(path, sizeof(path), (int)pageId);

    FILE *f = fopen(path, "rb");
    if (!f) return;
    setvbuf(f, dyn_buf, _IOFBF, sizeof(dyn_buf));

    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) == 1) {
        fseek(f, 0, SEEK_SET);
        if (magic == ATLAS_MAGIC) {
            AtlasHeader hdr;
            if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
                extract_legacy_page_file(ctx, dw, (uint32_t)idx, f, (int)hdr.w, (int)hdr.h);
            }
        }
    }
    fclose(f);
}

// Citro3D pipeline

static GPU_BLENDFACTOR gm_blend_factor_to_gpu(int32_t factor) {
    switch (factor) {
        case bm_zero:          return GPU_ZERO;
        case bm_one:           return GPU_ONE;
        case bm_src_color:     return GPU_SRC_COLOR;
        case bm_inv_src_color: return GPU_ONE_MINUS_SRC_COLOR;
        case bm_src_alpha:     return GPU_SRC_ALPHA;
        case bm_inv_src_alpha: return GPU_ONE_MINUS_SRC_ALPHA;
        case bm_dest_alpha:    return GPU_DST_ALPHA;
        case bm_inv_dest_alpha:return GPU_ONE_MINUS_DST_ALPHA;
        case bm_dest_color:    return GPU_DST_COLOR;
        case bm_inv_dest_color:return GPU_ONE_MINUS_DST_COLOR;
        case bm_src_alpha_sat: return GPU_SRC_ALPHA_SATURATE;
        default:               return GPU_ONE;
    }
}

static void apply_depth_write_mask(CtrRenderer *ctx) {
    C3D_DepthTest(false, GPU_GEQUAL, ctx->writeMask ? ctx->writeMask : GPU_WRITE_ALL);
}

static void apply_alpha_test_state(CtrRenderer *ctx) {
    C3D_AlphaTest(ctx->alphaTestEnabled, ctx->alphaTestEnabled ? GPU_GREATER : GPU_ALWAYS, ctx->alphaTestRef);
}

static void emit_blend_state(CtrRenderer *ctx) {
    if (!ctx->blendEnabled) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ZERO,
                       GPU_ONE, GPU_ZERO);
        return;
    }

    if (ctx->currentBlendMode == bm_complex) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       ctx->blendSrcColor, ctx->blendDstColor,
                       ctx->blendSrcAlpha, ctx->blendDstAlpha);
        return;
    }

    switch (ctx->currentBlendMode) {
        case bm_add:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE,
                           GPU_SRC_ALPHA, GPU_ONE);
            break;
        case bm_max:
            C3D_AlphaBlend(GPU_BLEND_MAX, GPU_BLEND_MAX,
                           GPU_ONE, GPU_ONE,
                           GPU_ONE, GPU_ONE);
            break;
        case bm_subtract:
            if (ctx->base.gameProfile == GAME_PROFILE_DELTARUNE) {
                // Deltarune's TP gauge uses bm_subtract as a white cutout mask.
                // This is the v4.6/v4.7-compatible path; the raw subtract path
                // makes the Chapter 2 tension surface collapse to transparent.
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                               GPU_ZERO, GPU_ONE_MINUS_SRC_COLOR,
                               GPU_ZERO, GPU_ONE_MINUS_SRC_ALPHA);
            } else {
                C3D_AlphaBlend(GPU_BLEND_SUBTRACT, GPU_BLEND_SUBTRACT,
                               GPU_SRC_ALPHA, GPU_ONE,
                               GPU_SRC_ALPHA, GPU_ONE);
            }
            break;
        case bm_min:
            C3D_AlphaBlend(GPU_BLEND_MIN, GPU_BLEND_MIN,
                           GPU_ONE, GPU_ONE,
                           GPU_ONE, GPU_ONE);
            break;
        case bm_reverse_subtract:
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT,
                           GPU_SRC_ALPHA, GPU_ONE,
                           GPU_SRC_ALPHA, GPU_ONE);
            break;
        case bm_normal:
        default:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            break;
    }
}

static void setup_pipeline(CtrRenderer *ctx) {
    if (ctx->pipelineReady) return;

    C3D_BindProgram(&g_shaderProg);
    ctx->uLoc_projection = shaderInstanceGetUniformLocation(g_shaderProg.vertexShader, "projection");

    AttrInfo_Init(&ctx->attrInfo);
    AttrInfo_AddLoader(&ctx->attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ctx->attrInfo, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ctx->attrInfo, 2, GPU_FLOAT, 4);
    C3D_SetAttrInfo(&ctx->attrInfo);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc (env, C3D_Both,  GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both,  GPU_MODULATE);

    ctx->currentBlendMode = bm_normal;
    ctx->blendEnabled = true;
    ctx->blendSrcColor = GPU_SRC_ALPHA;
    ctx->blendDstColor = GPU_ONE_MINUS_SRC_ALPHA;
    ctx->blendSrcAlpha = GPU_SRC_ALPHA;
    ctx->blendDstAlpha = GPU_ONE_MINUS_SRC_ALPHA;
    ctx->writeMask = GPU_WRITE_ALL;
    ctx->alphaTestEnabled = false;
    ctx->alphaTestRef = 0;

    apply_depth_write_mask(ctx);
    C3D_CullFace (GPU_CULL_NONE);
    apply_alpha_test_state(ctx);
    emit_blend_state(ctx);

    ctx->pipelineReady    = true;
}

static void rebind_state(CtrRenderer *ctx) {
    C3D_BindProgram(&g_shaderProg);
    C3D_SetAttrInfo(&ctx->attrInfo);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, ctx->vbuf, sizeof(CtrVertex), 3, 0x210);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc (env, C3D_Both,  GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both,  GPU_MODULATE);

    apply_depth_write_mask(ctx);
    C3D_CullFace (GPU_CULL_NONE);
    apply_alpha_test_state(ctx);
    emit_blend_state(ctx);
}

static void apply_projection(CtrRenderer *ctx, const C3D_Mtx *m) {
    ctx->currentProjection = *m;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, ctx->uLoc_projection, m);
}

static void make_ortho_topleft(C3D_Mtx *out, float w, float h) {
    Mtx_Identity(out);
    Mtx_Ortho(out, 0.f, w, h, 0.f, -1.f, 1.f, true);
}

static void make_ortho_top(C3D_Mtx *out, float w, float h) {
    Mtx_Identity(out);
    Mtx_OrthoTilt(out, 0.f, w, h, 0.f, -1.f, 1.f, true);
}

// Surfaces

static CtrSurface *get_surface(CtrRenderer *ctx, int32_t surfaceId) {
    if (surfaceId < 0 || (uint32_t)surfaceId >= ctx->surfaceCount) return NULL;
    return ctx->surfaces[surfaceId].used ? &ctx->surfaces[surfaceId] : NULL;
}

static bool surface_alloc_storage(CtrSurface *surf, int width, int height) {
    surf->width  = width;
    surf->height = height;
    surf->potW   = next_pow2(width);
    surf->potH   = next_pow2(height);

    if (!C3D_TexInitVRAM(&surf->tex, (u16)surf->potW, (u16)surf->potH, GPU_RGBA8)) {
        return false;
    }
    C3D_TexSetFilter(&surf->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap  (&surf->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    surf->target = C3D_RenderTargetCreateFromTex(&surf->tex, GPU_TEXFACE_2D, 0,
                                                 -1);
    if (!surf->target) {
        C3D_TexDelete(&surf->tex);
        return false;
    }
    return true;
}

static void surface_release_storage(CtrRenderer *ctx, CtrSurface *surf) {
    if (surf->target) {
        safe_delete_target(ctx, surf->target);
        surf->target = NULL;
    }
    if (surf->tex.data) {
        C3D_TexDelete(&surf->tex);
        memset(&surf->tex, 0, sizeof(surf->tex));
    }
}

// Application surface

static void destroy_app_surface(CtrRenderer *ctx) {
    if (ctx->appTarget) {
        safe_delete_target(ctx, ctx->appTarget);
        ctx->appTarget = NULL;
    }
    if (ctx->appTex.data) {
        C3D_TexDelete(&ctx->appTex);
        memset(&ctx->appTex, 0, sizeof(ctx->appTex));
    }
    ctx->appReady = false;
}

static bool ensure_app_surface(CtrRenderer *ctx, int gw, int gh) {
    if (ctx->appReady && ctx->appLogicW == gw && ctx->appLogicH == gh) return true;

    destroy_app_surface(ctx);

    ctx->appLogicW = gw;
    ctx->appLogicH = gh;
    ctx->appPotW   = next_pow2(gw);
    ctx->appPotH   = next_pow2(gh);

    if (!C3D_TexInitVRAM(&ctx->appTex, (u16)ctx->appPotW, (u16)ctx->appPotH, GPU_RGBA8)) {
        return false;
    }
    apply_app_filter(ctx);
    C3D_TexSetWrap  (&ctx->appTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    ctx->appTarget = C3D_RenderTargetCreateFromTex(&ctx->appTex, GPU_TEXFACE_2D, 0,
                                                   -1);
    if (!ctx->appTarget) {
        C3D_TexDelete(&ctx->appTex);
        memset(&ctx->appTex, 0, sizeof(ctx->appTex));
        return false;
    }

    // C3D_TexInitVRAM hands back uninitialised VRAM. The first frame's clear covers
    // the logic region, but GPU_LINEAR sampling at the logic <-> POT-padding boundary
    // can pull undefined bytes from the padding into the visible image (a 1-px
    // garbage seam along the top/left of the game on a black room before content
    // loads). Zero the entire POT framebuffer once at create time so even the
    // padding is well-defined when the first sample lands on it.
    C3D_RenderTargetClear(ctx->appTarget, C3D_CLEAR_ALL, 0x000000FF, 0);

    ctx->appReady = true;
    return true;
}

// Target and viewport

static void bind_target(CtrRenderer *ctx, C3D_RenderTarget *tgt) {
    if (!ctx->inFrame) return;
    bool switchingTarget = ctx->activeTarget && ctx->activeTarget != tgt;
    flush_batch(ctx);
    if (switchingTarget) ctr_safe_frame_split(ctx);
    C3D_FrameDrawOn(tgt);
    ctx->activeTarget = tgt;
    rebind_state(ctx);
    apply_projection(ctx, &ctx->currentProjection);
    emit_blend_state(ctx);
}

static void set_viewport_logical(CtrRenderer *ctx, C3D_RenderTarget *tgt,
                                 int x, int y, int w, int h) {
    int fbH = tgt->frameBuf.height;
    int vpY = fbH - y - h;
    if (vpY < 0) vpY = 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    C3D_SetViewport((u32)x, (u32)vpY, (u32)w, (u32)h);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)x, (u32)vpY, (u32)(x + w), (u32)(vpY + h));
    ctx->currentViewport[0] = x;
    ctx->currentViewport[1] = y;
    ctx->currentViewport[2] = w;
    ctx->currentViewport[3] = h;
}

static void disable_scissor(CtrRenderer *ctx) {
    (void)ctx;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Blend state

static void apply_blend(CtrRenderer *ctx, int mode) {
    flush_batch(ctx);
    ctx->currentBlendMode = mode;
    ctx->blendEnabled = true;
    emit_blend_state(ctx);
}

// Frame lifecycle

static void ctr_init(Renderer *ren, DataWin *dw) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    ren->dataWin = dw;

    setup_pipeline(ctx);

    if (!ctx->topTarget) {
        ctx->topTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (ctx->topTarget) {
            C3D_RenderTargetSetOutput(ctx->topTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        }
    }
    if (!ctx->bottomTarget) {
        ctx->bottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (ctx->bottomTarget) {
            C3D_RenderTargetSetOutput(ctx->bottomTarget, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        }
    }

    ctx->originalTpagCount   = dw->tpag.count;
    ctx->originalSpriteCount = dw->sprt.count;

    CtrTextureCache_prepare(dw);
    CtrTextureCache_apply(ctx, dw);

    char path[256];
    CtrTextureCache_indexPath(path, sizeof(path));
    ctx->atlasFile = fopen(path, "rb");
    if (ctx->atlasFile) {
        setvbuf(ctx->atlasFile, NULL, _IOFBF, 128 * 1024);
    }

    ctx->pageCount = dw->tpag.count;
    ctx->pages     = calloc(ctx->pageCount, sizeof(CtrPage));

    if (!ctx->vbuf) {
        // 4x batch capacity. With small vbuf, many tile-heavy rooms (bridge corridor
        // before/with Undyne) trigger constant FrameSplits which both eat cmdbuf
        // and stall the GPU. Larger vbuf = fewer splits per frame.
        ctx->vbufCap = BATCH_VERT_CAP * 4;
        ctx->vbuf    = linearAlloc(ctx->vbufCap * sizeof(CtrVertex));
        ctx->vbufHead   = 0;
        ctx->batchStart = 0;
        ctx->batchVerts = 0;
    }

    {
        if (C3D_TexInit(&ctx->whiteTex, 8, 8, GPU_RGBA4)) {
            uint16_t *tile = linearAlloc(8 * 8 * 2);
            if (tile) {
                for (int i = 0; i < 64; i++) tile[i] = 0xFFFF;
                C3D_TexLoadImage(&ctx->whiteTex, tile, GPU_TEXFACE_2D, 0);
                C3D_TexFlush(&ctx->whiteTex);
                linearFree(tile);
            }
            C3D_TexSetFilter(&ctx->whiteTex, GPU_LINEAR, GPU_NEAREST);
            C3D_TexSetWrap  (&ctx->whiteTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        }
    }

}

static void ctr_destroy(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;

    if (ctx->inFrame) {
        flush_batch(ctx);
        C3D_FrameEnd(0);
        ctx->inFrame = false;
    }
    gc_clear_targets();

    free_all_tilemap_caches(ctx);

    if (ctx->atlasFile) fclose(ctx->atlasFile);

    for (uint32_t i = 0; i < ctx->surfaceCount; i++) {
        surface_release_storage(ctx, &ctx->surfaces[i]);
    }
    free(ctx->surfaces);
    ctx->surfaces      = NULL;
    ctx->surfaceCount  = 0;

    destroy_app_surface(ctx);

    for (uint32_t i = 0; i < ctx->pageCount; i++) {
        if (!ctx->pages[i].loaded) continue;
        for (int cx = 0; cx < ctx->pages[i].chunksX; cx++) {
            for (int cy = 0; cy < ctx->pages[i].chunksY; cy++) {
                CtrAtlasChunk *ch = &ctx->pages[i].chunks[cx][cy];
                if (ch->valid) C3D_TexDelete(&ch->tex);
            }
        }
    }
    free(ctx->pages);
    ctx->pages = NULL;

    for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
        if (ctx->sourcePages[i].loaded) {
            C3D_TexDelete(&ctx->sourcePages[i].tex);
        }
    }
    free(ctx->sourcePages);
    ctx->sourcePages = NULL;
    free(ctx->cacheItems);
    ctx->cacheItems = NULL;
    ctx->cacheItemCount = 0;
    free(ctx->cacheSegments);
    ctx->cacheSegments = NULL;
    ctx->cacheSegmentCount = 0;
    ctx->sourcePageCount = 0;

    if (ctx->whiteTex.data) C3D_TexDelete(&ctx->whiteTex);

    if (ctx->vbuf) { linearFree(ctx->vbuf); ctx->vbuf = NULL; }

    if (ctx->pipelineReady) {
        ctx->pipelineReady = false;
    }

    if (ctx->topTarget)    { C3D_RenderTargetDelete(ctx->topTarget);    ctx->topTarget    = NULL; }
    if (ctx->bottomTarget) { C3D_RenderTargetDelete(ctx->bottomTarget); ctx->bottomTarget = NULL; }

    free(ctx);
}

static void ctr_begin_frame(Renderer *ren, int32_t gw, int32_t gh, int32_t ww, int32_t wh) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    gc_clear_targets();
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    ctx->winW  = ww;
    ctx->winH  = wh;
    ctx->gameW = gw;
    ctx->gameH = gh;

    if (!ensure_app_surface(ctx, gw, gh)) return;
    apply_app_filter(ctx);

    if (!ctx->inFrame) {
        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
        ctx->inFrame = true;
        ctx->vbufHead   = 0;
        ctx->batchStart = 0;
        ctx->batchVerts = 0;
        ctx->batchTex   = NULL;
    }

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, ctx->vbuf, sizeof(CtrVertex), 3, 0x210);

    C3D_RenderTargetClear(ctx->appTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    bind_target(ctx, ctx->appTarget);

    set_viewport_logical(ctx, ctx->appTarget, 0, 0, ctx->appLogicW, ctx->appLogicH);
    C3D_Mtx proj;
    make_ortho_topleft(&proj, (float)ctx->appLogicW, (float)ctx->appLogicH);
    apply_projection(ctx, &proj);

    ctx->appFrameCleared  = true;
    ctx->targetStackDepth = 0;
    ctx->surfaceDrawSuppressed = false;
    ctx->surfaceDrawSuppressedDepth = 0;
}

static void ctr_end_frame(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);

    if (!ctx->inFrame) return;

    if (ctx->appReady) {
        C3D_RenderTarget *primary = (g_ctr_game_screen == CTR_GAME_SCREEN_BOTTOM)
                                        ? ctx->bottomTarget : ctx->topTarget;
        C3D_RenderTarget *secondary = (g_ctr_game_screen == CTR_GAME_SCREEN_BOTTOM)
                                          ? ctx->topTarget : ctx->bottomTarget;
        int primaryW = (g_ctr_game_screen == CTR_GAME_SCREEN_BOTTOM) ? 320 : 400;
        int primaryH = 240;
        int secondaryW = (g_ctr_game_screen == CTR_GAME_SCREEN_BOTTOM) ? 400 : 320;

        if (secondary) {
            C3D_FrameDrawOn(secondary);
            ctx->activeTarget = secondary;
            rebind_state(ctx);

            C3D_RenderTargetClear(secondary, C3D_CLEAR_ALL, 0x050711FF, 0);
            C3D_SetViewport(0, 0, 240, (u32)secondaryW);
            disable_scissor(ctx);

            C3D_Mtx projS;
            make_ortho_top(&projS, (float)secondaryW, (float)primaryH);
            apply_projection(ctx, &projS);

            int savedWinW = ctx->winW;
            int savedWinH = ctx->winH;
            ctx->winW = secondaryW;
            ctx->winH = primaryH;
            draw_letterbox_backdrop(ctx);
            ctx->winW = savedWinW;
            ctx->winH = savedWinH;
            flush_batch(ctx);
        }

        if (primary) {
            C3D_FrameDrawOn(primary);
            ctx->activeTarget = primary;
            rebind_state(ctx);

            C3D_RenderTargetClear(primary, C3D_CLEAR_ALL, 0x050711FF, 0);
            C3D_SetViewport(0, 0, 240, (u32)primaryW);
            disable_scissor(ctx);

            // Override winW/winH so letterbox + scaling fit the chosen screen.
            int savedWinW = ctx->winW;
            int savedWinH = ctx->winH;
            ctx->winW = primaryW;
            ctx->winH = primaryH;

            C3D_Mtx proj;
            make_ortho_top(&proj, (float)ctx->winW, (float)ctx->winH);
            apply_projection(ctx, &proj);

            draw_letterbox_backdrop(ctx);

            int drawW, drawH;
            if (g_ctr_display_mode == CTR_DISPLAY_STRETCH) {
                drawW = ctx->winW;
                drawH = ctx->winH;
            } else if ((ctx->gameW * ctx->winH) / ctx->gameH < ctx->winW) {
                drawW = (ctx->gameW * ctx->winH) / ctx->gameH;
                drawH = ctx->winH;
            } else {
                drawW = ctx->winW;
                drawH = (ctx->gameH * ctx->winW) / ctx->gameW;
            }
            int drawX = (ctx->winW - drawW) / 2;
            int drawY = (ctx->winH - drawH) / 2;

            float u1 = (float)ctx->appLogicW / (float)ctx->appPotW;
            float v0 = (float)(ctx->appPotH - ctx->appLogicH) / (float)ctx->appPotH;
            float v1 = 1.f;
            float white[4] = {1.f, 1.f, 1.f, 1.f};

            push_quad(ctx, &ctx->appTex,
                      (float)drawX,           (float)drawY,
                      (float)(drawX + drawW), (float)drawY,
                      (float)(drawX + drawW), (float)(drawY + drawH),
                      (float)drawX,           (float)(drawY + drawH),
                      0.f, v1, u1, v0, white);
            flush_batch(ctx);

            ctx->winW = savedWinW;
            ctx->winH = savedWinH;
        }
    }

    C3D_FrameEnd(0);
    ctx->inFrame = false;
    g_frame++;
    for (int i = 0; i < g_gc_target_count; i++) {
        C3D_RenderTargetDelete(g_gc_targets[i]);
    }
    g_gc_target_count = 0;
}

static void ctr_flush(Renderer *ren) { flush_batch((CtrRenderer *)ren); }

// Views and GUI

static void ctr_begin_view(Renderer *ren, int32_t vx, int32_t vy, int32_t vw, int32_t vh,
                           int32_t px, int32_t py, int32_t pw, int32_t ph, float angle) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);

    if (ctx->activeTarget != ctx->appTarget) {
        bind_target(ctx, ctx->appTarget);
    }

    set_viewport_logical(ctx, ctx->appTarget, px, py, pw, ph);

    C3D_Mtx proj, rot, res;
    Mtx_Identity(&proj);
    Mtx_Ortho(&proj, (float)vx, (float)(vx + vw), (float)(vy + vh), (float)vy, -1.f, 1.f, true);

    if (angle != 0.f) {
        float cx = vx + vw / 2.f, cy = vy + vh / 2.f;
        Mtx_Identity(&rot);
        Mtx_Translate(&rot, cx, cy, 0.f, true);
        Mtx_RotateZ  (&rot, -angle * (float)(M_PI / 180.f), true);
        Mtx_Translate(&rot, -cx, -cy, 0.f, true);
        Mtx_Multiply(&res, &proj, &rot);
        proj = res;
    }
    apply_projection(ctx, &proj);

    // Enable culling against the view rect in room coords. Disabled when the view
    // is rotated — an axis-aligned cull rect doesn't match a rotated frustum.
    if (angle == 0.f) {
        ctx->cullEnabled = true;
        ctx->cullL = (float)vx;
        ctx->cullT = (float)vy;
        ctx->cullR = (float)(vx + vw);
        ctx->cullB = (float)(vy + vh);
    } else {
        ctx->cullEnabled = false;
    }
}

static void ctr_end_view(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    disable_scissor(ctx);
    ctx->cullEnabled = false;
}

static void ctr_begin_gui(Renderer *ren, int32_t gw, int32_t gh,
                          int32_t px, int32_t py, int32_t pw, int32_t ph) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    if (ctx->activeTarget != ctx->appTarget) bind_target(ctx, ctx->appTarget);

    set_viewport_logical(ctx, ctx->appTarget, px, py, pw, ph);

    C3D_Mtx proj;
    Mtx_Identity(&proj);
    Mtx_Ortho(&proj, 0.f, (float)gw, (float)gh, 0.f, -1.f, 1.f, true);
    apply_projection(ctx, &proj);

    // GUI uses logical coords (0..gw, 0..gh). Anything outside that rect is
    // off-screen for this layer and can be culled.
    ctx->cullEnabled = true;
    ctx->cullL = 0.f;
    ctx->cullT = 0.f;
    ctx->cullR = (float)gw;
    ctx->cullB = (float)gh;
}

static void ctr_end_gui(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    disable_scissor(ctx);
    ctx->cullEnabled = false;
}

// View-bounds culling. Returns true if the quad's bbox is fully outside the
// active cull rect (= safe to skip the draw entirely). Cheap: 4 fminf/fmaxf +
// 4 compares. Bridge corridor + Hotland have rooms ~5000+ tiles; the renderer
// previously emitted them all every frame, even though only ~400 fit on screen.
static inline bool quad_culled(const CtrRenderer *ctx,
                               float x0, float y0, float x1, float y1,
                               float x2, float y2, float x3, float y3) {
    if (!ctx->cullEnabled) return false;
    float minX = fminf(fminf(x0, x1), fminf(x2, x3));
    if (minX >= ctx->cullR) return true;
    float maxX = fmaxf(fmaxf(x0, x1), fmaxf(x2, x3));
    if (maxX <= ctx->cullL) return true;
    float minY = fminf(fminf(y0, y1), fminf(y2, y3));
    if (minY >= ctx->cullB) return true;
    float maxY = fmaxf(fmaxf(y0, y1), fmaxf(y2, y3));
    if (maxY <= ctx->cullT) return true;
    return false;
}

// Deltarune's large automatic GMS2 tilemaps can disappear on 3DS when drawn as
// many cached RGBA4 atlas slices. Compose those maps into small RGBA8 chunks so
// the room floor/background uses a single stable GPU texture path per chunk.
#define CTR_GMS2_TILE_INDEX_MASK  0x0007FFFFu
#define CTR_GMS2_TILE_MIRROR_MASK 0x10000000u
#define CTR_GMS2_TILE_FLIP_MASK   0x20000000u

static void free_tilemap_cache(CtrTilemapLayerCache *cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < cache->chunkCount; i++) {
        CtrTilemapChunk *chunk = &cache->chunks[i];
        if (chunk->valid) {
            C3D_TexDelete(&chunk->tex);
            chunk->valid = false;
        }
    }
    free(cache->chunks);
    memset(cache, 0, sizeof(*cache));
}

static void free_all_tilemap_caches(CtrRenderer *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->tilemapCacheCount; i++) {
        free_tilemap_cache(&ctx->tilemapCaches[i]);
    }
    free(ctx->tilemapCaches);
    ctx->tilemapCaches = NULL;
    ctx->tilemapCacheCount = 0;
}

static void tilemap_set_status(CtrRenderer *ctx, const char *status) {
    if (!ctx) return;
    snprintf(ctx->tilemapLastStatus, sizeof(ctx->tilemapLastStatus), "%s",
             status ? status : "unknown");
}

static uint32_t hash_tile_layer_data(const RoomLayerTilesData *data) {
    uint32_t hash = 2166136261u;
    if (!data || !data->tileData) return hash;
    const uint32_t meta[] = {
        data->tilesX, data->tilesY, (uint32_t)data->backgroundIndex
    };
    for (uint32_t i = 0; i < (uint32_t)(sizeof(meta) / sizeof(meta[0])); i++) {
        hash ^= meta[i];
        hash *= 16777619u;
    }
    uint64_t count = (uint64_t)data->tilesX * (uint64_t)data->tilesY;
    if (count > 262144u) count = 262144u;
    for (uint64_t i = 0; i < count; i++) {
        hash ^= data->tileData[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint8_t *decode_texture_page_rgba(CtrRenderer *ctx, int32_t texturePageId,
                                         int *outW, int *outH) {
    if (!ctx || !ctx->base.dataWin || texturePageId < 0 ||
        (uint32_t)texturePageId >= ctx->base.dataWin->txtr.count) {
        return NULL;
    }

    DataWin *dw = ctx->base.dataWin;
    Texture *tex = &dw->txtr.textures[texturePageId];
    const uint8_t *blob = tex->blobData;
    uint8_t *ownedBlob = NULL;
    size_t blobSize = (size_t)tex->blobSize;

    if (!blob && tex->blobOffset != 0 && tex->blobSize != 0 && g_current_data_path[0] != '\0') {
        FILE *fp = fopen(g_current_data_path, "rb");
        if (fp) {
            ownedBlob = read_blob(fp, tex->blobOffset, tex->blobSize);
            fclose(fp);
            blob = ownedBlob;
        }
    }

    if (!blob || blobSize == 0) {
        free(ownedBlob);
        return NULL;
    }

    uint8_t *pixels = ImageDecoder_decodeToRgba(
        blob, blobSize, DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), outW, outH);
    free(ownedBlob);
    return pixels;
}

static void blend_rgba_pixel(uint8_t *dst, const uint8_t *src, uint8_t layerAlpha) {
    uint32_t srcA = ((uint32_t)src[3] * (uint32_t)layerAlpha + 127u) / 255u;
    if (srcA == 0) return;
    if (srcA >= 255u || dst[3] == 0) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = (uint8_t)srcA;
        return;
    }

    uint32_t dstA = dst[3];
    uint32_t invA = 255u - srcA;
    uint32_t outA = srcA + (dstA * invA + 127u) / 255u;
    if (outA == 0) return;

    dst[0] = (uint8_t)((src[0] * srcA + dst[0] * dstA * invA / 255u) / outA);
    dst[1] = (uint8_t)((src[1] * srcA + dst[1] * dstA * invA / 255u) / outA);
    dst[2] = (uint8_t)((src[2] * srcA + dst[2] * dstA * invA / 255u) / outA);
    dst[3] = (uint8_t)outA;
}

static inline uint8_t expand4(uint8_t v) {
    return (uint8_t)((v << 4) | v);
}

static bool sample_tiled_source_page_rgba(const CtrSourcePage *page, int x, int y,
                                          uint8_t out[4]) {
    if (!page || !page->loaded || !page->tex.data || !out) return false;
    if (x < 0 || y < 0 || x >= page->potW || y >= page->potH) return false;

    uint32_t invY = (uint32_t)(page->potH - 1 - y);
    uint32_t bx = (uint32_t)x >> 3;
    uint32_t by = invY >> 3;
    uint32_t localX = (uint32_t)x & 7u;
    uint32_t localY = invY & 7u;
    uint32_t blocksX = (uint32_t)page->potW >> 3;
    uint32_t pixelIndex = (by * blocksX + bx) * 64u + morton_pos(localX, localY);

    const uint8_t *data = (const uint8_t *)page->tex.data;
    switch (page->format) {
        case CTR_TEXTURE_CACHE_FORMAT_LA4: {
            uint8_t px = data[pixelIndex];
            uint8_t l = expand4(px >> 4);
            uint8_t a = expand4(px & 0x0Fu);
            out[0] = l; out[1] = l; out[2] = l; out[3] = a;
            return true;
        }
        case CTR_TEXTURE_CACHE_FORMAT_A4: {
            uint8_t byte = data[pixelIndex >> 1];
            uint8_t nibble = (pixelIndex & 1u) ? (byte >> 4) : (byte & 0x0Fu);
            out[0] = 255; out[1] = 255; out[2] = 255; out[3] = expand4(nibble);
            return true;
        }
        case CTR_TEXTURE_CACHE_FORMAT_RGBA4:
        default: {
            uint16_t px = ((const uint16_t *)data)[pixelIndex];
            out[0] = expand4((uint8_t)((px >> 12) & 0x0Fu));
            out[1] = expand4((uint8_t)((px >> 8) & 0x0Fu));
            out[2] = expand4((uint8_t)((px >> 4) & 0x0Fu));
            out[3] = expand4((uint8_t)(px & 0x0Fu));
            return true;
        }
    }
}

static bool sample_cached_tpag_rgba(CtrRenderer *ctx, uint32_t tpagIndex,
                                    int relX, int relY, uint8_t out[4]) {
    if (!ctx || !out || !cache_item_available(ctx, tpagIndex)) return false;
    if (relX < 0 || relY < 0) return false;

    CtrCachedTpag *entry = &ctx->cacheItems[tpagIndex];
    for (uint32_t i = 0; i < entry->segmentCount; i++) {
        CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + i];
        int segL = (int)seg->sourceX;
        int segT = (int)seg->sourceY;
        int segR = segL + (int)seg->width;
        int segB = segT + (int)seg->height;
        if (relX < segL || relX >= segR || relY < segT || relY >= segB) continue;
        if (seg->atlasIndex >= ctx->sourcePageCount) return false;

        int pageId = (int)(ctx->repackBasePageId + seg->atlasIndex);
        if (!load_source_page_dyn(ctx, pageId)) return false;
        CtrSourcePage *page = &ctx->sourcePages[seg->atlasIndex];
        int atlasX = (int)seg->atlasX + (relX - segL);
        int atlasY = (int)seg->atlasY + (relY - segT);
        return sample_tiled_source_page_rgba(page, atlasX, atlasY, out);
    }

    return false;
}

static bool upload_tilemap_chunk(CtrTilemapChunk *chunk, const uint8_t *rgba) {
    if (!chunk || !rgba || chunk->potW <= 0 || chunk->potH <= 0) return false;

    size_t pixelCount = (size_t)chunk->potW * (size_t)chunk->potH;
    uint16_t *linear = malloc(pixelCount * sizeof(uint16_t));
    if (!linear) return false;

    for (size_t i = 0; i < pixelCount; i++) {
        const uint8_t *p = &rgba[i * 4u];
        linear[i] = pack_rgba4444(p[0], p[1], p[2], p[3]);
    }

    uint16_t *tiled = linearAlloc(pixelCount * sizeof(uint16_t));
    if (!tiled) {
        free(linear);
        return false;
    }
    tile_rgba4(linear, tiled, chunk->potW, chunk->potH, chunk->potW, chunk->potH);
    free(linear);

    bool ok = C3D_TexInit(&chunk->tex, (u16)chunk->potW, (u16)chunk->potH, GPU_RGBA4);
    if (ok) {
        C3D_TexSetFilter(&chunk->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&chunk->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexLoadImage(&chunk->tex, tiled, GPU_TEXFACE_2D, 0);
        C3D_TexFlush(&chunk->tex);
        chunk->valid = true;
    }

    linearFree(tiled);
    return ok;
}

static bool compose_tilemap_chunk(CtrRenderer *ctx, CtrTilemapLayerCache *cache,
                                  CtrTilemapChunk *chunk, const uint8_t *pagePixels,
                                  int pageW, int pageH, const TexturePageItem *tpag,
                                  const Background *tileset, uint8_t layerAlpha) {
    if (!ctx || !cache || !chunk || !pagePixels || !tpag || !tileset) return false;

    size_t rgbaBytes = (size_t)chunk->potW * (size_t)chunk->potH * 4u;
    uint8_t *rgba = calloc(1, rgbaBytes);
    if (!rgba) return false;

    int startTx = chunk->x / (int)cache->tileW;
    int startTy = chunk->y / (int)cache->tileH;
    int endTx = (chunk->x + chunk->width + (int)cache->tileW - 1) / (int)cache->tileW;
    int endTy = (chunk->y + chunk->height + (int)cache->tileH - 1) / (int)cache->tileH;
    if (startTx < 0) startTx = 0;
    if (startTy < 0) startTy = 0;
    if (endTx > (int)cache->tilesX) endTx = (int)cache->tilesX;
    if (endTy > (int)cache->tilesY) endTy = (int)cache->tilesY;

    bool anyPixel = false;
    uint32_t opaquePixels = 0;
    for (int ty = startTy; ty < endTy; ty++) {
        for (int tx = startTx; tx < endTx; tx++) {
            uint32_t cell = cache->data->tileData[(uint32_t)ty * cache->tilesX + (uint32_t)tx];
            uint32_t tileIndex = cell & CTR_GMS2_TILE_INDEX_MASK;
            if (tileIndex == 0) continue;

            bool mirror = (cell & CTR_GMS2_TILE_MIRROR_MASK) != 0;
            bool flip = (cell & CTR_GMS2_TILE_FLIP_MASK) != 0;
            uint32_t col = tileIndex % tileset->gms2TileColumns;
            uint32_t row = tileIndex / tileset->gms2TileColumns;
            int srcBaseX = (int)(col * (cache->tileW + 2u * tileset->gms2OutputBorderX) +
                                 tileset->gms2OutputBorderX);
            int srcBaseY = (int)(row * (cache->tileH + 2u * tileset->gms2OutputBorderY) +
                                 tileset->gms2OutputBorderY);
            int dstBaseX = tx * (int)cache->tileW;
            int dstBaseY = ty * (int)cache->tileH;

            int drawL = dstBaseX > chunk->x ? dstBaseX : chunk->x;
            int drawT = dstBaseY > chunk->y ? dstBaseY : chunk->y;
            int drawR = dstBaseX + (int)cache->tileW;
            int drawB = dstBaseY + (int)cache->tileH;
            if (drawR > chunk->x + chunk->width) drawR = chunk->x + chunk->width;
            if (drawB > chunk->y + chunk->height) drawB = chunk->y + chunk->height;

            for (int py = drawT; py < drawB; py++) {
                int tileY = py - dstBaseY;
                int sampleY = flip ? ((int)cache->tileH - 1 - tileY) : tileY;
                int localSrcY = srcBaseY + sampleY;
                int relSrcY = localSrcY - (int)tpag->targetY;
                int absSrcY = (int)tpag->sourceY + relSrcY;
                if (relSrcY < 0 || relSrcY >= (int)tpag->sourceHeight ||
                    absSrcY < 0 || absSrcY >= pageH) {
                    continue;
                }

                for (int px = drawL; px < drawR; px++) {
                    int tileX = px - dstBaseX;
                    int sampleX = mirror ? ((int)cache->tileW - 1 - tileX) : tileX;
                    int localSrcX = srcBaseX + sampleX;
                    int relSrcX = localSrcX - (int)tpag->targetX;
                    int absSrcX = (int)tpag->sourceX + relSrcX;
                    if (relSrcX < 0 || relSrcX >= (int)tpag->sourceWidth ||
                        absSrcX < 0 || absSrcX >= pageW) {
                        continue;
                    }

                    const uint8_t *src = &pagePixels[((size_t)absSrcY * (size_t)pageW + (size_t)absSrcX) * 4u];
                    uint8_t *dst = &rgba[((size_t)(py - chunk->y) * (size_t)chunk->potW +
                                          (size_t)(px - chunk->x)) * 4u];
                    blend_rgba_pixel(dst, src, layerAlpha);
                    if (src[3] != 0) {
                        anyPixel = true;
                        opaquePixels++;
                    }
                }
            }
        }
    }

    bool ok = anyPixel && upload_tilemap_chunk(chunk, rgba);
    if (ok) {
        ctx->tilemapChunksBuilt++;
        ctx->tilemapOpaquePixels += opaquePixels;
    }
    free(rgba);
    return ok;
}

static bool compose_tilemap_chunk_from_cache(CtrRenderer *ctx, CtrTilemapLayerCache *cache,
                                             CtrTilemapChunk *chunk,
                                             const TexturePageItem *tpag,
                                             const Background *tileset,
                                             uint8_t layerAlpha) {
    if (!ctx || !cache || !chunk || !tpag || !tileset) return false;
    if (!cache_item_available(ctx, (uint32_t)tileset->tpagIndex)) return false;

    size_t rgbaBytes = (size_t)chunk->potW * (size_t)chunk->potH * 4u;
    uint8_t *rgba = calloc(1, rgbaBytes);
    if (!rgba) return false;

    int startTx = chunk->x / (int)cache->tileW;
    int startTy = chunk->y / (int)cache->tileH;
    int endTx = (chunk->x + chunk->width + (int)cache->tileW - 1) / (int)cache->tileW;
    int endTy = (chunk->y + chunk->height + (int)cache->tileH - 1) / (int)cache->tileH;
    if (startTx < 0) startTx = 0;
    if (startTy < 0) startTy = 0;
    if (endTx > (int)cache->tilesX) endTx = (int)cache->tilesX;
    if (endTy > (int)cache->tilesY) endTy = (int)cache->tilesY;

    bool anyPixel = false;
    uint32_t opaquePixels = 0;
    uint8_t srcPx[4];

    for (int ty = startTy; ty < endTy; ty++) {
        for (int tx = startTx; tx < endTx; tx++) {
            uint32_t cell = cache->data->tileData[(uint32_t)ty * cache->tilesX + (uint32_t)tx];
            uint32_t tileIndex = cell & CTR_GMS2_TILE_INDEX_MASK;
            if (tileIndex == 0) continue;

            bool mirror = (cell & CTR_GMS2_TILE_MIRROR_MASK) != 0;
            bool flip = (cell & CTR_GMS2_TILE_FLIP_MASK) != 0;
            uint32_t col = tileIndex % tileset->gms2TileColumns;
            uint32_t row = tileIndex / tileset->gms2TileColumns;
            int srcBaseX = (int)(col * (cache->tileW + 2u * tileset->gms2OutputBorderX) +
                                 tileset->gms2OutputBorderX);
            int srcBaseY = (int)(row * (cache->tileH + 2u * tileset->gms2OutputBorderY) +
                                 tileset->gms2OutputBorderY);
            int dstBaseX = tx * (int)cache->tileW;
            int dstBaseY = ty * (int)cache->tileH;

            int drawL = dstBaseX > chunk->x ? dstBaseX : chunk->x;
            int drawT = dstBaseY > chunk->y ? dstBaseY : chunk->y;
            int drawR = dstBaseX + (int)cache->tileW;
            int drawB = dstBaseY + (int)cache->tileH;
            if (drawR > chunk->x + chunk->width) drawR = chunk->x + chunk->width;
            if (drawB > chunk->y + chunk->height) drawB = chunk->y + chunk->height;

            for (int py = drawT; py < drawB; py++) {
                int tileY = py - dstBaseY;
                int sampleY = flip ? ((int)cache->tileH - 1 - tileY) : tileY;
                int localSrcY = srcBaseY + sampleY;
                int relSrcY = localSrcY - (int)tpag->targetY;
                if (relSrcY < 0 || relSrcY >= (int)tpag->sourceHeight) continue;

                for (int px = drawL; px < drawR; px++) {
                    int tileX = px - dstBaseX;
                    int sampleX = mirror ? ((int)cache->tileW - 1 - tileX) : tileX;
                    int localSrcX = srcBaseX + sampleX;
                    int relSrcX = localSrcX - (int)tpag->targetX;
                    if (relSrcX < 0 || relSrcX >= (int)tpag->sourceWidth) continue;

                    if (!sample_cached_tpag_rgba(ctx, (uint32_t)tileset->tpagIndex,
                                                relSrcX, relSrcY, srcPx)) {
                        continue;
                    }

                    uint8_t *dst = &rgba[((size_t)(py - chunk->y) * (size_t)chunk->potW +
                                          (size_t)(px - chunk->x)) * 4u];
                    blend_rgba_pixel(dst, srcPx, layerAlpha);
                    if (srcPx[3] != 0) {
                        anyPixel = true;
                        opaquePixels++;
                    }
                }
            }
        }
    }

    bool ok = anyPixel && upload_tilemap_chunk(chunk, rgba);
    if (ok) {
        ctx->tilemapChunksBuilt++;
        ctx->tilemapOpaquePixels += opaquePixels;
    }
    free(rgba);
    return ok;
}

static CtrTilemapLayerCache *build_tilemap_cache(CtrRenderer *ctx, RoomLayerTilesData *data,
                                                 uint32_t hash) {
    if (!ctx || !data || !data->tileData || data->backgroundIndex < 0) return NULL;
    ctx->tilemapBuildAttempts++;

    DataWin *dw = ctx->base.dataWin;
    if (!dw || (uint32_t)data->backgroundIndex >= dw->bgnd.count) {
        ctx->tilemapBuildFailures++;
        tilemap_set_status(ctx, "invalid background index");
        return NULL;
    }
    Background *tileset = &dw->bgnd.backgrounds[data->backgroundIndex];
    if (tileset->tpagIndex < 0 || (uint32_t)tileset->tpagIndex >= dw->tpag.count) {
        ctx->tilemapBuildFailures++;
        tilemap_set_status(ctx, "invalid tileset tpag");
        return NULL;
    }
    if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 ||
        tileset->gms2TileColumns == 0) {
        ctx->tilemapBuildFailures++;
        tilemap_set_status(ctx, "invalid gms2 tile geometry");
        return NULL;
    }

    TexturePageItem *tpag = &dw->tpag.items[tileset->tpagIndex];
    int pageW = 0, pageH = 0;
    uint8_t *pagePixels = NULL;
    bool useCachedAtlas = cache_item_available(ctx, (uint32_t)tileset->tpagIndex);
    if (!useCachedAtlas) {
        pagePixels = decode_texture_page_rgba(ctx, tpag->texturePageId, &pageW, &pageH);
        if (!pagePixels || pageW <= 0 || pageH <= 0) {
            ImageDecoder_freeRgba(pagePixels);
            ctx->tilemapBuildFailures++;
            tilemap_set_status(ctx, "failed to decode source texture page");
            return NULL;
        }
    }

    uint32_t roomW = data->tilesX * tileset->gms2TileWidth;
    uint32_t roomH = data->tilesY * tileset->gms2TileHeight;
    if (roomW == 0 || roomH == 0 || roomW > 8192u || roomH > 8192u) {
        ImageDecoder_freeRgba(pagePixels);
        ctx->tilemapBuildFailures++;
        tilemap_set_status(ctx, "invalid tilemap room size");
        return NULL;
    }

    uint32_t slot = ctx->tilemapCacheCount++;
    ctx->tilemapCaches = safeRealloc(ctx->tilemapCaches,
                                     ctx->tilemapCacheCount * sizeof(CtrTilemapLayerCache));
    CtrTilemapLayerCache *cache = &ctx->tilemapCaches[slot];
    memset(cache, 0, sizeof(*cache));
    cache->data = data;
    cache->hash = hash;
    cache->backgroundIndex = data->backgroundIndex;
    cache->tilesX = data->tilesX;
    cache->tilesY = data->tilesY;
    cache->tileW = tileset->gms2TileWidth;
    cache->tileH = tileset->gms2TileHeight;
    cache->chunkSize = 128u;
    cache->chunkCols = (roomW + cache->chunkSize - 1u) / cache->chunkSize;
    cache->chunkRows = (roomH + cache->chunkSize - 1u) / cache->chunkSize;
    cache->chunkCount = cache->chunkCols * cache->chunkRows;
    cache->chunks = calloc(cache->chunkCount, sizeof(CtrTilemapChunk));
    if (!cache->chunks) {
        ImageDecoder_freeRgba(pagePixels);
        free_tilemap_cache(cache);
        ctx->tilemapBuildFailures++;
        tilemap_set_status(ctx, "failed to allocate chunk table");
        return NULL;
    }

    uint32_t validChunks = 0;
    for (uint32_t cy = 0; cy < cache->chunkRows; cy++) {
        for (uint32_t cx = 0; cx < cache->chunkCols; cx++) {
            CtrTilemapChunk *chunk = &cache->chunks[cy * cache->chunkCols + cx];
            chunk->x = (int)(cx * cache->chunkSize);
            chunk->y = (int)(cy * cache->chunkSize);
            chunk->width = (int)fminf((float)(roomW - (uint32_t)chunk->x), (float)cache->chunkSize);
            chunk->height = (int)fminf((float)(roomH - (uint32_t)chunk->y), (float)cache->chunkSize);
            chunk->potW = next_pow2(chunk->width);
            chunk->potH = next_pow2(chunk->height);
            bool chunkOk = useCachedAtlas
                ? compose_tilemap_chunk_from_cache(ctx, cache, chunk, tpag, tileset, 255u)
                : compose_tilemap_chunk(ctx, cache, chunk, pagePixels, pageW, pageH,
                                        tpag, tileset, 255u);
            if (chunkOk) {
                validChunks++;
            }
        }
    }

    ImageDecoder_freeRgba(pagePixels);
    if (validChunks == 0) {
        free_tilemap_cache(cache);
        ctx->tilemapBuildFailures++;
        tilemap_set_status(ctx, "built zero visible chunks");
        return NULL;
    }

    cache->valid = true;
    ctx->tilemapBuildSuccesses++;
    tilemap_set_status(ctx, useCachedAtlas ? "chunk cache ready from atlas"
                                           : "chunk cache ready from source");
    return cache;
}

static CtrTilemapLayerCache *get_tilemap_cache(CtrRenderer *ctx, RoomLayerTilesData *data) {
    uint32_t hash = hash_tile_layer_data(data);
    for (uint32_t i = 0; i < ctx->tilemapCacheCount; i++) {
        CtrTilemapLayerCache *cache = &ctx->tilemapCaches[i];
        if (cache->valid && cache->data == data && cache->hash == hash &&
            cache->backgroundIndex == data->backgroundIndex) {
            return cache;
        }
    }
    return build_tilemap_cache(ctx, data, hash);
}

bool CtrRenderer_drawGms2TileLayer(Renderer *ren, RoomLayerTilesData *data,
                                   float layerOffsetX, float layerOffsetY, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (!ctx || !data || !data->tileData || alpha <= 0.f) return false;
    if (alpha > 1.f) alpha = 1.f;

    CtrTilemapLayerCache *cache = get_tilemap_cache(ctx, data);
    if (!cache || !cache->valid || !cache->chunks) return false;

    float col[4] = {1.f, 1.f, 1.f, alpha};
    bool drew = false;
    ctx->tilemapDrawCalls++;
    for (uint32_t i = 0; i < cache->chunkCount; i++) {
        CtrTilemapChunk *chunk = &cache->chunks[i];
        if (!chunk->valid) continue;
        float x0 = (float)chunk->x + layerOffsetX;
        float y0 = (float)chunk->y + layerOffsetY;
        float x1 = x0 + (float)chunk->width;
        float y1 = y0 + (float)chunk->height;
        if (quad_culled(ctx, x0, y0, x1, y0, x1, y1, x0, y1)) continue;

        push_quad(ctx, &chunk->tex,
                  x0, y0, x1, y0, x1, y1, x0, y1,
                  0.f, 0.f,
                  (float)chunk->width / (float)chunk->potW,
                  (float)chunk->height / (float)chunk->potH,
                  col);
        drew = true;
        ctx->tilemapChunksDrawn++;
    }

    return drew;
}

// Region drawing

static bool cache_item_available(CtrRenderer *ctx, uint32_t id) {
    return ctx->cacheItems &&
           id < ctx->cacheItemCount &&
           ctx->cacheItems[id].valid &&
           ctx->cacheItems[id].segmentStart + ctx->cacheItems[id].segmentCount <= ctx->cacheSegmentCount;
}

static bool draw_cached_region(CtrRenderer *ctx, uint32_t id,
                               float sx, float sy, float sw, float sh,
                               float x0, float y0, float x1, float y1,
                               float x2, float y2, float x3, float y3,
                               const float col[4]) {
    if (!cache_item_available(ctx, id) || sw <= 0 || sh <= 0) return false;

    CtrCachedTpag *entry = &ctx->cacheItems[id];
    float rL = sx;
    float rT = sy;
    float rR = sx + sw;
    float rB = sy + sh;
    bool drew = false;

    for (uint32_t i = 0; i < entry->segmentCount; i++) {
        CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + i];
        float sL = (float)seg->sourceX;
        float sT = (float)seg->sourceY;
        float sR = sL + (float)seg->width;
        float sB = sT + (float)seg->height;
        float dL = fmaxf(rL, sL);
        float dT = fmaxf(rT, sT);
        float dR = fminf(rR, sR);
        float dB = fminf(rB, sB);
        if (dL >= dR || dT >= dB) continue;

        int pageId = (int)(ctx->repackBasePageId + seg->atlasIndex);
        if (!load_source_page_dyn(ctx, pageId)) continue;
        CtrSourcePage *src = get_source_page(ctx, pageId);
        if (!src || !src->loaded) continue;

        float mX = 0.0f;
        float mY = 0.0f;
        float u0 = ((float)seg->atlasX + (dL - sL) + mX) / (float)src->potW;
        float v0 = ((float)seg->atlasY + (dT - sT) + mY) / (float)src->potH;
        float u1 = ((float)seg->atlasX + (dR - sL) - mX) / (float)src->potW;
        float v1 = ((float)seg->atlasY + (dB - sT) - mY) / (float)src->potH;

        float tL = (dL - rL) / sw, tR = (dR - rL) / sw;
        float tT = (dT - rT) / sh, tB = (dB - rT) / sh;

        float topX0 = x0 + (x1 - x0) * tL, topY0 = y0 + (y1 - y0) * tL;
        float topX1 = x0 + (x1 - x0) * tR, topY1 = y0 + (y1 - y0) * tR;
        float botX0 = x3 + (x2 - x3) * tL, botY0 = y3 + (y2 - y3) * tL;
        float botX1 = x3 + (x2 - x3) * tR, botY1 = y3 + (y2 - y3) * tR;

        push_quad(ctx, &src->tex,
                  topX0 + (botX0 - topX0) * tT, topY0 + (botY0 - topY0) * tT,
                  topX1 + (botX1 - topX1) * tT, topY1 + (botY1 - topY1) * tT,
                  topX1 + (botX1 - topX1) * tB, topY1 + (botY1 - topY1) * tB,
                  topX0 + (botX0 - topX0) * tB, topY0 + (botY0 - topY0) * tB,
                  u0, v0, u1, v1, col);
        src->lastFrame = g_frame;
        drew = true;
    }

    return drew;
}

static void draw_region(CtrRenderer *ctx, uint32_t id,
                        float sx, float sy, float sw, float sh,
                        float x0, float y0, float x1, float y1,
                        float x2, float y2, float x3, float y3,
                        const float col[4]) {
    if (id >= ctx->pageCount) return;
    // View-bounds culling. Single choke point: every sprite, tile, glyph, and
    // tilemap cell goes through here. If the destination quad is entirely
    // outside the active view/GUI rect, skip the source-page load + chunk
    // intersection + push_quad entirely.
    if (quad_culled(ctx, x0, y0, x1, y1, x2, y2, x3, y3)) return;
    if (id < ctx->originalTpagCount) {
        draw_cached_region(ctx, id, sx, sy, sw, sh,
                           x0, y0, x1, y1, x2, y2, x3, y3, col);
        return;
    }
    TexturePageItem *item = &ctx->base.dataWin->tpag.items[id];

    if (is_repacked_page(ctx, item->texturePageId)) {
        if (!load_source_page_dyn(ctx, item->texturePageId)) return;
        CtrSourcePage *src = get_source_page(ctx, item->texturePageId);
        if (!src || !src->loaded || sw <= 0 || sh <= 0) return;

        float rL = sx;
        float rT = sy;
        float rR = sx + sw;
        float rB = sy + sh;
        float dL = fmaxf(rL, 0.f);
        float dT = fmaxf(rT, 0.f);
        float dR = fminf(rR, (float)item->sourceWidth);
        float dB = fminf(rB, (float)item->sourceHeight);
        if (dL >= dR || dT >= dB) return;

        float mX = 0.0f;
        float mY = 0.0f;
        float u0 = ((float)item->sourceX + dL + mX) / (float)src->potW;
        float v0 = ((float)item->sourceY + dT + mY) / (float)src->potH;
        float u1 = ((float)item->sourceX + dR - mX) / (float)src->potW;
        float v1 = ((float)item->sourceY + dB - mY) / (float)src->potH;

        float tL = (dL - rL) / sw, tR = (dR - rL) / sw;
        float tT = (dT - rT) / sh, tB = (dB - rT) / sh;

        float topX0 = x0 + (x1 - x0) * tL, topY0 = y0 + (y1 - y0) * tL;
        float topX1 = x0 + (x1 - x0) * tR, topY1 = y0 + (y1 - y0) * tR;
        float botX0 = x3 + (x2 - x3) * tL, botY0 = y3 + (y2 - y3) * tL;
        float botX1 = x3 + (x2 - x3) * tR, botY1 = y3 + (y2 - y3) * tR;

        push_quad(ctx, &src->tex,
                  topX0 + (botX0 - topX0) * tT, topY0 + (botY0 - topY0) * tT,
                  topX1 + (botX1 - topX1) * tT, topY1 + (botY1 - topY1) * tT,
                  topX1 + (botX1 - topX1) * tB, topY1 + (botY1 - topY1) * tB,
                  topX0 + (botX0 - topX0) * tB, topY0 + (botY0 - topY0) * tB,
                  u0, v0, u1, v1, col);
        src->lastFrame = g_frame;
        return;
    }

    if (!ctx->pages[id].loaded) load_page_dyn(ctx, ctx->base.dataWin, (int32_t)id);
    if (!ctx->pages[id].loaded || sw <= 0 || sh <= 0) return;

    CtrPage *page = &ctx->pages[id];
    page->lastFrame = g_frame;

    float rL = sx, rT = sy, rR = sx + sw, rB = sy + sh;

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            CtrAtlasChunk *c = &page->chunks[cx][cy];
            if (!c->valid) continue;

            float dL = fmaxf(rL, (float)c->srcX);
            float dT = fmaxf(rT, (float)c->srcY);
            float dR = fminf(rR, (float)(c->srcX + c->width));
            float dB = fminf(rB, (float)(c->srcY + c->height));
            if (dL >= dR || dT >= dB) continue;

            float mX = 0.0f;
            float mY = 0.0f;

            float u0 = (dL - c->srcX + mX) / (float)c->potW;
            float v0 = (dT - c->srcY + mY) / (float)c->potH;
            float u1 = (dR - c->srcX - mX) / (float)c->potW;
            float v1 = (dB - c->srcY - mY) / (float)c->potH;

            float tL = (dL - rL) / sw, tR = (dR - rL) / sw;
            float tT = (dT - rT) / sh, tB = (dB - rT) / sh;

            float topX0 = x0 + (x1 - x0) * tL, topY0 = y0 + (y1 - y0) * tL;
            float topX1 = x0 + (x1 - x0) * tR, topY1 = y0 + (y1 - y0) * tR;
            float botX0 = x3 + (x2 - x3) * tL, botY0 = y3 + (y2 - y3) * tL;
            float botX1 = x3 + (x2 - x3) * tR, botY1 = y3 + (y2 - y3) * tR;

            push_quad(ctx, &c->tex,
                      topX0 + (botX0 - topX0) * tT, topY0 + (botY0 - topY0) * tT,
                      topX1 + (botX1 - topX1) * tT, topY1 + (botY1 - topY1) * tT,
                      topX1 + (botX1 - topX1) * tB, topY1 + (botY1 - topY1) * tB,
                      topX0 + (botX0 - topX0) * tB, topY0 + (botY0 - topY0) * tB,
                      u0, v0, u1, v1, col);
        }
    }
}

// Sprites

static void ctr_draw_sprite(Renderer *ren, int32_t id, float x, float y,
                            float ox, float oy, float sx, float sy, float ang,
                            uint32_t color, float a) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (id < 0 || (uint32_t)id >= ctx->pageCount) return;
    float c[4]; col2fv(color, a, c);
    if (c[3] <= 0.f) return;

    TexturePageItem *item = &ren->dataWin->tpag.items[id];
    float l0 = (item->targetX - ox) * sx;
    float t0 = (item->targetY - oy) * sy;
    float l1 = l0 + item->sourceWidth  * sx;
    float t1 = t0 + item->sourceHeight * sy;

    if (ang == 0.f) {
        draw_region(ctx, (uint32_t)id, 0, 0, item->sourceWidth, item->sourceHeight,
                    x + l0, y + t0,  x + l1, y + t0,
                    x + l1, y + t1,  x + l0, y + t1,  c);
    } else {
        float rad = -ang * (float)(M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        draw_region(ctx, (uint32_t)id, 0, 0, item->sourceWidth, item->sourceHeight,
                    l0 * cs - t0 * sn + x, l0 * sn + t0 * cs + y,
                    l1 * cs - t0 * sn + x, l1 * sn + t0 * cs + y,
                    l1 * cs - t1 * sn + x, l1 * sn + t1 * cs + y,
                    l0 * cs - t1 * sn + x, l0 * sn + t1 * cs + y,  c);
    }
}

static void ctr_draw_sprite_part(Renderer *ren, int32_t id,
                                 int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                                 float x, float y, float xscale, float yscale,
                                 float angleDeg, float pivotX, float pivotY,
                                 uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    float c[4]; col2fv(color, alpha, c);
    if (c[3] <= 0.f) return;

    float l0 = -pivotX * xscale;
    float t0 = -pivotY * yscale;
    float l1 = l0 + sw * xscale;
    float t1 = t0 + sh * yscale;

    if (angleDeg == 0.f) {
        draw_region(ctx, (uint32_t)id, sx, sy, sw, sh,
                    x + l0, y + t0,
                    x + l1, y + t0,
                    x + l1, y + t1,
                    x + l0, y + t1, c);
    } else {
        float rad = -angleDeg * (float)(M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        draw_region(ctx, (uint32_t)id, sx, sy, sw, sh,
                    l0 * cs - t0 * sn + x, l0 * sn + t0 * cs + y,
                    l1 * cs - t0 * sn + x, l1 * sn + t0 * cs + y,
                    l1 * cs - t1 * sn + x, l1 * sn + t1 * cs + y,
                    l0 * cs - t1 * sn + x, l0 * sn + t1 * cs + y, c);
    }
}

static void ctr_draw_sprite_pos(Renderer *ren, int32_t id,
                                float x1, float y1, float x2, float y2,
                                float x3, float y3, float x4, float y4, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    float c[4]; col2fv(ren->drawColor, alpha, c);
    if (c[3] <= 0.f) return;
    if (id < 0 || (uint32_t)id >= ren->dataWin->tpag.count) return;
    if (ren->dataWin->tpag.items == nullptr) return;
    TexturePageItem *item = &ren->dataWin->tpag.items[id];
    draw_region(ctx, (uint32_t)id, 0, 0, item->sourceWidth, item->sourceHeight,
                x1, y1, x2, y2, x3, y3, x4, y4, c);
}

// Tiles

static void ctr_draw_tile(Renderer *ren, RoomTile *tile, float ox, float oy) {
    CtrRenderer *ctx = (CtrRenderer *)ren;

    // Early cull: skip the TPAG resolve, clipping math, and the draw_region call
    // chain entirely if the tile is off-camera. Big rooms (Undyne bridge, hotland)
    // emit thousands of tiles per frame and only a few hundred fit on screen.
    {
        float dxA = (float)tile->x + ox;
        float dyA = (float)tile->y + oy;
        float dxB = dxA + (float)tile->width  * tile->scaleX;
        float dyB = dyA + (float)tile->height * tile->scaleY;
        if (quad_culled(ctx, dxA, dyA, dxB, dyA, dxB, dyB, dxA, dyB)) return;
    }

    int32_t id = Renderer_resolveObjectTPAGIndex(ren->dataWin, tile);
    if (id < 0 || (uint32_t)id >= ren->dataWin->tpag.count) return;
    if (ren->dataWin->tpag.items == nullptr) return;

    TexturePageItem *tpag = &ren->dataWin->tpag.items[id];
    int sx = tile->sourceX, sy = tile->sourceY;
    int sw = (int)tile->width, sh = (int)tile->height;
    float dx = tile->x + ox, dy = tile->y + oy;

    if (tpag->targetX > sx) { dx += (tpag->targetX - sx) * tile->scaleX; sw -= tpag->targetX - sx; sx = tpag->targetX; }
    if (tpag->targetY > sy) { dy += (tpag->targetY - sy) * tile->scaleY; sh -= tpag->targetY - sy; sy = tpag->targetY; }

    int cR = tpag->targetX + tpag->sourceWidth;
    int cB = tpag->targetY + tpag->sourceHeight;
    if (sx + sw > cR) sw = cR - sx;
    if (sy + sh > cB) sh = cB - sy;
    if (sw <= 0 || sh <= 0) return;

    uint8_t a = (tile->color >> 24) & 0xFF;
    ren->vtable->drawSpritePart(ren, id, sx - tpag->targetX, sy - tpag->targetY, sw, sh,
                                dx, dy, tile->scaleX, tile->scaleY,
                                0.f, 0.f, 0.f,
                                tile->color & 0xFFFFFF, a == 0 ? 1.f : a / 255.f);
}

static void ctr_draw_tiled(Renderer *ren, int32_t id, float ox, float oy,
                           float x, float y, float sx, float sy,
                           bool tx, bool ty, float rw, float rh,
                           uint32_t col, float a) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (id < 0 || (uint32_t)id >= ren->dataWin->tpag.count) return;
    TexturePageItem *t = &ren->dataWin->tpag.items[id];
    float tw = t->boundingWidth  * fabsf(sx);
    float th = t->boundingHeight * fabsf(sy);
    if (tw <= 0.f || th <= 0.f) return;

    float sX = tx ? fmodf(x - ox * fabsf(sx), tw) : x - ox * fabsf(sx);
    if (sX > 0.f && tx) sX -= tw;
    float sY = ty ? fmodf(y - oy * fabsf(sy), th) : y - oy * fabsf(sy);
    if (sY > 0.f && ty) sY -= th;

    float eX = tx ? rw : sX + tw;
    float eY = ty ? rh : sY + th;

    // Clamp the iteration window to the active cull rect so tiled-room-sized
    // backgrounds (Hotland mountain BGs etc.) don't iterate the entire room.
    // Per-quad cull in draw_region still catches partial-overlap edge cases.
    if (ctx->cullEnabled) {
        float minX = ctx->cullL - tw;
        if (sX < minX) {
            float skip = floorf((minX - sX) / tw);
            if (skip > 0.f) sX += skip * tw;
        }
        if (eX > ctx->cullR) eX = ctx->cullR;

        float minY = ctx->cullT - th;
        if (sY < minY) {
            float skip = floorf((minY - sY) / th);
            if (skip > 0.f) sY += skip * th;
        }
        if (eY > ctx->cullB) eY = ctx->cullB;
    }

    for (float dy = sY; dy < eY; dy += th) {
        for (float dx = sX; dx < eX; dx += tw) {
            ren->vtable->drawSprite(ren, id, dx + ox * fabsf(sx), dy + oy * fabsf(sy),
                                    ox, oy, sx, sy, 0.f, col, a);
        }
    }
}

static void ctr_draw_tiled_part(Renderer *ren, int32_t id,
                                int32_t srcX, int32_t srcY,
                                int32_t srcW, int32_t srcH,
                                float dstX, float dstY,
                                float dstW, float dstH,
                                uint32_t col, float a) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (id < 0 || (uint32_t)id >= ren->dataWin->tpag.count) return;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0.f || dstH <= 0.f) return;
    if (!isfinite(dstX) || !isfinite(dstY) || !isfinite(dstW) ||
        !isfinite(dstH) || !isfinite(a)) {
        return;
    }

    float c[4];
    col2fv(col, a, c);
    if (c[3] <= 0.f) return;

    float endX = dstX + dstW;
    float endY = dstY + dstH;
    float startX = dstX;
    float startY = dstY;

    if (ctx->cullEnabled) {
        if (endX <= ctx->cullL || startX >= ctx->cullR ||
            endY <= ctx->cullT || startY >= ctx->cullB) {
            return;
        }
        if (startX < ctx->cullL) {
            float skip = floorf((ctx->cullL - startX) / (float)srcW);
            if (skip > 0.f) startX += skip * (float)srcW;
        }
        if (startY < ctx->cullT) {
            float skip = floorf((ctx->cullT - startY) / (float)srcH);
            if (skip > 0.f) startY += skip * (float)srcH;
        }
    }

    for (float y = startY; y < endY; y += (float)srcH) {
        float h = (float)srcH;
        if (y + h > endY) h = endY - y;
        if (h <= 0.f) continue;

        for (float x = startX; x < endX; x += (float)srcW) {
            float w = (float)srcW;
            if (x + w > endX) w = endX - x;
            if (w <= 0.f) continue;

            draw_region(ctx, (uint32_t)id,
                        (float)srcX, (float)srcY, w, h,
                        x, y, x + w, y, x + w, y + h, x, y + h, c);
        }
    }
}

// Shapes

static void ctr_draw_rect_c(Renderer *ren, float x1, float y1, float x2, float y2,
                            uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                            float a, bool out) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (a <= 0.f) return;
    float l = roundf(fminf(x1, x2)), r = roundf(fmaxf(x1, x2));
    float t = roundf(fminf(y1, y2)), b = roundf(fmaxf(y1, y2));

    float r1[4], r2[4], r3[4], r4[4];
    col2fv(c1, a, r1);  col2fv(c2, a, r2);  col2fv(c3, a, r3);  col2fv(c4, a, r4);

    if (out) {
        float xs[4]  = {l, r + 1, r + 1, l};
        float ys[4]  = {t, t, t + 2, t + 2};
        float cs[4][4] = { {r1[0],r1[1],r1[2],a}, {r2[0],r2[1],r2[2],a},
                           {r2[0],r2[1],r2[2],a}, {r1[0],r1[1],r1[2],a} };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys, .5f, .5f, .5f, .5f, cs);

        float ys2[4] = {b - 1, b - 1, b + 1, b + 1};
        float cs2[4][4] = { {r4[0],r4[1],r4[2],a}, {r3[0],r3[1],r3[2],a},
                            {r3[0],r3[1],r3[2],a}, {r4[0],r4[1],r4[2],a} };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys2, .5f, .5f, .5f, .5f, cs2);

        float xsL[4] = {l, l + 2, l + 2, l};
        float ysM[4] = {t + 2, t + 2, b - 1, b - 1};
        float csL[4][4] = { {r1[0],r1[1],r1[2],a}, {r1[0],r1[1],r1[2],a},
                            {r4[0],r4[1],r4[2],a}, {r4[0],r4[1],r4[2],a} };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xsL, ysM, .5f, .5f, .5f, .5f, csL);

        float xsR[4] = {r - 1, r + 1, r + 1, r - 1};
        float csR[4][4] = { {r2[0],r2[1],r2[2],a}, {r2[0],r2[1],r2[2],a},
                            {r3[0],r3[1],r3[2],a}, {r3[0],r3[1],r3[2],a} };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xsR, ysM, .5f, .5f, .5f, .5f, csR);
    } else {
        float xs[4] = {l, r + 1, r + 1, l};
        float ys[4] = {t, t, b + 1, b + 1};
        float cs[4][4] = { {r1[0],r1[1],r1[2],a}, {r2[0],r2[1],r2[2],a},
                           {r3[0],r3[1],r3[2],a}, {r4[0],r4[1],r4[2],a} };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys, .5f, .5f, .5f, .5f, cs);
    }
}

static void ctr_draw_rect(Renderer *ren, float x1, float y1, float x2, float y2,
                          uint32_t col, float a, bool out) {
    ctr_draw_rect_c(ren, x1, y1, x2, y2, col, col, col, col, a, out);
}

static void ctr_draw_line_c(Renderer *ren, float x1, float y1, float x2, float y2,
                            float w, uint32_t c1, uint32_t c2, float a) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    float r1[4], r2[4]; col2fv(c1, a, r1); col2fv(c2, a, r2);
    if (r1[3] <= 0.f) return;

    w = fmaxf(2.f, w);
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);

    if (len < 0.01f) {
        ctr_draw_rect_c(ren, x1, y1, x2, y2, c1, c2, c2, c1, a, false);
        return;
    }

    x2 += (dx / len) * .5f;  y2 += (dy / len) * .5f;
    x1 -= (dx / len) * .5f;  y1 -= (dy / len) * .5f;
    dx = x2 - x1; dy = y2 - y1; len = sqrtf(dx * dx + dy * dy);

    float px = (-dy / len) * (w * .5f), py = (dx / len) * (w * .5f);
    float xs[4] = {x1 + px, x1 - px, x2 - px, x2 + px};
    float ys[4] = {y1 + py, y1 - py, y2 - py, y2 + py};
    float cs[4][4] = { {r1[0],r1[1],r1[2],r1[3]}, {r1[0],r1[1],r1[2],r1[3]},
                       {r2[0],r2[1],r2[2],r2[3]}, {r2[0],r2[1],r2[2],r2[3]} };
    push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys, .5f, .5f, .5f, .5f, cs);
}

static void ctr_draw_line(Renderer *ren, float x1, float y1, float x2, float y2,
                          float w, uint32_t c, float a) {
    ctr_draw_line_c(ren, x1, y1, x2, y2, w, c, c, a);
}

static void ctr_draw_tri_c(Renderer *ren, float x1, float y1, float x2, float y2,
                           float x3, float y3, uint32_t c1, uint32_t c2, uint32_t c3,
                           float a, bool out) {
    if (out) {
        ctr_draw_line_c(ren, x1, y1, x2, y2, 1.f, c1, c2, a);
        ctr_draw_line_c(ren, x2, y2, x3, y3, 1.f, c2, c3, a);
        ctr_draw_line_c(ren, x3, y3, x1, y1, 1.f, c3, c1, a);
        return;
    }
    float r1[4], r2[4], r3[4];
    col2fv(c1, a, r1);  col2fv(c2, a, r2);  col2fv(c3, a, r3);
    push_solid_tri((CtrRenderer *)ren, x1, y1, x2, y2, x3, y3, r1, r2, r3);
}

static void ctr_draw_tri(Renderer *ren, float x1, float y1, float x2, float y2,
                         float x3, float y3, bool out) {
    ctr_draw_tri_c(ren, x1, y1, x2, y2, x3, y3,
                   ren->drawColor, ren->drawColor, ren->drawColor, ren->drawAlpha, out);
}

static void ctr_draw_ellipse(Renderer *ren, float cx, float cy, float rx, float ry,
                             uint32_t c, float a, bool out, int32_t prec) {
    (void)prec;
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx <= 2.5f && ry <= 2.5f) {
        ctr_draw_rect(ren, cx - rx, cy - ry, cx + rx, cy + ry, c, a, out);
        return;
    }
    int segs = (int)fmaxf(8.f, fminf(32.f, sqrtf(fmaxf(rx, ry)) * 3.5f));
    float step = (2.f * (float)M_PI) / segs;
    float rgb[4]; col2fv(c, a, rgb);
    float px = cx + rx, py = cy;
    for (int i = 1; i <= segs; i++) {
        float nx = cx + cosf(step * i) * rx;
        float ny = cy + sinf(step * i) * ry;
        if (out) ctr_draw_line(ren, px, py, nx, ny, 1.f, c, a);
        else     push_solid_tri((CtrRenderer *)ren, cx, cy, px, py, nx, ny, rgb, rgb, rgb);
        px = nx; py = ny;
    }
}

static void ctr_draw_circle(Renderer *ren, float x, float y, float r,
                            uint32_t c, float a, bool out, int32_t prec) {
    ctr_draw_ellipse(ren, x, y, r, r, c, a, out, prec);
}

static void get_arc(float *px, float *py, int *cnt,
                    float cx, float cy, float rx, float ry,
                    float a1, float a2, int seg, bool skip) {
    for (int i = 0; i <= seg; i++) {
        if (skip && !i) continue;
        if (*cnt >= MAX_RR_POINTS) break;
        float ang = a1 + (a2 - a1) * (seg ? (float)i / seg : 0);
        px[*cnt]   = cx + cosf(ang) * rx;
        py[(*cnt)] = cy + sinf(ang) * ry;
        (*cnt)++;
    }
}

static void ctr_draw_rr(Renderer *ren, float x1, float y1, float x2, float y2,
                        float rx, float ry, uint32_t c, float a, bool out, int32_t prec) {
    (void)prec;
    float l = fminf(x1, x2), r = fmaxf(x1, x2);
    float t = fminf(y1, y2), b = fmaxf(y1, y2);
    float w = r - l, h = b - t;
    if (w <= 0 || h <= 0 || rx <= 0 || ry <= 0) {
        ctr_draw_rect(ren, l, t, r, b, c, a, out);
        return;
    }
    rx = fminf(fabsf(rx), w * .5f);
    ry = fminf(fabsf(ry), h * .5f);

    int seg = (int)fmaxf(2.f, fminf(8.f, sqrtf(fmaxf(rx, ry)) * 0.8f));
    float px[MAX_RR_POINTS], py[MAX_RR_POINTS];
    int cnt = 0;
    float hp = (float)M_PI * .5f;

    get_arc(px, py, &cnt, r - rx, t + ry, rx, ry, -hp,            0.f,             seg, false);
    get_arc(px, py, &cnt, r - rx, b - ry, rx, ry,  0.f,           hp,              seg, true);
    get_arc(px, py, &cnt, l + rx, b - ry, rx, ry,  hp,            (float)M_PI,     seg, true);
    get_arc(px, py, &cnt, l + rx, t + ry, rx, ry, (float)M_PI,    (float)M_PI + hp, seg, true);

    float rgb[4]; col2fv(c, a, rgb);
    if (out) {
        for (int i = 0; i < cnt; i++)
            ctr_draw_line(ren, px[i], py[i], px[(i + 1) % cnt], py[(i + 1) % cnt], 1.f, c, a);
    } else {
        float ccx = (l + r) * .5f, ccy = (t + b) * .5f;
        for (int i = 0; i < cnt; i++)
            push_solid_tri((CtrRenderer *)ren, ccx, ccy, px[i], py[i],
                           px[(i + 1) % cnt], py[(i + 1) % cnt], rgb, rgb, rgb);
    }
}

// Text

static void ctr_draw_font_glyph(CtrRenderer *ctx, int tpagIndex, FontGlyph *g,
                                Matrix4f *tr, float lx0, float ly0, float rgb[4]) {
    float lx1 = lx0 + g->sourceWidth, ly1 = ly0 + g->sourceHeight;
    float px0, py0, px1, py1, px2, py2, px3, py3;
    Matrix4f_transformPoint(tr, lx0, ly0, &px0, &py0);
    Matrix4f_transformPoint(tr, lx1, ly0, &px1, &py1);
    Matrix4f_transformPoint(tr, lx1, ly1, &px2, &py2);
    Matrix4f_transformPoint(tr, lx0, ly1, &px3, &py3);

    draw_region(ctx, tpagIndex,
                g->sourceX, g->sourceY, g->sourceWidth, g->sourceHeight,
                px0, py0, px1, py1, px2, py2, px3, py3, rgb);
}

static bool ctr_resolve_font_glyph_tpag(CtrRenderer *ctx, DataWin *dw, Font *font,
                                        FontGlyph *glyph, int *outTpagIndex,
                                        float *outYOffset) {
    if (outTpagIndex == NULL || outYOffset == NULL) return false;
    *outYOffset = 0.0f;

    if (!font->isSpriteFont) {
        int tpagIndex = font->tpagIndex;
        if (tpagIndex < 0 || (uint32_t)tpagIndex >= ctx->pageCount) return false;
        *outTpagIndex = tpagIndex;
        return true;
    }

    if (font->spriteIndex < 0 || (uint32_t)font->spriteIndex >= dw->sprt.count)
        return false;

    Sprite *sprite = &dw->sprt.sprites[font->spriteIndex];
    int32_t glyphIndex = (int32_t)(glyph - font->glyphs);
    if (glyphIndex < 0 || (uint32_t)glyphIndex >= sprite->textureCount)
        return false;

    int32_t tpagIndex = sprite->tpagIndices ? sprite->tpagIndices[glyphIndex] : -1;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count ||
        (uint32_t)tpagIndex >= ctx->pageCount || dw->tpag.items == NULL)
        return false;

    TexturePageItem *tpag = &dw->tpag.items[tpagIndex];
    *outTpagIndex = tpagIndex;
    *outYOffset = (float)((int32_t)tpag->targetY - sprite->originY);
    return true;
}

static void ctr_draw_text(Renderer *ren, const char *txt, float x, float y,
                          float sx, float sy, float ang) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;
    int fidx = ren->drawFont;
    if (fidx < 0 || (uint32_t)fidx >= dw->font.count) return;

    Font *font = &dw->font.fonts[fidx];
    if (!font->isSpriteFont) {
        if (font->tpagIndex < 0 || (uint32_t)font->tpagIndex >= ctx->pageCount) return;
        if ((uint32_t)font->tpagIndex < ctx->originalTpagCount) {
            if (!cache_item_available(ctx, (uint32_t)font->tpagIndex)) return;
        } else {
            if (!ctx->pages[font->tpagIndex].loaded) return;
        }
    }

    float rgb[4]; col2fv(ren->drawColor, ren->drawAlpha, rgb);
    if (rgb[3] <= 0.f) return;

    PreprocessedText ptxt = TextUtils_preprocessGmlText(txt);
    if (!ptxt.text[0]) { PreprocessedText_free(ptxt); return; }

    int len = strlen(ptxt.text);
    float stride = TextUtils_lineStride(font);
    int lines = TextUtils_countLines(ptxt.text, len);
    float valOff = ren->drawValign == 1 ? -(lines * stride / 2.f)
                                        : (ren->drawValign == 2 ? -(lines * stride) : 0);

    Matrix4f tr;
    Matrix4f_setTransform2D(&tr, roundf(x), roundf(y),
                             sx * font->scaleX, sy * font->scaleY,
                             -ang * (float)(M_PI / 180.f));

    float cy = valOff;
    int start = 0;
    for (int l = 0; l < lines; l++) {
        int end = start;
        while (end < len && !TextUtils_isNewlineChar(ptxt.text[end])) end++;

        float lw = TextUtils_measureLineWidth(font, ptxt.text + start, end - start);
        float cx = ren->drawHalign == 1 ? -lw / 2.f
                                        : (ren->drawHalign == 2 ? -lw : 0);

        int32_t pos = 0;
        while (pos < end - start) {
            int old = pos;
            uint16_t ch = TextUtils_decodeUtf8(ptxt.text + start, end - start, &pos);
            if (pos == old) { pos++; continue; }

            FontGlyph *exact = TextUtils_findExactGlyph(font, ch);
            FontGlyph *g = exact ? exact : TextUtils_findGlyph(font, ch);
            if (!g) continue;
            if (!g->sourceWidth || !g->sourceHeight) { cx += g->shift; continue; }

            float baseX = cx + g->offset;
            int glyphTpag = -1;
            float glyphYOffset = 0.0f;
            if (!ctr_resolve_font_glyph_tpag(ctx, dw, font, g, &glyphTpag, &glyphYOffset))
                continue;
            ctr_draw_font_glyph(ctx, glyphTpag, g, &tr, baseX, cy + glyphYOffset, rgb);

            uint16_t accentCh = exact ? 0 : TextUtils_latinAccentCodepoint(ch);
            FontGlyph *accent = accentCh ? TextUtils_findGlyph(font, accentCh) : nullptr;
            if (accent && accent->sourceWidth && accent->sourceHeight) {
                int accentTpag = -1;
                float accentYOffset = 0.0f;
                if (!ctr_resolve_font_glyph_tpag(ctx, dw, font, accent, &accentTpag, &accentYOffset)) {
                    cx += g->shift;
                    continue;
                }
                float accentX = baseX + (g->sourceWidth - accent->sourceWidth) * 0.5f;
                float accentY = (accentCh == ',')
                                    ? (cy + g->sourceHeight - accent->sourceHeight * 0.25f)
                                    : (cy - accent->sourceHeight + 2.f);
                ctr_draw_font_glyph(ctx, accentTpag, accent, &tr, accentX, accentY + accentYOffset, rgb);
            }
            cx += g->shift;
        }
        cy += stride;
        start = end < len ? TextUtils_skipNewline(ptxt.text, end, len) : end;
    }
    PreprocessedText_free(ptxt);
}

static void ctr_draw_text_c(Renderer *ren, const char *t, float x, float y,
                            float xs, float ys, float ang,
                            int32_t c1, int32_t c2, int32_t c3, int32_t c4, float a) {
    (void)c1; (void)c2; (void)c3; (void)c4;
    float old = ren->drawAlpha;
    ren->drawAlpha = a;
    ctr_draw_text(ren, t, x, y, xs, ys, ang);
    ren->drawAlpha = old;
}

// Room cache

static void mark_res(CtrRenderer *ctx, int id) {
    if (id < 0 || (uint32_t)id >= ctx->pageCount) return;
    ctx->pages[id].keepResident = true;
    if ((uint32_t)id < ctx->originalTpagCount && cache_item_available(ctx, (uint32_t)id)) {
        CtrCachedTpag *entry = &ctx->cacheItems[id];
        for (uint32_t i = 0; i < entry->segmentCount; i++) {
            CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + i];
            if (seg->atlasIndex < ctx->sourcePageCount)
                ctx->sourcePages[seg->atlasIndex].keepResident = true;
        }
    }
}

static void mark_spr(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id < 0 || (uint32_t)id >= dw->sprt.count) return;
    Sprite *s = &dw->sprt.sprites[id];
    for (uint32_t i = 0; i < s->textureCount; i++) mark_res(ctx, s->tpagIndices[i]);
}

static void mark_bg(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id >= 0 && (uint32_t)id < dw->bgnd.count) mark_res(ctx, dw->bgnd.backgrounds[id].tpagIndex);
}

static uint32_t queue_tpag_prefetch(CtrRenderer *ctx, DataWin *dw, int id) {
    if (!ctx || !dw || id < 0 || (uint32_t)id >= dw->tpag.count) return 0;

    uint32_t queued = 0;
    if ((uint32_t)id < ctx->originalTpagCount && cache_item_available(ctx, (uint32_t)id)) {
        CtrCachedTpag *entry = &ctx->cacheItems[id];
        for (uint32_t i = 0; i < entry->segmentCount; i++) {
            CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + i];
            if (seg->atlasIndex >= ctx->sourcePageCount) continue;
            if (queue_source_page_prefetch(ctx, (int32_t)(ctx->repackBasePageId + seg->atlasIndex)))
                queued++;
        }
        return queued;
    }

    TexturePageItem *item = &dw->tpag.items[id];
    if (is_repacked_page(ctx, item->texturePageId) &&
        queue_source_page_prefetch(ctx, item->texturePageId)) {
        queued++;
    }
    return queued;
}

static uint32_t queue_sprite_prefetch(CtrRenderer *ctx, DataWin *dw, int id) {
    if (!ctx || !dw || id < 0 || (uint32_t)id >= dw->sprt.count) return 0;
    Sprite *s = &dw->sprt.sprites[id];
    uint32_t queued = 0;
    for (uint32_t i = 0; i < s->textureCount; i++) {
        queued += queue_tpag_prefetch(ctx, dw, s->tpagIndices[i]);
        if (ctx->prefetchQueueCount >= CTR_PREFETCH_QUEUE_CAP) break;
    }
    return queued;
}

static uint32_t queue_room_warmup_pages(CtrRenderer *ctx, int32_t roomIndex) {
    if (!ctx || !g_current_cache_dir[0]) return 0;

    char path[320];
    snprintf(path, sizeof(path), "%s/warmup_pages.txt", g_current_cache_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    uint32_t queued = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        int room = -999999;
        int page = -1;
        if (sscanf(line, "room=%d page=%d", &room, &page) != 2 &&
            sscanf(line, "%d %d", &room, &page) != 2) {
            continue;
        }
        if (room != roomIndex && room != -1) continue;

        int pageId = page;
        if (!is_repacked_page(ctx, pageId) && page >= 0 && (uint32_t)page < ctx->sourcePageCount) {
            pageId = (int)(ctx->repackBasePageId + (uint32_t)page);
        }
        if (queue_source_page_prefetch(ctx, pageId)) queued++;
        if (ctx->prefetchQueueCount >= CTR_PREFETCH_QUEUE_CAP) break;
    }
    fclose(f);
    return queued;
}

static bool is_hot_font_name(const char *name) {
    if (!name) return false;
    static const char *hotFonts[] = {
        "fnt_main",
        "fnt_maintext",
        "fnt_comicsans",
        "fnt_papyrus",
        "fnt_ja_main",
        "fnt_ja_maintext",
        "fnt_ja_comicsans",
        "fnt_ja_comicsans_big",
        "fnt_ja_papyrus",
        "fnt_ja_papyrus_btl",
    };
    for (uint32_t i = 0; i < sizeof(hotFonts) / sizeof(hotFonts[0]); i++) {
        if (strcmp(name, hotFonts[i]) == 0) return true;
    }
    return false;
}

static void mark_hot_fonts(CtrRenderer *ctx, DataWin *dw) {
    for (uint32_t i = 0; i < dw->font.count; i++) {
        if (is_hot_font_name(dw->font.fonts[i].name)) {
            mark_res(ctx, dw->font.fonts[i].tpagIndex);
        }
    }
}

static void mark_sprite_by_name(CtrRenderer *ctx, DataWin *dw, const char *spriteName) {
    if (!ctx || !dw || !spriteName) return;
    for (uint32_t i = 0; i < dw->sprt.count; i++) {
        const char *name = dw->sprt.sprites[i].name;
        if (name && strcmp(name, spriteName) == 0) {
            mark_spr(ctx, dw, (int)i);
            return;
        }
    }
}

// Keep only the frame that a fixed draw path actually submits.  Several
// Deltarune teacup assets spread their unused animation frames across many
// source atlas pages; pinning the whole sprite can starve the room cache.
static void mark_sprite_frame_by_name(CtrRenderer *ctx, DataWin *dw,
                                      const char *spriteName, uint32_t frame) {
    if (!ctx || !dw || !spriteName) return;
    for (uint32_t i = 0; i < dw->sprt.count; i++) {
        Sprite *sprite = &dw->sprt.sprites[i];
        if (!sprite->name || strcmp(sprite->name, spriteName) != 0) continue;
        if (frame < sprite->textureCount) mark_res(ctx, sprite->tpagIndices[frame]);
        return;
    }
}

static bool room_contains_object_named(const Room *room, DataWin *dw, const char *objectName) {
    if (!room || !dw || !objectName) return false;
    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int objectIndex = room->gameObjects[i].objectDefinition;
        if (objectIndex < 0 || (uint32_t)objectIndex >= dw->objt.count) continue;
        const char *name = dw->objt.objects[objectIndex].name;
        if (name && strcmp(name, objectName) == 0) return true;
    }
    return false;
}

static void mark_deltarune_critical_sprites(CtrRenderer *ctx, DataWin *dw, const Room *room) {
    static const char *tensionSprites[] = {
        "spr_tensionbar",
        "spr_tensionbar_cutout",
        "spr_tensionmarker",
        "spr_tplogo",
    };
    for (uint32_t i = 0; i < sizeof(tensionSprites) / sizeof(tensionSprites[0]); i++) {
        mark_sprite_by_name(ctx, dw, tensionSprites[i]);
    }
    if (room_contains_object_named(room, dw, "obj_teacup")) {
        // obj_teacup Draw always uses frame 0 for these support pieces.
        mark_sprite_frame_by_name(ctx, dw, "spr_teacup_screw", 0);
        mark_sprite_frame_by_name(ctx, dw, "spr_teacup_screw_end", 0);
        mark_sprite_frame_by_name(ctx, dw, "spr_teacup_base", 0);
        mark_sprite_frame_by_name(ctx, dw, "spr_teacup_platform", 0);
        // The rotating center deliberately selects spin div 22.5 (0..7).
        mark_sprite_by_name(ctx, dw, "spr_teacup_center");
    }
}

static uint32_t queue_sprite_prefetch(CtrRenderer *ctx, DataWin *dw, int id);

static char ascii_tolower_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') return (char)(ch + ('a' - 'A'));
    return ch;
}

static bool ascii_contains_ignore_case(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    for (const char *h = haystack; *h; h++) {
        const char *hs = h;
        const char *ns = needle;
        while (*hs && *ns && ascii_tolower_char(*hs) == ascii_tolower_char(*ns)) {
            hs++;
            ns++;
        }
        if (*ns == '\0') return true;
    }
    return false;
}

static bool ctr_is_deltarune_asset_set(const DataWin *dw) {
    if (!dw) return false;
    return ascii_contains_ignore_case(dw->filePath, "deltarune") ||
           ascii_contains_ignore_case(dw->filePath, "chapter1_windows") ||
           ascii_contains_ignore_case(dw->filePath, "chapter2_windows") ||
           ascii_contains_ignore_case(dw->filePath, "chapter3_windows") ||
           ascii_contains_ignore_case(dw->filePath, "chapter4_windows") ||
           ascii_contains_ignore_case(dw->gen8.name, "deltarune");
}

static bool ctr_is_deltarune_profile(CtrRenderer *ctx, DataWin *dw) {
    if (ctx) {
        if (ctx->base.gameProfile == GAME_PROFILE_DELTARUNE) return true;
        if (ctx->base.gameProfile == GAME_PROFILE_UNDERTALE) return false;
    }
    return ctr_is_deltarune_asset_set(dw);
}

static void mark_tile(CtrRenderer *ctx, DataWin *dw, const RoomTile *tile) {
    if (!tile) return;
    if (tile->useSpriteDefinition) {
        mark_spr(ctx, dw, tile->backgroundDefinition);
    } else {
        mark_bg(ctx, dw, tile->backgroundDefinition);
    }
}

static void mark_obj_and_parents(CtrRenderer *ctx, DataWin *dw, int id) {
    int guard = 0;
    while (id >= 0 && (uint32_t)id < dw->objt.count && guard++ < 64) {
        GameObject *obj = &dw->objt.objects[id];
        mark_spr(ctx, dw, obj->spriteId);
        id = obj->parentId;
    }
}

static uint32_t queue_obj_and_parents(CtrRenderer *ctx, DataWin *dw, int id) {
    uint32_t queued = 0;
    int guard = 0;
    while (id >= 0 && (uint32_t)id < dw->objt.count && guard++ < 64) {
        GameObject *obj = &dw->objt.objects[id];
        queued += queue_sprite_prefetch(ctx, dw, obj->spriteId);
        if (ctx->prefetchQueueCount >= CTR_PREFETCH_QUEUE_CAP) break;
        id = obj->parentId;
    }
    return queued;
}

static void mark_room_layer(CtrRenderer *ctx, DataWin *dw, const RoomLayer *layer) {
    if (!layer || !layer->visible) return;
    switch (layer->type) {
        case RoomLayerType_Assets:
            if (layer->assetsData) {
                for (uint32_t i = 0; i < layer->assetsData->legacyTileCount; i++)
                    mark_tile(ctx, dw, &layer->assetsData->legacyTiles[i]);
                for (uint32_t i = 0; i < layer->assetsData->spriteCount; i++)
                    mark_spr(ctx, dw, layer->assetsData->sprites[i].spriteIndex);
            }
            break;
        case RoomLayerType_Background:
            if (layer->backgroundData && layer->backgroundData->visible)
                mark_spr(ctx, dw, layer->backgroundData->spriteIndex);
            break;
        case RoomLayerType_Tiles:
            if (layer->tilesData)
                mark_bg(ctx, dw, layer->tilesData->backgroundIndex);
            break;
        default:
            break;
    }
}

static uint32_t collect_tpag_source_pages(CtrRenderer *ctx, int tpagIndex, bool *sourceLoadMap) {
    if (!ctx || !sourceLoadMap || tpagIndex < 0 || (uint32_t)tpagIndex >= ctx->pageCount) return 0;

    mark_res(ctx, tpagIndex);
    if ((uint32_t)tpagIndex >= ctx->originalTpagCount ||
        !cache_item_available(ctx, (uint32_t)tpagIndex)) {
        return 0;
    }

    CtrCachedTpag *entry = &ctx->cacheItems[tpagIndex];
    uint32_t newlyMarked = 0;
    for (uint32_t i = 0; i < entry->segmentCount; i++) {
        CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + i];
        if (seg->atlasIndex >= ctx->sourcePageCount) continue;
        if (!sourceLoadMap[seg->atlasIndex]) newlyMarked++;
        sourceLoadMap[seg->atlasIndex] = true;
        ctx->sourcePages[seg->atlasIndex].keepResident = true;
    }
    return newlyMarked;
}

static uint32_t load_deltarune_tilemap_pages_first(CtrRenderer *ctx, DataWin *dw, Room *room) {
    if (!ctx || !dw || !room || ctx->sourcePageCount == 0) return 0;

    bool *sourceLoadMap = calloc(ctx->sourcePageCount, sizeof(bool));
    if (!sourceLoadMap) return 0;

    uint32_t sourceNeedCount = 0;
    uint32_t dynamicLoadCount = 0;

    for (uint32_t i = 0; i < room->layerCount; i++) {
        RoomLayer *layer = &room->layers[i];
        if (!layer->visible || layer->type != RoomLayerType_Tiles || !layer->tilesData) continue;
        int bgIndex = layer->tilesData->backgroundIndex;
        if (bgIndex < 0 || (uint32_t)bgIndex >= dw->bgnd.count) continue;
        int tpagIndex = dw->bgnd.backgrounds[bgIndex].tpagIndex;
        if (tpagIndex < 0 || (uint32_t)tpagIndex >= ctx->pageCount) continue;

        sourceNeedCount += collect_tpag_source_pages(ctx, tpagIndex, sourceLoadMap);
        if ((uint32_t)tpagIndex >= ctx->originalTpagCount) {
            load_page_dyn(ctx, dw, tpagIndex);
            dynamicLoadCount++;
        }
    }

    uint32_t sourceReadyCount = 0;
    ctx->preloadingAtlases = true;
    for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
        if (!sourceLoadMap[i]) continue;
        if (load_source_page_dyn(ctx, (int32_t)(ctx->repackBasePageId + i))) {
            sourceReadyCount++;
        }
    }
    ctx->preloadingAtlases = false;

    CTR_DIAG("CTR cache: Deltarune tilemap priority preload %lu/%lu atlas textures, %lu dynamic; linear free %.2f MB\n",
             (unsigned long)sourceReadyCount,
             (unsigned long)sourceNeedCount,
             (unsigned long)dynamicLoadCount,
             (double)linearSpaceFree() / (1024.0 * 1024.0));

    free(sourceLoadMap);
    return sourceReadyCount;
}

static void load_marked_room_pages(CtrRenderer *ctx, DataWin *dw) {
    bool *sourceLoadMap = NULL;
    if (ctx->sourcePageCount > 0)
        sourceLoadMap = calloc(ctx->sourcePageCount, sizeof(bool));
    uint32_t markedTpagCount = 0;
    uint32_t dynamicLoadCount = 0;

    for (uint32_t i = 0; i < ctx->pageCount; i++) {
        if (!ctx->pages[i].keepResident) continue;
        markedTpagCount++;
        if (i < ctx->originalTpagCount && sourceLoadMap && cache_item_available(ctx, i)) {
            CtrCachedTpag *entry = &ctx->cacheItems[i];
            for (uint32_t s = 0; s < entry->segmentCount; s++) {
                CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + s];
                if (seg->atlasIndex < ctx->sourcePageCount) sourceLoadMap[seg->atlasIndex] = true;
            }
        } else if (i >= ctx->originalTpagCount) {
            load_page_dyn(ctx, dw, (int32_t)i);
            dynamicLoadCount++;
        }
    }

    if (sourceLoadMap) {
        uint32_t sourceNeedCount = 0;
        uint32_t sourceReadyCount = 0;
        uint32_t sourceNewCount = 0;
        ctx->preloadingAtlases = true;
        for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
            if (!sourceLoadMap[i]) continue;
            sourceNeedCount++;
            bool wasLoaded = ctx->sourcePages[i].loaded;
            if (load_source_page_dyn(ctx, (int32_t)(ctx->repackBasePageId + i))) {
                sourceReadyCount++;
                if (!wasLoaded && ctx->sourcePages[i].loaded) sourceNewCount++;
            }
        }
        ctx->preloadingAtlases = false;

        uint32_t sourceResidentCount = 0;
        for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
            if (ctx->sourcePages[i].loaded) sourceResidentCount++;
        }
        double sourceResidentMB =
            0.0;
        for (uint32_t i = 0; i < ctx->sourcePageCount; i++) {
            if (!ctx->sourcePages[i].loaded) continue;
            sourceResidentMB += (double)ctx->sourcePages[i].dataSize / (1024.0 * 1024.0);
        }

        CTR_DIAG("CTR cache: room preload %lu TPAGs -> %lu/%lu atlas textures (%lu new, %lu resident, %.2f MB), %lu dynamic; linear free %.2f MB\n",
                 (unsigned long)markedTpagCount,
                 (unsigned long)sourceReadyCount,
                 (unsigned long)sourceNeedCount,
                 (unsigned long)sourceNewCount,
                 (unsigned long)sourceResidentCount,
                 sourceResidentMB,
                 (unsigned long)dynamicLoadCount,
                 (double)linearSpaceFree() / (1024.0 * 1024.0));
        free(sourceLoadMap);
    }
}

void CtrRenderer_dumpTextureDiagnostics(Renderer *ren, FILE *out, const Room *room) {
    if (!ren || !out) return;
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;

    fprintf(out, "\n[3ds-texture-cache]\n");
    fprintf(out, "profile=%d pageCount=%lu originalTpagCount=%lu sourcePageCount=%lu repackBase=%lu cacheItems=%lu cacheSegments=%lu linearFree=%.2fMB room=%ld/%s lazyLoadsThisRoom=%lu\n",
            (int)ren->gameProfile,
            (unsigned long)ctx->pageCount,
            (unsigned long)ctx->originalTpagCount,
            (unsigned long)ctx->sourcePageCount,
            (unsigned long)ctx->repackBasePageId,
            (unsigned long)ctx->cacheItemCount,
            (unsigned long)ctx->cacheSegmentCount,
            (double)linearSpaceFree() / (1024.0 * 1024.0),
            (long)ctx->currentRoomIndex,
            ctx->currentRoomName,
            (unsigned long)ctx->lazyLoadsThisRoom);
    fprintf(out, "tilemapChunks caches=%lu buildAttempts=%lu buildOk=%lu buildFail=%lu chunksBuilt=%lu opaquePixels=%lu drawCalls=%lu chunksDrawn=%lu status=%s\n",
            (unsigned long)ctx->tilemapCacheCount,
            (unsigned long)ctx->tilemapBuildAttempts,
            (unsigned long)ctx->tilemapBuildSuccesses,
            (unsigned long)ctx->tilemapBuildFailures,
            (unsigned long)ctx->tilemapChunksBuilt,
            (unsigned long)ctx->tilemapOpaquePixels,
            (unsigned long)ctx->tilemapDrawCalls,
            (unsigned long)ctx->tilemapChunksDrawn,
            ctx->tilemapLastStatus[0] ? ctx->tilemapLastStatus : "none");

    if (!dw || !room) return;

    for (uint32_t i = 0; i < room->layerCount; i++) {
        const RoomLayer *layer = &room->layers[i];
        if (layer->type != RoomLayerType_Tiles || !layer->tilesData) continue;

        int bgIndex = layer->tilesData->backgroundIndex;
        int tpagIndex = -1;
        const char *bgName = "<invalid>";
        if (bgIndex >= 0 && (uint32_t)bgIndex < dw->bgnd.count) {
            const Background *bg = &dw->bgnd.backgrounds[bgIndex];
            tpagIndex = bg->tpagIndex;
            bgName = bg->name ? bg->name : "<unnamed>";
        }

        bool cacheValid = (tpagIndex >= 0) && cache_item_available(ctx, (uint32_t)tpagIndex);
        uint32_t segmentCount = cacheValid ? ctx->cacheItems[tpagIndex].segmentCount : 0;
        fprintf(out, "tileLayer[%lu] id=%lu name=%s visible=%d bg=%d/%s tpag=%d cacheValid=%d segments=%lu\n",
                (unsigned long)i,
                (unsigned long)layer->id,
                layer->name ? layer->name : "<unnamed>",
                layer->visible,
                bgIndex,
                bgName,
                tpagIndex,
                cacheValid ? 1 : 0,
                (unsigned long)segmentCount);

        if (!cacheValid) continue;
        CtrCachedTpag *entry = &ctx->cacheItems[tpagIndex];
        for (uint32_t s = 0; s < entry->segmentCount; s++) {
            CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + s];
            CtrSourcePage *src = seg->atlasIndex < ctx->sourcePageCount
                               ? &ctx->sourcePages[seg->atlasIndex]
                               : NULL;
            fprintf(out, "  segment[%lu] atlas=%lu src=(%u,%u %ux%u) atlasPos=(%u,%u) loaded=%d keep=%d failed=%d fmt=%lu bytes=%lu lastFrame=%lu\n",
                    (unsigned long)s,
                    (unsigned long)seg->atlasIndex,
                    seg->sourceX,
                    seg->sourceY,
                    seg->width,
                    seg->height,
                    seg->atlasX,
                    seg->atlasY,
                    src ? src->loaded : 0,
                    src ? src->keepResident : 0,
                    src ? src->loadFailed : 0,
                    src ? (unsigned long)src->format : 0ul,
                    src ? (unsigned long)src->dataSize : 0ul,
                    src ? (unsigned long)src->lastFrame : 0ul);
        }
    }
}

void CtrRenderer_prefetchTexturePage(Renderer *ren, int32_t tpagIdx) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (!ctx || !ren || !ren->dataWin) return;
    queue_tpag_prefetch(ctx, ren->dataWin, tpagIdx);
    if (!ctx->inFrame) process_prefetch_queue(ctx, 1);
}

static void CtrRenderer_prefetchSprites(Renderer *ren, const int32_t *spriteIndices, uint32_t spriteCount) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    for (uint32_t i = 0; i < spriteCount; i++) {
        queue_sprite_prefetch(ctx, ren->dataWin, spriteIndices[i]);
    }
    if (!ctx->inFrame) process_prefetch_queue(ctx, 2);
}

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx) {
    CtrRenderer_prefetchSprites(ren, &sprIdx, 1);
}

static void ctr_on_room(Renderer *ren, int32_t rm) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;
    if (rm < 0 || (uint32_t)rm >= dw->room.count) return;
    Room *room = &dw->room.rooms[rm];
    bool deltaruneProfile = ctr_is_deltarune_profile(ctx, dw);
    bool conservativeObjectPrefetch = deltaruneProfile;

    ctx->prefetchQueueCount = 0;
    ctx->currentRoomIndex = rm;
    ctx->lazyLoadsThisRoom = 0;
    free_all_tilemap_caches(ctx);
    ctx->tilemapBuildAttempts = 0;
    ctx->tilemapBuildSuccesses = 0;
    ctx->tilemapBuildFailures = 0;
    ctx->tilemapChunksBuilt = 0;
    ctx->tilemapOpaquePixels = 0;
    ctx->tilemapDrawCalls = 0;
    ctx->tilemapChunksDrawn = 0;
    tilemap_set_status(ctx, "room changed");
    snprintf(ctx->currentRoomName, sizeof(ctx->currentRoomName), "%s",
             room->name ? room->name : "?");

    for (uint32_t i = 0; i < ctx->originalTpagCount && i < ctx->pageCount; i++)
        ctx->pages[i].keepResident = false;
    for (uint32_t i = 0; i < ctx->sourcePageCount; i++)
        ctx->sourcePages[i].keepResident = false;
    for (uint32_t i = 0; i < ctx->sourcePageCount; i++)
        ctx->sourcePages[i].loadFailed = false;

    if (room->backgrounds) {
        for (int i = 0; i < 8; i++)
            if (room->backgrounds[i].enabled)
                mark_bg(ctx, dw, room->backgrounds[i].backgroundDefinition);
    }
    for (uint32_t i = 0; i < room->tileCount; i++)
        mark_tile(ctx, dw, &room->tiles[i]);
    for (uint32_t i = 0; i < room->layerCount; i++)
        mark_room_layer(ctx, dw, &room->layers[i]);
    if (!conservativeObjectPrefetch) {
        for (uint32_t i = 0; i < room->gameObjectCount; i++) {
            int id = room->gameObjects[i].objectDefinition;
            mark_obj_and_parents(ctx, dw, id);
        }
    }

    mark_hot_fonts(ctx, dw);
    if (deltaruneProfile) {
        mark_deltarune_critical_sprites(ctx, dw, room);
    }

    unload_nonresident_source_pages(ctx);
    uint32_t tilemapPriorityLoaded = deltaruneProfile
        ? load_deltarune_tilemap_pages_first(ctx, dw, room)
        : 0;
    load_marked_room_pages(ctx, dw);
    uint32_t warmupQueued = queue_room_warmup_pages(ctx, rm);
    uint32_t hotQueued = 0;
    if (conservativeObjectPrefetch) {
        for (uint32_t i = 0; i < room->gameObjectCount; i++) {
            int id = room->gameObjects[i].objectDefinition;
            hotQueued += queue_obj_and_parents(ctx, dw, id);
            if (ctx->prefetchQueueCount >= CTR_PREFETCH_QUEUE_CAP) break;
        }
    }
    uint32_t warmupLoaded = process_prefetch_queue(ctx, CTR_PREFETCH_ROOM_BUDGET);
    (void)tilemapPriorityLoaded;
    (void)warmupQueued;
    (void)hotQueued;
    (void)warmupLoaded;
    CTR_DIAG("CTR cache: queued warmup room=%d warmup=%lu hot=%lu loadedNow=%lu pending=%lu\n",
             rm,
             (unsigned long)warmupQueued,
             (unsigned long)hotQueued,
             (unsigned long)warmupLoaded,
             (unsigned long)ctx->prefetchQueueCount);
}

// Surface API

static int32_t ctr_create_surface(Renderer *ren, int32_t width, int32_t height) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (width <= 0 || height <= 0) return -1;
    if (width  > 1024) width  = 1024;
    if (height > 1024) height = 1024;

    uint32_t slot = 0;
    for (; slot < ctx->surfaceCount; slot++) {
        CtrSurface *cand = &ctx->surfaces[slot];
        if (!cand->used) break;
    }
    if (slot == ctx->surfaceCount) {
        ctx->surfaceCount++;
        ctx->surfaces = safeRealloc(ctx->surfaces, ctx->surfaceCount * sizeof(CtrSurface));
        memset(&ctx->surfaces[slot], 0, sizeof(CtrSurface));
    }

    CtrSurface *surf = &ctx->surfaces[slot];

    if (surf->target && surf->width == width && surf->height == height) {
        surf->used = true;
        flush_batch(ctx);
        if (ctx->inFrame) ctr_safe_frame_split(ctx);

        C3D_RenderTarget *oldTgt = ctx->activeTarget;
        C3D_FrameDrawOn(surf->target);
        C3D_RenderTargetClear(surf->target, C3D_CLEAR_ALL, 0x00000000, 0);
        if (oldTgt) {
            C3D_FrameDrawOn(oldTgt);
            // Re-bind shader/buffer state for the restored target so subsequent
            // draws don't use stale BufInfo/AttrInfo from before the split.
            rebind_state(ctx);
            apply_projection(ctx, &ctx->currentProjection);
            emit_blend_state(ctx);
        }

        return (int32_t)slot;
    }

    surface_release_storage(ctx, surf);
    memset(surf, 0, sizeof(*surf));
    if (!surface_alloc_storage(surf, width, height)) {
        memset(surf, 0, sizeof(*surf));
        return -1;
    }
    surf->used = true;

    flush_batch(ctx);
    if (ctx->inFrame) ctr_safe_frame_split(ctx);
    C3D_RenderTarget *oldTgt = ctx->activeTarget;
    C3D_FrameDrawOn(surf->target);
    C3D_RenderTargetClear(surf->target, C3D_CLEAR_ALL, 0x00000000, 0);
    if (oldTgt) {
        C3D_FrameDrawOn(oldTgt);
        rebind_state(ctx);
        apply_projection(ctx, &ctx->currentProjection);
        emit_blend_state(ctx);
    }

    return (int32_t)slot;
}

static void ctr_free_surface(Renderer *ren, int32_t surfaceId) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    CtrSurface *surf = get_surface(ctx, surfaceId);
    if (!surf) return;

    flush_batch(ctx);
    if (ctx->activeTarget == surf->target) {
        bind_target(ctx, ctx->appTarget);
    }
    surf->used = false;
}

static bool ctr_surface_exists(Renderer *ren, int32_t surfaceId) {
    if (surfaceId < 0) return false;
    return get_surface((CtrRenderer *)ren, surfaceId) != NULL;
}

static bool ctr_surface_get_size(Renderer *ren, int32_t surfaceId, int32_t *w, int32_t *h) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (surfaceId == -1) {
        if (w) *w = ctx->gameW;
        if (h) *h = ctx->gameH;
        return true;
    }
    CtrSurface *surf = get_surface(ctx, surfaceId);
    if (!surf) return false;
    if (w) *w = surf->width;
    if (h) *h = surf->height;
    return true;
}

static bool ctr_surface_set_target(Renderer *ren, int32_t surfaceId) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    CtrSurface *surf = get_surface(ctx, surfaceId);
    if (!surf || ctx->targetStackDepth >= CTR_TARGET_STACK_DEPTH) {
        if (ctx->base.gameProfile == GAME_PROFILE_DELTARUNE) {
            ctx->surfaceDrawSuppressed = true;
            ctx->surfaceDrawSuppressedDepth = ctx->targetStackDepth;
        }
        return false;
    }

    flush_batch(ctx);

    CtrTargetState *st = &ctx->targetStack[ctx->targetStackDepth++];
    st->target     = ctx->activeTarget ? ctx->activeTarget : ctx->appTarget;
    st->viewport[0]= ctx->currentViewport[0];
    st->viewport[1]= ctx->currentViewport[1];
    st->viewport[2]= ctx->currentViewport[2];
    st->viewport[3]= ctx->currentViewport[3];
    st->projection = ctx->currentProjection;
    st->cullEnabled = ctx->cullEnabled;
    st->cullL = ctx->cullL;
    st->cullT = ctx->cullT;
    st->cullR = ctx->cullR;
    st->cullB = ctx->cullB;

    bind_target(ctx, surf->target);
    set_viewport_logical(ctx, surf->target, 0, 0, surf->width, surf->height);

    C3D_Mtx proj;
    make_ortho_topleft(&proj, (float)surf->width, (float)surf->height);
    apply_projection(ctx, &proj);
    ctx->cullEnabled = true;
    ctx->cullL = 0.0f;
    ctx->cullT = 0.0f;
    ctx->cullR = (float) surf->width;
    ctx->cullB = (float) surf->height;
    ctx->surfaceDrawSuppressed = false;
    ctx->surfaceDrawSuppressedDepth = 0;
    return true;
}

static void ctr_surface_reset_target(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (ctx->surfaceDrawSuppressed &&
        ctx->targetStackDepth == ctx->surfaceDrawSuppressedDepth) {
        ctx->surfaceDrawSuppressed = false;
        ctx->surfaceDrawSuppressedDepth = 0;
        return;
    }
    if (ctx->targetStackDepth <= 0) return;

    flush_batch(ctx);

    CtrTargetState st = ctx->targetStack[--ctx->targetStackDepth];
    C3D_RenderTarget *tgt = st.target ? st.target : ctx->appTarget;

    bind_target(ctx, tgt);
    set_viewport_logical(ctx, tgt,
                         st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3]);
    apply_projection(ctx, &st.projection);
    ctx->cullEnabled = st.cullEnabled;
    ctx->cullL = st.cullL;
    ctx->cullT = st.cullT;
    ctx->cullR = st.cullR;
    ctx->cullB = st.cullB;
    ctx->surfaceDrawSuppressed = false;
    ctx->surfaceDrawSuppressedDepth = 0;
}

static void ctr_draw_surface(Renderer *ren, int32_t surfaceId,
                             float x, float y, float xscale, float yscale,
                             float angleDeg, uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    if (ctx->surfaceDrawSuppressed) return;
    float c[4]; col2fv(color, alpha, c);
    if (c[3] <= 0.f) return;

    C3D_Tex *tex; int drawW, drawH, potW, potH;
    C3D_RenderTarget *sourceTarget = NULL;
    if (surfaceId == -1) {
        if (!ctx->appReady) return;
        tex   = &ctx->appTex;
        drawW = ctx->appLogicW;
        drawH = ctx->appLogicH;
        potW  = ctx->appPotW;
        potH  = ctx->appPotH;
        sourceTarget = ctx->appTarget;
    } else {
        CtrSurface *surf = get_surface(ctx, surfaceId);
        if (!surf) return;
        tex   = &surf->tex;
        drawW = surf->width;
        drawH = surf->height;
        potW  = surf->potW;
        potH  = surf->potH;
        sourceTarget = surf->target;
    }
    if (sourceTarget && sourceTarget == ctx->activeTarget) return;

    float w = drawW * xscale, h = drawH * yscale;
    float u1 = (float)drawW / (float)potW;
    float v0 = (float)(potH - drawH) / (float)potH;
    float v1 = 1.f;

    float x0 = 0, y0 = 0;
    float x1 = w, y1 = 0;
    float x2 = w, y2 = h;
    float x3 = 0, y3 = h;

    if (angleDeg != 0.f) {
        float rad = -angleDeg * (float)(M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        float pts[4][2] = {{x0, y0}, {x1, y1}, {x2, y2}, {x3, y3}};
        for (int i = 0; i < 4; i++) {
            float qx = pts[i][0], qy = pts[i][1];
            pts[i][0] = qx * cs - qy * sn;
            pts[i][1] = qx * sn + qy * cs;
        }
        x0=pts[0][0]; y0=pts[0][1]; x1=pts[1][0]; y1=pts[1][1];
        x2=pts[2][0]; y2=pts[2][1]; x3=pts[3][0]; y3=pts[3][1];
    }

    if (quad_culled(ctx, x + x0, y + y0, x + x1, y + y1,
                          x + x2, y + y2, x + x3, y + y3)) return;

    flush_batch(ctx);
    // Surface texture must be fully resolved before sampling; FrameSplit forces a
    // GPU sync point. Use the safe variant — without rebind, the queued push_quad
    // is later drawn with stale BufInfo/AttrInfo, which manifests as missing
    // tiles in tile-heavy rooms (e.g. the Undyne bridge).
    ctr_safe_frame_split(ctx);
    push_quad(ctx, tex,
              x + x0, y + y0,  x + x1, y + y1,
              x + x2, y + y2,  x + x3, y + y3,
              0.f, v1, u1, v0, c);
}

static void ctr_clear_target(Renderer *ren, uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    if (!ctx->inFrame || !ctx->activeTarget) return;
    if (ctx->surfaceDrawSuppressed) return;

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color), aa = clamp_u8(alpha);
    uint32_t rgba = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | aa;
    // Use the safe variant: a bare C3D_FrameSplit leaves BufInfo/AttrInfo stale,
    // and the next batched draw on this target then reads garbage vertices.
    ctr_safe_frame_split(ctx);
    C3D_RenderTargetClear(ctx->activeTarget, C3D_CLEAR_ALL, rgba, 0);
}

// Sprite capture

static uint32_t findOrAllocTpagSlot(CtrRenderer *ctx) {
    DataWin *dw = ctx->base.dataWin;
    for (uint32_t i = ctx->originalTpagCount; i < dw->tpag.count; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;

    if (newIndex >= ctx->pageCount) {
        uint32_t old = ctx->pageCount;
        ctx->pageCount = newIndex + 1;
        ctx->pages = safeRealloc(ctx->pages, ctx->pageCount * sizeof(CtrPage));
        memset(&ctx->pages[old], 0, (ctx->pageCount - old) * sizeof(CtrPage));
    }
    return newIndex;
}

static int32_t ctr_create_surf_ex(Renderer *ren, int32_t surfaceId,
                                  int32_t x, int32_t y, int32_t w, int32_t h,
                                  bool removeback, bool smooth, int32_t xo, int32_t yo) {
    (void)removeback;
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;

    if (w <= 0 || h <= 0) return -1;

    C3D_Tex *srcTex;
    int sourcePotW, sourcePotH;

    if (surfaceId == -1) {
        if (!ctx->appReady) return -1;
        srcTex     = &ctx->appTex;
        sourcePotW = ctx->appPotW;
        sourcePotH = ctx->appPotH;
    } else {
        CtrSurface *surf = get_surface(ctx, surfaceId);
        if (!surf) return -1;
        srcTex     = &surf->tex;
        sourcePotW = surf->potW;
        sourcePotH = surf->potH;
    }

    C3D_RenderTarget *oldTgt = ctx->inFrame
                                   ? (ctx->activeTarget ? ctx->activeTarget : ctx->appTarget)
                                   : ctx->activeTarget;
    C3D_Mtx oldProj = ctx->currentProjection;
    int oldVp[4] = { ctx->currentViewport[0], ctx->currentViewport[1], ctx->currentViewport[2], ctx->currentViewport[3] };
    int oldBlend = ctx->currentBlendMode;

    int potW = next_pow2(w), potH = next_pow2(h);
    CtrAtlasChunk dstChunk;
    memset(&dstChunk, 0, sizeof(dstChunk));

    if (!C3D_TexInitVRAM(&dstChunk.tex, (u16)potW, (u16)potH, GPU_RGBA8)) {
        if (!C3D_TexInit(&dstChunk.tex, (u16)potW, (u16)potH, GPU_RGBA8)) return -1;
    }
    C3D_TexSetFilter(&dstChunk.tex, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&dstChunk.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    C3D_RenderTarget *tmpTarget = C3D_RenderTargetCreateFromTex(&dstChunk.tex, GPU_TEXFACE_2D, 0, -1);
    if (tmpTarget) {
        bool ownFrame = false;
        if (!ctx->inFrame) {
            if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
                C3D_RenderTargetDelete(tmpTarget);
                C3D_TexDelete(&dstChunk.tex);
                return -1;
            }
            ownFrame = true;
            ctx->inFrame = true;
            ctx->activeTarget = NULL;
            ctx->vbufHead   = 0;
            ctx->batchStart = 0;
            ctx->batchVerts = 0;
            ctx->batchTex   = NULL;
        }

        bind_target(ctx, tmpTarget);
        C3D_RenderTargetClear(tmpTarget, C3D_CLEAR_ALL, 0x00000000, 0);

        flush_batch(ctx);
        C3D_SetViewport(0, 0, w, h);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

        C3D_Mtx proj;
        Mtx_Identity(&proj);
        Mtx_Ortho(&proj, 0.f, (float)w, (float)h, 0.f, -1.f, 1.f, true);
        apply_projection(ctx, &proj);

        float u0 = (float)x / (float)sourcePotW;
        float u1 = (float)(x + w) / (float)sourcePotW;
        float vTop = (float)(sourcePotH - y) / (float)sourcePotH;
        float vBot = (float)(sourcePotH - (y + h)) / (float)sourcePotH;

        float white[4] = {1.f, 1.f, 1.f, 1.f};

        push_quad(ctx, srcTex,
                  0, 0,  w, 0,
                  w, h,  0, h,
                  u0, vBot, u1, vTop, white);

        flush_batch(ctx);
        if (ownFrame) {
            C3D_FrameEnd(0);
            ctx->inFrame = false;
            ctx->activeTarget = NULL;
            C3D_RenderTargetDelete(tmpTarget);
        } else {
            C3D_FrameSplit(0);

            gc_add_target(tmpTarget);

            bind_target(ctx, oldTgt);
            set_viewport_logical(ctx, oldTgt, oldVp[0], oldVp[1], oldVp[2], oldVp[3]);
            apply_projection(ctx, &oldProj);
            apply_blend(ctx, oldBlend);
        }
    } else {
        C3D_TexDelete(&dstChunk.tex);
        return -1;
    }

    dstChunk.valid  = true;
    dstChunk.srcX   = 0;
    dstChunk.srcY   = 0;
    dstChunk.width  = w;
    dstChunk.height = h;
    dstChunk.potW   = potW;
    dstChunk.potH   = potH;

    uint32_t tpagIndex = findOrAllocTpagSlot(ctx);
    TexturePageItem *tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0; tpag->sourceY = 0;
    tpag->sourceWidth   = (uint16_t)w; tpag->sourceHeight   = (uint16_t)h;
    tpag->targetX = 0; tpag->targetY = 0;
    tpag->targetWidth   = (uint16_t)w; tpag->targetHeight   = (uint16_t)h;
    tpag->boundingWidth = (uint16_t)w; tpag->boundingHeight = (uint16_t)h;
    tpag->texturePageId = (int16_t)tpagIndex;

    CtrPage *page = &ctx->pages[tpagIndex];
    memset(page, 0, sizeof(*page));
    page->loaded = true;
    page->keepResident = true;
    page->origW = w; page->origH = h;
    page->chunksX = 1; page->chunksY = 1;
    page->chunks[0][0] = dstChunk;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, ctx->originalSpriteCount);
    Sprite *sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t)w; sprite->height = (uint32_t)h;
    sprite->originX = xo; sprite->originY = yo;
    sprite->textureCount = 1;
    sprite->tpagIndices = safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t)tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = NULL;

    return (int32_t)spriteIndex;
}

static int32_t ctr_create_surf(Renderer *ren, int32_t x, int32_t y, int32_t w, int32_t h,
                               bool rb, bool sm, int32_t xo, int32_t yo) {
    return ctr_create_surf_ex(ren, -1, x, y, w, h, rb, sm, xo, yo);
}

static void ctr_del_sprite(Renderer *ren, int32_t spriteIndex) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;
    if (spriteIndex < 0 || (uint32_t)spriteIndex >= dw->sprt.count) return;
    if ((uint32_t)spriteIndex < ctx->originalSpriteCount) return;

    Sprite *sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    flush_batch(ctx);

    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx < 0 || (uint32_t)tpagIdx < ctx->originalTpagCount) continue;

        TexturePageItem *tpag = &dw->tpag.items[tpagIdx];
        if ((uint32_t)tpagIdx < ctx->pageCount) {
            CtrPage *page = &ctx->pages[tpagIdx];
            if (page->loaded) {
                for (int cx = 0; cx < page->chunksX; cx++) {
                    for (int cy = 0; cy < page->chunksY; cy++) {
                        CtrAtlasChunk *ch = &page->chunks[cx][cy];
                        if (ch->valid) C3D_TexDelete(&ch->tex);
                    }
                }
            }
            memset(page, 0, sizeof(*page));
        }
        tpag->texturePageId = -1;
    }

    free(sprite->tpagIndices);
    const char *keep = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keep;
}

// Misc

static void ctr_gpu_blend_mode(Renderer *ren, int32_t mode) {
    apply_blend((CtrRenderer *)ren, mode);
}

static void ctr_gpu_blend_mode_ext(Renderer *ren, int32_t sfactor, int32_t dfactor) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    ctx->currentBlendMode = bm_complex;
    ctx->blendEnabled = true;
    ctx->blendSrcColor = gm_blend_factor_to_gpu(sfactor);
    ctx->blendDstColor = gm_blend_factor_to_gpu(dfactor);
    ctx->blendSrcAlpha = ctx->blendSrcColor;
    ctx->blendDstAlpha = ctx->blendDstColor;
    emit_blend_state(ctx);
}

static void ctr_gpu_blend_enable(Renderer *ren, bool enable) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    ctx->blendEnabled = enable;
    emit_blend_state(ctx);
}

static void ctr_gpu_alpha_test_enable(Renderer *ren, bool enable) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    ctx->alphaTestEnabled = enable;
    apply_alpha_test_state(ctx);
}

static void ctr_gpu_alpha_test_ref(Renderer *ren, uint8_t ref) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    ctx->alphaTestRef = ref;
    apply_alpha_test_state(ctx);
}

static void ctr_gpu_color_write_enable(Renderer *ren, bool r, bool g, bool b, bool a) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    GPU_WRITEMASK mask = GPU_WRITE_DEPTH;
    if (r) mask = (GPU_WRITEMASK)(mask | GPU_WRITE_RED);
    if (g) mask = (GPU_WRITEMASK)(mask | GPU_WRITE_GREEN);
    if (b) mask = (GPU_WRITEMASK)(mask | GPU_WRITE_BLUE);
    if (a) mask = (GPU_WRITEMASK)(mask | GPU_WRITE_ALPHA);
    flush_batch(ctx);
    ctx->writeMask = mask;
    apply_depth_write_mask(ctx);
}

static void ctr_set_blend(Renderer *ren, int32_t mode) {
    apply_blend((CtrRenderer *)ren, mode);
}

static void ctr_set_3d_depth(Renderer *ren, float depth) {
    (void)ren; (void)depth;
}

// Renderer vtable

static RendererVtable vtable = {
    .init = ctr_init,                            .destroy = ctr_destroy,
    .beginFrame = ctr_begin_frame,               .endFrame = ctr_end_frame,
    .beginView = ctr_begin_view,                 .endView = ctr_end_view,
    .beginGUI = ctr_begin_gui,                   .endGUI = ctr_end_gui,
    .drawSprite = ctr_draw_sprite,
    .drawSpritePart = ctr_draw_sprite_part,
    .drawSpritePos = ctr_draw_sprite_pos,
    .drawRectangle = ctr_draw_rect,
    .drawRectangleColor = ctr_draw_rect_c,
    .drawLine = ctr_draw_line,                   .drawLineColor = ctr_draw_line_c,
    .drawTriangle = ctr_draw_tri,                .drawTriangleColor = ctr_draw_tri_c,
    .drawText = ctr_draw_text,                   .drawTextColor = ctr_draw_text_c,
    .flush = ctr_flush,
    .prefetchTexturePage = CtrRenderer_prefetchTexturePage,
    .prefetchSprite = CtrRenderer_prefetchSprite,
    .prefetchSprites = CtrRenderer_prefetchSprites,
    .createSpriteFromSurface = ctr_create_surf,
    .createSpriteFromSurfaceEx = ctr_create_surf_ex,
    .deleteSprite = ctr_del_sprite,
    .createSurface = ctr_create_surface,         .freeSurface = ctr_free_surface,
    .surfaceExists = ctr_surface_exists,         .surfaceGetSize = ctr_surface_get_size,
    .surfaceSetTarget = ctr_surface_set_target,  .surfaceResetTarget = ctr_surface_reset_target,
    .drawSurface = ctr_draw_surface,
    .clearTarget = ctr_clear_target,
    .gpuSetBlendMode = ctr_gpu_blend_mode,
    .gpuSetBlendModeExt = ctr_gpu_blend_mode_ext,
    .gpuSetBlendEnable = ctr_gpu_blend_enable,
    .gpuSetAlphaTestEnable = ctr_gpu_alpha_test_enable,
    .gpuSetAlphaTestRef = ctr_gpu_alpha_test_ref,
    .gpuSetColorWriteEnable = ctr_gpu_color_write_enable,
    .drawTile = ctr_draw_tile,                   .drawTiled = ctr_draw_tiled,
    .drawTiledPart = ctr_draw_tiled_part,
    .drawCircle = ctr_draw_circle,               .drawEllipse = ctr_draw_ellipse,
    .drawRoundrect = ctr_draw_rr,
    .onRoomChanged = ctr_on_room,
    .set3DDepthOffset = ctr_set_3d_depth,        .setBlendMode = ctr_set_blend,
};

Renderer *CtrRenderer_create(void) {
    CtrRenderer *ctx = calloc(1, sizeof(CtrRenderer));
    ctx->base.vtable          = &vtable;
    ctx->base.drawColor       = 0xFFFFFF;
    ctx->base.drawAlpha       = 1.0f;
    ctx->base.drawFont        = -1;
    ctx->base.circlePrecision = 36;
    return (Renderer *)ctx;
}
