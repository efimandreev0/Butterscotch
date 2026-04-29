#include "ctr_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <3ds.h>
#include <malloc.h>
#include "stb_image.h"
#include "image_decoder.h"
#include "utils.h"

#define BATCH_CAP 2048
#define MAX_RR_SEGMENTS 64
#define MAX_RR_POINTS (MAX_RR_SEGMENTS * 4 + 1)
#define ATLAS_MAGIC 0x534C5441

typedef struct {
    uint32_t magic;
    uint32_t w, h;
} AtlasHeader;

static uint32_t g_frame = 0;

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} VertexData;

//utils
static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static int next_pow2(int x) {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
    return x < 8 ? 8 : x;
}

static inline uint8_t f2b_alpha(float a) {
    return (uint8_t) (fmaxf(0.f, fminf(1.f, a)) * 255.f);
}

static void col2rgb(uint32_t col, float a, uint8_t *out) {
    out[0] = BGR_R(col);
    out[1] = BGR_G(col);
    out[2] = BGR_B(col);
    out[3] = f2b_alpha(a);
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

//gen cache on first boot
static void build_texture_cache(CtrRenderer *ctx) {
    DataWin *dw = ctx->base.dataWin;
    const char *flagFile = "sdmc:/3ds/butterscotch/cache/cache_ready.flag";

    FILE *f = fopen(flagFile, "r");
    if (f) {
        fclose(f);
        return;
    }

    FILE *dwFile = dw->filePath ? fopen(dw->filePath, "rb") : NULL;
    if (dwFile) setvbuf(dwFile, NULL, _IOFBF, 256 * 1024);

    for (uint32_t i = 0; i < dw->txtr.count; i++) {
        char path[256];
        snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/page_%u.atlas", i);

        FILE *check = fopen(path, "r");
        if (check) {
            fclose(check);
            continue;
        }

        Texture *t = &dw->txtr.textures[i];
        if (!t->blobSize) continue;

        uint8_t *blob = read_blob(dwFile, t->blobOffset, t->blobSize);
        if (!blob) continue;

        int w, h;
        uint8_t *pixels = ImageDecoder_decodeToRgba(blob, t->blobSize, DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), &w,
                                                    &h);
        free(blob);

        if (pixels) {
            uint16_t *out = malloc(w * h * 2);
            for (int p = 0; p < w * h; p++) {
                int src = p * 4;
                out[p] = pack_rgba4444(pixels[src], pixels[src + 1], pixels[src + 2], pixels[src + 3]);
            }

            FILE *outF = fopen(path, "wb");
            if (outF) {
                setvbuf(outF, NULL, _IOFBF, 256 * 1024);
                AtlasHeader hdr = {ATLAS_MAGIC, w, h};
                fwrite(&hdr, sizeof(hdr), 1, outF);
                fwrite(out, 1, w * h * 2, outF);
                fclose(outF);
            }
            free(out);
            free(pixels);
        }
    }
    if (dwFile) fclose(dwFile);

    FILE *outFlag = fopen(flagFile, "w");
    if (outFlag) {
        fputs("READY", outFlag);
        fclose(outFlag);
    }
}

//core drawing layer
static void flush_batch(CtrRenderer *ctx) {
    if (!ctx->batchCount || !ctx->batchTex) return;
    glBindTexture(GL_TEXTURE_2D, ctx->batchTex);
    glDrawArrays(GL_TRIANGLES, 0, ctx->batchCount * 6);
    ctx->batchCount = 0;
    ctx->batchTex = 0;
}

static void push_quad_grad(CtrRenderer *ctx, GLuint tex, float *x, float *y, float u0, float v0, float u1, float v1,
                           uint8_t *c) {
    if (ctx->batchCount > 0 && ctx->batchTex != tex) flush_batch(ctx);
    if (ctx->batchCount >= ctx->batchCap) flush_batch(ctx);

    ctx->batchTex = tex;
    VertexData *v = (VertexData *) ctx->batchVerts + ctx->batchCount * 6;

    v[0] = (VertexData){x[0], y[0], 0, u0, v0, c[0], c[1], c[2], c[3]};
    v[1] = (VertexData){x[1], y[1], 0, u1, v0, c[4], c[5], c[6], c[7]};
    v[2] = (VertexData){x[2], y[2], 0, u1, v1, c[8], c[9], c[10], c[11]};
    v[3] = v[0];
    v[4] = v[2];
    v[5] = (VertexData){x[3], y[3], 0, u0, v1, c[12], c[13], c[14], c[15]};

    ctx->batchCount++;
}

static void push_quad(CtrRenderer *ctx, GLuint tex, float x0, float y0, float x1, float y1, float x2, float y2,
                      float x3, float y3, float u0, float v0, float u1, float v1, uint8_t *col) {
    float x[4] = {x0, x1, x2, x3};
    float y[4] = {y0, y1, y2, y3};
    uint8_t c[16] = {
        col[0], col[1], col[2], col[3], col[0], col[1], col[2], col[3], col[0], col[1], col[2], col[3], col[0], col[1],
        col[2], col[3]
    };
    push_quad_grad(ctx, tex, x, y, u0, v0, u1, v1, c);
}

static void push_tri_grad(CtrRenderer *ctx, float x1, float y1, float x2, float y2, float x3, float y3, uint8_t *c1,
                          uint8_t *c2, uint8_t *c3) {
    float x[4] = {x1, x2, x3, x3};
    float y[4] = {y1, y2, y3, y3};
    uint8_t c[16] = {
        c1[0], c1[1], c1[2], c1[3], c2[0], c2[1], c2[2], c2[3], c3[0], c3[1], c3[2], c3[3], c3[0], c3[1], c3[2], c3[3]
    };
    push_quad_grad(ctx, ctx->whiteTex, x, y, 0.5f, 0.5f, 0.5f, 0.5f, c);
}

//memory mngmnt
#define LINEAR_LOW (1024u * 1024u)
#define LINEAR_SAFE (2u * 1024u * 1024u)

static void free_old_pages(CtrRenderer *ctx) {
    if (linearSpaceFree() >= LINEAR_LOW) return;
    bool flushed = false;
    int evicted = 0;

    while (evicted < 32 && linearSpaceFree() < LINEAR_SAFE) {
        uint32_t oldest = UINT32_MAX;
        int victim = -1;

        for (uint32_t i = 0; i < ctx->pageCount; i++) {
            if (!ctx->pages[i].loaded || ctx->pages[i].keepResident || ctx->pages[i].lastFrame >= g_frame) continue;
            if (ctx->pages[i].lastFrame < oldest) {
                oldest = ctx->pages[i].lastFrame;
                victim = i;
            }
        }
        if (victim < 0) break;

        if (!flushed) {
            flush_batch(ctx);
            flushed = true;
        }

        for (int cx = 0; cx < ctx->pages[victim].chunksX; cx++) {
            for (int cy = 0; cy < ctx->pages[victim].chunksY; cy++) {
                glDeleteTextures(1, &ctx->pages[victim].chunks[cx][cy].tex);
                ctx->pages[victim].chunks[cx][cy].tex = 0;
            }
        }
        ctx->pages[victim].loaded = false;
        evicted++;
    }
}

static void extract_page_file(CtrRenderer *ctx, DataWin *dw, uint32_t id, FILE *f, int aw, int ah) {
    TexturePageItem *item = &dw->tpag.items[id];
    int w = item->sourceWidth > 0 ? item->sourceWidth : 1;
    int h = item->sourceHeight > 0 ? item->sourceHeight : 1;

    PageData *page = &ctx->pages[id];
    page->origW = w;
    page->origH = h;
    page->chunksX = fminf((w + 1023) / 1024, MAX_CHUNKS_X);
    page->chunksY = fminf((h + 1023) / 1024, MAX_CHUNKS_Y);

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            AtlasChunk *chunk = &page->chunks[cx][cy];
            chunk->srcX = cx * 1024;
            chunk->srcY = cy * 1024;
            chunk->width = fminf(w - chunk->srcX, 1024);
            chunk->height = fminf(h - chunk->srcY, 1024);
            chunk->potW = next_pow2(chunk->width);
            chunk->potH = next_pow2(chunk->height);

            uint16_t *pixels = calloc(chunk->potW * chunk->potH, 2);
            if (!pixels) continue;

            for (int y = 0; y < chunk->height; y++) {
                int sy = item->sourceY + chunk->srcY + y;
                if (sy < 0 || sy >= ah) continue;
                fseek(f, sizeof(AtlasHeader) + (sy * aw + item->sourceX + chunk->srcX) * 2, SEEK_SET);
                fread(&pixels[y * chunk->potW], 2, chunk->width, f);
            }

            glGenTextures(1, &chunk->tex);
            glBindTexture(GL_TEXTURE_2D, chunk->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, chunk->potW, chunk->potH, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4,
                         pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            free(pixels);
        }
    }
    page->loaded = true;
}

static __attribute__((aligned(8))) char dyn_buf[64 * 1024];

static void load_page_dyn(CtrRenderer *ctx, DataWin *dw, int32_t idx) {
    if (idx < 0 || idx >= ctx->pageCount || ctx->pages[idx].loaded) return;
    free_old_pages(ctx);

    uint32_t pageId = dw->tpag.items[idx].texturePageId;
    char path[256];
    snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/page_%u.atlas", pageId);

    FILE *f = fopen(path, "rb");
    if (!f) return;
    setvbuf(f, dyn_buf, _IOFBF, sizeof(dyn_buf));

    AtlasHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
        extract_page_file(ctx, dw, idx, f, hdr.w, hdr.h);
    }
    fclose(f);
}

static void ctr_init(Renderer *ren, DataWin *dw) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    ren->dataWin = dw;

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    ctx->pageCount = dw->tpag.count;
    ctx->pages = calloc(ctx->pageCount, sizeof(PageData));

    ctx->batchCap = BATCH_CAP;
    ctx->batchVerts = linearAlloc(ctx->batchCap * 6 * sizeof(VertexData));

    VertexData *v = (VertexData *) ctx->batchVerts;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(VertexData), &v[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(VertexData), &v[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(VertexData), &v[0].r);

    glGenTextures(1, &ctx->whiteTex);
    glBindTexture(GL_TEXTURE_2D, ctx->whiteTex);
    uint32_t white = 0xFFFFFFFF;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    build_texture_cache(ctx);
}

static void ctr_destroy(Renderer *ren) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    glDeleteTextures(1, &ctx->whiteTex);

    for (uint32_t i = 0; i < ctx->pageCount; i++) {
        if (!ctx->pages[i].loaded) continue;
        for (int cx = 0; cx < ctx->pages[i].chunksX; cx++) {
            for (int cy = 0; cy < ctx->pages[i].chunksY; cy++) glDeleteTextures(1, &ctx->pages[i].chunks[cx][cy].tex);
        }
    }
    free(ctx->pages);
    if (ctx->batchVerts) linearFree(ctx->batchVerts);
    free(ctx);
}

