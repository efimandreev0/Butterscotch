//
// Created by efimandreev0 on 01.05.2026.
//
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
#include "utils.h"

#include "render2d_shader_shbin.h"

extern char g_current_cache_dir[256];

#define BATCH_QUAD_CAP        2048
#define BATCH_VERT_CAP        (BATCH_QUAD_CAP * 6)
#define ATLAS_MAGIC           0x534C5441
#define MAX_RR_SEGMENTS       64
#define MAX_RR_POINTS         (MAX_RR_SEGMENTS * 4 + 1)
#define LINEAR_LOW            (1024u * 1024u)
#define LINEAR_SAFE           (2u * 1024u * 1024u)
#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define MAX_GC_TARGETS 64
static C3D_RenderTarget* g_gc_targets[MAX_GC_TARGETS];
static int g_gc_target_count = 0;

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
        C3D_RenderTargetDelete(tgt); // На случай переполнения
    }
}

typedef struct {
    uint32_t magic;
    uint32_t w, h;
} AtlasHeader;

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

static void build_texture_cache(CtrRenderer *ctx) {
    DataWin *dw = ctx->base.dataWin;
    char flagFile[256];
    snprintf(flagFile, sizeof(flagFile), "%s/cache_ready.flag", g_current_cache_dir);

    FILE *f = fopen(flagFile, "r");
    if (f) { fclose(f); return; }

    FILE *dwFile = dw->filePath ? fopen(dw->filePath, "rb") : NULL;
    if (dwFile) setvbuf(dwFile, NULL, _IOFBF, 256 * 1024);

    for (uint32_t i = 0; i < dw->txtr.count; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/page_%u.atlas", g_current_cache_dir, i);
        if (g_cacheProgressCallback) {
            g_cacheProgressCallback(i, dw->txtr.count, path, g_cacheProgressUser);
        }

        FILE *check = fopen(path, "r");
        if (check) { fclose(check); continue; }

        Texture *t = &dw->txtr.textures[i];
        if (!t->blobSize) continue;

        uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
        if (!blob) continue;

        int w, h;
        uint8_t *pixels = ImageDecoder_decodeToRgba(
            blob, t->blobSize, DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), &w, &h);
        free(blob);

        if (pixels) {
            uint16_t *out = malloc((size_t)w * h * 2);
            for (int p = 0; p < w * h; p++) {
                int s = p * 4;
                out[p] = pack_rgba4444(pixels[s], pixels[s + 1], pixels[s + 2], pixels[s + 3]);
            }
            FILE *outF = fopen(path, "wb");
            if (outF) {
                setvbuf(outF, NULL, _IOFBF, 256 * 1024);
                AtlasHeader hdr = {ATLAS_MAGIC, (uint32_t)w, (uint32_t)h};
                fwrite(&hdr, sizeof(hdr), 1, outF);
                fwrite(out, 1, (size_t)w * h * 2, outF);
                fclose(outF);
            }
            free(out);
            free(pixels);
        }
    }
    if (dwFile) fclose(dwFile);

    if (g_cacheProgressCallback) {
        g_cacheProgressCallback(dw->txtr.count, dw->txtr.count, NULL, g_cacheProgressUser);
    }

    FILE *outFlag = fopen(flagFile, "w");
    if (outFlag) { fputs("READY", outFlag); fclose(outFlag); }
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
        C3D_FrameSplit(0);
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
    CtrVertex *v = vbuf_reserve(ctx, 3, &ctx->whiteTex);
    v[0] = (CtrVertex){x1, y1, 0, .5f, .5f, c1[0], c1[1], c1[2], c1[3]};
    v[1] = (CtrVertex){x2, y2, 0, .5f, .5f, c2[0], c2[1], c2[2], c2[3]};
    v[2] = (CtrVertex){x3, y3, 0, .5f, .5f, c3[0], c3[1], c3[2], c3[3]};
}

static void draw_letterbox_gradient(CtrRenderer *ctx) {
    float t = (float)g_frame * 0.025f;
    float a = sinf(t) * 0.5f + 0.5f;
    float b = sinf(t * 0.73f + 2.1f) * 0.5f + 0.5f;

    float c0[4] = {0.025f + 0.030f * a, 0.035f + 0.035f * b, 0.115f + 0.055f * a, 1.f};
    float c1[4] = {0.105f + 0.050f * b, 0.035f + 0.020f * a, 0.145f + 0.040f * b, 1.f};
    float c2[4] = {0.035f + 0.035f * b, 0.125f + 0.060f * a, 0.150f + 0.050f * b, 1.f};
    float c3[4] = {0.115f + 0.055f * a, 0.075f + 0.030f * b, 0.030f + 0.030f * a, 1.f};

    float x[4] = {0.f, (float)ctx->winW, (float)ctx->winW, 0.f};
    float y[4] = {0.f, 0.f, (float)ctx->winH, (float)ctx->winH};
    float colors[4][4] = {
        {c0[0], c0[1], c0[2], c0[3]},
        {c1[0], c1[1], c1[2], c1[3]},
        {c2[0], c2[1], c2[2], c2[3]},
        {c3[0], c3[1], c3[2], c3[3]},
    };
    push_quad_uvgrad(ctx, &ctx->whiteTex, x, y, .5f, .5f, .5f, .5f, colors);

    for (int i = 0; i < 18; i++) {
        float seed = (float)i * 15.37f;
        float px = fmodf(seed * 19.1f + t * (18.f + (float)(i % 4) * 7.f), (float)ctx->winW + 48.f) - 24.f;
        float py = fmodf(seed * 11.3f + sinf(t + seed) * 16.f, (float)ctx->winH + 32.f) - 16.f;
        float s = 1.2f + (float)(i % 3) * 0.8f;
        float alpha = 0.08f + 0.10f * (sinf(t * 1.7f + seed) * 0.5f + 0.5f);
        float pc[4] = {0.9f, 0.72f + 0.18f * a, 0.28f + 0.35f * b, alpha};
        push_quad(ctx, &ctx->whiteTex, px, py, px + s, py, px + s, py + s, px, py + s,
                  .5f, .5f, .5f, .5f, pc);
    }
}

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
        if (victim < 0) break;

        if (!flushed) { flush_batch(ctx); flushed = true; }

        for (int cx = 0; cx < ctx->pages[victim].chunksX; cx++) {
            for (int cy = 0; cy < ctx->pages[victim].chunksY; cy++) {
                CtrAtlasChunk *ch = &ctx->pages[victim].chunks[cx][cy];
                if (ch->valid) { C3D_TexDelete(&ch->tex); ch->valid = false; }
            }
        }
        ctx->pages[victim].loaded = false;
        evicted++;
    }
}

