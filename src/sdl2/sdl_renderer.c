#include "sdl_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

#define DYNAMIC_TPAG_OFFSET_BASE 0xD0000000u

static void transformWorldToView(SDLRenderer* sdl, float wx, float wy, float* vx, float* vy) {
    float lx = wx - sdl->currentViewX;
    float ly = wy - sdl->currentViewY;

    if (sdl->currentViewAngle != 0.0f) {
        float cx = sdl->currentViewW / 2.0f;
        float cy = sdl->currentViewH / 2.0f;
        
        lx -= cx;
        ly -= cy;
        
        float angleRad = -sdl->currentViewAngle * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        
        float nx = lx * cosA - ly * sinA;
        float ny = lx * sinA + ly * cosA;
        
        lx = nx + cx;
        ly = ny + cy;
    }

    *vx = lx * (sdl->currentPortW / sdl->currentViewW);
    *vy = ly * (sdl->currentPortH / sdl->currentViewH);
}

static void emitQuad(SDLRenderer* sdl, SDL_Texture* tex,
                     float x[4], float y[4], float u[4], float v[4], 
                     float r[4], float g[4], float b[4], float a[4]) {
    SDL_Vertex verts[4];
    
    for (int i = 0; i < 4; i++) {
        float vx, vy;
        transformWorldToView(sdl, x[i], y[i], &vx, &vy);
        
        verts[i].position.x = vx;
        verts[i].position.y = vy;
        verts[i].tex_coord.x = u[i];
        verts[i].tex_coord.y = v[i];
        
        verts[i].color.r = (uint8_t)(r[i] * 255.0f);
        verts[i].color.g = (uint8_t)(g[i] * 255.0f);
        verts[i].color.b = (uint8_t)(b[i] * 255.0f);
        verts[i].color.a = (uint8_t)(a[i] * 255.0f);
    }

    int indices[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(sdl->sdlRenderer, tex, verts, 4, indices, 6);
}

static void emitColoredQuad(SDLRenderer* sdl, SDL_Texture* tex, float x[4], float y[4], float u[4], float v[4], float r, float g, float b, float a) {
    float rc[4] = {r, r, r, r};
    float gc[4] = {g, g, g, g};
    float bc[4] = {b, b, b, b};
    float ac[4] = {a, a, a, a};
    emitQuad(sdl, tex, x, y, u, v, rc, gc, bc, ac);
}

static void sdlInit(Renderer* renderer, DataWin* dataWin) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    renderer->dataWin = dataWin;

    sdl->textureCount = dataWin->txtr.count;
    sdl->sdlTextures = safeCalloc(sdl->textureCount, sizeof(SDL_Texture*));
    sdl->textureWidths = safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->textureHeights = safeCalloc(sdl->textureCount, sizeof(int32_t));

    for (uint32_t i = 0; sdl->textureCount > i; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        int w, h, channels;
        uint8_t* pixels = stbi_load_from_memory(txtr->blobData, (int) txtr->blobSize, &w, &h, &channels, 4);
        
        if (pixels == nullptr) {
            fprintf(stderr, "SDL: Failed to decode TXTR page %u\n", i);
            continue;
        }

        sdl->textureWidths[i] = w;
        sdl->textureHeights[i] = h;

        SDL_Texture* tex = SDL_CreateTexture(sdl->sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        
        sdl->sdlTextures[i] = tex;
        stbi_image_free(pixels);
        fprintf(stderr, "SDL: Loaded TXTR page %u (%dx%d)\n", i, w, h);
    }

    sdl->whiteTexture = SDL_CreateTexture(sdl->sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    SDL_UpdateTexture(sdl->whiteTexture, nullptr, whitePixel, 4);
    SDL_SetTextureBlendMode(sdl->whiteTexture, SDL_BLENDMODE_BLEND);

    sdl->originalTexturePageCount = sdl->textureCount;
    sdl->originalTpagCount = dataWin->tpag.count;
    sdl->originalSpriteCount = dataWin->sprt.count;
}

static void sdlDestroy(Renderer* renderer) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;

    if (sdl->fboTexture) SDL_DestroyTexture(sdl->fboTexture);
    if (sdl->whiteTexture) SDL_DestroyTexture(sdl->whiteTexture);

    for (uint32_t i = 0; sdl->textureCount > i; i++) {
        if (sdl->sdlTextures[i]) SDL_DestroyTexture(sdl->sdlTextures[i]);
    }

    free(sdl->sdlTextures);
    free(sdl->textureWidths);
    free(sdl->textureHeights);
    free(sdl);
}

static void sdlBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;

    sdl->windowW = windowW;
    sdl->windowH = windowH;
    sdl->gameW = gameW;
    sdl->gameH = gameH;

    if (gameW != sdl->fboWidth || gameH != sdl->fboHeight) {
        if (sdl->fboTexture) SDL_DestroyTexture(sdl->fboTexture);
        sdl->fboTexture = SDL_CreateTexture(sdl->sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, gameW, gameH);
        sdl->fboWidth = gameW;
        sdl->fboHeight = gameH;
        fprintf(stderr, "SDL: FBO resized to %dx%d\n", gameW, gameH);
    }

    SDL_SetRenderTarget(sdl->sdlRenderer, sdl->fboTexture);
    SDL_RenderSetViewport(sdl->sdlRenderer, nullptr);
    SDL_RenderSetClipRect(sdl->sdlRenderer, nullptr);
}

