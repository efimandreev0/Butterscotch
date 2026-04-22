// --- START OF FILE ctr_renderer.c ---

#include "ctr_renderer.h"
#include "common.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

#define CTR_QUAD_BATCH_CAPACITY 1024

// 🔥 ОПТИМИЗАЦИЯ СТАТТЕРОВ:
// Увеличиваем жизнь кэша до 30 секунд (1800 кадров при 60 FPS).
// Теперь снаряды Азгора, Санса и Андайн не будут выгружаться прямо посреди битвы!
#define LRU_SWEEP_INTERVAL 120
#define LRU_IDLE_THRESHOLD 1800

static int next_pot(int x) {
    x--;
    x |= x >> 1;  x |= x >> 2;
    x |= x >> 4;  x |= x >> 8;
    x |= x >> 16; x++;
    return x < 8 ? 8 : x;
}

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} CtrPackedVertex;

static void ctrFlushBatch(CtrRenderer* gl) {
    if (gl->quadBatchCount == 0 || gl->quadBatchTexture == 0 || gl->quadBatchVertices == nullptr) return;

    glBindTexture(GL_TEXTURE_2D, gl->quadBatchTexture);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei) (gl->quadBatchCount * 6));

    gl->quadBatchCount = 0;
    gl->quadBatchTexture = 0;
}

static void ctrPushQuadGradient(CtrRenderer* gl, GLuint textureId,
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r0, uint8_t g0, uint8_t b0, uint8_t a0,
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1,
    uint8_t r2, uint8_t g2, uint8_t b2, uint8_t a2,
    uint8_t r3, uint8_t g3, uint8_t b3, uint8_t a3)
{
    if (gl->quadBatchVertices == nullptr || textureId == 0) return;

    if (gl->quadBatchCount > 0 && gl->quadBatchTexture != textureId) {
        ctrFlushBatch(gl);
    }
    if (gl->quadBatchCount >= gl->quadBatchCapacity) {
        ctrFlushBatch(gl);
    }

    gl->quadBatchTexture = textureId;

    CtrPackedVertex* verts = (CtrPackedVertex*) gl->quadBatchVertices;
    CtrPackedVertex* tri = verts + gl->quadBatchCount * 6;

    tri[0] = (CtrPackedVertex) { .x = x0, .y = y0, .z = 0.0f, .u = u0, .v = v0, .r = r0, .g = g0, .b = b0, .a = a0 };
    tri[1] = (CtrPackedVertex) { .x = x1, .y = y1, .z = 0.0f, .u = u1, .v = v0, .r = r1, .g = g1, .b = b1, .a = a1 };
    tri[2] = (CtrPackedVertex) { .x = x2, .y = y2, .z = 0.0f, .u = u1, .v = v1, .r = r2, .g = g2, .b = b2, .a = a2 };

    tri[3] = (CtrPackedVertex) { .x = x0, .y = y0, .z = 0.0f, .u = u0, .v = v0, .r = r0, .g = g0, .b = b0, .a = a0 };
    tri[4] = (CtrPackedVertex) { .x = x2, .y = y2, .z = 0.0f, .u = u1, .v = v1, .r = r2, .g = g2, .b = b2, .a = a2 };
    tri[5] = (CtrPackedVertex) { .x = x3, .y = y3, .z = 0.0f, .u = u0, .v = v1, .r = r3, .g = g3, .b = b3, .a = a3 };

    gl->quadBatchCount++;
}

static void ctrPushQuad(CtrRenderer* gl, GLuint textureId, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float u0, float v0, float u1, float v1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ctrPushQuadGradient(gl, textureId, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
}

static uint32_t g_frameCounter = 0;

static void ctrDrawTpagRegion(CtrRenderer* gl, uint32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH,
                              float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (tpagIndex >= gl->tpagCount || !gl->tpags[tpagIndex].isLoaded) return;
    CtrTpagData* tpagData = &gl->tpags[tpagIndex];
    tpagData->lastUsedFrame = g_frameCounter;

    float drawSrcX = srcOffX * tpagData->downscaleFactor;
    float drawSrcY = srcOffY * tpagData->downscaleFactor;
    float drawSrcW = srcW * tpagData->downscaleFactor;
    float drawSrcH = srcH * tpagData->downscaleFactor;

    if (drawSrcW <= 0.001f || drawSrcH <= 0.001f) return;

    float u0 = drawSrcX * tpagData->uvScaleX;
    float v0 = drawSrcY * tpagData->uvScaleY;
    float u1 = (drawSrcX + drawSrcW) * tpagData->uvScaleX;
    float v1 = (drawSrcY + drawSrcH) * tpagData->uvScaleY;

    ctrPushQuad(gl, tpagData->tex, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

static void unloadTpagTexture(CtrRenderer* gl, uint32_t tpagIndex) {
    if (tpagIndex >= gl->tpagCount) return;
    CtrTpagData* tpag = &gl->tpags[tpagIndex];

    if (tpag->isLoaded && tpag->tex != 0 && tpag->tex != gl->whiteTexture) {
        glDeleteTextures(1, &tpag->tex);
    }
    tpag->tex = 0;
    tpag->isLoaded = false;
}

static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static uint32_t compute_tpag_hash(Texture* txtr, uint32_t pId, uint32_t tId) {
    uint32_t hash = 2166136261u ^ 0x7777;
    hash ^= (pId * 31337);
    hash ^= (tId * 1000003);
    hash ^= (txtr->blobSize * 199999);
    return hash;
}

static uint16_t* g_conversionBuffer = NULL;
static size_t g_conversionBufferCap = 0;

static void prefetchRoomTextures(CtrRenderer* gl) {
    DataWin* dw = gl->base.dataWin;

    for (uint32_t pId = 0; pId < gl->texturePageCount; pId++) {
        Texture* txtr = &dw->txtr.textures[pId];

        bool pageNeedsLoading = false;
        bool all_in_cache = true;

        for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
            TexturePageItem* item = &dw->tpag.items[tId];
            if (item->texturePageId == pId && gl->tpags[tId].keepResident && !gl->tpags[tId].isLoaded) {
                pageNeedsLoading = true;
                uint32_t hash = compute_tpag_hash(txtr, pId, tId);
                if (!nova_texture_cache_has(hash)) {
                    all_in_cache = false;
                }
            }
        }

        if (!pageNeedsLoading) continue;

        uint8_t* pixels = nullptr;
        int origW = 0, origH = 0, channels = 0;

        if (!all_in_cache) {
            uint8_t* tempPngBuffer = nullptr;
#ifdef __3DS__
            if (txtr->blobOffset > 0 && txtr->blobSize > 0 && dw->lazyLoadFile != nullptr) {
                tempPngBuffer = malloc(txtr->blobSize);
                if (tempPngBuffer) {
                    fseek(dw->lazyLoadFile, txtr->blobOffset, SEEK_SET);
                    fread(tempPngBuffer, 1, txtr->blobSize, dw->lazyLoadFile);
                }
            }
#else
            tempPngBuffer = txtr->blobData;
#endif
            if (tempPngBuffer != nullptr) {
                pixels = stbi_load_from_memory(tempPngBuffer, (int) txtr->blobSize, &origW, &origH, &channels, 4);
#ifdef __3DS__
                free(tempPngBuffer);
#endif
            }
        }

        for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
            TexturePageItem* item = &dw->tpag.items[tId];
            if (item->texturePageId == pId && !gl->tpags[tId].isLoaded && gl->tpags[tId].keepResident) {

                uint32_t hash = compute_tpag_hash(txtr, pId, tId);
                GLuint tex;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);

                int cacheW, cacheH;
                if (nova_texture_cache_load(hash, &cacheW, &cacheH)) {
                    gl->tpags[tId].tex = tex;
                    gl->tpags[tId].uvScaleX = 1.0f / (float)cacheW;
                    gl->tpags[tId].uvScaleY = 1.0f / (float)cacheH;
                    gl->tpags[tId].downscaleFactor = 1.0f;
                    gl->tpags[tId].isLoaded = true;
                    gl->tpags[tId].lastUsedFrame = g_frameCounter;
                    continue;
                }

                if (pixels == nullptr) {
                    glDeleteTextures(1, &tex);
                    continue;
                }

                int extractX = item->sourceX;
                int extractY = item->sourceY;
                int extW = item->sourceWidth;
                int extH = item->sourceHeight;

                if (extW <= 0) extW = 1;
                if (extH <= 0) extH = 1;

                int potW = next_pot(extW);
                int potH = next_pot(extH);
                if (potW > 1024) potW = 1024;
                if (potH > 1024) potH = 1024;

                bool has_alpha = false;
                for (int y = 0; y < extH; y++) {
                    for (int x = 0; x < extW; x++) {
                        int srcY = extractY + y;
                        int srcX = extractX + x;
                        if (srcY < origH && srcX < origW) {
                            if (pixels[(srcY * origW + srcX) * 4 + 3] < 255) {
                                has_alpha = true; goto alpha_found;
                            }
                        }
                    }
                }
                alpha_found:;

                // 🔥 ФИКС ПАМЯТИ: Используем глобальный буфер. Никаких malloc в цикле!
                size_t neededSize = potW * potH * sizeof(uint16_t);
                if (neededSize > g_conversionBufferCap) {
                    g_conversionBuffer = realloc(g_conversionBuffer, neededSize);
                    g_conversionBufferCap = neededSize;
                }

                uint16_t* pixels16 = g_conversionBuffer;
                if (pixels16) {
                    memset(pixels16, 0, neededSize); // Зануляем для паддинга (прозрачности по краям)

                    for (int y = 0; y < extH; y++) {
                        for (int x = 0; x < extW; x++) {
                            int srcY = extractY + y;
                            int srcX = extractX + x;
                            if (srcY >= origH || srcX >= origW) continue;

                            int srcOffset = (srcY * origW + srcX) * 4;
                            uint8_t r = pixels[srcOffset + 0];
                            uint8_t g = pixels[srcOffset + 1];
                            uint8_t b = pixels[srcOffset + 2];
                            uint8_t a = pixels[srcOffset + 3];

                            if (has_alpha) pixels16[y * potW + x] = pack_rgba4444(r, g, b, a);
                            else           pixels16[y * potW + x] = pack_rgb565(r, g, b);
                        }
                    }

                    GLenum format = has_alpha ? GL_RGBA : GL_RGB;
                    GLenum type   = has_alpha ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_SHORT_5_6_5;

                    while (glGetError() != GL_NO_ERROR); // Очищаем старые ошибки GL
                    glTexImage2D(GL_TEXTURE_2D, 0, format, potW, potH, 0, format, type, pixels16);

                    if (glGetError() == GL_NO_ERROR) {
                        gl->tpags[tId].tex = tex;
                        gl->tpags[tId].uvScaleX = 1.0f / (float)potW;
                        gl->tpags[tId].uvScaleY = 1.0f / (float)potH;
                        gl->tpags[tId].downscaleFactor = 1.0f;
                        gl->tpags[tId].isLoaded = true;
                        gl->tpags[tId].lastUsedFrame = g_frameCounter;

                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        nova_texture_cache_save(hash);
                    } else {
                        // 🔥 ФИКС ОШИБОК VRAM: Если LinearRAM забилась, не пытаемся грузить текстуру бесконечно (это роняло FPS до 0)
                        glDeleteTextures(1, &tex);
                        gl->tpags[tId].tex = gl->whiteTexture; // Ставим заглушку
                        gl->tpags[tId].uvScaleX = 1.0f;
                        gl->tpags[tId].uvScaleY = 1.0f;
                        gl->tpags[tId].downscaleFactor = 1.0f;
                        gl->tpags[tId].isLoaded = true; // Отмечаем как загруженный, чтобы движок отстал от нее
                    }
                }
            }
        }
        if (pixels != nullptr) stbi_image_free(pixels);
    }
}

static void loadDynamicSprite(CtrRenderer* gl, DataWin* dw, int32_t tpagIndex) {
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount || gl->tpags[tpagIndex].isLoaded) return;

    int32_t tempTpags[64];
    int tempCount = 0;

    int32_t sprIdx = gl->tpagToSprite[tpagIndex];
    if (sprIdx >= 0) {
        for (int i = 0; i <= 1; i++) {
            int32_t neighbor = sprIdx + i;
            if (neighbor >= 0 && (uint32_t)neighbor < dw->sprt.count) {
                Sprite* s = &dw->sprt.sprites[neighbor];
                for (uint32_t f = 0; f < s->textureCount; f++) {
                    int32_t tIdx = DataWin_resolveTPAG(dw, s->textureOffsets[f]);
                    if (tIdx >= 0 && tIdx < (int32_t)gl->tpagCount) {
                        if (!gl->tpags[tIdx].isLoaded && !gl->tpags[tIdx].keepResident && tempCount < 64) {
                            gl->tpags[tIdx].keepResident = true;
                            tempTpags[tempCount++] = tIdx;
                        }
                    }
                }
            }
        }
    } else {
        if (!gl->tpags[tpagIndex].keepResident && tempCount < 64) {
            gl->tpags[tpagIndex].keepResident = true;
            tempTpags[tempCount++] = tpagIndex;
        }
    }

    prefetchRoomTextures(gl);

    for (int i = 0; i < tempCount; i++) {
        gl->tpags[tempTpags[i]].keepResident = false;
        gl->tpags[tempTpags[i]].lastUsedFrame = g_frameCounter;
    }
}

