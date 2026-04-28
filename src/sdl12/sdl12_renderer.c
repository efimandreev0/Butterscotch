#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS

#include "sdl12_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

#define DYNAMIC_TPAG_OFFSET_BASE 0xD0000000u

// 3DS Little Endian RGB565
#define RMASK16 0xF800
#define GMASK16 0x07E0
#define BMASK16 0x001F
#define AMASK16 0
#define MAGENTA_565 0xF81F


#define FORCE_INTERNAL_3DS_RES 0
#define FORCE_INTERNAL_3DS_W 400
#define FORCE_INTERNAL_3DS_H 240

#define MAX_CACHED_TEXTURES 26
#define MAX_TEXTURE_PAGES 8

#if defined(__GNUC__) || defined(__clang__)
#define RESTRICT __restrict__
#elif defined(_MSC_VER)
#define RESTRICT __restrict
#else
#define RESTRICT
#endif

static int g_renderParity = -1;
static SDL_Surface* g_presentBuf = nullptr;
static int g_presentW = 0;
static int g_presentH = 0;
static int g_interlaceParity = 0;

static uint32_t g_currentFrame = 0;
static uint32_t* g_texLastUsed = nullptr;
static int g_loadedTexCount = 0;

static void fastDraw_Sprite(SDL_Surface* src, SDL_Rect* sr, SDL_Surface* dst,
                            int dstX, int dstY, int dstW, int dstH,
                            bool flipX, bool flipY, uint32_t tint, float global_alpha) {
    if (!src || !dst || !sr || dstW <= 0 || dstH <= 0 || global_alpha <= 0.05f) return;

    int dx1 = dstX < 0 ? 0 : dstX;
    int dy1 = dstY < 0 ? 0 : dstY;
    int dx2 = (dstX + dstW) > dst->w ? dst->w : (dstX + dstW);
    int dy2 = (dstY + dstH) > dst->h ? dst->h : (dstY + dstH);

    int draw_w = dx2 - dx1;
    int draw_h = dy2 - dy1;
    if (draw_w <= 0 || draw_h <= 0) return;

    int src_pitch_hwords = src->pitch / 2;
    int dst_pitch_hwords = dst->pitch / 2;

    bool is_1to1 = (dstW == sr->w && dstH == sr->h);
    bool use_tint = (tint != 0xFFFFFF);

    uint32_t tr = tint & 0xFF, tg = (tint >> 8) & 0xFF, tb = (tint >> 16) & 0xFF;

    if (is_1to1 && !flipX && !flipY) {
        int sx_start = sr->x + (dx1 - dstX);
        int sy_start = sr->y + (dy1 - dstY);
        if (!use_tint) {
            for (int y = 0; y < draw_h; y++) {
                uint16_t* RESTRICT src_ptr = (uint16_t*)src->pixels + (sy_start + y) * src_pitch_hwords + sx_start;
                uint16_t* RESTRICT dst_ptr = (uint16_t*)dst->pixels + (dy1 + y) * dst_pitch_hwords + dx1;
                for (int x = 0; x < draw_w; x++) {
                    uint16_t p = src_ptr[x];
                    if (p != MAGENTA_565) dst_ptr[x] = p;
                }
            }
        } else {
            for (int y = 0; y < draw_h; y++) {
                uint16_t* RESTRICT src_ptr = (uint16_t*)src->pixels + (sy_start + y) * src_pitch_hwords + sx_start;
                uint16_t* RESTRICT dst_ptr = (uint16_t*)dst->pixels + (dy1 + y) * dst_pitch_hwords + dx1;
                for (int x = 0; x < draw_w; x++) {
                    uint16_t p = src_ptr[x];
                    if (p != MAGENTA_565) {
                        uint32_t r = ((p >> 11) << 3) * tr >> 8;
                        uint32_t g = (((p >> 5) & 0x3F) << 2) * tg >> 8;
                        uint32_t b = ((p & 0x1F) << 3) * tb >> 8;
                        dst_ptr[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                    }
                }
            }
        }
    } else {
        uint32_t ratio_x_fp = (sr->w << 16) / dstW;
        uint32_t ratio_y_fp = (sr->h << 16) / dstH;
        uint32_t cur_sy_fp_start = (dy1 - dstY) * ratio_y_fp;
        uint32_t start_sx_fp = (dx1 - dstX) * ratio_x_fp;
        uint32_t cur_sy_fp = cur_sy_fp_start;
        for (int y = 0; y < draw_h; y++) {
            int sy = (cur_sy_fp >> 16);
            cur_sy_fp += ratio_y_fp;
            if (sy >= sr->h) sy = sr->h - 1;
            int actual_sy = flipY ? (sr->y + sr->h - 1 - sy) : (sr->y + sy);
            uint16_t* RESTRICT src_row = (uint16_t*)src->pixels + actual_sy * src_pitch_hwords;
            uint16_t* RESTRICT dst_row = (uint16_t*)dst->pixels + (dy1 + y) * dst_pitch_hwords + dx1;
            uint32_t cur_sx_fp = start_sx_fp;
            for (int x = 0; x < draw_w; x++) {
                int sx = (cur_sx_fp >> 16);
                cur_sx_fp += ratio_x_fp;
                if (sx >= sr->w) sx = sr->w - 1;
                uint16_t p = src_row[sr->x + (flipX ? (sr->w - 1 - sx) : sx)];
                if (p != MAGENTA_565) {
                    if (!use_tint) {
                        dst_row[x] = p;
                    } else {
                        uint32_t r = ((p >> 11) << 3) * tr >> 8;
                        uint32_t g = (((p >> 5) & 0x3F) << 2) * tg >> 8;
                        uint32_t b = ((p & 0x1F) << 3) * tb >> 8;
                        dst_row[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                    }
                }
            }
        }
    }
}

static void fastDraw_Text(SDL_Surface* src, SDL_Rect* sr, SDL_Surface* dst,
                          int dstX, int dstY, int dstW, int dstH, uint32_t tint, float global_alpha) {
    if (!src || !dst || !sr || dstW <= 0 || dstH <= 0 || global_alpha <= 0.05f) return;

    int dx1 = dstX < 0 ? 0 : dstX;
    int dy1 = dstY < 0 ? 0 : dstY;
    int dx2 = (dstX + dstW) > dst->w ? dst->w : (dstX + dstW);
    int dy2 = (dstY + dstH) > dst->h ? dst->h : (dstY + dstH);
    int draw_w = dx2 - dx1, draw_h = dy2 - dy1;
    if (draw_w <= 0 || draw_h <= 0) return;

    int src_pitch_hwords = src->pitch / 2;
    int dst_pitch_hwords = dst->pitch / 2;

    bool is_1to1 = (dstW == sr->w && dstH == sr->h);
    bool use_tint = (tint != 0xFFFFFF);

    uint32_t tr = tint & 0xFF, tg = (tint >> 8) & 0xFF, tb = (tint >> 16) & 0xFF;

    if (is_1to1) {
        int sx_start = sr->x + (dx1 - dstX);
        int sy_start = sr->y + (dy1 - dstY);

        for (int y = 0; y < draw_h; y++) {
            uint16_t* RESTRICT src_ptr = (uint16_t*)src->pixels + (sy_start + y) * src_pitch_hwords + sx_start;
            uint16_t* RESTRICT dst_ptr = (uint16_t*)dst->pixels + (dy1 + y) * dst_pitch_hwords + dx1;

            for (int x = 0; x < draw_w; x++) {
                uint16_t p = src_ptr[x];

                if (p != MAGENTA_565) {
                    if (!use_tint) {
                        dst_ptr[x] = p;
                    } else {
                        uint32_t r = ((p >> 11) << 3) * tr >> 8;
                        uint32_t g = (((p >> 5) & 0x3F) << 2) * tg >> 8;
                        uint32_t b = ((p & 0x1F) << 3) * tb >> 8;
                        dst_ptr[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                    }
                }
            }
        }
    }
    else {
        uint32_t ratio_x_fp = (sr->w << 16) / dstW;
        uint32_t ratio_y_fp = (sr->h << 16) / dstH;
        uint32_t cur_sy_fp_start = (dy1 - dstY) * ratio_y_fp;
        uint32_t start_sx_fp = (dx1 - dstX) * ratio_x_fp;

        uint32_t cur_sy_fp = cur_sy_fp_start;
        for (int y = 0; y < draw_h; y++) {
            int sy = (cur_sy_fp >> 16);
            cur_sy_fp += ratio_y_fp;

            uint16_t* RESTRICT src_row = (uint16_t*)src->pixels + (sr->y + sy) * src_pitch_hwords;
            uint16_t* RESTRICT dst_row = (uint16_t*)dst->pixels + (dy1 + y) * dst_pitch_hwords + dx1;
            uint32_t cur_sx_fp = start_sx_fp;

            for (int x = 0; x < draw_w; x++) {
                int sx = (cur_sx_fp >> 16);
                cur_sx_fp += ratio_x_fp;

                uint16_t p = src_row[sr->x + sx];

                if (p != MAGENTA_565) {
                    if (!use_tint) {
                        dst_row[x] = p;
                    } else {
                        uint32_t r = ((p >> 11) << 3) * tr >> 8;
                        uint32_t g = (((p >> 5) & 0x3F) << 2) * tg >> 8;
                        uint32_t b = ((p & 0x1F) << 3) * tb >> 8;
                        dst_row[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                    }
                }
            }
        }
    }
}

static void drawBlendRect(SDL_Surface* dst, int x, int y, int w, int h, uint32_t col, float alpha) {
    if (alpha <= 0.05f) return;

    SDL_Rect clip; SDL_GetClipRect(dst, &clip);
    int x1 = x < clip.x ? clip.x : x;
    int y1 = y < clip.y ? clip.y : y;
    int x2 = (x + w) > (clip.x + clip.w) ? (clip.x + clip.w) : (x + w);
    int y2 = (y + h) > (clip.y + clip.h) ? (clip.y + clip.h) : (y + h);
    if (x1 >= x2 || y1 >= y2) return;

    uint8_t fr = col & 0xFF;
    uint8_t fg = (col >> 8) & 0xFF;
    uint8_t fb = (col >> 16) & 0xFF;

    if (alpha >= 0.95f) {
        SDL_Rect f = {x1, y1, x2 - x1, y2 - y1};
        SDL_FillRect(dst, &f, SDL_MapRGB(dst->format, fr, fg, fb));
    } else {
        uint32_t a = (uint32_t)(alpha * 255);
        uint32_t inv_a = 255 - a;
        int dst_pitch = dst->pitch / 2;
        for (int ry = y1; ry < y2; ry++) {
            uint16_t* ptr = (uint16_t*)dst->pixels + ry * dst_pitch + x1;
            for (int rx = x1; rx < x2; rx++) {
                uint16_t bg = *ptr;
                uint32_t br = (bg >> 11) << 3, bg_g = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
                uint32_t out_r = (fr * a + br * inv_a) >> 8;
                uint32_t out_g = (fg * a + bg_g * inv_a) >> 8;
                uint32_t out_b = (fb * a + bb * inv_a) >> 8;
                *ptr++ = ((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3);
            }
        }
    }
}

static inline void transformWorldToView(SDLRenderer* sdl, float wx, float wy, int* vx, int* vy) {
    float lx = wx - sdl->currentViewX;
    float ly = wy - sdl->currentViewY;

    if (sdl->currentViewAngle != 0.0f) {
        lx -= sdl->camCX;
        ly -= sdl->camCY;
        float nx = lx * sdl->camCos - ly * sdl->camSin;
        float ny = lx * sdl->camSin + ly * sdl->camCos;
        lx = nx + sdl->camCX;
        ly = ny + sdl->camCY;
    }

    *vx = (int)(lx * sdl->camScaleX + sdl->currentPortX);
    *vy = (int)(ly * sdl->camScaleY + sdl->currentPortY);
}

static inline int shouldDraw(SDLRenderer* sdl, int x, int y, int w, int h) {
    if (!sdl || !sdl->fboSurface) return 0;
    int x1 = x, x2 = x + w, y1 = y, y2 = y + h;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    SDL_Rect cr; SDL_GetClipRect(sdl->fboSurface, &cr);
    int margin = 32;
    return (x2 > cr.x - margin && x1 < cr.x + cr.w + margin &&
            y2 > cr.y - margin && y1 < cr.y + cr.h + margin);
}

typedef struct { FILE* f; int remaining; } StbiFileContext;
static int stbi_file_read(void *user, char *data, int size) {
    StbiFileContext *ctx = (StbiFileContext*)user;
    if (size > ctx->remaining) size = ctx->remaining;
    int res = fread(data, 1, size, ctx->f);
    ctx->remaining -= res;
    return res;
}
static void stbi_file_skip(void *user, int n) {
    StbiFileContext *ctx = (StbiFileContext*)user;
    fseek(ctx->f, n, SEEK_CUR);
    ctx->remaining -= n;
}
static int stbi_file_eof(void *user) {
    StbiFileContext *ctx = (StbiFileContext*)user;
    return ctx->remaining <= 0 || feof(ctx->f);
}


static void ensureTextureLoaded(SDLRenderer* sdl, int16_t pageId) {
    if (pageId < 0 || pageId >= sdl->textureCount) return;

    g_texLastUsed[pageId] = g_currentFrame;

    if (sdl->sdlSurfaces[pageId]) return;

    int loadedCount = 0;
    for (int i = 0; i < sdl->originalTexturePageCount; i++) {
        if (sdl->sdlSurfaces[i]) loadedCount++;
    }

    if (loadedCount >= MAX_TEXTURE_PAGES) {
        int oldestId = -1;
        uint32_t oldestTime = 0xFFFFFFFF;
        for (int i = 0; i < sdl->originalTexturePageCount; i++) {
            if (sdl->sdlSurfaces[i] && g_texLastUsed[i] < oldestTime) {
                oldestTime = g_texLastUsed[i];
                oldestId = i;
            }
        }
        if (oldestId != -1) {
            fprintf(stderr, "Memory: Evicting texture page %d to free 2MB\n", oldestId);
            SDL_FreeSurface(sdl->sdlSurfaces[oldestId]);
            sdl->sdlSurfaces[oldestId] = NULL;
        }
    }

    Texture* txtr = &sdl->base.dataWin->txtr.textures[pageId];
    if (!txtr->blobSize) return;

    int current_loaded_originals = 0;
    for (int i = 0; i < sdl->originalTexturePageCount; i++) {
        if (sdl->sdlSurfaces[i]) current_loaded_originals++;
    }

    if (current_loaded_originals >= MAX_CACHED_TEXTURES) {
        int oldest_id = -1;
        uint32_t oldest_time = 0xFFFFFFFF;
        for (int i = 0; i < sdl->originalTexturePageCount; i++) {
            if (sdl->sdlSurfaces[i] && g_texLastUsed[i] < oldest_time) {
                oldest_time = g_texLastUsed[i];
                oldest_id = i;
            }
        }
        if (oldest_id != -1) {
            SDL_FreeSurface(sdl->sdlSurfaces[oldest_id]);
            sdl->sdlSurfaces[oldest_id] = NULL;
        }
    }


    FILE* f = sdl->base.dataWin->file;
    fseek(f, (long)txtr->blobOffset, SEEK_SET);
    StbiFileContext stbiCtx = { .f = f, .remaining = (int)txtr->blobSize };
    stbi_io_callbacks stbiCbs = { stbi_file_read, stbi_file_skip, stbi_file_eof };

    int w, h, ch;
    uint8_t* px32 = stbi_load_from_callbacks(&stbiCbs, &stbiCtx, &w, &h, &ch, 4);

    SDL_Surface* opt16 = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 16, RMASK16, GMASK16, BMASK16, AMASK16);

    if (px32 && opt16) {
        uint16_t* dst_pixels = (uint16_t*)opt16->pixels;

        for (int i = 0; i < w * h; i++) {
            uint8_t r = px32[i*4 + 0];
            uint8_t g = px32[i*4 + 1];
            uint8_t b = px32[i*4 + 2];
            uint8_t a = px32[i*4 + 3];

            if (a < 128) {
                dst_pixels[i] = MAGENTA_565;
            } else {
                uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                if (color == MAGENTA_565) color = 0xF81E;
                dst_pixels[i] = color;
            }
        }
        sdl->sdlSurfaces[pageId] = opt16;
    }

    stbi_image_free(px32);
    sdl->textureWidths[pageId] = w;
    sdl->textureHeights[pageId] = h;
}

static void sdlInit(Renderer* r, DataWin* dw) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    r->dataWin = dw;
    sdl->textureCount = dw->txtr.count;
    sdl->sdlSurfaces    = safeCalloc(sdl->textureCount, sizeof(SDL_Surface*));
    sdl->textureWidths  = safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->textureHeights = safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->originalTexturePageCount = sdl->textureCount;
    sdl->originalTpagCount        = dw->tpag.count;
    sdl->originalSpriteCount      = dw->sprt.count;

    g_texLastUsed = safeCalloc(sdl->textureCount, sizeof(uint32_t));
    g_loadedTexCount = 0;
    g_currentFrame = 0;
}

static void sdlDestroy(Renderer* r) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    if (g_presentBuf) { SDL_FreeSurface(g_presentBuf); g_presentBuf = NULL; }
    if (sdl->fboSurface) SDL_FreeSurface(sdl->fboSurface);
    if (sdl->prevFboSurface) SDL_FreeSurface(sdl->prevFboSurface);

    for (int i = 0; i < sdl->textureCount; i++) {
        if (sdl->sdlSurfaces[i]) SDL_FreeSurface(sdl->sdlSurfaces[i]);
    }
    free(sdl->sdlSurfaces);
    free(sdl->textureWidths);
    free(sdl->textureHeights);
    if (g_texLastUsed) { free(g_texLastUsed); g_texLastUsed = NULL; }
    free(sdl);
}

static void sdlBeginFrame(Renderer* r, int32_t gw, int32_t gh, int32_t ww, int32_t wh) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    g_currentFrame++;
    sdl->windowW = ww;
    sdl->windowH = wh;

#if FORCE_INTERNAL_3DS_RES
    sdl->gameW = FORCE_INTERNAL_3DS_W;
    sdl->gameH = FORCE_INTERNAL_3DS_H;
#else
    sdl->gameW = (gw > 0) ? gw : ww;
    sdl->gameH = (gh > 0) ? gh : wh;
#endif

    if (sdl->fboWidth != sdl->gameW || sdl->fboHeight != sdl->gameH || !sdl->fboSurface) {
        if (sdl->fboSurface) SDL_FreeSurface(sdl->fboSurface);
        sdl->fboSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, sdl->gameW, sdl->gameH, 16, RMASK16, GMASK16, BMASK16, AMASK16);
        sdl->fboWidth  = sdl->gameW;
        sdl->fboHeight = sdl->gameH;
    }

    if (sdl->fboSurface) SDL_FillRect(sdl->fboSurface, NULL, 0);
}

static void sdlBeginView(Renderer* r, int32_t vx, int32_t vy, int32_t vw, int32_t vh,
                         int32_t px, int32_t py, int32_t pw, int32_t ph, float va) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    sdl->currentViewX = vx; sdl->currentViewY = vy;
    sdl->currentViewW = vw; sdl->currentViewH = vh;
    sdl->currentPortX = px; sdl->currentPortY = py;
    sdl->currentPortW = pw; sdl->currentPortH = ph;
    sdl->currentViewAngle = va;
    sdl->camScaleX = (float)pw / vw;
    sdl->camScaleY = (float)ph / vh;
    sdl->camCX = vw * 0.5f; sdl->camCY = vh * 0.5f;

    if (va != 0.0f) {
        float rad = -va * 3.14159265f / 180.0f;
        sdl->camCos = cosf(rad); sdl->camSin = sinf(rad);
    } else {
        sdl->camCos = 1.0f; sdl->camSin = 0.0f;
    }

    SDL_Rect cr = {px, py, pw, ph};
    if (cr.x < 0) cr.x = 0; if (cr.y < 0) cr.y = 0;
    if (cr.x + cr.w > sdl->fboSurface->w) cr.w = sdl->fboSurface->w - cr.x;
    if (cr.y + cr.h > sdl->fboSurface->h) cr.h = sdl->fboSurface->h - cr.y;
    if (cr.w < 0) cr.w = 0; if (cr.h < 0) cr.h = 0;
    SDL_SetClipRect(sdl->fboSurface, &cr);
}

