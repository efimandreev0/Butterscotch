#pragma once

#include "renderer.h"
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>

#define CTR_REPACK_ATLAS_SIZE   512
#define CTR_MAX_CHUNKS_X        4
#define CTR_MAX_CHUNKS_Y        4
#define CTR_TARGET_STACK_DEPTH  8
#define CTR_PREFETCH_QUEUE_CAP  128

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
    bool     loaded;
    bool     keepResident;
    int      width, height;
    int      potW,  potH;
    uint32_t fileOffset;
    uint32_t dataSize;
    uint32_t format;
    C3D_Tex  tex;
    uint32_t lastFrame;
    bool     loadFailed;
} CtrSourcePage;

typedef struct {
    bool     valid;
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
    bool              used;
    int               width, height;
    int               potW,  potH;
    C3D_Tex           tex;
    C3D_RenderTarget *target;
} CtrSurface;

typedef struct {
    bool     valid;
    int      x, y;
    int      width, height;
    int      potW, potH;
    C3D_Tex  tex;
} CtrTilemapChunk;

typedef struct {
    bool                 valid;
    const RoomLayerTilesData *data;
    uint32_t             hash;
    int32_t              backgroundIndex;
    uint32_t             tilesX, tilesY;
    uint32_t             tileW, tileH;
    uint32_t             chunkSize;
    uint32_t             chunkCols, chunkRows;
    uint32_t             chunkCount;
    CtrTilemapChunk     *chunks;
} CtrTilemapLayerCache;

typedef struct {
    C3D_RenderTarget *target;
    int               viewport[4];
    bool              hasScissor;
    int               scissor[4];
    C3D_Mtx           projection;
    bool              cullEnabled;
    float             cullL, cullT, cullR, cullB;
} CtrTargetState;

typedef struct CtrRenderer {
    Renderer base;

    // Citro3D shader pipeline
    int              uLoc_projection;
    C3D_AttrInfo     attrInfo;
    bool             pipelineReady;

    // Screen render targets
    C3D_RenderTarget *topTarget;     // GFX_TOP
    C3D_RenderTarget *bottomTarget;  // GFX_BOTTOM
    bool              gfxOwned;

    // Game-sized render target
    bool              appReady;
    int               appLogicW, appLogicH;
    int               appPotW,   appPotH;
    C3D_Tex           appTex;
    C3D_RenderTarget *appTarget;
    bool              appFrameCleared;

    // Screen size
    int               winW, winH;
    int               gameW, gameH;

    // Texture cache
    CtrPage          *pages;
    uint32_t          pageCount;
    uint32_t          originalTpagCount;
    uint32_t          originalSpriteCount;
    CtrSourcePage    *sourcePages;
    uint32_t          sourcePageCount;
    uint32_t          repackBasePageId;
    uint32_t          repackPageCount;
    CtrCachedTpag    *cacheItems;
    uint32_t          cacheItemCount;
    CtrCachedSegment *cacheSegments;
    uint32_t          cacheSegmentCount;

    // Game surfaces
    CtrSurface       *surfaces;
    uint32_t          surfaceCount;

    // Deltarune GMS2 tile layers. These room-sized maps are more reliable as
    // precomposited chunks than as hundreds of atlas sub-rect draws on 3DS.
    CtrTilemapLayerCache *tilemapCaches;
    uint32_t              tilemapCacheCount;
    uint32_t              tilemapBuildAttempts;
    uint32_t              tilemapBuildSuccesses;
    uint32_t              tilemapBuildFailures;
    uint32_t              tilemapChunksBuilt;
    uint32_t              tilemapOpaquePixels;
    uint32_t              tilemapDrawCalls;
    uint32_t              tilemapChunksDrawn;
    char                  tilemapLastStatus[96];

    // Vertex batch
    void             *vbuf;          // linearAlloc
    uint32_t          vbufCap;
    uint32_t          vbufHead;
    uint32_t          batchStart;
    uint32_t          batchVerts;
    C3D_Tex          *batchTex;

    // Solid-color texture
    C3D_Tex           whiteTex;

    // Target stack
    CtrTargetState    targetStack[CTR_TARGET_STACK_DEPTH];
    int               targetStackDepth;
    bool              surfaceDrawSuppressed;
    int               surfaceDrawSuppressedDepth;

    // Current target state
    C3D_RenderTarget *activeTarget;
    C3D_Mtx           currentProjection;
    int               currentViewport[4];

    // Frame state
    bool              inFrame;
    int               currentBlendMode;
    bool              blendEnabled;
    GPU_BLENDFACTOR   blendSrcColor;
    GPU_BLENDFACTOR   blendDstColor;
    GPU_BLENDFACTOR   blendSrcAlpha;
    GPU_BLENDFACTOR   blendDstAlpha;
    GPU_WRITEMASK     writeMask;
    bool              alphaTestEnabled;
    uint8_t           alphaTestRef;

    // Repacked atlas stream.
    FILE             *atlasFile;
    bool              preloadingAtlases;

    // Runtime atlas prefetch. This is intentionally tiny and frame-budgeted:
    // it warms newly discovered effect/battle sprites without causing the
    // same multi-page stall as a big preload.
    int32_t           prefetchQueue[CTR_PREFETCH_QUEUE_CAP];
    uint32_t          prefetchQueueCount;

    // Diagnostics state for optional lazy-load logging.
    int32_t           currentRoomIndex;
    char              currentRoomName[64];
    uint32_t          lazyLoadsThisRoom;

    // Per-view culling rect in room coords (or GUI coords). Set by begin_view /
    // begin_gui, cleared by end_view / end_gui. Skips quads whose bounding box
    // doesn't overlap. Saves thousands of off-screen tile drawcalls in big rooms
    // (Undyne bridge, hotland labs, etc.).
    bool              cullEnabled;
    float             cullL, cullT, cullR, cullB;
} CtrRenderer;

Renderer *CtrRenderer_create(void);

typedef void (*CtrRendererCacheProgressFn)(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user);

void CtrRenderer_setCacheProgressCallback(CtrRendererCacheProgressFn callback, void *user);

// Standalone texture cache build. Не трогает Citro3D state/pipeline — можно
// безопасно вызывать пока активен другой рендер (например, лаунчер).
void CtrRenderer_prepareTextureCache(DataWin *dw);

// Called between launcher/game sessions. This keeps Citro3D's queued work and
// deferred render-target deletes from leaking across game launches.
void CtrRenderer_resetSessionState(void);

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx);
void CtrRenderer_prefetchTexturePage(Renderer *ren, int32_t tpagIdx);
void CtrRenderer_dumpTextureDiagnostics(Renderer *ren, FILE *out, const Room *room);
bool CtrRenderer_drawGms2TileLayer(Renderer *ren, RoomLayerTilesData *data,
                                   float layerOffsetX, float layerOffsetY, float alpha);

// ---- Live appearance / layout controls --------------------------------------

typedef enum {
    CTR_GAME_SCREEN_TOP    = 0,
    CTR_GAME_SCREEN_BOTTOM = 1
} CtrGameScreen;

typedef enum {
    CTR_BACKDROP_GRADIENT = 0,
    CTR_BACKDROP_BLUR,
    CTR_BACKDROP_BLACK,
    CTR_BACKDROP_STRETCH,
} CtrBackdropMode;

typedef enum {
    CTR_DISPLAY_ORIGINAL = 0,
    CTR_DISPLAY_STRETCH,
    CTR_DISPLAY_3DS_WIDE,
    CTR_DISPLAY_MODE_COUNT
} CtrDisplayMode;

typedef enum {
    CTR_APP_FILTER_LINEAR = 0,
    CTR_APP_FILTER_NEAREST,
    CTR_APP_FILTER_COUNT
} CtrAppFilterMode;

void CtrRenderer_setGameScreen(CtrGameScreen which);
CtrGameScreen CtrRenderer_getGameScreen(void);
void CtrRenderer_setBackdropMode(CtrBackdropMode mode);
CtrBackdropMode CtrRenderer_getBackdropMode(void);
void CtrRenderer_setDisplayMode(CtrDisplayMode mode);
CtrDisplayMode CtrRenderer_getDisplayMode(void);
void CtrRenderer_setAppFilterMode(CtrAppFilterMode mode);
CtrAppFilterMode CtrRenderer_getAppFilterMode(void);

void CtrRenderer_setLetterboxTheme(float topR, float topG, float topB,
                                   float botR, float botG, float botB,
                                   float accentR, float accentG, float accentB,
                                   float blurAlpha, float particleAlpha);

C3D_RenderTarget *CtrRenderer_getTopTarget(Renderer *ren);
C3D_RenderTarget *CtrRenderer_getBottomTarget(Renderer *ren);