static void ctr_begin_frame(Renderer *ren, int32_t gw, int32_t gh, int32_t ww, int32_t wh) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    flush_batch(ctx);
    glBindTexture(GL_TEXTURE_2D, 0);
    ctx->winW = ww;
    ctx->winH = wh;
    ctx->gameW = gw;
    ctx->gameH = gh;
}

static void set_viewport(CtrRenderer *ctx, int32_t px, int32_t py, int32_t pw, int32_t ph) {
    float scale = fminf((float) ctx->winW / ctx->gameW, (float) ctx->winH / ctx->gameH);
    int32_t bw = roundf(ctx->gameW * scale), bh = roundf(ctx->gameH * scale);
    int32_t bx = (ctx->winW - bw) / 2, by = (ctx->winH - bh) / 2;
    glViewport(bx + roundf(px * scale), by + roundf(py * scale), fmaxf(1, roundf(pw * scale)),
               fmaxf(1, roundf(ph * scale)));
}

static void ctr_begin_view(Renderer *ren, int32_t vx, int32_t vy, int32_t vw, int32_t vh, int32_t px, int32_t py,
                           int32_t pw, int32_t ph, float angle) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    flush_batch(ctx);
    set_viewport(ctx, px, py, pw, ph);

    Matrix4f proj, rot, res;
    Matrix4f_identity(&proj);
    Matrix4f_ortho(&proj, vx, vx + vw, vy + vh, vy, -1.f, 1.f);

    if (angle != 0.f) {
        float cx = vx + vw / 2.f, cy = vy + vh / 2.f;
        Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.f);
        Matrix4f_rotateZ(&rot, -angle * M_PI / 180.f);
        Matrix4f_translate(&rot, -cx, -cy, 0.f);
        Matrix4f_multiply(&res, &proj, &rot);
        proj = res;
    }
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void ctr_begin_gui(Renderer *ren, int32_t gw, int32_t gh, int32_t px, int32_t py, int32_t pw, int32_t ph) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    flush_batch(ctx);
    set_viewport(ctx, px, py, pw, ph);
    Matrix4f proj;
    Matrix4f_identity(&proj);
    Matrix4f_ortho(&proj, 0, gw, gh, 0, -1.f, 1.f);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj.m);
}

static void ctr_end_frame(Renderer *ren) {
    flush_batch((CtrRenderer *) ren);
    g_frame++;
}

static void ctr_flush(Renderer *ren) { flush_batch((CtrRenderer *) ren); }

static void draw_region(CtrRenderer *ctx, uint32_t id, float sx, float sy, float sw, float sh, float x0, float y0,
                        float x1, float y1, float x2, float y2, float x3, float y3, uint8_t *col) {
    if (id >= ctx->pageCount) return;
    if (!ctx->pages[id].loaded) load_page_dyn(ctx, ctx->base.dataWin, id);
    if (!ctx->pages[id].loaded || sw <= 0 || sh <= 0) return;

    PageData *page = &ctx->pages[id];
    page->lastFrame = g_frame;

    float rL = sx, rT = sy, rR = sx + sw, rB = sy + sh;

    for (int cy = 0; cy < page->chunksY; cy++) {
        for (int cx = 0; cx < page->chunksX; cx++) {
            AtlasChunk *c = &page->chunks[cx][cy];
            float dL = fmaxf(rL, c->srcX), dT = fmaxf(rT, c->srcY);
            float dR = fminf(rR, c->srcX + c->width), dB = fminf(rB, c->srcY + c->height);

            if (dL >= dR || dT >= dB) continue;

            float u0 = (dL - c->srcX) / c->potW, v0 = (dT - c->srcY) / c->potH;
            float u1 = (dR - c->srcX) / c->potW, v1 = (dB - c->srcY) / c->potH;

            float tL = (dL - rL) / sw, tR = (dR - rL) / sw;
            float tT = (dT - rT) / sh, tB = (dB - rT) / sh;

            float topX0 = x0 + (x1 - x0) * tL, topY0 = y0 + (y1 - y0) * tL;
            float topX1 = x0 + (x1 - x0) * tR, topY1 = y0 + (y1 - y0) * tR;
            float botX0 = x3 + (x2 - x3) * tL, botY0 = y3 + (y2 - y3) * tL;
            float botX1 = x3 + (x2 - x3) * tR, botY1 = y3 + (y2 - y3) * tR;

            push_quad(ctx, c->tex,
                      topX0 + (botX0 - topX0) * tT, topY0 + (botY0 - topY0) * tT,
                      topX1 + (botX1 - topX1) * tT, topY1 + (botY1 - topY1) * tT,
                      topX1 + (botX1 - topX1) * tB, topY1 + (botY1 - topY1) * tB,
                      topX0 + (botX0 - topX0) * tB, topY0 + (botY0 - topY0) * tB,
                      u0, v0, u1, v1, col);
        }
    }
}

static void ctr_draw_sprite(Renderer *ren, int32_t id, float x, float y, float ox, float oy, float sx, float sy,
                            float ang, uint32_t color, float a) {
    uint8_t c[4];
    col2rgb(color, a, c);
    if (!c[3] || id < 0 || id >= ((CtrRenderer *) ren)->pageCount) return;

    TexturePageItem *item = &ren->dataWin->tpag.items[id];
    float l0 = (item->targetX - ox) * sx, t0 = (item->targetY - oy) * sy;
    float l1 = l0 + item->sourceWidth * sx, t1 = t0 + item->sourceHeight * sy;

    if (ang == 0.f) {
        draw_region((CtrRenderer *) ren, id, 0, 0, item->sourceWidth, item->sourceHeight, x + l0, y + t0, x + l1,
                    y + t0, x + l1, y + t1, x + l0, y + t1, c);
    } else {
        float rad = -ang * (M_PI / 180.f);
        float sn = sinf(rad), cs = cosf(rad);
        draw_region((CtrRenderer *) ren, id, 0, 0, item->sourceWidth, item->sourceHeight,
                    l0 * cs - t0 * sn + x, l0 * sn + t0 * cs + y,
                    l1 * cs - t0 * sn + x, l1 * sn + t0 * cs + y,
                    l1 * cs - t1 * sn + x, l1 * sn + t1 * cs + y,
                    l0 * cs - t1 * sn + x, l0 * sn + t1 * cs + y, c);
    }
}

static void ctr_draw_sprite_part(Renderer *ren, int32_t id, int32_t sx, int32_t sy, int32_t sw, int32_t sh, float x,
                                 float y, float xscale, float yscale, uint32_t color, float alpha) {
    uint8_t c[4];
    col2rgb(color, alpha, c);
    if (c[3])
        draw_region((CtrRenderer *) ren, id, sx, sy, sw, sh, x, y, x + sw * xscale, y, x + sw * xscale,
                    y + sh * yscale, x, y + sh * yscale, c);
}

static void ctr_draw_sprite_pos(Renderer *ren, int32_t id, float x1, float y1, float x2, float y2, float x3, float y3,
                                float x4, float y4, float alpha) {
    uint8_t c[4];
    col2rgb(ren->drawColor, alpha, c);
    if (c[3])
        draw_region((CtrRenderer *) ren, id, 0, 0, ren->dataWin->tpag.items[id].sourceWidth,
                    ren->dataWin->tpag.items[id].sourceHeight, x1, y1, x2, y2, x3, y3, x4, y4, c);
}

static void ctr_draw_tile(Renderer *ren, RoomTile *tile, float ox, float oy) {
    int32_t id = Renderer_resolveObjectTPAGIndex(ren->dataWin, tile);
    if (id < 0) return;

    TexturePageItem *tpag = &ren->dataWin->tpag.items[id];
    int sx = tile->sourceX, sy = tile->sourceY, sw = tile->width, sh = tile->height;
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

    int cR = tpag->targetX + tpag->sourceWidth, cB = tpag->targetY + tpag->sourceHeight;
    if (sx + sw > cR) sw = cR - sx;
    if (sy + sh > cB) sh = cB - sy;
    if (sw <= 0 || sh <= 0) return;

    uint8_t a = (tile->color >> 24) & 0xFF;
    ren->vtable->drawSpritePart(ren, id, sx - tpag->targetX, sy - tpag->targetY, sw, sh, dx, dy, tile->scaleX,
                                tile->scaleY, tile->color & 0xFFFFFF, a == 0 ? 1.f : a / 255.f);
}

static void ctr_draw_tiled(Renderer *ren, int32_t id, float ox, float oy, float x, float y, float sx, float sy, bool tx,
                           bool ty, float rw, float rh, uint32_t col, float a) {
    if (id < 0 || id >= ren->dataWin->tpag.count) return;
    TexturePageItem *t = &ren->dataWin->tpag.items[id];
    float tw = t->boundingWidth * fabsf(sx), th = t->boundingHeight * fabsf(sy);
    if (tw <= 0.f || th <= 0.f) return;

    float sX = tx ? fmodf(x - ox * fabsf(sx), tw) : x - ox * fabsf(sx);
    if (sX > 0.f && tx) sX -= tw;
    float sY = ty ? fmodf(y - oy * fabsf(sy), th) : y - oy * fabsf(sy);
    if (sY > 0.f && ty) sY -= th;

    float eX = tx ? rw : sX + tw;
    float eY = ty ? rh : sY + th;

    for (float dy = sY; dy < eY; dy += th) {
        for (float dx = sX; dx < eX; dx += tw) {
            ren->vtable->drawSprite(ren, id, dx + ox * fabsf(sx), dy + oy * fabsf(sy), ox, oy, sx, sy, 0.f, col, a);
        }
    }
}

//shapes
static void ctr_draw_rect_c(Renderer *ren, float x1, float y1, float x2, float y2, uint32_t c1, uint32_t c2,
                            uint32_t c3, uint32_t c4, float a, bool out) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    uint8_t aB = f2b_alpha(a);
    if (!aB) return;
    float l = roundf(fminf(x1, x2)), r = roundf(fmaxf(x1, x2));
    float t = roundf(fminf(y1, y2)), b = roundf(fmaxf(y1, y2));

    uint8_t r1[4], r2[4], r3[4], r4[4];
    col2rgb(c1, a, r1);
    col2rgb(c2, a, r2);
    col2rgb(c3, a, r3);
    col2rgb(c4, a, r4);

    if (out) {
        float vx[4] = {l, r + 1, r + 1, l};
        float vyT[4] = {t, t, t + 2, t + 2};
        uint8_t ct[16] = {
            r1[0], r1[1], r1[2], aB, r2[0], r2[1], r2[2], aB, r2[0], r2[1], r2[2], aB, r1[0], r1[1], r1[2], aB
        };
        push_quad_grad(ctx, ctx->whiteTex, vx, vyT, .5f, .5f, .5f, .5f, ct);

        float vyB[4] = {b - 1, b - 1, b + 1, b + 1};
        uint8_t cb[16] = {
            r4[0], r4[1], r4[2], aB, r3[0], r3[1], r3[2], aB, r3[0], r3[1], r3[2], aB, r4[0], r4[1], r4[2], aB
        };
        push_quad_grad(ctx, ctx->whiteTex, vx, vyB, .5f, .5f, .5f, .5f, cb);

        float vxL[4] = {l, l + 2, l + 2, l};
        float vyM[4] = {t + 2, t + 2, b - 1, b - 1};
        uint8_t cl[16] = {
            r1[0], r1[1], r1[2], aB, r1[0], r1[1], r1[2], aB, r4[0], r4[1], r4[2], aB, r4[0], r4[1], r4[2], aB
        };
        push_quad_grad(ctx, ctx->whiteTex, vxL, vyM, .5f, .5f, .5f, .5f, cl);

        float vxR[4] = {r - 1, r + 1, r + 1, r - 1};
        uint8_t cr[16] = {
            r2[0], r2[1], r2[2], aB, r2[0], r2[1], r2[2], aB, r3[0], r3[1], r3[2], aB, r3[0], r3[1], r3[2], aB
        };
        push_quad_grad(ctx, ctx->whiteTex, vxR, vyM, .5f, .5f, .5f, .5f, cr);
    } else {
        float vx[4] = {l, r + 1, r + 1, l};
        float vy[4] = {t, t, b + 1, b + 1};
        uint8_t cx[16] = {
            r1[0], r1[1], r1[2], aB, r2[0], r2[1], r2[2], aB, r3[0], r3[1], r3[2], aB, r4[0], r4[1], r4[2], aB
        };
        push_quad_grad(ctx, ctx->whiteTex, vx, vy, .5f, .5f, .5f, .5f, cx);
    }
}

static void ctr_draw_rect(Renderer *ren, float x1, float y1, float x2, float y2, uint32_t col, float a, bool out) {
    ctr_draw_rect_c(ren, x1, y1, x2, y2, col, col, col, col, a, out);
}

static void ctr_draw_line_c(Renderer *ren, float x1, float y1, float x2, float y2, float w, uint32_t c1, uint32_t c2,
                            float a) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    uint8_t r1[4], r2[4];
    col2rgb(c1, a, r1);
    col2rgb(c2, a, r2);
    if (!r1[3]) return;

    w = fmaxf(2.f, w);
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);

    if (len < 0.01f) {
        float l = roundf(fminf(x1, x2)), t = roundf(fminf(y1, y2));
        float r = roundf(fmaxf(x1, x2)), b = roundf(fmaxf(y1, y2));
        if (fabsf(x1 - x2) < 0.01f) {
            l -= w * .5f;
            r += w * .5f;
            b += 1.f;
        } else {
            r += 1.f;
            t -= w * .5f;
            b += w * .5f;
        }
        float vx[4] = {l, r, r, l};
        float vy[4] = {t, t, b, b};
        uint8_t c[16] = {
            r1[0], r1[1], r1[2], r1[3], r2[0], r2[1], r2[2], r2[3], r2[0], r2[1], r2[2], r2[3], r1[0], r1[1], r1[2],
            r1[3]
        };
        push_quad_grad(ctx, ctx->whiteTex, vx, vy, .5f, .5f, .5f, .5f, c);
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
    float vx[4] = {x1 + px, x1 - px, x2 - px, x2 + px};
    float vy[4] = {y1 + py, y1 - py, y2 - py, y2 + py};
    uint8_t cx[16] = {
        r1[0], r1[1], r1[2], r1[3], r1[0], r1[1], r1[2], r1[3], r2[0], r2[1], r2[2], r2[3], r2[0], r2[1], r2[2], r2[3]
    };
    push_quad_grad(ctx, ctx->whiteTex, vx, vy, .5f, .5f, .5f, .5f, cx);
}

static void ctr_draw_line(Renderer *ren, float x1, float y1, float x2, float y2, float w, uint32_t c, float a) {
    ctr_draw_line_c(ren, x1, y1, x2, y2, w, c, c, a);
}

static void ctr_draw_tri_c(Renderer *ren, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c1,
                           uint32_t c2, uint32_t c3, float a, bool out) {
    if (out) {
        ctr_draw_line_c(ren, x1, y1, x2, y2, 1.f, c1, c2, a);
        ctr_draw_line_c(ren, x2, y2, x3, y3, 1.f, c2, c3, a);
        ctr_draw_line_c(ren, x3, y3, x1, y1, 1.f, c3, c1, a);
        return;
    }
    uint8_t r1[4], r2[4], r3[4];
    col2rgb(c1, a, r1);
    col2rgb(c2, a, r2);
    col2rgb(c3, a, r3);
    push_tri_grad((CtrRenderer *) ren, x1, y1, x2, y2, x3, y3, r1, r2, r3);
}

static void ctr_draw_tri(Renderer *ren, float x1, float y1, float x2, float y2, float x3, float y3, bool out) {
    ctr_draw_tri_c(ren, x1, y1, x2, y2, x3, y3, ren->drawColor, ren->drawColor, ren->drawColor, ren->drawAlpha, out);
}

static void ctr_draw_ellipse(Renderer *ren, float cx, float cy, float rx, float ry, uint32_t c, float a, bool out,
                             int32_t prec) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx <= 2.5f && ry <= 2.5f) {
        ctr_draw_rect(ren, cx - rx, cy - ry, cx + rx, cy + ry, c, a, out);
        return;
    }

    int segs = fmaxf(8, fminf(32, sqrtf(fmaxf(rx, ry)) * 3.5f));
    float step = (2.f * M_PI) / segs;

    uint8_t rgb[4];
    col2rgb(c, a, rgb);
    float px = cx + rx, py = cy;

    for (int i = 1; i <= segs; i++) {
        float nx = cx + cosf(step * i) * rx;
        float ny = cy + sinf(step * i) * ry;
        if (out) ctr_draw_line(ren, px, py, nx, ny, 1.f, c, a);
        else push_tri_grad(ctx, cx, cy, px, py, nx, ny, rgb, rgb, rgb);
        px = nx;
        py = ny;
    }
}

static void ctr_draw_circle(Renderer *ren, float x, float y, float r, uint32_t c, float a, bool out, int32_t prec) {
    ctr_draw_ellipse(ren, x, y, r, r, c, a, out, prec);
}

static void get_arc(float *px, float *py, int *cnt, float cx, float cy, float rx, float ry, float a1, float a2, int seg,
                    bool skip) {
    for (int i = 0; i <= seg; i++) {
        if (skip && !i) continue;
        if (*cnt >= MAX_RR_POINTS) break;
        float ang = a1 + (a2 - a1) * (seg ? (float) i / seg : 0);
        px[*cnt] = cx + cosf(ang) * rx;
        py[(*cnt)++] = cy + sinf(ang) * ry;
    }
}

static void ctr_draw_rr(Renderer *ren, float x1, float y1, float x2, float y2, float rx, float ry, uint32_t c, float a,
                        bool out, int32_t prec) {
    float l = fminf(x1, x2), r = fmaxf(x1, x2);
    float t = fminf(y1, y2), b = fmaxf(y1, y2);
    float w = r - l, h = b - t;

    if (w <= 0 || h <= 0 || rx <= 0 || ry <= 0) {
        ctr_draw_rect(ren, l, t, r, b, c, a, out);
        return;
    }
    rx = fminf(fabsf(rx), w * .5f);
    ry = fminf(fabsf(ry), h * .5f);

    int seg = fmaxf(2, fminf(8, sqrtf(fmaxf(rx, ry)) * 0.8f));
    float px[MAX_RR_POINTS], py[MAX_RR_POINTS];
    int cnt = 0;
    float hp = M_PI * .5f;

    get_arc(px, py, &cnt, r - rx, t + ry, rx, ry, -hp, 0.f, seg, false);
    get_arc(px, py, &cnt, r - rx, b - ry, rx, ry, 0.f, hp, seg, true);
    get_arc(px, py, &cnt, l + rx, b - ry, rx, ry, hp, M_PI, seg, true);
    get_arc(px, py, &cnt, l + rx, t + ry, rx, ry, M_PI, M_PI + hp, seg, true);

    uint8_t rgb[4];
    col2rgb(c, a, rgb);
    if (out) {
        for (int i = 0; i < cnt; i++) ctr_draw_line(ren, px[i], py[i], px[(i + 1) % cnt], py[(i + 1) % cnt], 1.f, c, a);
    } else {
        float cx = (l + r) * .5f, cy = (t + b) * .5f;
        for (int i = 0; i < cnt; i++)
            push_tri_grad((CtrRenderer *) ren, cx, cy, px[i], py[i], px[(i + 1) % cnt],
                          py[(i + 1) % cnt], rgb, rgb, rgb);
    }
}

static void ctr_draw_text(Renderer *ren, const char *txt, float x, float y, float sx, float sy, float ang) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ren->dataWin;
    int fidx = ren->drawFont;
    if (fidx < 0 || fidx >= dw->font.count) return;

    Font *font = &dw->font.fonts[fidx];
    if (font->tpagIndex < 0 || font->tpagIndex >= ctx->pageCount) return;

    if (!ctx->pages[font->tpagIndex].loaded) load_page_dyn(ctx, dw, font->tpagIndex);
    if (!ctx->pages[font->tpagIndex].loaded) return;

    uint8_t rgb[4];
    col2rgb(ren->drawColor, ren->drawAlpha, rgb);
    if (!rgb[3]) return;

    PreprocessedText ptxt = TextUtils_preprocessGmlText(txt);
    if (!ptxt.text[0]) {
        PreprocessedText_free(ptxt);
        return;
    }

    int len = strlen(ptxt.text);
    float stride = font->emSize > 0 ? font->emSize : 10.f;
    int lines = TextUtils_countLines(ptxt.text, len);

    float valOff = ren->drawValign == 1 ? -(lines * stride / 2.f) : (ren->drawValign == 2 ? -(lines * stride) : 0);

    Matrix4f tr;
    Matrix4f_setTransform2D(&tr, roundf(x), roundf(y), sx * font->scaleX, sy * font->scaleY, -ang * M_PI / 180.f);

    float cy = valOff;
    int start = 0;

    for (int l = 0; l < lines; l++) {
        int end = start;
        while (end < len && !TextUtils_isNewlineChar(ptxt.text[end])) end++;

        float lw = TextUtils_measureLineWidth(font, ptxt.text + start, end - start);
        float cx = ren->drawHalign == 1 ? -lw / 2.f : (ren->drawHalign == 2 ? -lw : 0);

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

            draw_region(ctx, font->tpagIndex, g->sourceX, g->sourceY, g->sourceWidth, g->sourceHeight,
                        px0, py0, px1, py1, px2, py2, px3, py3, rgb);
            cx += g->shift;
        }
        cy += stride;
        start = end < len ? TextUtils_skipNewline(ptxt.text, end, len) : end;
    }
    PreprocessedText_free(ptxt);
}

static void ctr_draw_text_c(Renderer *ren, const char *t, float x, float y, float xs, float ys,
                            float ang, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float a) {
    float old = ren->drawAlpha;
    ren->drawAlpha = a;
    ctr_draw_text(ren, t, x, y, xs, ys, ang);
    ren->drawAlpha = old;
}

static void mark_res(CtrRenderer *ctx, int id) {
    if (id >= 0 && id < ctx->pageCount) ctx->pages[id].keepResident = true;
}

static void mark_spr(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id < 0 || id >= dw->sprt.count) return;
    Sprite *s = &dw->sprt.sprites[id];
    for (uint32_t i = 0; i < s->textureCount; i++) mark_res(ctx, s->tpagIndices[i]);
}

static void mark_bg(CtrRenderer *ctx, DataWin *dw, int id) {
    if (id >= 0 && id < dw->bgnd.count) mark_res(ctx, dw->bgnd.backgrounds[id].tpagIndex);
}

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx) { mark_spr((CtrRenderer *) ren, ren->dataWin, sprIdx); }

static void ctr_on_room(Renderer *ren, int32_t rm) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ren->dataWin;
    if (rm < 0 || rm >= dw->room.count) return;
    Room *room = &dw->room.rooms[rm];

    for (uint32_t i = 0; i < ctx->pageCount; i++) ctx->pages[i].keepResident = false;
    for (uint32_t i = 0; i < dw->font.count; i++) mark_res(ctx, dw->font.fonts[i].tpagIndex);

    if (room->backgrounds) {
        for (int i = 0; i < 8; i++) {
            if (room->backgrounds[i].enabled) mark_bg(ctx, dw, room->backgrounds[i].backgroundDefinition);
        }
    }
    for (uint32_t i = 0; i < room->tileCount; i++) {
        int id = room->tiles[i].backgroundDefinition;
        if (room->tiles[i].useSpriteDefinition) mark_spr(ctx, dw, id);
        else mark_bg(ctx, dw, id);
    }
    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int id = room->gameObjects[i].objectDefinition;
        if (id >= 0 && id < dw->objt.count) {
            mark_spr(ctx, dw, dw->objt.objects[id].spriteId);
            int p = dw->objt.objects[id].parentId;
            if (p >= 0 && p < dw->objt.count) mark_spr(ctx, dw, dw->objt.objects[p].spriteId);
        }
    }

    bool loadMap[256] = {0};
    for (uint32_t i = 0; i < ctx->pageCount; i++) {
        if (ctx->pages[i].keepResident && !ctx->pages[i].loaded) loadMap[dw->tpag.items[i].texturePageId & 0xFF] = true;
    }

    for (int p = 0; p < 256; p++) {
        if (!loadMap[p]) continue;
        char path[256];
        snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/page_%d.atlas", p);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        setvbuf(f, dyn_buf, _IOFBF, sizeof(dyn_buf));

        AtlasHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
            for (uint32_t i = 0; i < ctx->pageCount; i++) {
                if (dw->tpag.items[i].texturePageId == p && ctx->pages[i].keepResident && !ctx->pages[i].loaded)
                    extract_page_file(ctx, dw, i, f, hdr.w, hdr.h);
            }
        }
        fclose(f);
    }
}