static void sdlBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;

    sdl->currentViewX = (float)viewX;
    sdl->currentViewY = (float)viewY;
    sdl->currentViewW = (float)viewW;
    sdl->currentViewH = (float)viewH;
    sdl->currentPortX = (float)portX;
    sdl->currentPortY = (float)portY;
    sdl->currentPortW = (float)portW;
    sdl->currentPortH = (float)portH;
    sdl->currentViewAngle = viewAngle;

    SDL_Rect portRect = {portX, portY, portW, portH};
    SDL_RenderSetViewport(sdl->sdlRenderer, &portRect);
    SDL_RenderSetClipRect(sdl->sdlRenderer, &portRect);
}

static void sdlEndView(Renderer* renderer) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    SDL_RenderSetClipRect(sdl->sdlRenderer, nullptr);
}

static void sdlEndFrame(Renderer* renderer) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;

    SDL_SetRenderTarget(sdl->sdlRenderer, nullptr);
    SDL_RenderSetViewport(sdl->sdlRenderer, nullptr);
    SDL_RenderSetClipRect(sdl->sdlRenderer, nullptr);

    SDL_Rect dstRect = {0, 0, sdl->windowW, sdl->windowH};
    SDL_RenderCopy(sdl->sdlRenderer, sdl->fboTexture, nullptr, &dstRect);
}

static void sdlRendererFlush(Renderer* renderer) {
    (void) renderer;
}

static void sdlDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;

    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float)sdl->textureWidths[pageId];
    float texH = (float)sdl->textureHeights[pageId];

    float u0 = (float) tpag->sourceX / texW;
    float v0 = (float) tpag->sourceY / texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / texH;

    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float xs[4], ys[4];
    Matrix4f_transformPoint(&transform, localX0, localY0, &xs[0], &ys[0]); // top-left
    Matrix4f_transformPoint(&transform, localX1, localY0, &xs[1], &ys[1]); // top-right
    Matrix4f_transformPoint(&transform, localX1, localY1, &xs[2], &ys[2]); // bottom-right
    Matrix4f_transformPoint(&transform, localX0, localY1, &xs[3], &ys[3]); // bottom-left

    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void sdlDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;

    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float)sdl->textureWidths[pageId];
    float texH = (float)sdl->textureHeights[pageId];

    float u0 = (float) (tpag->sourceX + srcOffX) / texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / texH;

    float xs[4] = { x, x + srcW * xscale, x + srcW * xscale, x };
    float ys[4] = { y, y, y + srcH * yscale, y + srcH * yscale };
    float us[4] = { u0, u1, u1, u0 };
    float vs[4] = { v0, v0, v1, v1 };

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void sdlDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;
    float us[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float vs[4] = {0.5f, 0.5f, 0.5f, 0.5f};

    if (outline) {
        float tx[4][4] = {
            {x1, x2 + 1, x2 + 1, x1}, // top
            {x1, x2 + 1, x2 + 1, x1}, // bottom
            {x1, x1 + 1, x1 + 1, x1}, // left
            {x2, x2 + 1, x2 + 1, x2}  // right
        };
        float ty[4][4] = {
            {y1, y1, y1 + 1, y1 + 1}, // top
            {y2, y2, y2 + 1, y2 + 1}, // bottom
            {y1 + 1, y1 + 1, y2, y2}, // left
            {y1 + 1, y1 + 1, y2, y2}  // right
        };
        for (int i = 0; i < 4; i++) {
            emitColoredQuad(sdl, sdl->whiteTexture, tx[i], ty[i], us, vs, r, g, b, alpha);
        }
    } else {
        float xs[4] = {x1, x2 + 1, x2 + 1, x1};
        float ys[4] = {y1, y1, y2 + 1, y2 + 1};
        emitColoredQuad(sdl, sdl->whiteTexture, xs, ys, us, vs, r, g, b, alpha);
    }
}
static void sdlDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    renderer->vtable->drawLineColor(renderer, x1, y1, x2, y2, width, color, color, alpha);
}

