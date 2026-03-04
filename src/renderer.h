#pragma once

#include <stdint.h>
#include <math.h>

#include "data_win.h"
#include "instance.h"

// ===[ Renderer Vtable ]===

typedef struct Renderer Renderer;

typedef struct {
    void (*init)(Renderer* renderer, DataWin* dataWin);
    void (*destroy)(Renderer* renderer);
    void (*beginFrame)(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t windowW, int32_t windowH);
    void (*endFrame)(Renderer* renderer);
    void (*drawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
    void (*drawRectangle)(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline);
    void (*flush)(Renderer* renderer);
} RendererVtable;

// ===[ Renderer Base Struct ]===

struct Renderer {
    RendererVtable* vtable;
    DataWin* dataWin;
    uint32_t drawColor;  // BGR format, default 0xFFFFFF (white)
    float drawAlpha;     // default 1.0
    int32_t drawFont;    // default -1 (no font)
    int32_t drawHalign;  // 0=left, 1=center, 2=right
    int32_t drawValign;  // 0=top, 1=middle, 2=bottom
};

// ===[ Shared Helpers (platform-agnostic) ]===

// Resolves a sprite + subimage to a TPAG index, with frame wrapping
static int32_t Renderer_resolveTPAGIndex(DataWin* dataWin, int32_t spriteIndex, int32_t subimg) {
    if (0 > spriteIndex || dataWin->sprt.count <= (uint32_t) spriteIndex) return -1;

    Sprite* sprite = &dataWin->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return -1;

    // Wrap subimage index
    int32_t frameIndex = subimg % (int32_t) sprite->textureCount;
    if (0 > frameIndex) frameIndex += (int32_t) sprite->textureCount;

    uint32_t tpagOffset = sprite->textureOffsets[frameIndex];
    return DataWin_resolveTPAG(dataWin, tpagOffset);
}

// Convenience: draw_sprite(sprite, subimg, x, y)
static void Renderer_drawSprite(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, (float) sprite->originX, (float) sprite->originY, 1.0f, 1.0f, 0.0f, 0xFFFFFF, renderer->drawAlpha);
}

// Full version: draw_sprite_ext(sprite, subimg, x, y, xscale, yscale, rot, color, alpha)
static void Renderer_drawSpriteExt(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float x, float y, float xscale, float yscale, float rot, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    renderer->vtable->drawSprite(renderer, tpagIndex, x, y, (float) sprite->originX, (float) sprite->originY, xscale, yscale, rot, color, alpha);
}

// Default draw: draws instance's sprite using its image_* properties
static void Renderer_drawSelf(Renderer* renderer, Instance* instance) {
    if (0 > instance->spriteIndex) return;

    int32_t subimg = (int32_t) instance->imageIndex;
    Renderer_drawSpriteExt(
        renderer,
        instance->spriteIndex,
        subimg,
        (float) instance->x,
        (float) instance->y,
        (float) instance->imageXscale,
        (float) instance->imageYscale,
        (float) instance->imageAngle,
        instance->imageBlend,
        (float) instance->imageAlpha
    );
}
