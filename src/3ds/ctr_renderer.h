#pragma once

#include "renderer.h"
#include <NovaGL.h>

#define CTR_MAX_CHUNKS_X 4
#define CTR_MAX_CHUNKS_Y 4
typedef struct {
    GLuint tex;
    int srcX;
    int srcY;
    int width;
    int height;
    int potW;
    int potH;
} CtrTpagChunk;

typedef struct {
    bool isLoaded;
    bool keepResident;

    int origW;
    int origH;
    int chunksX;
    int chunksY;

    CtrTpagChunk chunks[CTR_MAX_CHUNKS_X][CTR_MAX_CHUNKS_Y];
    uint32_t lastFrameUsed;
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