static void sdlEndView(Renderer* r) {
    SDL_SetClipRect(((SDLRenderer*)r)->fboSurface, NULL);
}

static void sdlEndFrame(Renderer* r) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    if (!sdl->screenSurface || !sdl->fboSurface) return;

    float ga = (float)sdl->gameW / sdl->gameH;
    float sa = (float)sdl->windowW / sdl->windowH;
    SDL_Rect dr;

    if (sa > ga) {
        dr.h = sdl->windowH; dr.w = (int)(sdl->windowH * ga);
        dr.x = (sdl->windowW - dr.w) / 2; dr.y = 0;
    } else {
        dr.w = sdl->windowW; dr.h = (int)(sdl->windowW / ga);
        dr.x = 0; dr.y = (sdl->windowH - dr.h) / 2;
    }

    SDL_FillRect(sdl->screenSurface, NULL, 0);

    if (dr.w > 0 && dr.h > 0) {
        if (sdl->screenSurface->format->BitsPerPixel == 16) {
            SDL_SoftStretch(sdl->fboSurface, NULL, sdl->screenSurface, &dr);
        } else {
            if (!sdl->prevFboSurface || sdl->prevFboSurface->w != dr.w || sdl->prevFboSurface->h != dr.h) {
                if (sdl->prevFboSurface) SDL_FreeSurface(sdl->prevFboSurface);
                sdl->prevFboSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, dr.w, dr.h, 16, RMASK16, GMASK16, BMASK16, AMASK16);
            }
            SDL_Rect ir = {0, 0, dr.w, dr.h};
            SDL_SoftStretch(sdl->fboSurface, NULL, sdl->prevFboSurface, &ir);
            SDL_BlitSurface(sdl->prevFboSurface, &ir, sdl->screenSurface, &dr);
        }
    }

    if (SDL_Flip(sdl->screenSurface) != 0) SDL_UpdateRect(sdl->screenSurface, 0, 0, 0, 0);
}