static void sdlDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    float xs[4] = {x1 + px, x1 - px, x2 - px, x2 + px};
    float ys[4] = {y1 + py, y1 - py, y2 - py, y2 + py};
    float us[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float vs[4] = {0.5f, 0.5f, 0.5f, 0.5f};

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;
    
    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    float rc[4] = {r1, r1, r2, r2};
    float gc[4] = {g1, g1, g2, g2};
    float bc[4] = {b1, b1, b2, b2};
    float ac[4] = {alpha, alpha, alpha, alpha};

    emitQuad(sdl, sdl->whiteTexture, xs, ys, us, vs, rc, gc, bc, ac);
}

static void sdlDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;

    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float)sdl->textureWidths[pageId];
    float texH = (float)sdl->textureHeights[pageId];

    uint32_t color = renderer->drawColor;
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;

        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float u0 = (float) (fontTpag->sourceX + glyph->sourceX) / texW;
            float v0 = (float) (fontTpag->sourceY + glyph->sourceY) / texH;
            float u1 = (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / texW;
            float v1 = (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / texH;

            float localX0 = cursorX + glyph->offset;
            float localY0 = cursorY;
            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            float xs[4], ys[4];
            Matrix4f_transformPoint(&transform, localX0, localY0, &xs[0], &ys[0]);
            Matrix4f_transformPoint(&transform, localX1, localY0, &xs[1], &ys[1]);
            Matrix4f_transformPoint(&transform, localX1, localY1, &xs[2], &ys[2]);
            Matrix4f_transformPoint(&transform, localX0, localY1, &xs[3], &ys[3]);

            float us[4] = {u0, u1, u1, u0};
            float vs[4] = {v0, v0, v1, v1};

            emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, renderer->drawAlpha);

            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        if (textLen > lineEnd) lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        else lineStart = lineEnd;
    }
    free(processed);
}


static uint32_t findOrAllocTexturePageSlot(SDLRenderer* sdl) {
    for (uint32_t i = sdl->originalTexturePageCount; sdl->textureCount > i; i++) {
        if (sdl->sdlTextures[i] == nullptr) return i;
    }
    uint32_t newPageId = sdl->textureCount++;
    sdl->sdlTextures = safeRealloc(sdl->sdlTextures, sdl->textureCount * sizeof(SDL_Texture*));
    sdl->textureWidths = safeRealloc(sdl->textureWidths, sdl->textureCount * sizeof(int32_t));
    sdl->textureHeights = safeRealloc(sdl->textureHeights, sdl->textureCount * sizeof(int32_t));
    sdl->sdlTextures[newPageId] = nullptr;
    return newPageId;
}

static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static uint32_t findOrAllocSpriteSlot(DataWin* dw, uint32_t originalSpriteCount) {
    for (uint32_t i = originalSpriteCount; dw->sprt.count > i; i++) {
        if (dw->sprt.sprites[i].textureCount == 0) return i;
    }
    uint32_t newIndex = dw->sprt.count++;
    dw->sprt.sprites = safeRealloc(dw->sprt.sprites, dw->sprt.count * sizeof(Sprite));
    memset(&dw->sprt.sprites[newIndex], 0, sizeof(Sprite));
    return newIndex;
}

