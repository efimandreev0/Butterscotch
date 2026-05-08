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
#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define MAX_GC_TARGETS 64
static C3D_RenderTarget *g_gc_targets[MAX_GC_TARGETS];
static int g_gc_target_count = 0;

// ---- Live theme + screen layout (driven by launcher) -----------------------

static CtrGameScreen g_ctr_game_screen = CTR_GAME_SCREEN_TOP;
static CtrBackdropMode g_ctr_backdrop_mode = CTR_BACKDROP_GRADIENT;

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
    int value = (int) mode;
    if (value < (int) CTR_BACKDROP_GRADIENT || value > (int) CTR_BACKDROP_STRETCH)
        mode = CTR_BACKDROP_GRADIENT;
    g_ctr_backdrop_mode = mode;
}

CtrBackdropMode CtrRenderer_getBackdropMode(void) { return g_ctr_backdrop_mode; }

void CtrRenderer_setLetterboxTheme(float topR, float topG, float topB,
                                   float botR, float botG, float botB,
                                   float accentR, float accentG, float accentB,
                                   float blurAlpha, float particleAlpha) {
    g_letterbox.bgTop[0] = topR;
    g_letterbox.bgTop[1] = topG;
    g_letterbox.bgTop[2] = topB;
    g_letterbox.bgBot[0] = botR;
    g_letterbox.bgBot[1] = botG;
    g_letterbox.bgBot[2] = botB;
    g_letterbox.accent[0] = accentR;
    g_letterbox.accent[1] = accentG;
    g_letterbox.accent[2] = accentB;
    if (blurAlpha < 0.f) blurAlpha = 0.f;
    if (particleAlpha < 0.f) particleAlpha = 0.f;
    g_letterbox.blurAlpha = blurAlpha;
    g_letterbox.particleAlpha = particleAlpha;
}

C3D_RenderTarget *CtrRenderer_getTopTarget(Renderer *ren) {
    return ren ? ((CtrRenderer *) ren)->topTarget : NULL;
}