static void ctrInit(Renderer* renderer, DataWin* dataWin) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    renderer->dataWin = dataWin;

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    gl->tpagCount = dataWin->tpag.count;
    gl->texturePageCount = dataWin->txtr.count;
    gl->tpags = safeCalloc(gl->tpagCount, sizeof(CtrTpagData));

    gl->tpagToSprite = safeMalloc(gl->tpagCount * sizeof(int32_t));
    for (uint32_t i = 0; i < gl->tpagCount; i++) gl->tpagToSprite[i] = -1;

    for (uint32_t i = 0; i < dataWin->sprt.count; i++) {
        Sprite* s = &dataWin->sprt.sprites[i];
        for (uint32_t f = 0; f < s->textureCount; f++) {
            int32_t tIdx = DataWin_resolveTPAG(dataWin, s->textureOffsets[f]);
            if (tIdx >= 0 && tIdx < (int32_t)gl->tpagCount) {
                gl->tpagToSprite[tIdx] = (int32_t)i;
            }
        }
    }

    gl->quadBatchCapacity = CTR_QUAD_BATCH_CAPACITY;
    gl->quadBatchCount = 0;
    gl->quadBatchTexture = 0;
    gl->quadBatchVertices = safeMalloc((size_t) gl->quadBatchCapacity * 6 * sizeof(CtrPackedVertex));

    {
        CtrPackedVertex* verts = (CtrPackedVertex*) gl->quadBatchVertices;
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(CtrPackedVertex), &verts[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(CtrPackedVertex), &verts[0].u);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CtrPackedVertex), &verts[0].r);
    }

    glGenTextures(1, &gl->whiteTexture);
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, 0);

    fprintf(stderr, "CTR: Renderer initialized (Eager Extraction Mode, %u tpags)\n", gl->tpagCount);
}

static void ctrDestroy(Renderer* renderer) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    glDeleteTextures(1, &gl->whiteTexture);
    for (uint32_t i = 0; i < gl->tpagCount; i++) {
        unloadTpagTexture(gl, i);
    }

    free(gl->tpagToSprite);
    free(gl->tpags);
    free(gl->quadBatchVertices);
    free(gl);
}

static void ctrBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);

    gl->windowW = windowW;
    gl->windowH = windowH;
    gl->gameW = gameW;
    gl->gameH = gameH;

    if (g_frameCounter > 0 && g_frameCounter % LRU_SWEEP_INTERVAL == 0) {
        for (uint32_t i = 0; i < gl->tpagCount; i++) {
            if (gl->tpags[i].keepResident || !gl->tpags[i].isLoaded) continue;
            if (g_frameCounter - gl->tpags[i].lastUsedFrame > LRU_IDLE_THRESHOLD) {
                unloadTpagTexture(gl, i);
            }
        }
    }
}

static void ctrBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, gl->windowW, gl->windowH);
    glDisable(GL_SCISSOR_TEST);

    Matrix4f projection;
    Matrix4f_identity(&projection);

    Matrix4f_ortho(&projection,
        (float)viewX,
        (float)(viewX + viewW),
        (float)(viewY + viewH),
        (float)viewY,
        -1.0f, 1.0f);

    if (viewAngle != 0.0f) {
        float cx = (float) viewX + (float) viewW / 2.0f;
        float cy = (float) viewY + (float) viewH / 2.0f;
        Matrix4f rot;
        Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.0f);
        Matrix4f_rotateZ(&rot, -viewAngle * (float) M_PI / 180.0f);
        Matrix4f_translate(&rot, -cx, -cy, 0.0f);
        Matrix4f result;
        Matrix4f_multiply(&result, &projection, &rot);
        projection = result;
    }

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void ctrEndView(Renderer* renderer) {}

static void ctrBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, gl->windowW, gl->windowH);
    glDisable(GL_SCISSOR_TEST);

    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, 0.0f, (float)guiW, (float)guiH, 0.0f, -1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void ctrEndGUI(Renderer* renderer) {}
static void ctrEndFrame(Renderer* renderer) {
    ctrFlushBatch((CtrRenderer*) renderer);
    g_frameCounter++;
}

static void ctrRendererFlush(Renderer* renderer) {
    ctrFlushBatch((CtrRenderer*) renderer);
}

static void markTpagResident(CtrRenderer* gl, int32_t tpagIndex) {
    if (tpagIndex >= 0 && (uint32_t)tpagIndex < gl->tpagCount) {
        gl->tpags[tpagIndex].keepResident = true;
    }
}

static void markTpagOffsetResident(CtrRenderer* gl, DataWin* dw, uint32_t tpagOffset) {
    int32_t tpagIdx = DataWin_resolveTPAG(dw, tpagOffset);
    markTpagResident(gl, tpagIdx);
}

static void markSpriteResident(CtrRenderer* gl, DataWin* dw, int32_t spriteIndex) {
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return;
    Sprite* s = &dw->sprt.sprites[spriteIndex];
    for (uint32_t f = 0; f < s->textureCount; f++) markTpagOffsetResident(gl, dw, s->textureOffsets[f]);
}

static void markSpriteNeighborhoodResident(CtrRenderer* gl, DataWin* dw, int32_t baseSpriteIndex, int range) {
    if (baseSpriteIndex < 0) return;

    for (int i = 0; i <= 1; i++) {
        int32_t sprIdx = baseSpriteIndex + i;
        if (sprIdx >= 0 && (uint32_t)sprIdx < dw->sprt.count) {
            markSpriteResident(gl, dw, sprIdx);
        }
    }
}

static void markBackgroundResident(CtrRenderer* gl, DataWin* dw, int32_t bgndIndex) {
    if (bgndIndex >= 0 && (uint32_t) bgndIndex < dw->bgnd.count) {
        markTpagOffsetResident(gl, dw, dw->bgnd.backgrounds[bgndIndex].textureOffset);
    }
}

void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex) {
    if (!renderer || !renderer->dataWin) return;
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    markSpriteResident(gl, dw, spriteIndex);
    prefetchRoomTextures(gl);
}

static void ctrOnRoomChanged(Renderer* renderer, int32_t roomIndex) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (roomIndex < 0 || (uint32_t) roomIndex >= dw->room.count) return;
    Room* room = &dw->room.rooms[roomIndex];

    for (uint32_t i = 0; i < gl->tpagCount; i++) gl->tpags[i].keepResident = false;

    if (!room->payloadLoaded) goto unload_stale;

    if (room->backgrounds) {
        for (int i = 0; i < 8; i++) if (room->backgrounds[i].enabled) markBackgroundResident(gl, dw, room->backgrounds[i].backgroundDefinition);
    }

    for (uint32_t i = 0; i < room->tileCount; i++) {
        if (room->tiles[i].useSpriteDefinition) markSpriteResident(gl, dw, room->tiles[i].backgroundDefinition);
        else markBackgroundResident(gl, dw, room->tiles[i].backgroundDefinition);
    }

    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int32_t objIdx = room->gameObjects[i].objectDefinition;
        if (objIdx >= 0 && (uint32_t) objIdx < dw->objt.count) {
            markSpriteNeighborhoodResident(gl, dw, dw->objt.objects[objIdx].spriteId, 1);

            int32_t parent = dw->objt.objects[objIdx].parentId;
            int guard = 8;
            while (parent >= 0 && (uint32_t) parent < dw->objt.count && guard-- > 0) {
                markSpriteNeighborhoodResident(gl, dw, dw->objt.objects[parent].spriteId, 1);
                parent = dw->objt.objects[parent].parentId;
            }
        }
    }

    for (uint32_t li = 0; li < room->layerCount; li++) {
        RoomLayer* layer = &room->layers[li];
        if (layer->backgroundData) markSpriteResident(gl, dw, layer->backgroundData->spriteIndex);
        if (layer->tilesData) markBackgroundResident(gl, dw, layer->tilesData->backgroundIndex);
        if (layer->assetsData) {
            for (uint32_t i = 0; i < layer->assetsData->legacyTileCount; i++) {
                if (layer->assetsData->legacyTiles[i].useSpriteDefinition) markSpriteResident(gl, dw, layer->assetsData->legacyTiles[i].backgroundDefinition);
                else markBackgroundResident(gl, dw, layer->assetsData->legacyTiles[i].backgroundDefinition);
            }
            for (uint32_t i = 0; i < layer->assetsData->spriteCount; i++) markSpriteResident(gl, dw, layer->assetsData->sprites[i].spriteIndex);
        }
    }

unload_stale:
    for (uint32_t i = 0; i < gl->tpagCount; i++) {
        if (!gl->tpags[i].keepResident && gl->tpags[i].isLoaded) {
            unloadTpagTexture(gl, i);
        }
    }

    prefetchRoomTextures(gl);
    gl->pendingResidencyUpdate = false;
}

static void ctrDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount) return;

    if (!gl->tpags[tpagIndex].isLoaded) {
        loadDynamicSprite(gl, dw, tpagIndex);
        if (!gl->tpags[tpagIndex].isLoaded) return;
    }

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    float lx0 = ((float) tpag->targetX - originX) * xscale;
    float ly0 = ((float) tpag->targetY - originY) * yscale;
    float lx1 = lx0 + ((float) tpag->sourceWidth) * xscale;
    float ly1 = ly0 + ((float) tpag->sourceHeight) * yscale;

    uint8_t r = BGR_R(color); uint8_t g = BGR_G(color); uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    if (angleDeg == 0.0f) {
        float cx = roundf(x);
        float cy = roundf(y);
        float x0 = cx + roundf(lx0);
        float y0 = cy + roundf(ly0);
        float x1 = x0 + roundf(((float) tpag->sourceWidth) * xscale);
        float y1 = y0 + roundf(((float) tpag->sourceHeight) * yscale);

        ctrDrawTpagRegion(gl, tpagIndex, 0.0f, 0.0f, (float)tpag->sourceWidth, (float)tpag->sourceHeight,
                          x0, y0, x1, y0, x1, y1, x0, y1, r, g, b, a);
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float c = cosf(angleRad);
        float s = sinf(angleRad);

        float x0 = lx0 * c - ly0 * s + x; float y0 = lx0 * s + ly0 * c + y;
        float x1 = lx1 * c - ly0 * s + x; float y1 = lx1 * s + ly0 * c + y;
        float x2 = lx1 * c - ly1 * s + x; float y2 = lx1 * s + ly1 * c + y;
        float x3 = lx0 * c - ly1 * s + x; float y3 = lx0 * s + ly1 * c + y;

        ctrDrawTpagRegion(gl, tpagIndex, 0.0f, 0.0f, (float)tpag->sourceWidth, (float)tpag->sourceHeight,
                          x0, y0, x1, y1, x2, y2, x3, y3, r, g, b, a);
    }
}

static void ctrDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount) return;

    if (!gl->tpags[tpagIndex].isLoaded) {
        loadDynamicSprite(gl, dw, tpagIndex);
        if (!gl->tpags[tpagIndex].isLoaded) return;
    }

    uint8_t r = BGR_R(color); uint8_t g = BGR_G(color); uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    float cx = roundf(x);
    float cy = roundf(y);
    float x1 = cx + roundf((float)srcW * xscale);
    float y1 = cy + roundf((float)srcH * yscale);

    ctrDrawTpagRegion(gl, tpagIndex, (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH,
                      cx, cy, x1, cy, x1, y1, cx, y1, r, g, b, a);
}

static void emitColoredQuad(CtrRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    uint8_t cr = (uint8_t)(r * 255.0f); uint8_t cg = (uint8_t)(g * 255.0f);
    uint8_t cb = (uint8_t)(b * 255.0f); uint8_t ca = (uint8_t)(a * 255.0f);
    ctrPushQuad(gl, gl->whiteTexture, x0, y0, x1, y0, x1, y1, x0, y1, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    float r = (float) BGR_R(color) / 255.0f; float g = (float) BGR_G(color) / 255.0f; float b = (float) BGR_B(color) / 255.0f;

    float minX = fminf(x1, x2); float maxX = fmaxf(x1, x2);
    float minY = fminf(y1, y2); float maxY = fmaxf(y1, y2);

    if (outline) {
        emitColoredQuad(gl, minX, minY, maxX + 1.0f, minY + 1.0f, r, g, b, alpha);
        emitColoredQuad(gl, minX, maxY, maxX + 1.0f, maxY + 1.0f, r, g, b, alpha);
        emitColoredQuad(gl, minX, minY + 1.0f, minX + 1.0f, maxY, r, g, b, alpha);
        emitColoredQuad(gl, maxX, minY + 1.0f, maxX + 1.0f, maxY, r, g, b, alpha);
    } else {
        emitColoredQuad(gl, minX, minY, maxX + 1.0f, maxY + 1.0f, r, g, b, alpha);
    }
}

static void ctrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t cr = BGR_R(color); uint8_t cg = BGR_G(color); uint8_t cb = BGR_B(color);
    uint8_t ca = (uint8_t)(alpha * 255.0f);

    float w = fmaxf(1.0f, width);

    if (fabsf(x1 - x2) < 0.01f || fabsf(y1 - y2) < 0.01f) {
        float minX = fminf(x1, x2); float maxX = fmaxf(x1, x2) + 1.0f;
        float minY = fminf(y1, y2); float maxY = fmaxf(y1, y2) + 1.0f;

        if (fabsf(x1 - x2) < 0.01f) { // Вертикально
            minX -= floorf(w * 0.5f); maxX = minX + w;
        } else { // Горизонтально
            minY -= floorf(w * 0.5f); maxY = minY + w;
        }
        ctrPushQuad(gl, gl->whiteTexture, minX, minY, maxX, minY, maxX, maxY, minX, maxY, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
        return;
    }

    // Диагональные
    float dx = x2 - x1; float dy = y2 - y1; float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) {
        ctrPushQuad(gl, gl->whiteTexture, x1, y1, x1+1, y1, x1+1, y1+1, x1, y1+1, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
        return;
    }

    x2 += (dx / len); y2 += (dy / len); dx = x2 - x1; dy = y2 - y1; len += 1.0f;
    float halfW = w * 0.5f; float px = (-dy / len) * halfW; float py = (dx / len) * halfW;

    ctrPushQuad(gl, gl->whiteTexture, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t c1r = BGR_R(color1); uint8_t c1g = BGR_G(color1); uint8_t c1b = BGR_B(color1);
    uint8_t c2r = BGR_R(color2); uint8_t c2g = BGR_G(color2); uint8_t c2b = BGR_B(color2);
    uint8_t ca = (uint8_t)(alpha * 255.0f);

    float w = fmaxf(1.0f, width);

    if (fabsf(x1 - x2) < 0.01f || fabsf(y1 - y2) < 0.01f) {
        float minX = fminf(x1, x2); float maxX = fmaxf(x1, x2) + 1.0f;
        float minY = fminf(y1, y2); float maxY = fmaxf(y1, y2) + 1.0f;

        if (fabsf(x1 - x2) < 0.01f) { minX -= floorf(w * 0.5f); maxX = minX + w; }
        else { minY -= floorf(w * 0.5f); maxY = minY + w; }

        uint8_t r0, g0, b0, r1, g1, b1;
        if (x1 <= x2 && y1 <= y2) { r0=c1r; g0=c1g; b0=c1b; r1=c2r; g1=c2g; b1=c2b; }
        else { r0=c2r; g0=c2g; b0=c2b; r1=c1r; g1=c1g; b1=c1b; }

        if (fabsf(y1 - y2) < 0.01f) {
            ctrPushQuadGradient(gl, gl->whiteTexture, minX, minY, maxX, minY, maxX, maxY, minX, maxY, 0.5f, 0.5f, 0.5f, 0.5f, r0, g0, b0, ca, r1, g1, b1, ca, r1, g1, b1, ca, r0, g0, b0, ca);
        } else {
            ctrPushQuadGradient(gl, gl->whiteTexture, minX, minY, maxX, minY, maxX, maxY, minX, maxY, 0.5f, 0.5f, 0.5f, 0.5f, r0, g0, b0, ca, r0, g0, b0, ca, r1, g1, b1, ca, r1, g1, b1, ca);
        }
        return;
    }

    float dx = x2 - x1; float dy = y2 - y1; float len = sqrtf(dx * dx + dy * dy);
    x2 += (dx / len); y2 += (dy / len); dx = x2 - x1; dy = y2 - y1; len += 1.0f;
    float halfW = w * 0.5f; float px = (-dy / len) * halfW; float py = (dx / len) * halfW;

    ctrPushQuadGradient(gl, gl->whiteTexture, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, 0.5f, 0.5f, 0.5f, 0.5f, c1r, c1g, c1b, ca, c1r, c1g, c1b, ca, c2r, c2g, c2b, ca, c2r, c2g, c2b, ca);
}

static void ctrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    if (outline) {
        ctrDrawLine(renderer, x1, y1, x2, y2, 1, renderer->drawColor, 1.0f);
        ctrDrawLine(renderer, x2, y2, x3, y3, 1, renderer->drawColor, 1.0f);
        ctrDrawLine(renderer, x3, y3, x1, y1, 1, renderer->drawColor, 1.0f);
    } else {
        uint8_t r = BGR_R(renderer->drawColor);
        uint8_t g = BGR_G(renderer->drawColor);
        uint8_t b = BGR_B(renderer->drawColor);
        uint8_t a = (uint8_t)(renderer->drawAlpha * 255.0f);

        ctrPushQuad(gl, gl->whiteTexture, x1, y1, x2, y2, x3, y3, x3, y3, 0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
    }
}

// ===[ Text Drawing ]===

typedef struct {
    Font* font;
    uint32_t tpagIndex;
    Sprite* spriteFontSprite;
} CtrFontState;

static bool ctrResolveFontState(CtrRenderer* gl, DataWin* dw, Font* font, CtrFontState* state) {
    state->font = font;
    state->tpagIndex = 0;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
        if (fontTpagIndex < 0 || fontTpagIndex >= gl->tpagCount) return false;

        if (!gl->tpags[fontTpagIndex].isLoaded) {
            loadDynamicSprite(gl, dw, fontTpagIndex);
            if (!gl->tpags[fontTpagIndex].isLoaded) return false;
        }
        state->tpagIndex = fontTpagIndex;
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    return true;
}

static bool ctrResolveGlyph(CtrRenderer* gl, DataWin* dw, CtrFontState* state, FontGlyph* glyph, float cursorX, float cursorY, uint32_t* outTpagIndex, float* outSrcX, float* outSrcY, float* outSrcW, float* outSrcH, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex || glyphIndex >= (int32_t) sprite->textureCount) return false;

        uint32_t tpagOffset = sprite->textureOffsets[glyphIndex];
        int32_t tpagIdx = DataWin_resolveTPAG(dw, tpagOffset);
        if (tpagIdx < 0 || tpagIdx >= gl->tpagCount) return false;

        if (!gl->tpags[tpagIdx].isLoaded) {
            loadDynamicSprite(gl, dw, tpagIdx);
            if (!gl->tpags[tpagIdx].isLoaded) return false;
        }

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        *outTpagIndex = tpagIdx;
        *outSrcX = 0;
        *outSrcY = 0;
        *outSrcW = (float) glyphTpag->sourceWidth;
        *outSrcH = (float) glyphTpag->sourceHeight;
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) ((int32_t) glyphTpag->targetY - sprite->originY);
    } else {
        *outTpagIndex = state->tpagIndex;
        *outSrcX = (float) glyph->sourceX;
        *outSrcY = (float) glyph->sourceY;
        *outSrcW = (float) glyph->sourceWidth;
        *outSrcH = (float) glyph->sourceHeight;
        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void ctrDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    CtrFontState fontState;
    if (!ctrResolveFontState(gl, dw, font, &fontState)) return;

    uint8_t r = BGR_R(renderer->drawColor); uint8_t g = BGR_G(renderer->drawColor); uint8_t b = BGR_B(renderer->drawColor);
    uint8_t a = (uint8_t) ((renderer->drawAlpha <= 0.0f) ? 0 : (renderer->drawAlpha >= 1.0f ? 255 : (renderer->drawAlpha * 255.0f + 0.5f)));

    int32_t textLen = (int32_t) strlen(text); if (textLen == 0) return;
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = TextUtils_lineStride(font);
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;

        while (lineLen > pos) {
            int32_t oldPos = pos;
            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            if (pos == oldPos) { pos++; continue; }

            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) { cursorX += glyph->shift; continue; }

            float srcX, srcY, srcW, srcH, localX0, localY0;
            uint32_t tpagIndex;
            if (!ctrResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &tpagIndex, &srcX, &srcY, &srcW, &srcH, &localX0, &localY0)) {
                cursorX += glyph->shift; continue;
            }

            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

            ctrDrawTpagRegion(gl, tpagIndex, srcX, srcY, srcW, srcH, px0, py0, px1, py1, px2, py2, px3, py3, r, g, b, a);

            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }
        cursorY += lineStride;
        lineStart = (textLen > lineEnd) ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }
}

static void ctrDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float floatAlpha) {
    float savedAlpha = renderer->drawAlpha;
    renderer->drawAlpha = floatAlpha;
    ctrDrawText(renderer, text, x, y, xscale, yscale, angleDeg);
    renderer->drawAlpha = savedAlpha;
}

static int32_t ctrCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    return -1;
}

static void ctrDeleteSprite(Renderer* renderer, int32_t spriteIndex) {}

static RendererVtable ctrVtable = {
    .init = ctrInit, .destroy = ctrDestroy, .beginFrame = ctrBeginFrame, .endFrame = ctrEndFrame,
    .beginView = ctrBeginView, .endView = ctrEndView, .beginGUI = ctrBeginGUI, .endGUI = ctrEndGUI,
    .drawSprite = ctrDrawSprite, .drawSpritePart = ctrDrawSpritePart, .drawRectangle = ctrDrawRectangle,
    .drawLine = ctrDrawLine, .drawLineColor = ctrDrawLineColor, .drawTriangle = ctrDrawTriangle,
    .drawText = ctrDrawText, .drawTextColor = ctrDrawTextColor, .flush = ctrRendererFlush,
    .createSpriteFromSurface = ctrCreateSpriteFromSurface, .deleteSprite = ctrDeleteSprite,
    .drawTile = nullptr, .onRoomChanged = ctrOnRoomChanged,
};

Renderer* CtrRenderer_create(void) {
    CtrRenderer* gl = safeCalloc(1, sizeof(CtrRenderer));
    gl->base.vtable = &ctrVtable;
    gl->base.drawColor = 0xFFFFFF;
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    return (Renderer*) gl;
}
// --- END OF FILE ctr_renderer.c ---