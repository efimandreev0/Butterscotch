//
// Created by efimandreev0 on 01.05.2026.
//
#pragma once

#include "renderer.h"
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>

#define CTR_MAX_CHUNKS_X        4
#define CTR_MAX_CHUNKS_Y        4
#define CTR_TARGET_STACK_DEPTH  8

typedef struct {
    bool     valid;
    C3D_Tex  tex;
    int      srcX, srcY;
    int      width, height;
    int      potW,  potH;
} CtrAtlasChunk;

typedef struct {
    bool     loaded;
    bool     keepResident;
    int      origW, origH;
    int      chunksX, chunksY;
    CtrAtlasChunk chunks[CTR_MAX_CHUNKS_X][CTR_MAX_CHUNKS_Y];
    uint32_t lastFrame;
} CtrPage;

typedef struct {
    bool              used;
    int               width, height;
    int               potW,  potH;
    C3D_Tex           tex;
    C3D_RenderTarget *target;
} CtrSurface;

typedef struct {
    C3D_RenderTarget *target;
    int               viewport[4];
    bool              hasScissor;
    int               scissor[4];
    C3D_Mtx           projection;
} CtrTargetState;

typedef struct {
    Renderer base;

    DVLB_s          *vshaderDvlb;
    shaderProgram_s  shaderProg;
    int              uLoc_projection;
    C3D_AttrInfo     attrInfo;
    bool             pipelineReady;

    C3D_RenderTarget *topTarget;
    C3D_RenderTarget *bottomTarget;
    bool              gfxOwned;

    bool              appReady;
    int               appLogicW, appLogicH;
    int               appPotW,   appPotH;
    C3D_Tex           appTex;
    C3D_RenderTarget *appTarget;
    bool              appFrameCleared;

    int               winW, winH;
    int               gameW, gameH;

    CtrPage          *pages;
    uint32_t          pageCount;
    uint32_t          originalTpagCount;
    uint32_t          originalSpriteCount;

    CtrSurface       *surfaces;
    uint32_t          surfaceCount;

    void             *vbuf;
    uint32_t          vbufCap;
    uint32_t          vbufHead;
    uint32_t          batchStart;
    uint32_t          batchVerts;
    C3D_Tex          *batchTex;

    C3D_Tex           whiteTex;

    CtrTargetState    targetStack[CTR_TARGET_STACK_DEPTH];
    int               targetStackDepth;

    C3D_RenderTarget *activeTarget;
    C3D_Mtx           currentProjection;
    int               currentViewport[4];

    bool              inFrame;
    int               currentBlendMode;
} CtrRenderer;

Renderer *CtrRenderer_create(void);

typedef void (*CtrRendererCacheProgressFn)(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user);

void CtrRenderer_setCacheProgressCallback(CtrRendererCacheProgressFn callback, void *user);

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx);
