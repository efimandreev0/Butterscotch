#pragma once

#include "renderer.h"
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>

#define CTR_REPACK_ATLAS_SIZE   512
#define CTR_MAX_CHUNKS_X        4
#define CTR_MAX_CHUNKS_Y        4
#define CTR_TARGET_STACK_DEPTH  8
#define CTR_PENDING_LOADS_MAX   32

typedef struct {
    bool valid;
    C3D_Tex tex;
    int srcX, srcY;
    int width, height;
    int potW, potH;
} CtrAtlasChunk;

typedef struct {
    bool loaded;
    bool keepResident;
    int origW, origH;
    int chunksX, chunksY;
    CtrAtlasChunk chunks[CTR_MAX_CHUNKS_X][CTR_MAX_CHUNKS_Y];
    uint32_t lastFrame;
} CtrPage;

typedef struct {
    bool loaded;
    bool keepResident;
    int width, height;
    int potW, potH;
    uint32_t fileOffset;
    uint32_t dataSize;
    uint32_t format;
    C3D_Tex tex;
    uint32_t lastFrame;
    bool loadFailed;
} CtrSourcePage;

typedef struct {
    bool valid;
    uint32_t segmentStart;
    uint32_t segmentCount;
} CtrCachedTpag;

typedef struct {
    uint32_t atlasIndex;
    uint16_t sourceX, sourceY;
    uint16_t width, height;
    uint16_t atlasX, atlasY;
} CtrCachedSegment;

typedef struct {
    bool used;
    int width, height;
    int potW, potH;
    C3D_Tex tex;
    C3D_RenderTarget *target;
} CtrSurface;

typedef struct {
    C3D_RenderTarget *target;
    int viewport[4];
    bool hasScissor;
    int scissor[4];
    C3D_Mtx projection;
} CtrTargetState;

typedef struct CtrRenderer {
    Renderer base;
    int uLoc_projection;
    C3D_AttrInfo attrInfo;
    bool pipelineReady;
    C3D_RenderTarget *topTarget;
    C3D_RenderTarget *topTargetRight;
    C3D_RenderTarget *bottomTarget;
    bool gfxOwned;
    bool appReady;
    int appLogicW, appLogicH;
    int appScreenW, appScreenH;
    int appRenderW, appRenderH;
    int appPotW, appPotH;
    C3D_Tex appTex;
    C3D_RenderTarget *appTarget;
    C3D_Tex appTexRight;
    C3D_RenderTarget *appTargetRight;
    bool appFrameCleared;
    int currentEye;
    float depthSlider;
    float current3DDepth;
    bool isGUI;
    float currentShiftX;
    int winW, winH;
    int gameW, gameH;
    CtrPage *pages;
    uint32_t pageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
    CtrSourcePage *sourcePages;
    uint32_t sourcePageCount;
    uint32_t repackBasePageId;
    uint32_t repackPageCount;
    CtrCachedTpag *cacheItems;
    uint32_t cacheItemCount;
    CtrCachedSegment *cacheSegments;
    uint32_t cacheSegmentCount;
    CtrSurface *surfaces;
    uint32_t surfaceCount;
    void *vbuf;
    uint32_t vbufCap;
    uint32_t vbufHead;
    uint32_t batchStart;
    uint32_t batchVerts;
    C3D_Tex *batchTex;

    uint32_t drawsSinceSplit;
    C3D_Tex whiteTex;
    CtrTargetState targetStack[CTR_TARGET_STACK_DEPTH];
    int targetStackDepth;
    C3D_RenderTarget *activeTarget;
    C3D_Mtx currentProjection;
    int currentViewport[4];
    bool inFrame;
    int currentBlendMode;

    FILE *atlasFile;
    bool preloadingAtlases;
    bool cullEnabled;
    float cullL, cullT, cullR, cullB;
    bool stereoEnabled;
    int32_t pendingLoads[CTR_PENDING_LOADS_MAX];
    uint32_t pendingLoadCount;
} CtrRenderer;

Renderer *CtrRenderer_create(void);

void CtrRenderer_beginEye(Renderer *ren, int eye, float slider);

bool CtrRenderer_hasRightEye(Renderer *ren);

void CtrRenderer_setStereoEnabled(Renderer *ren, bool enabled);

bool CtrRenderer_getStereoEnabled(Renderer *ren);

typedef void (*CtrRendererCacheProgressFn)(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user);

void CtrRenderer_setCacheProgressCallback(CtrRendererCacheProgressFn callback, void *user);

void CtrRenderer_prepareTextureCache(DataWin *dw);

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx);

typedef enum {
    CTR_GAME_SCREEN_TOP = 0,
    CTR_GAME_SCREEN_BOTTOM = 1
} CtrGameScreen;

typedef enum {
    CTR_BACKDROP_GRADIENT = 0,
    CTR_BACKDROP_BLUR,
    CTR_BACKDROP_BLACK,
    CTR_BACKDROP_STRETCH,
} CtrBackdropMode;

void CtrRenderer_setGameScreen(CtrGameScreen which);

CtrGameScreen CtrRenderer_getGameScreen(void);

void CtrRenderer_setBackdropMode(CtrBackdropMode mode);

CtrBackdropMode CtrRenderer_getBackdropMode(void);

void CtrRenderer_setLetterboxTheme(float topR, float topG, float topB,
                                   float botR, float botG, float botB,
                                   float accentR, float accentG, float accentB,
                                   float blurAlpha, float particleAlpha);

C3D_RenderTarget *CtrRenderer_getTopTarget(Renderer *ren);

C3D_RenderTarget *CtrRenderer_getBottomTarget(Renderer *ren);
