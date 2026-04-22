// --- START OF FILE ctr_renderer.h ---

#pragma once

#include "renderer.h"
#include <NovaGL.h>

// Данные конкретного вырезанного куска из атласа (TPAG)
typedef struct {
    bool isLoaded;
    bool keepResident;
    uint32_t lastUsedFrame;

    GLuint tex;
    float uvScaleX;
    float uvScaleY;

    float downscaleFactor;
} CtrTpagData;

// Кэш распакованной страницы атласа в ОЗУ (чтобы не статтерить при частом спавне)
typedef struct {
    uint8_t* pixels;
    uint8_t* originalStbiPixels;
    int width;
    int height;
    float dsFactor;
    uint32_t lastExtractedFrame;
} CtrDecodedPage;

typedef struct {
    GLuint tex;              // ID текстуры OpenGL
    int potWidth;            // Ширина текстуры в VRAM (обычно 1024 или 512)
    int potHeight;           // Высота текстуры в VRAM
    float downscaleFactor;   // Насколько мы её ужали (например 0.5)

    bool isLoaded;
    bool keepResident;
    uint32_t lastUsedFrame;
} CtrPageData;

// ===[ CtrRenderer Struct ]===
typedef struct {
    Renderer base;

    CtrTpagData* tpags;
    uint32_t tpagCount;

    int32_t* tpagToSprite;

    CtrDecodedPage* decodedPages; // Массив кэшей атласов
    uint32_t texturePageCount;

    bool pendingResidencyUpdate;
    uint32_t pendingResidencyReadyFrame;

    GLuint whiteTexture;

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

    uint32_t vramUsed;
} CtrRenderer;

Renderer* CtrRenderer_create(void);
void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex);
// --- END OF FILE ctr_renderer.h ---