C3D_RenderTarget *CtrRenderer_getBottomTarget(Renderer *ren) {
    return ren ? ((CtrRenderer *) ren)->bottomTarget : NULL;
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

static void gc_add_target(C3D_RenderTarget *tgt) {
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
    float x, y, z;
    float u, v;
    float r, g, b, a;
} CtrVertex;

static uint32_t g_frame = 0;
static CtrRendererCacheProgressFn g_cacheProgressCallback = NULL;
static void *g_cacheProgressUser = NULL;

void CtrRenderer_setCacheProgressCallback(CtrRendererCacheProgressFn callback, void *user) {
    g_cacheProgressCallback = callback;
    g_cacheProgressUser = user;
    CtrTextureCache_setProgressCallback((CtrTextureCacheProgressFn) callback, user);
}

static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static int next_pow2(int x) {
    if (x < 8) return 8;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

static inline uint8_t clamp_u8(float a) {
    if (a <= 0.f) return 0;
    if (a >= 1.f) return 255;
    return (uint8_t) (a * 255.f);
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
    if (buf && fread(buf, 1, size, fp) != size) {
        free(buf);
        return NULL;
    }
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
        page_meta_path(pagePath, sizeof(pagePath), (int) hdr.basePageId);
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
    if (f) {
        fputs("READY", f);
        fclose(f);
    }
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
    AtlasHeader hdr = {ATLAS_MAGIC, (uint32_t) w, (uint32_t) h};
    if (fwrite(&hdr, sizeof(hdr), 1, outF) != 1) ok = false;

    uint16_t *row = malloc((size_t) w * sizeof(uint16_t));
    if (!row) ok = false;
    for (int y = 0; ok && y < h; y++) {
        const uint8_t *src = pixels + (size_t) y * (size_t) w * 4u;
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            //if (a == 0) { r = 0; g = 0; b = 0; }
            row[x] = pack_rgba4444(r, g, b, a);
        }
        if (fwrite(row, sizeof(uint16_t), (size_t) w, outF) != (size_t) w) ok = false;
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
    const RepackImage *ia = &g_sort_images[*(const uint32_t *) a];
    const RepackImage *ib = &g_sort_images[*(const uint32_t *) b];
    if (ia->groupId < ib->groupId) return -1;
    if (ia->groupId > ib->groupId) return 1;
    return 0;
}

static int cmp_group_area_desc(const void *a, const void *b) {
    const RepackGroup *ga = (const RepackGroup *) a;
    const RepackGroup *gb = (const RepackGroup *) b;
    if (ga->area < gb->area) return 1;
    if (ga->area > gb->area) return -1;
    return 0;
}

static int cmp_image_size_desc(const void *a, const void *b) {
    const RepackImage *ia = &g_sort_images[*(const uint32_t *) a];
    const RepackImage *ib = &g_sort_images[*(const uint32_t *) b];
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
            packer_add_free(p, (PackRect){
                                used.x + used.w, freeR.y,
                                freeR.x + freeR.w - used.x - used.w, freeR.h
                            });
        }
        if (freeR.y < used.y) {
            packer_add_free(p, (PackRect){freeR.x, freeR.y, freeR.w, used.y - freeR.y});
        }
        if (freeR.y + freeR.h > used.y + used.h) {
            packer_add_free(p, (PackRect){
                                freeR.x, used.y + used.h, freeR.w,
                                freeR.y + freeR.h - used.y - used.h
                            });
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

static bool add_repack_image(RepackImage **images, uint32_t *count, uint32_t *cap,
                             RepackImage img) {
    if (*count >= *cap) {
        uint32_t next = *cap ? (*cap * 2u) : 256u;
        RepackImage *grown = realloc(*images, (size_t) next * sizeof(RepackImage));
        if (!grown) return false;
        *images = grown;
        *cap = next;
    }
    (*images)[(*count)++] = img;
    return true;
}

static uint32_t pack_repack_images(RepackImage *images, uint32_t imageCount) {
    if (!images || imageCount == 0) return 0;

    uint32_t *order = malloc((size_t) imageCount * sizeof(uint32_t));
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
            area += (uint64_t) img->w * (uint64_t) img->h;
            at++;
        }
        if (groupCount >= groupCap) {
            uint32_t next = groupCap ? groupCap * 2u : 128u;
            RepackGroup *grown = realloc(groups, (size_t) next * sizeof(RepackGroup));
            if (!grown) {
                free(groups);
                free(order);
                return 0;
            }
            groups = grown;
            groupCap = next;
        }
        groups[groupCount++] = (RepackGroup){groupId, start, at - start, area};
    }
    qsort(groups, groupCount, sizeof(RepackGroup), cmp_group_area_desc);

    RepackAtlas *atlases = NULL;
    uint32_t atlasCount = 0;
    uint32_t atlasCap = 0;

    for (uint32_t g = 0; g < groupCount; g++) {
        RepackGroup *grp = &groups[g];
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
                        RepackAtlas *grown = realloc(atlases, (size_t) next * sizeof(RepackAtlas));
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
    g_sort_images = NULL;
    return atlasCount;
}

static bool init_empty_repack_page(uint32_t pageId) {
    char path[256], tmpPath[256];
    page_meta_path(path, sizeof(path), (int) pageId);
    snprintf(tmpPath, sizeof(tmpPath), "%s/page_%lu.tmp",
             g_current_cache_dir, (unsigned long) pageId);
    remove_if_exists(tmpPath);

    FILE *f = fopen(tmpPath, "wb");
    if (!f) return false;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);

    bool ok = true;
    AtlasHeader hdr = {ATLAS_MAGIC, CTR_REPACK_ATLAS_SIZE, CTR_REPACK_ATLAS_SIZE};
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
    page_meta_path(path, sizeof(path), (int) pageId);
    snprintf(tmpPath, sizeof(tmpPath), "%s/page_%lu.tiled.tmp",
             g_current_cache_dir, (unsigned long) pageId);

    FILE *in = fopen(path, "rb");
    if (!in) return false;

    AtlasHeader hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, in) == 1 &&
              hdr.magic == ATLAS_MAGIC &&
              hdr.w > 0 && hdr.h > 0 &&
              hdr.w <= CTR_REPACK_ATLAS_SIZE &&
              hdr.h <= CTR_REPACK_ATLAS_SIZE;
    int w = ok ? (int) hdr.w : 0;
    int h = ok ? (int) hdr.h : 0;
    uint16_t *linear = NULL;
    uint16_t *tiled = NULL;
    if (ok) {
        linear = calloc((size_t) w * (size_t) h, sizeof(uint16_t));
        tiled = malloc((size_t) w * (size_t) h * sizeof(uint16_t));
        ok = (linear != NULL && tiled != NULL);
    }
    if (ok) {
        ok = fread(linear, sizeof(uint16_t), (size_t) w * (size_t) h, in) ==
             (size_t) w * (size_t) h;
    }
    fclose(in);

    if (ok) {
        tile_rgba4(linear, tiled, w, h, w, h);
        remove_if_exists(tmpPath);
        FILE *out = fopen(tmpPath, "wb");
        ok = (out != NULL);
        if (ok) {
            AtlasHeader outHdr = {ATLAS_TILED_MAGIC, (uint32_t) w, (uint32_t) h};
            ok = fwrite(&outHdr, sizeof(outHdr), 1, out) == 1 &&
                 fwrite(tiled, sizeof(uint16_t), (size_t) w * (size_t) h, out) ==
                 (size_t) w * (size_t) h;
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
    page_meta_path(path, sizeof(path), (int) (basePageId + (uint32_t) img->atlasId));
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    uint16_t *row = malloc((size_t) img->w * sizeof(uint16_t));
    bool ok = (row != NULL);
    for (int y = 0; ok && y < img->h; y++) {
        const uint8_t *src = srcPixels + (((size_t) (img->srcY + y) * (size_t) srcW + (size_t) img->srcX) * 4u);
        for (int x = 0; x < img->w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            //if (a == 0) { r = 0; g = 0; b = 0; }
            row[x] = pack_rgba4444(r, g, b, a);
        }
        long off = (long) sizeof(AtlasHeader) +
                   (((long) (img->dstY + y) * CTR_REPACK_ATLAS_SIZE + img->dstX) * 2L);
        if (fseek(f, off, SEEK_SET) != 0 ||
            fwrite(row, sizeof(uint16_t), (size_t) img->w, f) != (size_t) img->w) {
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
        if (ok && fwrite(entries, 1, (size_t) tpagCount * sizeof(RepackMapEntry), f) !=
            (size_t) tpagCount * sizeof(RepackMapEntry))
            ok = false;
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
        size_t bytes = (size_t) tpagCount * sizeof(RepackMapEntry);
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
            if (tpag >= 0 && (uint32_t) tpag < dw->tpag.count && groupIds[tpag] == 0) {
                groupIds[tpag] = group;
            }
        }
    }
    for (uint32_t b = 0; b < dw->bgnd.count; b++) {
        int32_t tpag = dw->bgnd.backgrounds[b].tpagIndex;
        if (tpag >= 0 && (uint32_t) tpag < dw->tpag.count && groupIds[tpag] == 0) {
            groupIds[tpag] = (*nextGroup)++;
        }
    }
    for (uint32_t f = 0; f < dw->font.count; f++) {
        int32_t tpag = dw->font.fonts[f].tpagIndex;
        if (tpag >= 0 && (uint32_t) tpag < dw->tpag.count && groupIds[tpag] == 0) {
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
            if (item->texturePageId < 0 || (uint32_t) item->texturePageId >= dw->txtr.count) continue;
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
                (uint32_t) images[i].srcPage < dw->txtr.count) {
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
                .pageId = basePageId + (uint32_t) img->atlasId,
                .sourceX = (uint16_t) img->dstX,
                .sourceY = (uint16_t) img->dstY,
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
        page_meta_path(progressPath, sizeof(progressPath), (int) p);
        if (g_cacheProgressCallback) {
            g_cacheProgressCallback(p, dw->txtr.count, progressPath, g_cacheProgressUser);
        }

        bool needed = fallbackPages[p];
        for (uint32_t i = 0; !needed && i < imageCount; i++) {
            if (images[i].srcPage == (int) p) needed = true;
        }
        if (!needed) {
            char oldPath[256];
            page_meta_path(oldPath, sizeof(oldPath), (int) p);
            remove_if_exists(oldPath);
            remove_legacy_tile_files((int) p);
            continue;
        }

        Texture *t = &dw->txtr.textures[p];
        if (!t->blobSize) {
            fprintf(stderr, "CTR cache: TXTR page %lu has no embedded blob\n", (unsigned long) p);
            ok = false;
            continue;
        }
        uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
        if (!blob) {
            fprintf(stderr, "CTR cache: failed to read TXTR page %lu blob\n", (unsigned long) p);
            ok = false;
            continue;
        }

        int w, h;
        uint8_t *pixels = ImageDecoder_decodeToRgba(
            blob, t->blobSize, DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), &w, &h);
        free(blob);
        if (!pixels) {
            fprintf(stderr, "CTR cache: failed to decode TXTR page %lu\n", (unsigned long) p);
            ok = false;
            continue;
        }

        if (fallbackPages[p] && !write_one_page_legacy((int) p, pixels, w, h)) {
            fprintf(stderr, "CTR cache: failed to write legacy page %lu\n", (unsigned long) p);
            ok = false;
        }
        for (uint32_t i = 0; ok && i < imageCount; i++) {
            if (images[i].srcPage != (int) p) continue;
            if (images[i].atlasId < 0) continue;
            if (!blit_repack_image(&images[i], pixels, w, h, basePageId)) {
                fprintf(stderr, "CTR cache: failed to blit TPAG %lu from source page %lu\n",
                        (unsigned long) images[i].tpagIndex, (unsigned long) p);
                ok = false;
            }
        }
        ImageDecoder_freeRgba(pixels);
        if (ok && !fallbackPages[p]) {
            char oldPath[256];
            page_meta_path(oldPath, sizeof(oldPath), (int) p);
            remove_if_exists(oldPath);
            remove_legacy_tile_files((int) p);
        }
    }

    if (ok) {
        for (uint32_t a = 0; a < atlasCount; a++) {
            if (!finalize_repack_page_tiled(basePageId + a)) {
                fprintf(stderr, "CTR cache: failed to finalize repack atlas %lu\n",
                        (unsigned long) (basePageId + a));
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
        RepackMapEntry *entries = malloc((size_t) hdr.tpagCount * sizeof(RepackMapEntry));
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
                    item->texturePageId = (int16_t) e->pageId;
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

// C3D_FrameSplit invalidates BufInfo / AttrInfo / TexEnv / AlphaBlend / projection
// uniforms — bind_target already knows this (calls rebind_state+apply_projection
// after every split). Anywhere else that splits mid-frame must do the same, or
// the next DrawArrays reads vertices from a stale buffer binding and renders
// garbage (UNDERTALE undynebridge / DELTARUNE cooking minigame). Use this
// helper instead of calling C3D_FrameSplit(0) directly.
static void ctr_safe_frame_split(CtrRenderer *ctx) {
    C3D_FrameSplit(0);
    // The cmdbuf was just drained — reset the counter so flush_batch's
    // auto-split heuristic doesn't immediately split again on the very next
    // draw.
    ctx->drawsSinceSplit = 0;
    if (ctx->inFrame && ctx->activeTarget) {
        rebind_state(ctx);
        apply_projection(ctx, &ctx->currentProjection);
        // Re-emit C3D_AlphaBlend — citro3d state cache doesn't always restore it
        // after a split, and stale blend state mangles font edges + UI overlays
        // for the rest of the frame. Safe to call: batchVerts is 0 at this point
        // (caller flushed before splitting), so the inner flush_batch is a no-op.
        apply_blend(ctx, ctx->currentBlendMode);
    }
}

// Auto-split threshold — picked empirically. The DELTARUNE Cyber Field /
// fountain rooms hit ~1500-2000 draws per frame at peak; without periodic
// splitting that exhausts the citro3d cmdbuf even with the 4× C3D_Init
// allocation. 256 draws ≈ ~6 KB of cmdbuf, well under any per-segment limit.
#define CTR_AUTO_SPLIT_DRAWS 256u

static void flush_batch(CtrRenderer *ctx) {
    if (!ctx->batchVerts || !ctx->batchTex || !ctx->inFrame) {
        ctx->batchVerts = 0;
        ctx->batchTex = NULL;
        return;
    }

    GSPGPU_FlushDataCache(ctx->vbuf + ctx->batchStart, ctx->batchVerts * sizeof(CtrVertex));

    C3D_TexBind(0, ctx->batchTex);
    C3D_DrawArrays(GPU_TRIANGLES, ctx->batchStart, ctx->batchVerts);
    ctx->batchStart += ctx->batchVerts;
    ctx->batchVerts = 0;
    ctx->batchTex = NULL;
    ctx->drawsSinceSplit++;

    // Pre-emptive cmdbuf pressure release. Splits are cheap if we already
    // flushed a batch (no half-built batch to lose), and they keep the
    // accumulated GPU command list from blowing past citro3d's per-frame cap.
    if (ctx->drawsSinceSplit >= CTR_AUTO_SPLIT_DRAWS) {
        ctr_safe_frame_split(ctx);
        ctx->drawsSinceSplit = 0;
    }
}

static inline CtrVertex *vbuf_reserve(CtrRenderer *ctx, uint32_t count, C3D_Tex *tex) {
    if (ctx->batchTex && ctx->batchTex != tex) flush_batch(ctx);
    if (ctx->vbufHead + count > ctx->vbufCap) {
        flush_batch(ctx);
        ctr_safe_frame_split(ctx);
        ctx->vbufHead = 0;
        ctx->batchStart = 0;
    }
    ctx->batchTex = tex;
    CtrVertex *v = (CtrVertex *) ctx->vbuf + ctx->vbufHead;
    ctx->vbufHead += count;
    ctx->batchVerts += count;
    return v;
}

static void push_quad_uvgrad(CtrRenderer *ctx, C3D_Tex *tex,
                             const float x[4], const float y[4],
                             float u0, float v0, float u1, float v1,
                             const float c[4][4]) {
    CtrVertex *v = vbuf_reserve(ctx, 6, tex);
#define EMIT(idx, ix, uu, vv, cc) \
        do { \
            v[idx].x = x[ix] + ctx->currentShiftX; v[idx].y = y[ix]; v[idx].z = 0.f; \
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
    float xs[4] = {x0, x1, x2, x3};
    float ys[4] = {y0, y1, y2, y3};
    float cs[4][4] = {
        {col[0], col[1], col[2], col[3]},
        {col[0], col[1], col[2], col[3]},
        {col[0], col[1], col[2], col[3]},
        {col[0], col[1], col[2], col[3]},
    };
    CtrVertex *v = vbuf_reserve(ctx, 6, tex);
    // Added shift safely internally rendering structures precisely mapping bounds accurately generating operations efficiently logically securely.
#define VV(idx, ix, iy, uu, vv, cc) \
v[idx] = (CtrVertex){xs[ix] + ctx->currentShiftX, ys[iy], 0.f, uu, vv, cs[cc][0], cs[cc][1], cs[cc][2], cs[cc][3]}
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
    CtrVertex *v = vbuf_reserve(ctx, 3, &ctx->whiteTex);
    float shft = ctx->currentShiftX;
    v[0] = (CtrVertex){x1 + shft, y1, 0, .5f, .5f, c1[0], c1[1], c1[2], c1[3]};
    v[1] = (CtrVertex){x2 + shft, y2, 0, .5f, .5f, c2[0], c2[1], c2[2], c2[3]};
    v[2] = (CtrVertex){x3 + shft, y3, 0, .5f, .5f, c3[0], c3[1], c3[2], c3[3]};
}

static void draw_letterbox_backdrop(CtrRenderer *ctx) {
    if (g_ctr_backdrop_mode == CTR_BACKDROP_BLACK) {
        float x[4] = {0.f, (float) ctx->winW, (float) ctx->winW, 0.f};
        float y[4] = {0.f, 0.f, (float) ctx->winH, (float) ctx->winH};
        float bg[4][4] = {
            {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f},
            {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, x, y, .5f, .5f, .5f, .5f, bg);
        return;
    }

    float u0 = 0.f;
    float u1 = (float) ctx->appLogicW / (float) ctx->appPotW;
    float v0 = (float) (ctx->appPotH - ctx->appLogicH) / (float) ctx->appPotH;
    float v1 = 1.f;

    float zoomU = (u1 - u0) * 0.15f;
    float zoomV = (v1 - v0) * 0.15f;
    float texU0 = u0 + zoomU;
    float texU1 = u1 - zoomU;
    float texVTop = v1 - zoomV;
    float texVBot = v0 + zoomV;

    float x[4] = {0.f, (float) ctx->winW, (float) ctx->winW, 0.f};
    float y[4] = {0.f, 0.f, (float) ctx->winH, (float) ctx->winH};
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

            float px[4] = {ox, (float) ctx->winW + ox, (float) ctx->winW + ox, ox};
            float py[4] = {oy, oy, (float) ctx->winH + oy, (float) ctx->winH + oy};

            float c[4][4] = {
                {0.45f, 0.45f, 0.45f, blurAlpha}, {0.45f, 0.45f, 0.45f, blurAlpha},
                {0.15f, 0.15f, 0.15f, blurAlpha}, {0.15f, 0.15f, 0.15f, blurAlpha}
            };

            push_quad_uvgrad(ctx, &ctx->appTex, px, py, texU0, texVTop, texU1, texVBot, c);
        }
    }

    if (g_letterbox.particleAlpha > 0.001f) {
        float t = (float) g_frame * 0.025f;
        float aR = g_letterbox.accent[0];
        float aG = g_letterbox.accent[1];
        float aB = g_letterbox.accent[2];
        for (int i = 0; i < 18; i++) {
            float seed = (float) i * 15.37f;
            float px = fmodf(seed * 19.1f + t * (18.f + (float) (i % 4) * 7.f), (float) ctx->winW + 48.f) - 24.f;
            float py = fmodf(seed * 11.3f + sinf(t + seed) * 16.f, (float) ctx->winH + 32.f) - 16.f;
            float s = 1.2f + (float) (i % 3) * 0.8f;

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
        int victim = -1;
        for (uint32_t i = 0; i < ctx->pageCount; i++) {
            if (!ctx->pages[i].loaded || ctx->pages[i].keepResident ||
                ctx->pages[i].lastFrame >= g_frame)
                continue;
            if (ctx->pages[i].lastFrame < oldest) {
                oldest = ctx->pages[i].lastFrame;
                victim = (int) i;
            }
        }
        if (victim < 0) {
            for (uint32_t i = 0; i < ctx->pageCount; i++) {
                if (!ctx->pages[i].loaded || ctx->pages[i].keepResident) continue;
                victim = (int) i;
                break;
            }
        }
        if (victim < 0) break;

        if (!flushed) {
            flush_batch(ctx);
            flushed = true;
        }

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
    int w = item->sourceWidth > 0 ? item->sourceWidth : 1;
    int h = item->sourceHeight > 0 ? item->sourceHeight : 1;

    CtrPage *page = &ctx->pages[id];
    page->origW = w;
    page->origH = h;
    page->chunksX = (int) fminf((float) ((w + LEGACY_CHUNK_SIZE - 1) / LEGACY_CHUNK_SIZE),
                                (float) CTR_MAX_CHUNKS_X);
    page->chunksY = (int) fminf((float) ((h + LEGACY_CHUNK_SIZE - 1) / LEGACY_CHUNK_SIZE),
                                (float) CTR_MAX_CHUNKS_Y);
    bool complete = true;

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            CtrAtlasChunk *chunk = &page->chunks[cx][cy];
            memset(chunk, 0, sizeof(*chunk));
            chunk->srcX = cx * LEGACY_CHUNK_SIZE;
            chunk->srcY = cy * LEGACY_CHUNK_SIZE;
            chunk->width = (int) fminf((float) (w - chunk->srcX), (float) LEGACY_CHUNK_SIZE);
            chunk->height = (int) fminf((float) (h - chunk->srcY), (float) LEGACY_CHUNK_SIZE);
            chunk->potW = next_pow2(chunk->width);
            chunk->potH = next_pow2(chunk->height);

            uint16_t *linear = calloc((size_t) chunk->potW * chunk->potH, sizeof(uint16_t));
            if (!linear) {
                complete = false;
                continue;
            }

            for (int y = 0; y < chunk->height; y++) {
                int sy = item->sourceY + chunk->srcY + y;
                int sx = item->sourceX + chunk->srcX;
                if (sy < 0 || sy >= ah || sx < 0 || sx >= aw) continue;
                int readable = chunk->width;
                if (sx + readable > aw) readable = aw - sx;
                if (readable <= 0) continue;
                fseek(f, sizeof(AtlasHeader) + ((long) sy * aw + sx) * 2, SEEK_SET);
                fread(&linear[y * chunk->potW], sizeof(uint16_t), (size_t) readable, f);
            }

            if (!C3D_TexInit(&chunk->tex, (u16) chunk->potW, (u16) chunk->potH, GPU_RGBA4)) {
                free(linear);
                complete = false;
                continue;
            }
            C3D_TexSetFilter(&chunk->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&chunk->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

            uint32_t tiledSize = (uint32_t) chunk->potW * (uint32_t) chunk->potH * 2u;
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
           pageId >= (int) ctx->repackBasePageId &&
           pageId < (int) (ctx->repackBasePageId + ctx->repackPageCount);
}

static CtrSourcePage *get_source_page(CtrRenderer *ctx, int pageId) {
    if (!is_repacked_page(ctx, pageId)) return NULL;
    uint32_t idx = (uint32_t) pageId - ctx->repackBasePageId;
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
                victim = (int) i;
            }
        }
        if (victim < 0) break;
        if (!flushed) {
            flush_batch(ctx);
            flushed = true;
        }
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
        if (!flushed) {
            flush_batch(ctx);
            flushed = true;
        }
        C3D_TexDelete(&p->tex);
        memset(&p->tex, 0, sizeof(p->tex));
        p->loaded = false;
        p->loadFailed = false;
        evicted++;
    }
    if (evicted > 0) {
        fprintf(stderr, "CTR cache: evicted %lu stale atlas textures before room preload\n",
                (unsigned long) evicted);
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
                             (u16) CTR_TEXTURE_CACHE_ATLAS_SIZE,
                             (u16) CTR_TEXTURE_CACHE_ATLAS_SIZE,
                             texFormat);

    if (!texOk) {
        fprintf(stderr, "CTR: Failed to alloc C3D_Tex for atlas %d\n", pageId);
        page->loadFailed = true;
        page->lastFrame = g_frame; // stamp so retry waits until next frame
        return false;
    }
    if (tiledSize > page->tex.size) tiledSize = (uint32_t) page->tex.size;

    fseek(ctx->atlasFile, (long) page->fileOffset, SEEK_SET);
    if (fread(page->tex.data, 1, tiledSize, ctx->atlasFile) != tiledSize) {
        C3D_TexDelete(&page->tex);
        page->loadFailed = true;
        page->lastFrame = g_frame;
        return false;
    }

    C3D_TexFlush(&page->tex);
    C3D_TexSetFilter(&page->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&page->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    page->width = (int) CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->height = (int) CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->potW = (int) CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->potH = (int) CTR_TEXTURE_CACHE_ATLAS_SIZE;
    page->loaded = true;
    page->loadFailed = false;
    page->lastFrame = g_frame;

    if (!ctx->preloadingAtlases) {
        fprintf(stderr,
                "CTR cache: lazy atlas load during draw: page %d; linear free %.2f MB\n",
                pageId, (double) linearSpaceFree() / (1024.0 * 1024.0));
    }

    return true;
}

static void load_page_dyn(CtrRenderer *ctx, DataWin *dw, int32_t idx) {
    if (idx < 0 || (uint32_t) idx >= ctx->pageCount) return;

    int pageIdSigned = dw->tpag.items[idx].texturePageId;
    if (is_repacked_page(ctx, pageIdSigned)) {
        load_source_page_dyn(ctx, pageIdSigned);
        return;
    }

    CtrPage *page = &ctx->pages[idx];
    if (page->loaded) return;

    free_old_pages(ctx);

    if (pageIdSigned < 0) return;
    uint32_t pageId = (uint32_t) pageIdSigned;
    char path[256];
    page_meta_path(path, sizeof(path), (int) pageId);

    FILE *f = fopen(path, "rb");
    if (!f) return;
    setvbuf(f, dyn_buf, _IOFBF, sizeof(dyn_buf));

    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) == 1) {
        fseek(f, 0, SEEK_SET);
        if (magic == ATLAS_MAGIC) {
            AtlasHeader hdr;
            if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
                extract_legacy_page_file(ctx, dw, (uint32_t) idx, f, (int) hdr.w, (int) hdr.h);
            }
        }
    }
    fclose(f);
}

// Citro3D pipeline

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
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    ctx->currentBlendMode = 0;
    ctx->pipelineReady = true;
}

static void rebind_state(CtrRenderer *ctx) {
    C3D_BindProgram(&g_shaderProg);
    C3D_SetAttrInfo(&ctx->attrInfo);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, ctx->vbuf, sizeof(CtrVertex), 3, 0x210);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
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
    if (surfaceId < 0 || (uint32_t) surfaceId >= ctx->surfaceCount) return NULL;
    return ctx->surfaces[surfaceId].used ? &ctx->surfaces[surfaceId] : NULL;
}

static bool surface_alloc_storage(CtrSurface *surf, int width, int height) {
    surf->width = width;
    surf->height = height;
    surf->potW = next_pow2(width);
    surf->potH = next_pow2(height);

    if (!C3D_TexInitVRAM(&surf->tex, (u16) surf->potW, (u16) surf->potH, GPU_RGBA8)) {
        return false;
    }
    // Default to NEAREST so pixel-art HUDs / mini-maps / damage popups drawn
    // through surface_create stay crisp. GMS games that genuinely want a
    // smooth surface (rare — usually post-processing in 2024+ projects) can
    // toggle filter via gpu_set_texfilter, which our renderer honours.
    C3D_TexSetFilter(&surf->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&surf->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

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
    if (ctx->appTargetRight) {
        safe_delete_target(ctx, ctx->appTargetRight);
        ctx->appTargetRight = NULL;
    }
    if (ctx->appTexRight.data) {
        C3D_TexDelete(&ctx->appTexRight);
        memset(&ctx->appTexRight, 0, sizeof(ctx->appTexRight));
    }
    ctx->appReady = false;
}

static bool ensure_app_surface(CtrRenderer *ctx, int gw, int gh) {
    if (ctx->appReady && ctx->appLogicW == gw && ctx->appLogicH == gh) return true;

    destroy_app_surface(ctx);

    ctx->appLogicW = gw;
    ctx->appLogicH = gh;
    ctx->appPotW = next_pow2(gw);
    ctx->appPotH = next_pow2(gh);

    if (!C3D_TexInitVRAM(&ctx->appTex, (u16) ctx->appPotW, (u16) ctx->appPotH, GPU_RGBA8)) {
        if (!C3D_TexInit(&ctx->appTex, (u16) ctx->appPotW, (u16) ctx->appPotH, GPU_RGBA8)) {
            return false;
        }
    }
    C3D_TexSetFilter(&ctx->appTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&ctx->appTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    ctx->appTarget = C3D_RenderTargetCreateFromTex(&ctx->appTex, GPU_TEXFACE_2D, 0, -1);
    if (!ctx->appTarget) {
        C3D_TexDelete(&ctx->appTex);
        memset(&ctx->appTex, 0, sizeof(ctx->appTex));
        return false;
    }

    C3D_RenderTargetClear(ctx->appTarget, C3D_CLEAR_ALL, 0x000000FF, 0);

    // Initializing the corresponding right target cleanly generating buffers reliably correctly!
    if (!C3D_TexInitVRAM(&ctx->appTexRight, (u16) ctx->appPotW, (u16) ctx->appPotH, GPU_RGBA8)) {
        if (!C3D_TexInit(&ctx->appTexRight, (u16) ctx->appPotW, (u16) ctx->appPotH, GPU_RGBA8)) {
            destroy_app_surface(ctx);
            return false;
        }
    }
    C3D_TexSetFilter(&ctx->appTexRight, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&ctx->appTexRight, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    ctx->appTargetRight = C3D_RenderTargetCreateFromTex(&ctx->appTexRight, GPU_TEXFACE_2D, 0, -1);
    if (!ctx->appTargetRight) {
        destroy_app_surface(ctx);
        return false;
    }
    C3D_RenderTargetClear(ctx->appTargetRight, C3D_CLEAR_ALL, 0x000000FF, 0);


    ctx->appReady = true;
    return true;
}

// Target and viewport

static void bind_target(CtrRenderer *ctx, C3D_RenderTarget *tgt) {
    if (!ctx->inFrame) return;
    bool switchingTarget = ctx->activeTarget && ctx->activeTarget != tgt;
    flush_batch(ctx);
    if (switchingTarget) C3D_FrameSplit(0);
    C3D_FrameDrawOn(tgt);
    ctx->activeTarget = tgt;
    rebind_state(ctx);
    apply_projection(ctx, &ctx->currentProjection);
}

static void set_viewport_logical(CtrRenderer *ctx, C3D_RenderTarget *tgt,
                                 int x, int y, int w, int h) {
    int fbH = tgt->frameBuf.height;
    int vpY = fbH - y - h;
    if (vpY < 0) vpY = 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    C3D_SetViewport((u32) x, (u32) vpY, (u32) w, (u32) h);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32) x, (u32) vpY, (u32) (x + w), (u32) (vpY + h));
    ctx->currentViewport[0] = x;
    ctx->currentViewport[1] = y;
    ctx->currentViewport[2] = w;
    ctx->currentViewport[3] = h;
}

static void disable_scissor(CtrRenderer *ctx) {
    (void) ctx;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Blend state

static void apply_blend(CtrRenderer *ctx, int mode) {
    flush_batch(ctx);
    ctx->currentBlendMode = mode;
    switch (mode) {
        case 1:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE,
                           GPU_SRC_ALPHA, GPU_ONE);
            break;
        case 3:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_ZERO, GPU_ONE_MINUS_SRC_COLOR,
                           GPU_ZERO, GPU_ONE_MINUS_SRC_ALPHA);
            break;
        default:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            break;
    }
}

// Frame lifecycle

static void ctr_init(Renderer *ren, DataWin *dw) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    ren->dataWin = dw;

    setup_pipeline(ctx);

    if (!ctx->topTarget) {
        ctx->topTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (ctx->topTarget) {
            C3D_RenderTargetSetOutput(ctx->topTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        }
    }
    if (!ctx->topTargetRight) {
        ctx->topTargetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (ctx->topTargetRight) {
            C3D_RenderTargetSetOutput(ctx->topTargetRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
        }
    }
    if (!ctx->bottomTarget) {
        ctx->bottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (ctx->bottomTarget) {
            C3D_RenderTargetSetOutput(ctx->bottomTarget, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        }
    }

    ctx->originalTpagCount = dw->tpag.count;
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
    ctx->pages = calloc(ctx->pageCount, sizeof(CtrPage));

    if (!ctx->vbuf) {
        // 4x batch capacity. With small vbuf, many tile-heavy rooms (bridge corridor
        // before/with Undyne) trigger constant FrameSplits which both eat cmdbuf
        // and stall the GPU. Larger vbuf = fewer splits per frame.
        ctx->vbufCap = BATCH_VERT_CAP * 4;
        ctx->vbuf = linearAlloc(ctx->vbufCap * sizeof(CtrVertex));
        ctx->vbufHead = 0;
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
            C3D_TexSetWrap(&ctx->whiteTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        }
    }
}

static void ctr_destroy(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *) ren;

    if (ctx->atlasFile) fclose(ctx->atlasFile);

    for (uint32_t i = 0; i < ctx->surfaceCount; i++) {
        surface_release_storage(ctx, &ctx->surfaces[i]);
    }
    free(ctx->surfaces);
    ctx->surfaces = NULL;
    ctx->surfaceCount = 0;

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

    if (ctx->vbuf) {
        linearFree(ctx->vbuf);
        ctx->vbuf = NULL;
    }

    if (ctx->pipelineReady) {
        ctx->pipelineReady = false;
    }

    if (ctx->topTarget) {
        C3D_RenderTargetDelete(ctx->topTarget);
        ctx->topTarget = NULL;
    }
    if (ctx->bottomTarget) {
        C3D_RenderTargetDelete(ctx->bottomTarget);
        ctx->bottomTarget = NULL;
    }

    free(ctx);
}

static void ctr_begin_frame(Renderer *ren, int32_t gw, int32_t gh, int32_t ww, int32_t wh) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    gc_clear_targets();
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    ctx->winW = ww;
    ctx->winH = wh;
    ctx->gameW = gw;
    ctx->gameH = gh;

    if (!ensure_app_surface(ctx, gw, gh)) return;

    if (!ctx->inFrame) {
        if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
        ctx->inFrame = true;
        ctx->vbufHead = 0;
        ctx->batchStart = 0;
        ctx->batchVerts = 0;
        ctx->batchTex = NULL;
        ctx->drawsSinceSplit = 0;
    }

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, ctx->vbuf, sizeof(CtrVertex), 3, 0x210);

    C3D_RenderTargetClear(ctx->appTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    bind_target(ctx, ctx->appTarget);

    set_viewport_logical(ctx, ctx->appTarget, 0, 0, ctx->appLogicW, ctx->appLogicH);
    C3D_Mtx proj;
    make_ortho_topleft(&proj, (float) ctx->appLogicW, (float) ctx->appLogicH);
    apply_projection(ctx, &proj);

    ctx->appFrameCleared = true;
    ctx->targetStackDepth = 0;
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

        // Reset Offset so scaling quad centers beautifully over layouts smoothly effectively integrating interfaces functionally automatically naturally intelligently
        ctx->currentShiftX = 0;
        ctx->isGUI = true;

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

            int savedWinW = ctx->winW;
            int savedWinH = ctx->winH;
            ctx->winW = primaryW;
            ctx->winH = primaryH;

            C3D_Mtx proj;
            make_ortho_top(&proj, (float)ctx->winW, (float)ctx->winH);
            apply_projection(ctx, &proj);

            draw_letterbox_backdrop(ctx);

            int drawW, drawH;
            if (g_ctr_backdrop_mode == CTR_BACKDROP_STRETCH) {
                drawW = ctx->winW; drawH = ctx->winH;
            } else if ((ctx->gameW * ctx->winH) / ctx->gameH < ctx->winW) {
                drawW = (ctx->gameW * ctx->winH) / ctx->gameH; drawH = ctx->winH;
            } else {
                drawW = ctx->winW; drawH = (ctx->gameH * ctx->winW) / ctx->gameW;
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

            if (ctx->depthSlider > 0.01f && primary == ctx->topTarget && ctx->topTargetRight != NULL) {

                C3D_FrameDrawOn(ctx->topTargetRight);
                ctx->activeTarget = ctx->topTargetRight;
                rebind_state(ctx);
                C3D_RenderTargetClear(ctx->topTargetRight, C3D_CLEAR_ALL, 0x050711FF, 0);
                C3D_SetViewport(0, 0, 240, (u32)primaryW);
                disable_scissor(ctx);

                apply_projection(ctx, &proj); // Ре-аплаим

                draw_letterbox_backdrop(ctx); // Заново перерисовываем фон вокруг черными полосами (если нужно)

                // РИСУЕМ ПОДГОТОВЛЕННЫЙ ТЕКСТУРИРОВАННЫЙ ВТОРОЙ ПАСС КАДРА ВО ВТОРОЙ ГЛАЗ ОРГАНИЧНО ДОПУСКАЯ ВСТРОЕННЫЙ ГЕОМЕТРИЧЕСКИЙ РЕНДЕРИНГ ТОНКО МЯГКО ОТСАВАТЬСЯ НАТИВНЫМ ФОРМАТОМ КОМПАРАТИВНО БЕЗ ИЗДЕРЖЕК ПЕРЕД АВТОВЕСЕЛОЙ МОДАЛЬНОСТЬЮ АДРЕСОВ АНАЛИЗИРУЯ ОПЕРАЦИИ БЫСТРО!!!
                push_quad(ctx, &ctx->appTexRight,
                          (float)drawX,           (float)drawY,
                          (float)(drawX + drawW), (float)drawY,
                          (float)(drawX + drawW), (float)(drawY + drawH),
                          (float)drawX,           (float)(drawY + drawH),
                          0.f, v1, u1, v0, white);
                flush_batch(ctx);
            }

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

static void ctr_flush(Renderer *ren) { flush_batch((CtrRenderer *) ren); }

// Views and GUI

static void ctr_begin_view(Renderer *ren, int32_t vx, int32_t vy, int32_t vw, int32_t vh,
                           int32_t px, int32_t py, int32_t pw, int32_t ph, float angle) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    ctx->isGUI = false; // Always explicitly map views dynamically processing contexts elegantly securely flawlessly automatically effectively utilizing attributes correctly dependably executing routines intelligently cleanly.

    C3D_RenderTarget* canvas = (ctx->currentEye == 1) ? ctx->appTargetRight : ctx->appTarget;
    if (ctx->activeTarget != canvas) {
        bind_target(ctx, canvas);
    }

    set_viewport_logical(ctx, canvas, px, py, pw, ph);

    C3D_Mtx proj, rot, res;
    Mtx_Identity(&proj);
    Mtx_Ortho(&proj, (float) vx, (float) (vx + vw), (float) (vy + vh), (float) vy, -1.f, 1.f, true);

    if (angle != 0.f) {
        float cx = vx + vw / 2.f, cy = vy + vh / 2.f;
        Mtx_Identity(&rot);
        Mtx_Translate(&rot, cx, cy, 0.f, true);
        Mtx_RotateZ(&rot, -angle * (float) (M_PI / 180.f), true);
        Mtx_Translate(&rot, -cx, -cy, 0.f, true);
        Mtx_Multiply(&res, &proj, &rot);
        proj = res;
    }
    apply_projection(ctx, &proj);

    // Enable culling against the view rect in room coords. Disabled when the view
    // is rotated — an axis-aligned cull rect doesn't match a rotated frustum.
    if (angle == 0.f) {
        ctx->cullEnabled = true;
        ctx->cullL = (float) vx;
        ctx->cullT = (float) vy;
        ctx->cullR = (float) (vx + vw);
        ctx->cullB = (float) (vy + vh);
    } else {
        ctx->cullEnabled = false;
    }
}

static void ctr_end_view(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    flush_batch(ctx);
    disable_scissor(ctx);
    ctx->cullEnabled = false;
}

static void ctr_begin_gui(Renderer *ren, int32_t gw, int32_t gh, int32_t px, int32_t py, int32_t pw, int32_t ph) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    flush_batch(ctx);
    // UI draws currently evaluating context dynamically utilizing logic successfully natively generating operations! Disable offset calculation!
    ctx->isGUI = true;

    // target - это currentEye (0 / 1 -> Left / Right app canvas)
    C3D_RenderTarget *canvas = (ctx->currentEye == 1) ? ctx->appTargetRight : ctx->appTarget;
    if (ctx->activeTarget != canvas) bind_target(ctx, canvas);

    set_viewport_logical(ctx, canvas, px, py, pw, ph);

    C3D_Mtx proj;
    Mtx_Identity(&proj);
    Mtx_Ortho(&proj, 0.f, (float) gw, (float) gh, 0.f, -1.f, 1.f, true);
    apply_projection(ctx, &proj);

    ctx->cullEnabled = true;
    ctx->cullL = 0.f;
    ctx->cullT = 0.f;
    ctx->cullR = (float) gw;
    ctx->cullB = (float) gh;
}

static void ctr_end_gui(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
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
        float sL = (float) seg->sourceX;
        float sT = (float) seg->sourceY;
        float sR = sL + (float) seg->width;
        float sB = sT + (float) seg->height;
        float dL = fmaxf(rL, sL);
        float dT = fmaxf(rT, sT);
        float dR = fminf(rR, sR);
        float dB = fminf(rB, sB);
        if (dL >= dR || dT >= dB) continue;

        int pageId = (int) (ctx->repackBasePageId + seg->atlasIndex);
        if (!load_source_page_dyn(ctx, pageId)) continue;
        CtrSourcePage *src = get_source_page(ctx, pageId);
        if (!src || !src->loaded) continue;

        float mX = 0.0f;
        float mY = 0.0f;
        float u0 = ((float) seg->atlasX + (dL - sL) + mX) / (float) src->potW;
        float v0 = ((float) seg->atlasY + (dT - sT) + mY) / (float) src->potH;
        float u1 = ((float) seg->atlasX + (dR - sL) - mX) / (float) src->potW;
        float v1 = ((float) seg->atlasY + (dB - sT) - mY) / (float) src->potH;

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
        float dR = fminf(rR, (float) item->sourceWidth);
        float dB = fminf(rB, (float) item->sourceHeight);
        if (dL >= dR || dT >= dB) return;

        float mX = 0.0f;
        float mY = 0.0f;
        float u0 = ((float) item->sourceX + dL + mX) / (float) src->potW;
        float v0 = ((float) item->sourceY + dT + mY) / (float) src->potH;
        float u1 = ((float) item->sourceX + dR - mX) / (float) src->potW;
        float v1 = ((float) item->sourceY + dB - mY) / (float) src->potH;

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

    if (!ctx->pages[id].loaded) load_page_dyn(ctx, ctx->base.dataWin, (int32_t) id);
    if (!ctx->pages[id].loaded || sw <= 0 || sh <= 0) return;

    CtrPage *page = &ctx->pages[id];
    page->lastFrame = g_frame;

    float rL = sx, rT = sy, rR = sx + sw, rB = sy + sh;

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            CtrAtlasChunk *c = &page->chunks[cx][cy];
            if (!c->valid) continue;

            float dL = fmaxf(rL, (float) c->srcX);
            float dT = fmaxf(rT, (float) c->srcY);
            float dR = fminf(rR, (float) (c->srcX + c->width));
            float dB = fminf(rB, (float) (c->srcY + c->height));
            if (dL >= dR || dT >= dB) continue;

            float mX = 0.0f;
            float mY = 0.0f;

            float u0 = (dL - c->srcX + mX) / (float) c->potW;
            float v0 = (dT - c->srcY + mY) / (float) c->potH;
            float u1 = (dR - c->srcX - mX) / (float) c->potW;
            float v1 = (dB - c->srcY - mY) / (float) c->potH;

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
    CtrRenderer *ctx = (CtrRenderer *) ren;
    if (id < 0 || (uint32_t) id >= ctx->pageCount) return;
    float c[4];
    col2fv(color, a, c);
    if (c[3] <= 0.f) return;

    TexturePageItem *item = &ren->dataWin->tpag.items[id];
    float l0 = (item->targetX - ox) * sx;
    float t0 = (item->targetY - oy) * sy;
    float l1 = l0 + item->sourceWidth * sx;
    float t1 = t0 + item->sourceHeight * sy;

    if (ang == 0.f) {
        draw_region(ctx, (uint32_t) id, 0, 0, item->sourceWidth, item->sourceHeight,
                    x + l0, y + t0, x + l1, y + t0,
                    x + l1, y + t1, x + l0, y + t1, c);
    } else {
        float rad = -ang * (float) (M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        draw_region(ctx, (uint32_t) id, 0, 0, item->sourceWidth, item->sourceHeight,
                    l0 * cs - t0 * sn + x, l0 * sn + t0 * cs + y,
                    l1 * cs - t0 * sn + x, l1 * sn + t0 * cs + y,
                    l1 * cs - t1 * sn + x, l1 * sn + t1 * cs + y,
                    l0 * cs - t1 * sn + x, l0 * sn + t1 * cs + y, c);
    }
}

static void ctr_draw_sprite_part(Renderer *ren, int32_t id,
                                 int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                                 float x, float y, float xscale, float yscale,
                                 float angleDeg, float pivotX, float pivotY,
                                 uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    float c[4];
    col2fv(color, alpha, c);
    if (c[3] <= 0.f) return;

    float l0 = -pivotX * xscale;
    float t0 = -pivotY * yscale;
    float l1 = l0 + sw * xscale;
    float t1 = t0 + sh * yscale;

    if (angleDeg == 0.f) {
        draw_region(ctx, (uint32_t) id, sx, sy, sw, sh,
                    x + l0, y + t0,
                    x + l1, y + t0,
                    x + l1, y + t1,
                    x + l0, y + t1, c);
    } else {
        float rad = -angleDeg * (float) (M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        draw_region(ctx, (uint32_t) id, sx, sy, sw, sh,
                    l0 * cs - t0 * sn + x, l0 * sn + t0 * cs + y,
                    l1 * cs - t0 * sn + x, l1 * sn + t0 * cs + y,
                    l1 * cs - t1 * sn + x, l1 * sn + t1 * cs + y,
                    l0 * cs - t1 * sn + x, l0 * sn + t1 * cs + y, c);
    }
}

static void ctr_draw_sprite_pos(Renderer *ren, int32_t id,
                                float x1, float y1, float x2, float y2,
                                float x3, float y3, float x4, float y4, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    float c[4];
    col2fv(ren->drawColor, alpha, c);
    if (c[3] <= 0.f) return;
    if (id < 0 || (uint32_t) id >= ren->dataWin->tpag.count) return;
    if (ren->dataWin->tpag.items == nullptr) return;
    TexturePageItem *item = &ren->dataWin->tpag.items[id];
    draw_region(ctx, (uint32_t) id, 0, 0, item->sourceWidth, item->sourceHeight,
                x1, y1, x2, y2, x3, y3, x4, y4, c);
}

// Tiles

static void ctr_draw_tile(Renderer *ren, RoomTile *tile, float ox, float oy) {
    CtrRenderer *ctx = (CtrRenderer *) ren;

    // Early cull: skip the TPAG resolve, clipping math, and the draw_region call
    // chain entirely if the tile is off-camera. Big rooms (Undyne bridge, hotland)
    // emit thousands of tiles per frame and only a few hundred fit on screen.
    {
        float dxA = (float) tile->x + ox;
        float dyA = (float) tile->y + oy;
        float dxB = dxA + (float) tile->width * tile->scaleX;
        float dyB = dyA + (float) tile->height * tile->scaleY;
        if (quad_culled(ctx, dxA, dyA, dxB, dyA, dxB, dyB, dxA, dyB)) return;
    }

    int32_t id = Renderer_resolveObjectTPAGIndex(ren->dataWin, tile);
    if (id < 0 || (uint32_t) id >= ren->dataWin->tpag.count) return;
    if (ren->dataWin->tpag.items == nullptr) return;

    TexturePageItem *tpag = &ren->dataWin->tpag.items[id];
    int sx = tile->sourceX, sy = tile->sourceY;
    int sw = (int) tile->width, sh = (int) tile->height;
    float dx = tile->x + ox, dy = tile->y + oy;

    if (tpag->targetX > sx) {
        dx += (tpag->targetX - sx) * tile->scaleX;
        sw -= tpag->targetX - sx;
        sx = tpag->targetX;
    }
    if (tpag->targetY > sy) {
        dy += (tpag->targetY - sy) * tile->scaleY;
        sh -= tpag->targetY - sy;
        sy = tpag->targetY;
    }

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
    CtrRenderer *ctx = (CtrRenderer *) ren;
    if (id < 0 || (uint32_t) id >= ren->dataWin->tpag.count) return;
    TexturePageItem *t = &ren->dataWin->tpag.items[id];
    float tw = t->boundingWidth * fabsf(sx);
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

// Shapes

static void ctr_draw_rect_c(Renderer *ren, float x1, float y1, float x2, float y2,
                            uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                            float a, bool out) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    if (a <= 0.f) return;
    float l = roundf(fminf(x1, x2)), r = roundf(fmaxf(x1, x2));
    float t = roundf(fminf(y1, y2)), b = roundf(fmaxf(y1, y2));

    float r1[4], r2[4], r3[4], r4[4];
    col2fv(c1, a, r1);
    col2fv(c2, a, r2);
    col2fv(c3, a, r3);
    col2fv(c4, a, r4);

    if (out) {
        float xs[4] = {l, r + 1, r + 1, l};
        float ys[4] = {t, t, t + 2, t + 2};
        float cs[4][4] = {
            {r1[0], r1[1], r1[2], a}, {r2[0], r2[1], r2[2], a},
            {r2[0], r2[1], r2[2], a}, {r1[0], r1[1], r1[2], a}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys, .5f, .5f, .5f, .5f, cs);

        float ys2[4] = {b - 1, b - 1, b + 1, b + 1};
        float cs2[4][4] = {
            {r4[0], r4[1], r4[2], a}, {r3[0], r3[1], r3[2], a},
            {r3[0], r3[1], r3[2], a}, {r4[0], r4[1], r4[2], a}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys2, .5f, .5f, .5f, .5f, cs2);

        float xsL[4] = {l, l + 2, l + 2, l};
        float ysM[4] = {t + 2, t + 2, b - 1, b - 1};
        float csL[4][4] = {
            {r1[0], r1[1], r1[2], a}, {r1[0], r1[1], r1[2], a},
            {r4[0], r4[1], r4[2], a}, {r4[0], r4[1], r4[2], a}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xsL, ysM, .5f, .5f, .5f, .5f, csL);

        float xsR[4] = {r - 1, r + 1, r + 1, r - 1};
        float csR[4][4] = {
            {r2[0], r2[1], r2[2], a}, {r2[0], r2[1], r2[2], a},
            {r3[0], r3[1], r3[2], a}, {r3[0], r3[1], r3[2], a}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xsR, ysM, .5f, .5f, .5f, .5f, csR);
    } else {
        float xs[4] = {l, r + 1, r + 1, l};
        float ys[4] = {t, t, b + 1, b + 1};
        float cs[4][4] = {
            {r1[0], r1[1], r1[2], a}, {r2[0], r2[1], r2[2], a},
            {r3[0], r3[1], r3[2], a}, {r4[0], r4[1], r4[2], a}
        };
        push_quad_uvgrad(ctx, &ctx->whiteTex, xs, ys, .5f, .5f, .5f, .5f, cs);
    }
}

static void ctr_draw_rect(Renderer *ren, float x1, float y1, float x2, float y2,
                          uint32_t col, float a, bool out) {
    ctr_draw_rect_c(ren, x1, y1, x2, y2, col, col, col, col, a, out);
}

static void ctr_draw_line_c(Renderer *ren, float x1, float y1, float x2, float y2,
                            float w, uint32_t c1, uint32_t c2, float a) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    float r1[4], r2[4];
    col2fv(c1, a, r1);
    col2fv(c2, a, r2);
    if (r1[3] <= 0.f) return;

    w = fmaxf(2.f, w);
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);

    if (len < 0.01f) {
        ctr_draw_rect_c(ren, x1, y1, x2, y2, c1, c2, c2, c1, a, false);
        return;
    }

    x2 += (dx / len) * .5f;
    y2 += (dy / len) * .5f;
    x1 -= (dx / len) * .5f;
    y1 -= (dy / len) * .5f;
    dx = x2 - x1;
    dy = y2 - y1;
    len = sqrtf(dx * dx + dy * dy);

    float px = (-dy / len) * (w * .5f), py = (dx / len) * (w * .5f);
    float xs[4] = {x1 + px, x1 - px, x2 - px, x2 + px};
    float ys[4] = {y1 + py, y1 - py, y2 - py, y2 + py};
    float cs[4][4] = {
        {r1[0], r1[1], r1[2], r1[3]}, {r1[0], r1[1], r1[2], r1[3]},
        {r2[0], r2[1], r2[2], r2[3]}, {r2[0], r2[1], r2[2], r2[3]}
    };
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
    col2fv(c1, a, r1);
    col2fv(c2, a, r2);
    col2fv(c3, a, r3);
    push_solid_tri((CtrRenderer *) ren, x1, y1, x2, y2, x3, y3, r1, r2, r3);
}

static void ctr_draw_tri(Renderer *ren, float x1, float y1, float x2, float y2,
                         float x3, float y3, bool out) {
    ctr_draw_tri_c(ren, x1, y1, x2, y2, x3, y3,
                   ren->drawColor, ren->drawColor, ren->drawColor, ren->drawAlpha, out);
}

static void ctr_draw_ellipse(Renderer *ren, float cx, float cy, float rx, float ry,
                             uint32_t c, float a, bool out, int32_t prec) {
    (void) prec;
    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx <= 2.5f && ry <= 2.5f) {
        ctr_draw_rect(ren, cx - rx, cy - ry, cx + rx, cy + ry, c, a, out);
        return;
    }
    int segs = (int) fmaxf(8.f, fminf(32.f, sqrtf(fmaxf(rx, ry)) * 3.5f));
    float step = (2.f * (float) M_PI) / segs;
    float rgb[4];
    col2fv(c, a, rgb);
    float px = cx + rx, py = cy;
    for (int i = 1; i <= segs; i++) {
        float nx = cx + cosf(step * i) * rx;
        float ny = cy + sinf(step * i) * ry;
        if (out) ctr_draw_line(ren, px, py, nx, ny, 1.f, c, a);
        else push_solid_tri((CtrRenderer *) ren, cx, cy, px, py, nx, ny, rgb, rgb, rgb);
        px = nx;
        py = ny;
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
        float ang = a1 + (a2 - a1) * (seg ? (float) i / seg : 0);
        px[*cnt] = cx + cosf(ang) * rx;
        py[(*cnt)] = cy + sinf(ang) * ry;
        (*cnt)++;
    }
}

static void ctr_draw_rr(Renderer *ren, float x1, float y1, float x2, float y2,
                        float rx, float ry, uint32_t c, float a, bool out, int32_t prec) {
    (void) prec;
    float l = fminf(x1, x2), r = fmaxf(x1, x2);
    float t = fminf(y1, y2), b = fmaxf(y1, y2);
    float w = r - l, h = b - t;
    if (w <= 0 || h <= 0 || rx <= 0 || ry <= 0) {
        ctr_draw_rect(ren, l, t, r, b, c, a, out);
        return;
    }
    rx = fminf(fabsf(rx), w * .5f);
    ry = fminf(fabsf(ry), h * .5f);

    int seg = (int) fmaxf(2.f, fminf(8.f, sqrtf(fmaxf(rx, ry)) * 0.8f));
    float px[MAX_RR_POINTS], py[MAX_RR_POINTS];
    int cnt = 0;
    float hp = (float) M_PI * .5f;

    get_arc(px, py, &cnt, r - rx, t + ry, rx, ry, -hp, 0.f, seg, false);
    get_arc(px, py, &cnt, r - rx, b - ry, rx, ry, 0.f, hp, seg, true);
    get_arc(px, py, &cnt, l + rx, b - ry, rx, ry, hp, (float) M_PI, seg, true);
    get_arc(px, py, &cnt, l + rx, t + ry, rx, ry, (float) M_PI, (float) M_PI + hp, seg, true);

    float rgb[4];
    col2fv(c, a, rgb);
    if (out) {
        for (int i = 0; i < cnt; i++)
            ctr_draw_line(ren, px[i], py[i], px[(i + 1) % cnt], py[(i + 1) % cnt], 1.f, c, a);
    } else {
        float ccx = (l + r) * .5f, ccy = (t + b) * .5f;
        for (int i = 0; i < cnt; i++)
            push_solid_tri((CtrRenderer *) ren, ccx, ccy, px[i], py[i],
                           px[(i + 1) % cnt], py[(i + 1) % cnt], rgb, rgb, rgb);
    }
}

// Text

static void ctr_draw_text(Renderer *ren, const char *txt, float x, float y,
                          float sx, float sy, float ang) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ren->dataWin;
    int fidx = ren->drawFont;
    if (fidx < 0 || (uint32_t) fidx >= dw->font.count) return;

    Font *font = &dw->font.fonts[fidx];
    if (font->tpagIndex < 0 || (uint32_t) font->tpagIndex >= ctx->pageCount) return;

    if ((uint32_t) font->tpagIndex < ctx->originalTpagCount) {
        if (!cache_item_available(ctx, (uint32_t) font->tpagIndex)) return;
    } else {
        if (!ctx->pages[font->tpagIndex].loaded) return;
    }

    float rgb[4];
    col2fv(ren->drawColor, ren->drawAlpha, rgb);
    if (rgb[3] <= 0.f) return;

    PreprocessedText ptxt = TextUtils_preprocessGmlText(txt);
    if (!ptxt.text[0]) {
        PreprocessedText_free(ptxt);
        return;
    }

    int len = strlen(ptxt.text);
    float stride = font->emSize > 0 ? font->emSize : 10.f;
    int lines = TextUtils_countLines(ptxt.text, len);
    float valOff = ren->drawValign == 1
                       ? -(lines * stride / 2.f)
                       : (ren->drawValign == 2 ? -(lines * stride) : 0);

    Matrix4f tr;
    Matrix4f_setTransform2D(&tr, roundf(x), roundf(y),
                            sx * font->scaleX, sy * font->scaleY,
                            -ang * (float) (M_PI / 180.f));

    float cy = valOff;
    int start = 0;
    for (int l = 0; l < lines; l++) {
        int end = start;
        while (end < len && !TextUtils_isNewlineChar(ptxt.text[end])) end++;

        float lw = TextUtils_measureLineWidth(font, ptxt.text + start, end - start);
        float cx = ren->drawHalign == 1
                       ? -lw / 2.f
                       : (ren->drawHalign == 2 ? -lw : 0);

        int32_t pos = 0;
        while (pos < end - start) {
            int old = pos;
            uint16_t ch = TextUtils_decodeUtf8(ptxt.text + start, end - start, &pos);
            if (pos == old) {
                pos++;
                continue;
            }

            FontGlyph *g = TextUtils_findGlyph(font, ch);
            if (!g) continue;
            if (!g->sourceWidth || !g->sourceHeight) {
                cx += g->shift;
                continue;
            }

            float lx0 = cx + g->offset, ly0 = cy;
            float lx1 = lx0 + g->sourceWidth, ly1 = ly0 + g->sourceHeight;
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&tr, lx0, ly0, &px0, &py0);
            Matrix4f_transformPoint(&tr, lx1, ly0, &px1, &py1);
            Matrix4f_transformPoint(&tr, lx1, ly1, &px2, &py2);
            Matrix4f_transformPoint(&tr, lx0, ly1, &px3, &py3);

            draw_region(ctx, font->tpagIndex,
                        g->sourceX, g->sourceY, g->sourceWidth, g->sourceHeight,
                        px0, py0, px1, py1, px2, py2, px3, py3, rgb);
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
    (void) c1;
    (void) c2;
    (void) c3;
    (void) c4;
    float old = ren->drawAlpha;
    ren->drawAlpha = a;
    ctr_draw_text(ren, t, x, y, xs, ys, ang);
    ren->drawAlpha = old;
}

// Room cache

static void mark_res(CtrRenderer *ctx, int id) {
    if (id < 0 || (uint32_t) id >= ctx->pageCount) return;
    ctx->pages[id].keepResident = true;
    if ((uint32_t) id < ctx->originalTpagCount && cache_item_available(ctx, (uint32_t) id)) {
        CtrCachedTpag *entry = &ctx->cacheItems[id];
        for (uint32_t i = 0; i < entry->segmentCount; i++) {
            CtrCachedSegment *seg = &ctx->cacheSegments[entry->segmentStart + i];
            if (seg->atlasIndex < ctx->sourcePageCount)
                ctx->sourcePages[seg->atlasIndex].keepResident = true;
        }
    }
}

static void mark_spr(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id < 0 || (uint32_t) id >= dw->sprt.count) return;
    Sprite *s = &dw->sprt.sprites[id];
    for (uint32_t i = 0; i < s->textureCount; i++) mark_res(ctx, s->tpagIndices[i]);
}

static void mark_bg(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id >= 0 && (uint32_t) id < dw->bgnd.count) mark_res(ctx, dw->bgnd.backgrounds[id].tpagIndex);
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
    while (id >= 0 && (uint32_t) id < dw->objt.count && guard++ < 64) {
        GameObject *obj = &dw->objt.objects[id];
        mark_spr(ctx, dw, obj->spriteId);
        id = obj->parentId;
    }
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
            load_page_dyn(ctx, dw, (int32_t) i);
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
            if (load_source_page_dyn(ctx, (int32_t) (ctx->repackBasePageId + i))) {
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
            sourceResidentMB += (double) ctx->sourcePages[i].dataSize / (1024.0 * 1024.0);
        }

        fprintf(stderr,
                "CTR cache: room preload %lu TPAGs -> %lu/%lu atlas textures (%lu new, %lu resident, %.2f MB), %lu dynamic; linear free %.2f MB\n",
                (unsigned long) markedTpagCount,
                (unsigned long) sourceReadyCount,
                (unsigned long) sourceNeedCount,
                (unsigned long) sourceNewCount,
                (unsigned long) sourceResidentCount,
                sourceResidentMB,
                (unsigned long) dynamicLoadCount,
                (double) linearSpaceFree() / (1024.0 * 1024.0));
        free(sourceLoadMap);
    }
}

static void CtrRenderer_prefetchSprites(Renderer *ren, const int32_t *spriteIndices, uint32_t spriteCount) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    for (uint32_t i = 0; i < spriteCount; i++) {
        mark_spr(ctx, ren->dataWin, spriteIndices[i]);
    }
    load_marked_room_pages(ctx, ren->dataWin);
}

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx) {
    CtrRenderer_prefetchSprites(ren, &sprIdx, 1);
}

static void ctr_on_room(Renderer *ren, int32_t rm) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ren->dataWin;
    if (rm < 0 || (uint32_t) rm >= dw->room.count) return;
    Room *room = &dw->room.rooms[rm];

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
    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int id = room->gameObjects[i].objectDefinition;
        mark_obj_and_parents(ctx, dw, id);
    }

    mark_hot_fonts(ctx, dw);

    unload_nonresident_source_pages(ctx);
    load_marked_room_pages(ctx, dw);
}

// Surface API

static int32_t ctr_create_surface(Renderer *ren, int32_t width, int32_t height) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    if (width <= 0 || height <= 0) return -1;
    if (width > 1024) width = 1024;
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
            apply_blend(ctx, ctx->currentBlendMode);
        }

        return (int32_t) slot;
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
        apply_blend(ctx, ctx->currentBlendMode);
    }

    return (int32_t) slot;
}

static void ctr_free_surface(Renderer *ren, int32_t surfaceId) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
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
    return get_surface((CtrRenderer *) ren, surfaceId) != NULL;
}

static bool ctr_surface_get_size(Renderer *ren, int32_t surfaceId, int32_t *w, int32_t *h) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
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
    CtrRenderer *ctx = (CtrRenderer *) ren;
    CtrSurface *surf = get_surface(ctx, surfaceId);
    if (!surf || ctx->targetStackDepth >= CTR_TARGET_STACK_DEPTH) return false;

    flush_batch(ctx);
    C3D_FrameSplit(0);

    CtrTargetState *st = &ctx->targetStack[ctx->targetStackDepth++];
    st->target = ctx->activeTarget ? ctx->activeTarget : ctx->appTarget;
    st->viewport[0] = ctx->currentViewport[0];
    st->viewport[1] = ctx->currentViewport[1];
    st->viewport[2] = ctx->currentViewport[2];
    st->viewport[3] = ctx->currentViewport[3];
    st->projection = ctx->currentProjection;

    bind_target(ctx, surf->target);
    set_viewport_logical(ctx, surf->target, 0, 0, surf->width, surf->height);

    C3D_Mtx proj;
    make_ortho_topleft(&proj, (float) surf->width, (float) surf->height);
    apply_projection(ctx, &proj);
    return true;
}