static int32_t sdlCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 >= w || 0 >= h || !sdl->fboTexture) return -1;

    uint8_t* pixels = safeMalloc((size_t) w * (size_t) h * 4);
    
    SDL_Rect rect = { x, y, w, h };
    if (SDL_RenderReadPixels(sdl->sdlRenderer, &rect, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4) != 0) {
        free(pixels);
        return -1;
    }

    SDL_Texture* newTex = SDL_CreateTexture(sdl->sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(newTex, nullptr, pixels, w * 4);
    SDL_SetTextureBlendMode(newTex, SDL_BLENDMODE_BLEND);
    
    SDL_SetTextureScaleMode(newTex, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    free(pixels);

    uint32_t pageId = findOrAllocTexturePageSlot(sdl);
    sdl->sdlTextures[pageId] = newTex;
    sdl->textureWidths[pageId] = w;
    sdl->textureHeights[pageId] = h;

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, sdl->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0; tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w; tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0; tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w; tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w; tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = (int16_t) pageId;

    uint32_t fakeOffset = DYNAMIC_TPAG_OFFSET_BASE + tpagIndex;
    hmput(dw->tpagOffsetMap, fakeOffset, (int32_t) tpagIndex);

    uint32_t spriteIndex = findOrAllocSpriteSlot(dw, sdl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->name = "dynamic_sprite";
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->textureOffsets = safeMalloc(sizeof(uint32_t));
    sprite->textureOffsets[0] = fakeOffset;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    fprintf(stderr, "SDL: Created dynamic sprite %u (%dx%d) from surface at (%d,%d)\n", spriteIndex, w, h, x, y);
    return (int32_t) spriteIndex;
}

static void sdlDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    SDLRenderer* sdl = (SDLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;
    if (sdl->originalSpriteCount > (uint32_t) spriteIndex) {
        fprintf(stderr, "SDL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    repeat(sprite->textureCount, i) {
        uint32_t offset = sprite->textureOffsets[i];
        if (offset >= DYNAMIC_TPAG_OFFSET_BASE) {
            int32_t tpagIdx = DataWin_resolveTPAG(dw, offset);
            if (tpagIdx >= 0) {
                TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
                int16_t pageId = tpag->texturePageId;
                if (pageId >= 0 && sdl->textureCount > (uint32_t) pageId) {
                    if (sdl->sdlTextures[pageId]) {
                        SDL_DestroyTexture(sdl->sdlTextures[pageId]);
                        sdl->sdlTextures[pageId] = nullptr;
                    }
                }
                tpag->texturePageId = -1;
            }
            hmdel(dw->tpagOffsetMap, offset);
        }
    }

    free(sprite->textureOffsets);
    memset(sprite, 0, sizeof(Sprite));
    fprintf(stderr, "SDL: Deleted sprite %d\n", spriteIndex);
}

// ===[ Vtable ]===

static RendererVtable sdlVtable = {
    .init = sdlInit,
    .destroy = sdlDestroy,
    .beginFrame = sdlBeginFrame,
    .endFrame = sdlEndFrame,
    .beginView = sdlBeginView,
    .endView = sdlEndView,
    .drawSprite = sdlDrawSprite,
    .drawSpritePart = sdlDrawSpritePart,
    .drawRectangle = sdlDrawRectangle,
    .drawLine = sdlDrawLine,
    .drawLineColor = sdlDrawLineColor,
    .drawText = sdlDrawText,
    .flush = sdlRendererFlush,
    .createSpriteFromSurface = sdlCreateSpriteFromSurface,
    .deleteSprite = sdlDeleteSprite,
    .drawTile = nullptr,
};

// ===[ Public API ]===

Renderer* SDLRenderer_create(SDL_Window* window, SDL_Renderer* renderer) {
    SDLRenderer* sdl = safeCalloc(1, sizeof(SDLRenderer));
    sdl->base.vtable = &sdlVtable;
    sdl->base.drawColor = 0xFFFFFF; // white (BGR)
    sdl->base.drawAlpha = 1.0f;
    sdl->base.drawFont = -1;
    sdl->base.drawHalign = 0;
    sdl->base.drawValign = 0;
    
    sdl->sdlWindow = window;
    sdl->sdlRenderer = renderer;
    return (Renderer*) sdl;
}