static void extract_page_file(CtrRenderer *ctx, DataWin *dw, uint32_t id, FILE *f, int aw, int ah) {
    TexturePageItem *item = &dw->tpag.items[id];
    int w = item->sourceWidth  > 0 ? item->sourceWidth  : 1;
    int h = item->sourceHeight > 0 ? item->sourceHeight : 1;

    CtrPage *page = &ctx->pages[id];
    page->origW = w;
    page->origH = h;
    page->chunksX = (int)fminf((float)((w + 1023) / 1024), CTR_MAX_CHUNKS_X);
    page->chunksY = (int)fminf((float)((h + 1023) / 1024), CTR_MAX_CHUNKS_Y);

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            CtrAtlasChunk *chunk = &page->chunks[cx][cy];
            chunk->valid  = false;
            chunk->srcX   = cx * 1024;
            chunk->srcY   = cy * 1024;
            chunk->width  = (int)fminf((float)(w - chunk->srcX), 1024.f);
            chunk->height = (int)fminf((float)(h - chunk->srcY), 1024.f);
            chunk->potW   = next_pow2(chunk->width);
            chunk->potH   = next_pow2(chunk->height);

            uint16_t *linear = calloc((size_t)chunk->potW * chunk->potH, 2);
            if (!linear) continue;
            for (int y = 0; y < chunk->height; y++) {
                int sy = item->sourceY + chunk->srcY + y;
                if (sy < 0 || sy >= ah) continue;
                fseek(f, sizeof(AtlasHeader) +
                      ((long)sy * aw + item->sourceX + chunk->srcX) * 2, SEEK_SET);
                fread(&linear[y * chunk->potW], 2, (size_t)chunk->width, f);
            }

            if (!C3D_TexInit(&chunk->tex, (u16)chunk->potW, (u16)chunk->potH, GPU_RGBA4)) {
                free(linear);
                continue;
            }
            C3D_TexSetFilter(&chunk->tex, GPU_LINEAR, GPU_NEAREST);
            C3D_TexSetWrap  (&chunk->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

            uint32_t tiled_size = (uint32_t)chunk->potW * chunk->potH * 2;
            uint16_t *tiled = linearAlloc(tiled_size);
            if (!tiled) {
                free(linear);
                C3D_TexDelete(&chunk->tex);
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
    page->loaded = true;
}

static __attribute__((aligned(8))) char dyn_buf[64 * 1024];

static void load_page_dyn(CtrRenderer *ctx, DataWin *dw, int32_t idx) {
    if (idx < 0 || (uint32_t)idx >= ctx->pageCount || ctx->pages[idx].loaded) return;
    free_old_pages(ctx);

    uint32_t pageId = (uint32_t)dw->tpag.items[idx].texturePageId;
    char path[256];
    snprintf(path, sizeof(path), "%s/page_%u.atlas", g_current_cache_dir, pageId);

    FILE *f = fopen(path, "rb");
    if (!f) return;
    setvbuf(f, dyn_buf, _IOFBF, sizeof(dyn_buf));

    AtlasHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
        extract_page_file(ctx, dw, (uint32_t)idx, f, hdr.w, hdr.h);
    }
    fclose(f);
}

static void setup_pipeline(CtrRenderer *ctx) {
    if (ctx->pipelineReady) return;

    ctx->vshaderDvlb = DVLB_ParseFile((u32 *)render2d_shader_shbin, render2d_shader_shbin_size);
    shaderProgramInit(&ctx->shaderProg);
    shaderProgramSetVsh(&ctx->shaderProg, &ctx->vshaderDvlb->DVLE[0]);
    C3D_BindProgram(&ctx->shaderProg);
    ctx->uLoc_projection = shaderInstanceGetUniformLocation(
        ctx->shaderProg.vertexShader, "projection");

    AttrInfo_Init(&ctx->attrInfo);
    AttrInfo_AddLoader(&ctx->attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ctx->attrInfo, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ctx->attrInfo, 2, GPU_FLOAT, 4);
    C3D_SetAttrInfo(&ctx->attrInfo);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc (env, C3D_Both,  GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both,  GPU_MODULATE);

    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_CullFace (GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    ctx->currentBlendMode = 0;
    ctx->pipelineReady    = true;
}

static void rebind_state(CtrRenderer *ctx) {
    C3D_BindProgram(&ctx->shaderProg);
    C3D_SetAttrInfo(&ctx->attrInfo);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, ctx->vbuf, sizeof(CtrVertex), 3, 0x210);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc (env, C3D_Both,  GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both,  GPU_MODULATE);

    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_CullFace (GPU_CULL_NONE);
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
    C3D_TexSetFilter(&ctx->appTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap  (&ctx->appTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    ctx->appTarget = C3D_RenderTargetCreateFromTex(&ctx->appTex, GPU_TEXFACE_2D, 0,
                                                   -1);
    if (!ctx->appTarget) {
        C3D_TexDelete(&ctx->appTex);
        memset(&ctx->appTex, 0, sizeof(ctx->appTex));
        return false;
    }
    ctx->appReady = true;
    return true;
}

static void bind_target(CtrRenderer *ctx, C3D_RenderTarget *tgt) {
    if (!ctx->inFrame) return;
    flush_batch(ctx);
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

    ctx->pageCount = dw->tpag.count;
    ctx->pages     = calloc(ctx->pageCount, sizeof(CtrPage));

    ctx->originalTpagCount   = dw->tpag.count;
    ctx->originalSpriteCount = dw->sprt.count;

    if (!ctx->vbuf) {
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

    build_texture_cache(ctx);
}

static void ctr_destroy(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;

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

    if (ctx->whiteTex.data) C3D_TexDelete(&ctx->whiteTex);

    if (ctx->vbuf) { linearFree(ctx->vbuf); ctx->vbuf = NULL; }

    if (ctx->pipelineReady) {
        shaderProgramFree(&ctx->shaderProg);
        DVLB_Free(ctx->vshaderDvlb);
        ctx->pipelineReady = false;
    }

    if (ctx->topTarget)    { C3D_RenderTargetDelete(ctx->topTarget);    ctx->topTarget    = NULL; }
    if (ctx->bottomTarget) { C3D_RenderTargetDelete(ctx->bottomTarget); ctx->bottomTarget = NULL; }

    free(ctx);
}

static void ctr_begin_frame(Renderer *ren, int32_t gw, int32_t gh, int32_t ww, int32_t wh) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    gc_clear_targets();
    ctx->winW  = ww;
    ctx->winH  = wh;
    ctx->gameW = gw;
    ctx->gameH = gh;

    if (!ensure_app_surface(ctx, gw, gh)) return;

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
}

static void ctr_end_frame(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);

    if (!ctx->inFrame) return;

    if (ctx->appReady && ctx->topTarget) {
        C3D_FrameDrawOn(ctx->topTarget);
        ctx->activeTarget = ctx->topTarget;
        rebind_state(ctx);

        C3D_RenderTargetClear(ctx->topTarget, C3D_CLEAR_ALL, 0x050711FF, 0);
        C3D_SetViewport(0, 0, 240, 400);
        disable_scissor(ctx);

        C3D_Mtx proj;
        make_ortho_top(&proj, (float)ctx->winW, (float)ctx->winH);
        apply_projection(ctx, &proj);

        draw_letterbox_gradient(ctx);

        int drawW, drawH;
        if ((ctx->gameW * ctx->winH) / ctx->gameH < ctx->winW) {
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
}

static void ctr_end_view(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    disable_scissor(ctx);
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
}

static void ctr_end_gui(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    disable_scissor(ctx);
}

static void draw_region(CtrRenderer *ctx, uint32_t id,
                        float sx, float sy, float sw, float sh,
                        float x0, float y0, float x1, float y1,
                        float x2, float y2, float x3, float y3,
                        const float col[4]) {
    if (id >= ctx->pageCount) return;
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

            float u0 = (dL - c->srcX) / (float)c->potW;
            float v0 = (dT - c->srcY) / (float)c->potH;
            float u1 = (dR - c->srcX) / (float)c->potW;
            float v1 = (dB - c->srcY) / (float)c->potH;

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
                                 uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    float c[4]; col2fv(color, alpha, c);
    if (c[3] <= 0.f) return;
    draw_region(ctx, (uint32_t)id, sx, sy, sw, sh,
                x,                  y,
                x + sw * xscale,    y,
                x + sw * xscale,    y + sh * yscale,
                x,                  y + sh * yscale,  c);
}

static void ctr_draw_sprite_pos(Renderer *ren, int32_t id,
                                float x1, float y1, float x2, float y2,
                                float x3, float y3, float x4, float y4, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    float c[4]; col2fv(ren->drawColor, alpha, c);
    if (c[3] <= 0.f) return;
    TexturePageItem *item = &ren->dataWin->tpag.items[id];
    draw_region(ctx, (uint32_t)id, 0, 0, item->sourceWidth, item->sourceHeight,
                x1, y1, x2, y2, x3, y3, x4, y4, c);
}

static void ctr_draw_tile(Renderer *ren, RoomTile *tile, float ox, float oy) {
    int32_t id = Renderer_resolveObjectTPAGIndex(ren->dataWin, tile);
    if (id < 0) return;

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
                                tile->color & 0xFFFFFF, a == 0 ? 1.f : a / 255.f);
}

static void ctr_draw_tiled(Renderer *ren, int32_t id, float ox, float oy,
                           float x, float y, float sx, float sy,
                           bool tx, bool ty, float rw, float rh,
                           uint32_t col, float a) {
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

    for (float dy = sY; dy < eY; dy += th) {
        for (float dx = sX; dx < eX; dx += tw) {
            ren->vtable->drawSprite(ren, id, dx + ox * fabsf(sx), dy + oy * fabsf(sy),
                                    ox, oy, sx, sy, 0.f, col, a);
        }
    }
}

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

static void ctr_draw_text(Renderer *ren, const char *txt, float x, float y,
                          float sx, float sy, float ang) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;
    int fidx = ren->drawFont;
    if (fidx < 0 || fidx >= dw->font.count) return;

    Font *font = &dw->font.fonts[fidx];
    if (font->tpagIndex < 0 || (uint32_t)font->tpagIndex >= ctx->pageCount) return;

    if (!ctx->pages[font->tpagIndex].loaded) load_page_dyn(ctx, dw, font->tpagIndex);
    if (!ctx->pages[font->tpagIndex].loaded) return;

    float rgb[4]; col2fv(ren->drawColor, ren->drawAlpha, rgb);
    if (rgb[3] <= 0.f) return;

    PreprocessedText ptxt = TextUtils_preprocessGmlText(txt);
    if (!ptxt.text[0]) { PreprocessedText_free(ptxt); return; }

    int len = strlen(ptxt.text);
    float stride = font->emSize > 0 ? font->emSize : 10.f;
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

            FontGlyph *g = TextUtils_findGlyph(font, ch);
            if (!g) continue;
            if (!g->sourceWidth || !g->sourceHeight) { cx += g->shift; continue; }

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
    (void)c1; (void)c2; (void)c3; (void)c4;
    float old = ren->drawAlpha;
    ren->drawAlpha = a;
    ctr_draw_text(ren, t, x, y, xs, ys, ang);
    ren->drawAlpha = old;
}

static void mark_res(CtrRenderer *ctx, int id) {
    if (id >= 0 && (uint32_t)id < ctx->pageCount) ctx->pages[id].keepResident = true;
}

static void mark_spr(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id < 0 || (uint32_t)id >= dw->sprt.count) return;
    Sprite *s = &dw->sprt.sprites[id];
    for (uint32_t i = 0; i < s->textureCount; i++) mark_res(ctx, s->tpagIndices[i]);
}

static void mark_bg(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id >= 0 && (uint32_t)id < dw->bgnd.count) mark_res(ctx, dw->bgnd.backgrounds[id].tpagIndex);
}

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx) {
    mark_spr((CtrRenderer *)ren, ren->dataWin, sprIdx);
}

