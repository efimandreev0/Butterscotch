// --- START OF FILE ctr_renderer.h ---
#pragma once

#include "renderer.h"
#include <NovaGL.h>

typedef struct {
    bool isLoaded;
    bool keepResident;

    GLuint tex;
    float uvScaleX;
    float uvScaleY;
    float downscaleFactor;
} CtrTpagData;

typedef struct {
    Renderer base;

    CtrTpagData* tpags;
    uint32_t tpagCount;

    GLuint whiteTexture;

    uint8_t* quadBatchVertices;
    uint32_t quadBatchCapacity;
    uint32_t quadBatchCount;
    GLuint quadBatchTexture;

    int32_t windowW;
    int32_t windowH;
    int32_t gameW;
    int32_t gameH;
} CtrRenderer;

Renderer* CtrRenderer_create(void);
void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex);
// --- END OF FILE ctr_renderer.h ---