static void ctr_set_blend(Renderer *ren, int32_t mode) {
    flush_batch((CtrRenderer *) ren);
    //bm_normal = 0, bm_add = 1, bm_max = 2, bm_sub = 3
    if (mode == 1) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else if (mode == 3) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void set_3d_depth(Renderer *ren, float depth) {
    flush_batch((CtrRenderer *) ren);
    float z = depth >= 1000000.f
                  ? 0.025f
                  : (depth <= -100000.f ? -0.04f : fmaxf(-0.025f, fminf(0.025f, depth / 25000.f)));
    novaSet3DDepth(z);
}

static int32_t ctr_create_surf(Renderer *ren, int32_t x, int32_t y, int32_t w, int32_t h, bool rb, bool sm, int32_t xo,
                               int32_t yo) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ctx->base.dataWin;
    flush_batch(ctx);

    int vw = w, vh = h;
    static void *fbo_vram_buf = NULL;
    if (!fbo_vram_buf) {
        fbo_vram_buf = vramAlloc(640 * 480 * 4);
        if (!fbo_vram_buf) fbo_vram_buf = linearAlloc(640 * 480 * 4);
    }

    uint32_t *pixels = (uint32_t *) fbo_vram_buf;
    if (!pixels) return -1;

    float scale = fminf((float) ctx->winW / ctx->gameW, (float) ctx->winH / ctx->gameH);
    int32_t bw = roundf(ctx->gameW * scale), bh = roundf(ctx->gameH * scale);
    int32_t bx = (ctx->winW - bw) / 2, by = (ctx->winH - bh) / 2;

    int px = bx + roundf(x * scale);
    int py = by + roundf(y * scale);
    int pw = roundf(w * scale);
    int ph = roundf(h * scale);

    if (pw > 640) pw = 640;
    if (ph > 480) ph = 480;

    glReadPixels(px, ctx->winH - py - ph, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    uint32_t newTpagIdx = dw->tpag.count;
    dw->tpag.items = realloc(dw->tpag.items, (newTpagIdx + 1) * sizeof(TexturePageItem));
    dw->tpag.count++;

    TexturePageItem *tpag = &dw->tpag.items[newTpagIdx];
    memset(tpag, 0, sizeof(TexturePageItem));
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = pw;
    tpag->sourceHeight = ph;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->boundingWidth = vw;
    tpag->boundingHeight = vh;
    tpag->texturePageId = 0xFFFF; //mark as dyn

    ctx->pages = realloc(ctx->pages, dw->tpag.count * sizeof(PageData));
    PageData *page = &ctx->pages[newTpagIdx];
    memset(page, 0, sizeof(PageData));
    page->loaded = true;
    page->keepResident = true;
    page->origW = pw;
    page->origH = ph;
    page->chunksX = 1;
    page->chunksY = 1;

    AtlasChunk *chunk = &page->chunks[0][0];
    chunk->srcX = 0;
    chunk->srcY = 0;
    chunk->width = pw;
    chunk->height = ph;
    chunk->potW = next_pow2(pw);
    chunk->potH = next_pow2(ph);

    uint16_t *tex_data = linearAlloc(chunk->potW * chunk->potH * 2);
    if (tex_data) {
        for (int cy = 0; cy < ph; cy++) {
            for (int cx = 0; cx < pw; cx++) {
                uint32_t pxl = pixels[(ph - 1 - cy) * pw + cx];
                uint8_t r = pxl & 0xFF;
                uint8_t g = (pxl >> 8) & 0xFF;
                uint8_t b = (pxl >> 16) & 0xFF;
                uint8_t a = (pxl >> 24) & 0xFF;
                tex_data[cy * chunk->potW + cx] = pack_rgba4444(r, g, b, a);
            }
        }

        glGenTextures(1, &chunk->tex);
        glBindTexture(GL_TEXTURE_2D, chunk->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, chunk->potW, chunk->potH, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4,
                     tex_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, sm ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sm ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        linearFree(tex_data);
    }

    ctx->pageCount = dw->tpag.count;

    uint32_t newSprIdx = dw->sprt.count;
    dw->sprt.sprites = realloc(dw->sprt.sprites, (newSprIdx + 1) * sizeof(Sprite));
    dw->sprt.count++;

    Sprite *spr = &dw->sprt.sprites[newSprIdx];
    memset(spr, 0, sizeof(Sprite));
    spr->originX = xo;
    spr->originY = yo;
    spr->textureCount = 1;
    spr->tpagIndices = malloc(sizeof(int32_t));
    spr->tpagIndices[0] = newTpagIdx;

    return newSprIdx;
}

static void ctr_del_sprite(Renderer *ren, int32_t s) {
    CtrRenderer *ctx = (CtrRenderer *) ren;
    DataWin *dw = ctx->base.dataWin;
    if (s < 0 || s >= dw->sprt.count) return;

    Sprite *spr = &dw->sprt.sprites[s];
    if (spr->textureCount == 0 || !spr->tpagIndices) return;

    int tpagIdx = spr->tpagIndices[0];
    if (tpagIdx >= 0 && tpagIdx < ctx->pageCount) {
        PageData *page = &ctx->pages[tpagIdx];
        if (page->loaded) {
            for (int cx = 0; cx < page->chunksX; cx++) {
                for (int cy = 0; cy < page->chunksY; cy++) {
                    if (page->chunks[cx][cy].tex) {
                        glDeleteTextures(1, &page->chunks[cx][cy].tex);
                        page->chunks[cx][cy].tex = 0;
                    }
                }
            }
            page->loaded = false;
        }
    }

    spr->textureCount = 0;
    free(spr->tpagIndices);
    spr->tpagIndices = NULL;
}

static void ctr_end_view(Renderer *ren) {
    flush_batch((CtrRenderer *) ren);
}

static void ctr_end_gui(Renderer *ren) {
    flush_batch((CtrRenderer *) ren);
}

static RendererVtable vtable = {
    .init = ctr_init, .destroy = ctr_destroy,
    .beginFrame = ctr_begin_frame, .endFrame = ctr_end_frame,
    .beginView = ctr_begin_view, .endView = ctr_end_view,
    .beginGUI = ctr_begin_gui, .endGUI = ctr_end_gui,
    .drawSprite = ctr_draw_sprite, .drawSpritePart = ctr_draw_sprite_part, .drawSpritePos = ctr_draw_sprite_pos,
    .drawRectangle = ctr_draw_rect, .drawRectangleColor = ctr_draw_rect_c,
    .drawLine = ctr_draw_line, .drawLineColor = ctr_draw_line_c,
    .drawTriangle = ctr_draw_tri, .drawTriangleColor = ctr_draw_tri_c,
    .drawText = ctr_draw_text, .drawTextColor = ctr_draw_text_c, .flush = ctr_flush,
    .createSpriteFromSurface = ctr_create_surf, .deleteSprite = ctr_del_sprite,
    .drawTile = ctr_draw_tile, .drawTiled = ctr_draw_tiled,
    .drawCircle = ctr_draw_circle, .drawEllipse = ctr_draw_ellipse, .drawRoundrect = ctr_draw_rr,
    .onRoomChanged = ctr_on_room, .set3DDepthOffset = set_3d_depth, .setBlendMode = ctr_set_blend
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