static void ctr_on_room(Renderer *ren, int32_t rm) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    DataWin *dw = ren->dataWin;
    if (rm < 0 || (uint32_t)rm >= dw->room.count) return;
    Room *room = &dw->room.rooms[rm];

    for (uint32_t i = 0; i < ctx->originalTpagCount && i < ctx->pageCount; i++)
        ctx->pages[i].keepResident = false;
    for (uint32_t i = 0; i < dw->font.count; i++)
        mark_res(ctx, dw->font.fonts[i].tpagIndex);

    if (room->backgrounds) {
        for (int i = 0; i < 8; i++)
            if (room->backgrounds[i].enabled)
                mark_bg(ctx, dw, room->backgrounds[i].backgroundDefinition);
    }
    for (uint32_t i = 0; i < room->tileCount; i++) {
        int id = room->tiles[i].backgroundDefinition;
        if (room->tiles[i].useSpriteDefinition) mark_spr(ctx, dw, id);
        else mark_bg(ctx, dw, id);
    }
    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int id = room->gameObjects[i].objectDefinition;
        if (id >= 0 && (uint32_t)id < dw->objt.count) {
            mark_spr(ctx, dw, dw->objt.objects[id].spriteId);
            int p = dw->objt.objects[id].parentId;
            if (p >= 0 && (uint32_t)p < dw->objt.count)
                mark_spr(ctx, dw, dw->objt.objects[p].spriteId);
        }
    }

    bool loadMap[256] = {0};
    for (uint32_t i = 0; i < ctx->pageCount; i++) {
        if (ctx->pages[i].keepResident && !ctx->pages[i].loaded)
            loadMap[dw->tpag.items[i].texturePageId & 0xFF] = true;
    }

    for (int p = 0; p < 256; p++) {
        if (!loadMap[p]) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/page_%d.atlas", g_current_cache_dir, p);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        setvbuf(f, dyn_buf, _IOFBF, sizeof(dyn_buf));

        AtlasHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
            for (uint32_t i = 0; i < ctx->pageCount; i++) {
                if (dw->tpag.items[i].texturePageId == (int)p &&
                    ctx->pages[i].keepResident && !ctx->pages[i].loaded)
                    extract_page_file(ctx, dw, i, f, hdr.w, hdr.h);
            }
        }
        fclose(f);
    }
}

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
        if (ctx->inFrame) C3D_FrameSplit(0);

        C3D_RenderTarget *oldTgt = ctx->activeTarget;
        C3D_FrameDrawOn(surf->target);
        C3D_RenderTargetClear(surf->target, C3D_CLEAR_ALL, 0x00000000, 0);
        if (oldTgt) C3D_FrameDrawOn(oldTgt);

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
    if (ctx->inFrame) C3D_FrameSplit(0);
    C3D_RenderTarget *oldTgt = ctx->activeTarget;
    C3D_FrameDrawOn(surf->target);
    C3D_RenderTargetClear(surf->target, C3D_CLEAR_ALL, 0x00000000, 0);
    if (oldTgt) C3D_FrameDrawOn(oldTgt);

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
    if (!surf || ctx->targetStackDepth >= CTR_TARGET_STACK_DEPTH) return false;

    flush_batch(ctx);
    C3D_FrameSplit(0);

    CtrTargetState *st = &ctx->targetStack[ctx->targetStackDepth++];
    st->target     = ctx->activeTarget ? ctx->activeTarget : ctx->appTarget;
    st->viewport[0]= ctx->currentViewport[0];
    st->viewport[1]= ctx->currentViewport[1];
    st->viewport[2]= ctx->currentViewport[2];
    st->viewport[3]= ctx->currentViewport[3];
    st->projection = ctx->currentProjection;

    bind_target(ctx, surf->target);
    set_viewport_logical(ctx, surf->target, 0, 0, surf->width, surf->height);

    C3D_Mtx proj;
    make_ortho_topleft(&proj, (float)surf->width, (float)surf->height);
    apply_projection(ctx, &proj);
    return true;
}

static void ctr_surface_reset_target(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
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
    CtrRenderer *ctx = (CtrRenderer *)ren;
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

    flush_batch(ctx);
    C3D_FrameSplit(0);
    push_quad(ctx, tex,
              x + x0, y + y0,  x + x1, y + y1,
              x + x2, y + y2,  x + x3, y + y3,
              0.f, v1, u1, v0, c);
}

static void ctr_clear_target(Renderer *ren, uint32_t color, float alpha) {
    CtrRenderer *ctx = (CtrRenderer *)ren;
    flush_batch(ctx);
    if (!ctx->inFrame || !ctx->activeTarget) return;

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color), aa = clamp_u8(alpha);
    uint32_t rgba = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | aa;
    C3D_FrameSplit(0);
    C3D_RenderTargetClear(ctx->activeTarget, C3D_CLEAR_ALL, rgba, 0);
}

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

    C3D_RenderTarget *oldTgt = ctx->activeTarget ? ctx->activeTarget : ctx->appTarget;
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

static void ctr_set_blend(Renderer *ren, int32_t mode) {
    apply_blend((CtrRenderer *)ren, mode);
}

static void ctr_set_3d_depth(Renderer *ren, float depth) {
    (void)ren; (void)depth;
}

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
    .createSpriteFromSurface = ctr_create_surf,
    .createSpriteFromSurfaceEx = ctr_create_surf_ex,
    .deleteSprite = ctr_del_sprite,
    .createSurface = ctr_create_surface,         .freeSurface = ctr_free_surface,
    .surfaceExists = ctr_surface_exists,         .surfaceGetSize = ctr_surface_get_size,
    .surfaceSetTarget = ctr_surface_set_target,  .surfaceResetTarget = ctr_surface_reset_target,
    .drawSurface = ctr_draw_surface,             .clearTarget = ctr_clear_target,
    .drawTile = ctr_draw_tile,                   .drawTiled = ctr_draw_tiled,
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