static void sdlRendererFlush(Renderer* r) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    for (int i = 0; i < sdl->originalTexturePageCount; i++) {
        if (sdl->sdlSurfaces[i]) { SDL_FreeSurface(sdl->sdlSurfaces[i]); sdl->sdlSurfaces[i] = NULL; }
    }
}

static void sdlDrawSprite(Renderer* r, int32_t tpagIdx, float x, float y,
                          float ox, float oy, float xs, float ys,
                          float angle, uint32_t col, float alpha) {
    (void)angle;
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;

    if (tpagIdx < 0 || tpagIdx >= (int)dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
    ensureTextureLoaded(sdl, tpag->texturePageId);
    SDL_Surface* surf = sdl->sdlSurfaces[tpag->texturePageId];
    if (!surf) return;

    int vx, vy;
    transformWorldToView(sdl, x, y, &vx, &vy);

    float abs_xs = fabs(xs * sdl->camScaleX);
    float abs_ys = fabs(ys * sdl->camScaleY);
    int dstW = (int)(tpag->sourceWidth * abs_xs);
    int dstH = (int)(tpag->sourceHeight * abs_ys);
    if (dstW == 0 || dstH == 0) return;

    float cx = vx - ox * (xs * sdl->camScaleX);
    float cy = vy - oy * (ys * sdl->camScaleY);

    int dstX = (xs >= 0) ? (int)(cx + tpag->targetX * abs_xs)
                         : (int)(cx - (tpag->targetX + tpag->sourceWidth) * abs_xs);

    int dstY = (ys >= 0) ? (int)(cy + tpag->targetY * abs_ys)
                         : (int)(cy - (tpag->targetY + tpag->sourceHeight) * abs_ys);

    if (!shouldDraw(sdl, dstX, dstY, dstW, dstH)) return;

    SDL_Rect src = {tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight};
    fastDraw_Sprite(surf, &src, sdl->fboSurface, dstX, dstY, dstW, dstH, (xs < 0), (ys < 0), col, alpha);
}

static void sdlDrawSpritePart(Renderer* r, int32_t tpagIdx, int32_t sx, int32_t sy,
                              int32_t sw, int32_t sh, float x, float y,
                              float xs, float ys, uint32_t col, float alpha) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;

    if (tpagIdx < 0 || tpagIdx >= (int)dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
    ensureTextureLoaded(sdl, tpag->texturePageId);
    SDL_Surface* surf = sdl->sdlSurfaces[tpag->texturePageId];
    if (!surf) return;

    int actual_sx = sx - tpag->targetX;
    int actual_sy = sy - tpag->targetY;
    int actual_sw = sw, actual_sh = sh;

    if (actual_sx < 0) { actual_sw += actual_sx; actual_sx = 0; }
    if (actual_sy < 0) { actual_sh += actual_sy; actual_sy = 0; }
    if (actual_sx + actual_sw > tpag->sourceWidth) actual_sw = tpag->sourceWidth - actual_sx;
    if (actual_sy + actual_sh > tpag->sourceHeight) actual_sh = tpag->sourceHeight - actual_sy;
    if (actual_sw <= 0 || actual_sh <= 0) return;

    int offset_x = (sx < tpag->targetX) ? (tpag->targetX - sx) : 0;
    int offset_y = (sy < tpag->targetY) ? (tpag->targetY - sy) : 0;

    int vx, vy;
    transformWorldToView(sdl, x, y, &vx, &vy);

    float abs_xs = fabs(xs * sdl->camScaleX);
    float abs_ys = fabs(ys * sdl->camScaleY);
    int dstW = (int)(actual_sw * abs_xs);
    int dstH = (int)(actual_sh * abs_ys);
    if (dstW == 0 || dstH == 0) return;

    int dstX = (xs >= 0) ? (vx + (int)(offset_x * abs_xs)) : (vx - (int)((offset_x + actual_sw) * abs_xs));
    int dstY = (ys >= 0) ? (vy + (int)(offset_y * abs_ys)) : (vy - (int)((offset_y + actual_sh) * abs_ys));

    if (!shouldDraw(sdl, dstX, dstY, dstW, dstH)) return;

    SDL_Rect src = {tpag->sourceX + actual_sx, tpag->sourceY + actual_sy, actual_sw, actual_sh};
    fastDraw_Sprite(surf, &src, sdl->fboSurface, dstX, dstY, dstW, dstH, (xs < 0), (ys < 0), col, alpha);
}

static void sdlDrawRectangle(Renderer* r, float x1, float y1, float x2, float y2,
                             uint32_t col, float alpha, bool outline) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    int vx1, vy1, vx2, vy2;
    transformWorldToView(sdl, x1, y1, &vx1, &vy1);
    transformWorldToView(sdl, x2, y2, &vx2, &vy2);

    if (vx1 > vx2) { int t = vx1; vx1 = vx2; vx2 = t; }
    if (vy1 > vy2) { int t = vy1; vy1 = vy2; vy2 = t; }

    int w = vx2 - vx1 + 1, h = vy2 - vy1 + 1;
    if (!shouldDraw(sdl, vx1, vy1, w, h)) return;

    if (outline) {
        drawBlendRect(sdl->fboSurface, vx1, vy1, w, 1, col, alpha);
        drawBlendRect(sdl->fboSurface, vx1, vy2, w, 1, col, alpha);
        drawBlendRect(sdl->fboSurface, vx1, vy1, 1, h, col, alpha);
        drawBlendRect(sdl->fboSurface, vx2, vy1, 1, h, col, alpha);
    } else {
        drawBlendRect(sdl->fboSurface, vx1, vy1, w, h, col, alpha);
    }
}

