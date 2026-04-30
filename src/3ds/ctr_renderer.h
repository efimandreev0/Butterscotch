#pragma once

#include "renderer.h"
#include "matrix_math.h"
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
    bool used;
    int width;
    int height;
    GLuint fbo;
    GLuint tex;
    uint32_t tpagIndex;
    int potW;
    int potH;
} CtrSurface;

typedef struct {
    GLuint fbo;
    GLint viewport[4];
    Matrix4f projection;
} CtrTargetState;

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

    // Скейлинг для адаптации огромных игр под экран 3DS
    float scaleX;
    float scaleY;

    // Original counts from data.win — dynamic tpag/sprite slots start at these.
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    // Off-screen render target. The whole frame is drawn into this FBO; at
    // endFrame the texture is blitted to the actual screen target. NovaGL keeps
    // FBOs unrotated, so their logical size matches their texture size.
    bool fboCreated;
    bool fboFrameCleared;
    GLuint fboId;
    GLuint fboTexId;
    int fboLogicW;
    int fboLogicH;
    int fboPotW;
    int fboPotH;

    CtrSurface *surfaces;
    uint32_t surfaceCount;
    GLuint activeFbo;
    CtrTargetState targetStack[8];
    int targetStackDepth;
    Matrix4f currentProjection;
} CtrRenderer;

Renderer *CtrRenderer_create(void);

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx);