// --- START OF FILE ctr_renderer.h ---
#pragma once

#include "renderer.h"
#include <NovaGL.h>

// Вырезанный спрайт, лежащий в VRAM
typedef struct {
    bool isLoaded;
    bool keepResident;
    uint32_t lastUsedFrame;

    GLuint tex;
    float uvScaleX;         // <--- ТЕПЕРЬ ОНИ ЗДЕСЬ
    float uvScaleY;         // <--- И КОМПИЛЯТОР БУДЕТ ДОВОЛЕН
    float downscaleFactor;
} CtrTpagData;

// Главная структура рендерера
typedef struct {
    Renderer base;

    CtrTpagData* tpags;
    uint32_t tpagCount;

    void* rawAtlases;
    uint32_t rawAtlasCount;

    // Persistent memlz state — sizeof(memlz_state) ~768 KB. Allocating it
    // per-decompression call (as memlz_decompress does internally) blew up
    // the regular heap and inserted a 768 KB malloc + memset on every atlas
    // touch. We allocate once and reset before each call.
    void* memlzState;

    GLuint whiteTexture;

    uint8_t* quadBatchVertices;
    uint32_t quadBatchCapacity;
    uint32_t quadBatchCount;
    GLuint quadBatchTexture;

    int32_t windowW;
    int32_t windowH;
    int32_t gameW;
    int32_t gameH;

    int32_t* prefetchQueue;
} CtrRenderer;

Renderer* CtrRenderer_create(void);
void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex);
void CtrRenderer_drainPrefetchQueue(Renderer* renderer, int maxItems);
bool CtrRenderer_hasPendingPrefetch(Renderer* renderer);

// --- END OF FILE ctr_renderer.h ---