static void sdlDrawLineColor(Renderer* r, float x1, float y1, float x2, float y2,
                             float w, uint32_t c1, uint32_t c2, float alpha) {
    (void)w; (void)c2;
    SDLRenderer* sdl = (SDLRenderer*)r;
    int vx1, vy1, vx2, vy2;
    transformWorldToView(sdl, x1, y1, &vx1, &vy1);
    transformWorldToView(sdl, x2, y2, &vx2, &vy2);

    if (abs(vx1 - vx2) <= 1) {
        if (vy1 > vy2) { int t = vy1; vy1 = vy2; vy2 = t; }
        drawBlendRect(sdl->fboSurface, vx1, vy1, 1, vy2 - vy1 + 1, c1, alpha);
    } else if (abs(vy1 - vy2) <= 1) {
        if (vx1 > vx2) { int t = vx1; vx1 = vx2; vx2 = t; }
        drawBlendRect(sdl->fboSurface, vx1, vy1, vx2 - vx1 + 1, 1, c1, alpha);
    }
}

static void sdlDrawLine(Renderer* r, float x1, float y1, float x2, float y2,
                        float w, uint32_t c, float alpha) {
    sdlDrawLineColor(r, x1, y1, x2, y2, w, c, c, alpha);
}

static void sdlDrawText(Renderer* r, const char* text, float x, float y,
                        float xscale, float yscale, float angle) {
    (void)angle;
    if (!text || !text[0]) return;

    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;

    int fnt = r->drawFont;
    if (fnt < 0 || fnt >= (int)dw->font.count) return;
    Font* font = &dw->font.fonts[fnt];
    if (!font) return;

    int ti = DataWin_resolveTPAG(dw, font->textureOffset);
    if (ti < 0 || ti >= (int)dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[ti];
    ensureTextureLoaded(sdl, tpag->texturePageId);
    SDL_Surface* surf = sdl->sdlSurfaces[tpag->texturePageId];
    if (!surf) return;

    char* proc = TextUtils_preprocessGmlText(text);
    if (!proc) return;
    int len = strlen(proc);
    if (len == 0 || len > 2048) { free(proc); return; }

    int lines = TextUtils_countLines(proc, len);
    if (lines <= 0 || lines > 100) lines = 1;

    float th = lines * font->emSize;
    float voff = (r->drawValign==1)?-th/2.0f:(r->drawValign==2)?-th:0.0f;
    float cy = voff;
    int pos = 0;
    float fsx = xscale * sdl->camScaleX, fsy = yscale * sdl->camScaleY;

    for (int li = 0; li < lines && pos < len; li++) {
        int end = pos, ls = 0;
        while (end < len && !TextUtils_isNewlineChar(proc[end]) && ls < 1000) { end++; ls++; }
        int linelen = end - pos;
        if (linelen > 1000) linelen = 1000;

        float lw = TextUtils_measureLineWidth(font, proc + pos, linelen);
        float hoff = (r->drawHalign==1)?-lw/2.0f:(r->drawHalign==2)?-lw:0.0f;
        float cx = hoff;
        int p = 0, cs = 0;

        while (p < linelen && cs < 1000) {
            int pp = p;
            uint16_t ch = TextUtils_decodeUtf8(proc + pos, linelen, (int32_t*)&p);
            if (p <= pp) p = pp + 1;

            FontGlyph* g = TextUtils_findGlyph(font, ch);
            if (!g) { cx += font->emSize * 0.5f; cs++; continue; }

            if (g->sourceWidth > 0 && g->sourceHeight > 0) {
                int gw = (int)(g->sourceWidth * fsx);
                int gh = (int)(g->sourceHeight * fsy);
                int vx, vy;
                transformWorldToView(sdl, x + (cx + g->offset) * xscale, y + cy * yscale, &vx, &vy);

                SDL_Rect src = {tpag->sourceX + g->sourceX, tpag->sourceY + g->sourceY, g->sourceWidth, g->sourceHeight};
                fastDraw_Text(surf, &src, sdl->fboSurface, vx, vy, gw, gh, r->drawColor, r->drawAlpha);
            }
            cx += g->shift; cs++;
        }
        cy += font->emSize; pos = end;
        if (pos < len && TextUtils_isNewlineChar(proc[pos])) pos = TextUtils_skipNewline(proc, pos, len);
    }
    free(proc);
}

static int findOrAllocTex(SDLRenderer* sdl) {
    for (uint32_t i = sdl->originalTexturePageCount; i < sdl->textureCount; i++) {
        if (!sdl->sdlSurfaces[i]) return i;
    }
    int id = sdl->textureCount++;
    sdl->sdlSurfaces = safeRealloc(sdl->sdlSurfaces, sdl->textureCount * sizeof(SDL_Surface*));
    sdl->textureWidths = safeRealloc(sdl->textureWidths, sdl->textureCount * sizeof(int32_t));
    sdl->textureHeights = safeRealloc(sdl->textureHeights, sdl->textureCount * sizeof(int32_t));
    g_texLastUsed = safeRealloc(g_texLastUsed, sdl->textureCount * sizeof(uint32_t));
    g_texLastUsed[id] = g_currentFrame;
    sdl->sdlSurfaces[id] = NULL;
    return id;
}

static int findOrAllocTpag(DataWin* dw, uint32_t orig) {
    for (uint32_t i = orig; i < dw->tpag.count; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    int id = dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[id], 0, sizeof(TexturePageItem));
    dw->tpag.items[id].texturePageId = -1;
    return id;
}

static int findOrAllocSpr(DataWin* dw, uint32_t orig) {
    for (uint32_t i = orig; i < dw->sprt.count; i++) {
        if (dw->sprt.sprites[i].textureCount == 0) return i;
    }
    int id = dw->sprt.count++;
    dw->sprt.sprites = safeRealloc(dw->sprt.sprites, dw->sprt.count * sizeof(Sprite));
    memset(&dw->sprt.sprites[id], 0, sizeof(Sprite));
    return id;
}

static int32_t sdlCreateSpriteFromSurface(Renderer* r, int32_t x, int32_t y, int32_t w, int32_t h,
                                          bool rb, bool sm, int32_t xo, int32_t yo) {
    (void)rb; (void)sm;
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;
    if (w <= 0 || h <= 0 || !sdl->fboSurface) return -1;

    SDL_Surface* ns = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 16, RMASK16, GMASK16, BMASK16, AMASK16);
    if (!ns) return -1;

    SDL_Rect sr = {x, y, w, h};
    SDL_BlitSurface(sdl->fboSurface, &sr, ns, NULL);

    int pid = findOrAllocTex(sdl);
    sdl->sdlSurfaces[pid] = ns;
    sdl->textureWidths[pid] = w;
    sdl->textureHeights[pid] = h;

    int ti = findOrAllocTpag(dw, sdl->originalTpagCount);
    TexturePageItem* tp = &dw->tpag.items[ti];
    tp->sourceX = 0; tp->sourceY = 0;
    tp->sourceWidth = w; tp->sourceHeight = h;
    tp->texturePageId = pid;

    uint32_t foff = DYNAMIC_TPAG_OFFSET_BASE + ti;
    hmput(dw->tpagOffsetMap, foff, ti);

    int si = findOrAllocSpr(dw, sdl->originalSpriteCount);
    Sprite* sp = &dw->sprt.sprites[si];
    sp->width = w; sp->height = h;
    sp->originX = xo; sp->originY = yo;
    sp->textureCount = 1;
    sp->textureOffsets = safeMalloc(sizeof(uint32_t));
    sp->textureOffsets[0] = foff;

    return si;
}