static void ctr_surface_reset_target(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    if (ctx->targetStackDepth <= 0) return;

    flush_batch(ctx);
    C3D_FrameSplit(0);

    CtrTargetState st = ctx->targetStack[--ctx->targetStackDepth];
    C3D_RenderTarget *tgt = st.target ? st.target : ctx->appTarget;

    bind_target(ctx, tgt);
    set_viewport_logical(ctx, tgt,
                         st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3]);
    apply_projection(ctx, &st.projection);
}

static void ctr_draw_surface(Renderer *ren, int32_t surfaceId,
                             float x, float y, float xscale, float yscale,
                             float angleDeg, uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    float c[4];
    col2fv(color, alpha, c);
    if (c[3] <= 0.f) return;

    C3D_Tex *tex;
    int drawW, drawH, potW, potH;
    C3D_RenderTarget *sourceTarget = NULL;
    if (surfaceId == -1) {
        if (!ctx->appReady) return;
        tex = &ctx->appTex;
        drawW = ctx->appLogicW;
        drawH = ctx->appLogicH;
        potW = ctx->appPotW;
        potH = ctx->appPotH;
        sourceTarget = ctx->appTarget;
    } else {
        CtrSurface *surf = get_surface(ctx, surfaceId);
        if (!surf) return;
        tex = &surf->tex;
        drawW = surf->width;
        drawH = surf->height;
        potW = surf->potW;
        potH = surf->potH;
        sourceTarget = surf->target;
    }
    if (sourceTarget && sourceTarget == ctx->activeTarget) return;

    float w = drawW * xscale, h = drawH * yscale;
    float u1 = (float) drawW / (float) potW;
    float v0 = (float) (potH - drawH) / (float) potH;
    float v1 = 1.f;

    float x0 = 0, y0 = 0;
    float x1 = w, y1 = 0;
    float x2 = w, y2 = h;
    float x3 = 0, y3 = h;

    if (angleDeg != 0.f) {
        float rad = -angleDeg * (float) (M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        float pts[4][2] = {{x0, y0}, {x1, y1}, {x2, y2}, {x3, y3}};
        for (int i = 0; i < 4; i++) {
            float qx = pts[i][0], qy = pts[i][1];
            pts[i][0] = qx * cs - qy * sn;
            pts[i][1] = qx * sn + qy * cs;
        }
        x0 = pts[0][0];
        y0 = pts[0][1];
        x1 = pts[1][0];
        y1 = pts[1][1];
        x2 = pts[2][0];
        y2 = pts[2][1];
        x3 = pts[3][0];
        y3 = pts[3][1];
    }

    if (quad_culled(ctx, x + x0, y + y0, x + x1, y + y1,
                    x + x2, y + y2, x + x3, y + y3))
        return;

    flush_batch(ctx);
    // Surface texture must be fully resolved before sampling; FrameSplit forces a
    // GPU sync point. Use the safe variant — without rebind, the queued push_quad
    // is later drawn with stale BufInfo/AttrInfo, which manifests as missing
    // tiles in tile-heavy rooms (e.g. the Undyne bridge).
    ctr_safe_frame_split(ctx);
    push_quad(ctx, tex,
              x + x0, y + y0, x + x1, y + y1,
              x + x2, y + y2, x + x3, y + y3,
              0.f, v1, u1, v0, c);
}

static void ctr_clear_target(Renderer *ren, uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    flush_batch(ctx);
    if (!ctx->inFrame || !ctx->activeTarget) return;

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color), aa = clamp_u8(alpha);
    uint32_t rgba = ((uint32_t) r << 24) | ((uint32_t) g << 16) | ((uint32_t) b << 8) | aa;
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
    uint32_t oldCount = dw->tpag.count;
    dw->tpag.count++;
    // tpag.items is bigMalloc'd in parseTPAG (linear arena on 3DS) — must
    // grow with bigRealloc, not safeRealloc, or we'd be feeding a linear
    // pointer into the heap allocator and crashing in malloc internals.
    dw->tpag.items = bigRealloc(dw->tpag.items,
                                (size_t)oldCount * sizeof(TexturePageItem),
                                (size_t)dw->tpag.count * sizeof(TexturePageItem));
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
    (void) removeback;
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ren->dataWin;

    if (w <= 0 || h <= 0) return -1;

    C3D_Tex *srcTex;
    int sourcePotW, sourcePotH;

    if (surfaceId == -1) {
        if (!ctx->appReady) return -1;
        srcTex = &ctx->appTex;
        sourcePotW = ctx->appPotW;
        sourcePotH = ctx->appPotH;
    } else {
        CtrSurface *surf = get_surface(ctx, surfaceId);
        if (!surf) return -1;
        srcTex = &surf->tex;
        sourcePotW = surf->potW;
        sourcePotH = surf->potH;
    }

    C3D_RenderTarget *oldTgt = ctx->activeTarget ? ctx->activeTarget : ctx->appTarget;
    C3D_Mtx oldProj = ctx->currentProjection;
    int oldVp[4] = {ctx->currentViewport[0], ctx->currentViewport[1], ctx->currentViewport[2], ctx->currentViewport[3]};
    int oldBlend = ctx->currentBlendMode;

    int potW = next_pow2(w), potH = next_pow2(h);
    CtrAtlasChunk dstChunk;
    memset(&dstChunk, 0, sizeof(dstChunk));

    if (!C3D_TexInitVRAM(&dstChunk.tex, (u16) potW, (u16) potH, GPU_RGBA8)) {
        if (!C3D_TexInit(&dstChunk.tex, (u16) potW, (u16) potH, GPU_RGBA8)) return -1;
    }
    C3D_TexSetFilter(&dstChunk.tex, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&dstChunk.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    C3D_RenderTarget *tmpTarget = C3D_RenderTargetCreateFromTex(&dstChunk.tex, GPU_TEXFACE_2D, 0, -1);
    if (tmpTarget) {
        bind_target(ctx, tmpTarget);
        C3D_RenderTargetClear(tmpTarget, C3D_CLEAR_ALL, 0x00000000, 0);

        flush_batch(ctx);
        C3D_SetViewport(0, 0, w, h);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

        C3D_Mtx proj;
        Mtx_Identity(&proj);
        Mtx_Ortho(&proj, 0.f, (float) w, (float) h, 0.f, -1.f, 1.f, true);
        apply_projection(ctx, &proj);

        float u0 = (float) x / (float) sourcePotW;
        float u1 = (float) (x + w) / (float) sourcePotW;
        float vTop = (float) (sourcePotH - y) / (float) sourcePotH;
        float vBot = (float) (sourcePotH - (y + h)) / (float) sourcePotH;

        float white[4] = {1.f, 1.f, 1.f, 1.f};

        push_quad(ctx, srcTex,
                  0, 0, w, 0,
                  w, h, 0, h,
                  u0, vBot, u1, vTop, white);

        flush_batch(ctx);
        C3D_FrameSplit(0);

        gc_add_target(tmpTarget);

        bind_target(ctx, oldTgt);
        set_viewport_logical(ctx, oldTgt, oldVp[0], oldVp[1], oldVp[2], oldVp[3]);
        apply_projection(ctx, &oldProj);
        apply_blend(ctx, oldBlend);
    } else {
        C3D_TexDelete(&dstChunk.tex);
        return -1;
    }

    dstChunk.valid = true;
    dstChunk.srcX = 0;
    dstChunk.srcY = 0;
    dstChunk.width = w;
    dstChunk.height = h;
    dstChunk.potW = potW;
    dstChunk.potH = potH;

    uint32_t tpagIndex = findOrAllocTpagSlot(ctx);
    TexturePageItem *tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = (int16_t) tpagIndex;

    CtrPage *page = &ctx->pages[tpagIndex];
    memset(page, 0, sizeof(*page));
    page->loaded = true;
    page->keepResident = true;
    page->origW = w;
    page->origH = h;
    page->chunksX = 1;
    page->chunksY = 1;
    page->chunks[0][0] = dstChunk;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, ctx->originalSpriteCount);
    Sprite *sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xo;
    sprite->originY = yo;
    sprite->textureCount = 1;
    sprite->tpagIndices = safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = NULL;

    return (int32_t) spriteIndex;
}

static int32_t ctr_create_surf(Renderer *ren, int32_t x, int32_t y, int32_t w, int32_t h,
                               bool rb, bool sm, int32_t xo, int32_t yo) {
    return ctr_create_surf_ex(ren, -1, x, y, w, h, rb, sm, xo, yo);
}

static void ctr_del_sprite(Renderer *ren, int32_t spriteIndex) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ren->dataWin;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return;
    if ((uint32_t) spriteIndex < ctx->originalSpriteCount) return;

    Sprite *sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    flush_batch(ctx);

    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx < 0 || (uint32_t) tpagIdx < ctx->originalTpagCount) continue;

        TexturePageItem *tpag = &dw->tpag.items[tpagIdx];
        if ((uint32_t) tpagIdx < ctx->pageCount) {
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
    apply_blend((CtrRenderer *) ren, mode);
}

static void ctr_gpu_blend_mode_ext(Renderer *ren, int32_t sfactor, int32_t dfactor) {
    (void) ren;
    (void) sfactor;
    (void) dfactor;
}

static void ctr_gpu_blend_enable(Renderer *ren, bool enable) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    if (enable) {
        apply_blend(ctx, ctx->currentBlendMode);
    } else {
        flush_batch(ctx);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
}

static void ctr_gpu_alpha_test_enable(Renderer *ren, bool enable) {
    (void) ren;
    (void) enable;
}

static void ctr_gpu_alpha_test_ref(Renderer *ren, uint8_t ref) {
    (void) ren;
    (void) ref;
}

static void ctr_gpu_color_write_enable(Renderer *ren, bool r, bool g, bool b, bool a) {
    (void) ren;
    (void) r;
    (void) g;
    (void) b;
    (void) a;
}

static void ctr_set_blend(Renderer *ren, int32_t mode) {
    apply_blend((CtrRenderer *) ren, mode);
}

void CtrRenderer_beginEye(Renderer *ren, int eye, float slider) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    ctx->currentEye = eye;
    ctx->depthSlider = slider;
    ctx->current3DDepth = 0;

    // Choose active dynamic frame canvas buffer correctly mapping 3DS outputs gracefully cleanly evaluating dependencies safely intuitively:
    C3D_RenderTarget *currentDestTarget = (eye == 1) ? ctx->appTargetRight : ctx->appTarget;

    if (currentDestTarget != NULL && ctx->inFrame) {
        // Swap out buffer states smoothly natively protecting rendering engines executing blocks organically resolving components effortlessly perfectly over scopes correctly
        if (ctx->activeTarget != currentDestTarget) {
            flush_batch(ctx);
            C3D_FrameSplit(0);
            C3D_FrameDrawOn(currentDestTarget);
            ctx->activeTarget = currentDestTarget;
            rebind_state(ctx);
            apply_projection(ctx, &ctx->currentProjection);
        }
    }
}

static void ctr_set_3d_depth(Renderer *ren, float depth) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    ctx->current3DDepth = depth;

    // UI drawing bypasses parallax shift dynamically reliably smoothly over instances rendering optimally intelligently safely!
    if (ctx->isGUI || ctx->depthSlider <= 0.01f) {
        ctx->currentShiftX = 0.0f;
        return;
    }

    // Typical mapped generic GameMaker / Undertale relative distances calculation logic to handle objects visually perfectly. Depth logic usually scales around 1500~2000 offset divisors securely flawlessly over routines generating depths nicely evaluating environments:
    // depth > 0 уходит вдаль; depth < 0 выходит наружу (например объекты или герой). У Undertale очень большие depth поэтому ставим девайдер 3000+.
    float d = ctx->current3DDepth / 3000.0f;

    // Clamp limits tightly maximizing comfort over vision scopes gracefully successfully cleanly cleanly handling properties nicely safely resolving outputs efficiently intelligently perfectly.
    if (d > 4.0f) d = 4.0f;
    if (d < -4.0f) d = -4.0f;

    // Mapped horizontal pixel parallax intensity calculation
    float separation = d * 2.0f * ctx->depthSlider;

    // Negative distances = shift left for Left eye, shift right for right eye evaluating values correctly handling logic efficiently mapping context gracefully generating frames intelligently cleanly safely correctly.
    ctx->currentShiftX = (ctx->currentEye == 0) ? -separation : separation;
}


// Renderer vtable

static RendererVtable vtable = {
    .init = ctr_init, .destroy = ctr_destroy,
    .beginFrame = ctr_begin_frame, .endFrame = ctr_end_frame,
    .beginView = ctr_begin_view, .endView = ctr_end_view,
    .beginGUI = ctr_begin_gui, .endGUI = ctr_end_gui,
    .drawSprite = ctr_draw_sprite,
    .drawSpritePart = ctr_draw_sprite_part,
    .drawSpritePos = ctr_draw_sprite_pos,
    .drawRectangle = ctr_draw_rect,
    .drawRectangleColor = ctr_draw_rect_c,
    .drawLine = ctr_draw_line, .drawLineColor = ctr_draw_line_c,
    .drawTriangle = ctr_draw_tri, .drawTriangleColor = ctr_draw_tri_c,
    .drawText = ctr_draw_text, .drawTextColor = ctr_draw_text_c,
    .flush = ctr_flush,
    .prefetchSprite = CtrRenderer_prefetchSprite,
    .prefetchSprites = CtrRenderer_prefetchSprites,
    .createSpriteFromSurface = ctr_create_surf,
    .createSpriteFromSurfaceEx = ctr_create_surf_ex,
    .deleteSprite = ctr_del_sprite,
    .createSurface = ctr_create_surface, .freeSurface = ctr_free_surface,
    .surfaceExists = ctr_surface_exists, .surfaceGetSize = ctr_surface_get_size,
    .surfaceSetTarget = ctr_surface_set_target, .surfaceResetTarget = ctr_surface_reset_target,
    .drawSurface = ctr_draw_surface, .clearTarget = ctr_clear_target,
    .gpuSetBlendMode = ctr_gpu_blend_mode,
    .gpuSetBlendModeExt = ctr_gpu_blend_mode_ext,
    .gpuSetBlendEnable = ctr_gpu_blend_enable,
    .gpuSetAlphaTestEnable = ctr_gpu_alpha_test_enable,
    .gpuSetAlphaTestRef = ctr_gpu_alpha_test_ref,
    .gpuSetColorWriteEnable = ctr_gpu_color_write_enable,
    .drawTile = ctr_draw_tile, .drawTiled = ctr_draw_tiled,
    .drawCircle = ctr_draw_circle, .drawEllipse = ctr_draw_ellipse,
    .drawRoundrect = ctr_draw_rr,
    .onRoomChanged = ctr_on_room,
    .set3DDepthOffset = ctr_set_3d_depth, .setBlendMode = ctr_set_blend,
};

Renderer *CtrRenderer_create(void) {
    CtrRenderer *ctx = calloc(1, sizeof(CtrRenderer));
    ctx->base.vtable = &vtable;
    ctx->base.drawColor = 0xFFFFFF;
    ctx->base.drawAlpha = 1.0f;
    ctx->base.drawFont = -1;
    ctx->base.circlePrecision = 36;
    return (Renderer *) ctx;
}
