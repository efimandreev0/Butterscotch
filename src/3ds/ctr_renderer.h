#pragma once

#include "renderer.h"
#include <NovaGL.h>

// ===[ CtrRenderer Struct ]===
// 3DS renderer built on NovaGL (OpenGL ES 1.1 -> Citro3D translation layer).
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GLuint* glTextures;       // one GL texture per TXTR page
    float* uvScaleX;          // X Scaler
    float* uvScaleY;          // Y Scaler
    bool* textureLoaded;      // lazy loading: true once PNG decoded and uploaded
    uint32_t* lastUsedFrame;  // last frame index that touched the page through a draw path
    bool* keepResident;       // true if the page is required by the current room (don't auto-unload)
    bool pendingResidencyUpdate; // onRoomChanged sets this; the next beginFrame performs the actual glDeleteTextures when GPU state is idle
    uint32_t pendingResidencyMarkFrame;  // frame counter snapshot taken when residency was rebuilt
    uint32_t pendingResidencyReadyFrame; // earliest frame when stale pages may actually be deleted
    uint32_t textureCount;

    GLuint whiteTexture; // 1x1 white pixel for primitives

    uint8_t* textBatchVertices;
    uint32_t textBatchQuadCapacity;
    uint32_t textBatchQuadCount;
    GLuint textBatchTexture;

    uint8_t* quadBatchVertices;
    uint32_t quadBatchCapacity;
    uint32_t quadBatchCount;
    GLuint quadBatchTexture;

    int32_t windowW;
    int32_t windowH;
    int32_t gameW;
    int32_t gameH;

    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} CtrRenderer;

Renderer* CtrRenderer_create(void);
void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex);