static void sdlDeleteSprite(Renderer* r, int32_t idx) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;
    if (idx < 0 || idx >= (int)dw->sprt.count || idx < (int)sdl->originalSpriteCount) return;
    Sprite* sp = &dw->sprt.sprites[idx];
    if (!sp->textureCount) return;

    for (int i = 0; i < sp->textureCount; i++) {
        uint32_t off = sp->textureOffsets[i];
        if (off >= DYNAMIC_TPAG_OFFSET_BASE) {
            int ti = DataWin_resolveTPAG(dw, off);
            if (ti >= 0) {
                int pid = dw->tpag.items[ti].texturePageId;
                if (pid >= 0 && pid < sdl->textureCount && sdl->sdlSurfaces[pid]) {
                    SDL_FreeSurface(sdl->sdlSurfaces[pid]);
                    sdl->sdlSurfaces[pid] = NULL;
                }
                dw->tpag.items[ti].texturePageId = -1;
            }
            hmdel(dw->tpagOffsetMap, off);
        }
    }
    free(sp->textureOffsets);
    memset(sp, 0, sizeof(Sprite));
}

static RendererVtable sdlVtable = {
    .init = sdlInit, .destroy = sdlDestroy, .beginFrame = sdlBeginFrame, .endFrame = sdlEndFrame,
    .beginView = sdlBeginView, .endView = sdlEndView, .drawSprite = sdlDrawSprite,
    .drawSpritePart = sdlDrawSpritePart, .drawRectangle = sdlDrawRectangle, .drawLine = sdlDrawLine,
    .drawLineColor = sdlDrawLineColor, .drawText = sdlDrawText, .flush = sdlRendererFlush,
    .createSpriteFromSurface = sdlCreateSpriteFromSurface, .deleteSprite = sdlDeleteSprite, .drawTile = NULL,
};

Renderer* SDLRenderer_create(SDL_Surface* scr) {
    SDLRenderer* sdl = safeCalloc(1, sizeof(SDLRenderer));
    sdl->base.vtable = &sdlVtable;
    sdl->base.drawColor = 0xFFFFFF; sdl->base.drawAlpha = 1.0f; sdl->base.drawFont = -1;
    sdl->screenSurface = scr; sdl->frameTimeAvg = 0.0f; sdl->lastTicks = 0;
    return (Renderer*)sdl;
}