#pragma once

#include "renderer.h"
#include <NovaGL.h>

#define MAX_CHUNKS_X 4
#define MAX_CHUNKS_Y 4

typedef struct {
    GLuint tex;
    int srcX, srcY;
    int width, height;
    int potW, potH;
} AtlasChunk;

typedef struct {
    bool loaded;
    bool keepResident;

    int origW, origH;
    int chunksX, chunksY;

    AtlasChunk chunks[MAX_CHUNKS_X][MAX_CHUNKS_Y];
    uint32_t lastFrame;
} PageData;

typedef struct {
    Renderer base;

    PageData *pages;
    uint32_t pageCount;

    GLuint whiteTex;

    uint8_t *batchVerts;
    uint32_t batchCap;
    uint32_t batchCount;
    GLuint batchTex;

    int32_t winW, winH;
    int32_t gameW, gameH;
} CtrRenderer;

Renderer* CtrRenderer_create(void);
void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx);