// --- START OF FILE ctr_renderer.c ---

#include "ctr_renderer.h"
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
#define CTR_MAX_CIRCLE_SEGMENTS 128
#define CTR_MAX_ROUNDRECT_CORNER_SEGMENTS 64
#define CTR_MAX_ROUNDRECT_POINTS (CTR_MAX_ROUNDRECT_CORNER_SEGMENTS * 4 + 1)

// LRU кэш: очищает VRAM от неиспользуемых динамических текстур (выстрелы, UI) через 10 секунд
#define LRU_SWEEP_INTERVAL 60
#define LRU_IDLE_THRESHOLD 600

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

static int32_t ctrClampSegments(int32_t precision, int32_t minSegments, int32_t maxSegments) {
    if (precision < minSegments) return minSegments;
    if (precision > maxSegments) return maxSegments;
    return precision;
}

static uint8_t ctrAlphaToByte(float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return (uint8_t)(alpha * 255.0f);
}

static void ctrColorToBytes(uint32_t color, float alpha, uint8_t* outR, uint8_t* outG, uint8_t* outB, uint8_t* outA) {
    *outR = BGR_R(color);
    *outG = BGR_G(color);
    *outB = BGR_B(color);
    *outA = ctrAlphaToByte(alpha);
}

static void ctrPushTriangleGradient(CtrRenderer* gl,
    float x1, float y1, float x2, float y2, float x3, float y3,
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1,
    uint8_t r2, uint8_t g2, uint8_t b2, uint8_t a2,
    uint8_t r3, uint8_t g3, uint8_t b3, uint8_t a3)
{
    ctrPushQuadGradient(gl, gl->whiteTexture,
        x1, y1, x2, y2, x3, y3, x3, y3,
        0.5f, 0.5f, 0.5f, 0.5f,
        r1, g1, b1, a1,
        r2, g2, b2, a2,
        r3, g3, b3, a3,
        r3, g3, b3, a3);
}

static void ctrPushTriangleSolid(CtrRenderer* gl,
    float x1, float y1, float x2, float y2, float x3, float y3,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    ctrPushTriangleGradient(gl, x1, y1, x2, y2, x3, y3,
        r, g, b, a,
        r, g, b, a,
        r, g, b, a);
}

static void ctrAppendArcPoints(float* outX, float* outY, int32_t* count,
                               float cx, float cy, float rx, float ry,
                               float startAngle, float endAngle, int32_t segments, bool skipFirst) {
    for (int32_t i = 0; i <= segments; i++) {
        if (skipFirst && i == 0) continue;
        if (*count >= CTR_MAX_ROUNDRECT_POINTS) break;

        float t = (segments > 0) ? ((float)i / (float)segments) : 0.0f;
        float angle = startAngle + (endAngle - startAngle) * t;

        outX[*count] = cx + cosf(angle) * rx;
        outY[*count] = cy + sinf(angle) * ry;
        (*count)++;
    }
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

    // Half-Texel Inset: сшивает графику без зазоров
    float halfU = 0.5f * tpagData->uvScaleX;
    float halfV = 0.5f * tpagData->uvScaleY;

    float u0 = drawSrcX * tpagData->uvScaleX + halfU;
    float v0 = drawSrcY * tpagData->uvScaleY + halfV;
    float u1 = (drawSrcX + drawSrcW) * tpagData->uvScaleX - halfU;
    float v1 = (drawSrcY + drawSrcH) * tpagData->uvScaleY - halfV;

    ctrPushQuad(gl, tpagData->tex, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

static void unloadTpagTexture(CtrRenderer* gl, uint32_t tpagIndex) {
    if (tpagIndex >= gl->tpagCount) return;
    CtrTpagData* tpag = &gl->tpags[tpagIndex];
    if (tpag->isLoaded && tpag->tex != 0) {
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
    // Version the cache key so stale pre-fix oversized TPAG entries get rebuilt.
    uint32_t hash = 2166136261u ^ 0x7777u ^ 0x00020000u;
    hash ^= (pId * 31337);
    hash ^= (tId * 1000003);
    hash ^= (txtr->blobSize * 199999);
    return hash;
}

static inline uint8_t clamp_alpha(float alpha) {
    if (alpha <= 0.0f) return 0;
    if (alpha >= 1.0f) return 255;
    return (uint8_t)(alpha * 255.0f + 0.5f);
}

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
            uint8_t* tempPngBuffer = txtr->blobData;

            if (tempPngBuffer != nullptr) {
                pixels = stbi_load_from_memory(tempPngBuffer, (int) txtr->blobSize, &origW, &origH, &channels, 4);
            }
        }

        for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
            TexturePageItem* item = &dw->tpag.items[tId];
            if (item->texturePageId == pId && !gl->tpags[tId].isLoaded && gl->tpags[tId].keepResident) {

                int extractX = item->sourceX;
                int extractY = item->sourceY;
                int extW = item->sourceWidth;
                int extH = item->sourceHeight;

                if (extW <= 0) extW = 1;
                if (extH <= 0) extH = 1;

                float downscaleFactor = 1.0f;
                if (extW > 1024 || extH > 1024) {
                    float scaleX = 1024.0f / (float) extW;
                    float scaleY = 1024.0f / (float) extH;
                    downscaleFactor = fminf(scaleX, scaleY);
                    if (downscaleFactor > 1.0f) downscaleFactor = 1.0f;
                }

                uint32_t hash = compute_tpag_hash(txtr, pId, tId);
                GLuint tex;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);

                int cacheW, cacheH;
                if (nova_texture_cache_load(hash, &cacheW, &cacheH)) {
                    gl->tpags[tId].tex = tex;
                    gl->tpags[tId].uvScaleX = 1.0f / (float)cacheW;
                    gl->tpags[tId].uvScaleY = 1.0f / (float)cacheH;
                    gl->tpags[tId].downscaleFactor = downscaleFactor;
                    gl->tpags[tId].isLoaded = true;
                    gl->tpags[tId].lastUsedFrame = g_frameCounter;
                    continue;
                }

                if (pixels == nullptr) {
                    glDeleteTextures(1, &tex);

                    // ФИКС 0 FPS: Если текстура битая или загрузка провалилась (например, из-за битого кэша),
                    // мы ОБЯЗАНЫ пометить её как isLoaded = true, иначе игра будет пытаться
                    // декодировать её с нуля каждый кадр и убьет процессор в 0 FPS!
                    gl->tpags[tId].isLoaded = true;
                    gl->tpags[tId].tex = 0;
                    gl->tpags[tId].lastUsedFrame = g_frameCounter;
                    continue;
                }

                int scaledW = (int) ceilf((float) extW * downscaleFactor);
                int scaledH = (int) ceilf((float) extH * downscaleFactor);
                if (scaledW <= 0) scaledW = 1;
                if (scaledH <= 0) scaledH = 1;
                if (scaledW > 1024) scaledW = 1024;
                if (scaledH > 1024) scaledH = 1024;

                int potW = next_pot(scaledW);
                int potH = next_pot(scaledH);

                bool has_alpha = false;
                for (int y = 0; y < extH; y++) {
                    for (int x = 0; x < extW; x++) {
                        int srcY = extractY + y;
                        int srcX = extractX + x;
                        if (srcY < origH && srcX < origW) {
                            if (pixels[(srcY * origW + srcX) * 4 + 3] < 255) {
                                has_alpha = true;
                                goto alpha_found;
                            }
                        }
                    }
                }
                alpha_found:;

                uint16_t* pixels16 = malloc(potW * potH * sizeof(uint16_t));
                if (pixels16) {
                    memset(pixels16, 0, potW * potH * sizeof(uint16_t));

                    float invScale = (downscaleFactor > 0.0f) ? (1.0f / downscaleFactor) : 1.0f;
                    for (int y = 0; y < scaledH; y++) {
                        int srcLocalY = (int) floorf(((float) y + 0.5f) * invScale);
                        if (srcLocalY < 0) srcLocalY = 0;
                        if (srcLocalY >= extH) srcLocalY = extH - 1;
                        int srcY = extractY + srcLocalY;

                        for (int x = 0; x < scaledW; x++) {
                            int srcLocalX = (int) floorf(((float) x + 0.5f) * invScale);
                            if (srcLocalX < 0) srcLocalX = 0;
                            if (srcLocalX >= extW) srcLocalX = extW - 1;
                            int srcX = extractX + srcLocalX;

                            if (srcY < 0 || srcX < 0 || srcY >= origH || srcX >= origW) continue;

                            int srcOffset = (srcY * origW + srcX) * 4;
                            uint8_t r = pixels[srcOffset + 0];
                            uint8_t g = pixels[srcOffset + 1];
                            uint8_t b = pixels[srcOffset + 2];
                            uint8_t a = pixels[srcOffset + 3];

                            int dstOffset = y * potW + x;

                            if (has_alpha) {
                                pixels16[dstOffset] = pack_rgba4444(r, g, b, a);
                            } else {
                                pixels16[dstOffset] = pack_rgb565(r, g, b);
                            }
                        }
                    }

                    GLenum format = has_alpha ? GL_RGBA : GL_RGB;
                    GLenum type   = has_alpha ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_SHORT_5_6_5;

                    // Очистка старых ошибок GL (защита от бесконечного цикла, если контекст сломан)
                    int err_limit = 100;
                    while (glGetError() != GL_NO_ERROR && err_limit-- > 0);

                    glTexImage2D(GL_TEXTURE_2D, 0, format, potW, potH, 0, format, type, pixels16);

                    if (glGetError() == GL_NO_ERROR) {
                        gl->tpags[tId].tex = tex;
                        gl->tpags[tId].uvScaleX = 1.0f / (float)potW;
                        gl->tpags[tId].uvScaleY = 1.0f / (float)potH;
                        gl->tpags[tId].downscaleFactor = downscaleFactor;
                        nova_texture_cache_save(hash);

                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    } else {
                        glDeleteTextures(1, &tex);
                        gl->tpags[tId].tex = 0;
                    }

                    // ФИКС 0 FPS: Ставим флаг в любом случае, чтобы не застрять в ретраях!
                    gl->tpags[tId].isLoaded = true;
                    gl->tpags[tId].lastUsedFrame = g_frameCounter;

                    free(pixels16);
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

static void ctrBuildFullTextureCache(CtrRenderer* gl) {
    DataWin* dw = gl->base.dataWin;
    const char* flagPath = "sdmc:/3ds/butterscotch/cache/cache_ready.flag";

    // --- ФИКС БЫСТРОЙ ЗАГРУЗКИ ---
    // Проверяем, существует ли файл-флаг. Если да — кэш уже был собран ранее.
    FILE* flagFile = fopen(flagPath, "r");
    if (flagFile) {
        fclose(flagFile);
        fprintf(stderr, "--- FAST BOOT: Cache flag found. Skipping verification! ---\n");

        // ВАЖНО: Если мы пропускаем кэширование, нам всё равно нужно очистить ОЗУ
        // от "сырых" PNG картинок из data.win, иначе игре не хватит памяти и она вылетит.
        for (uint32_t pId = 0; pId < dw->txtr.count; pId++) {
            if (dw->txtr.textures[pId].blobData) {
                free(dw->txtr.textures[pId].blobData);
                dw->txtr.textures[pId].blobData = NULL;
            }
        }
        return; // Мгновенно выходим из функции!
    }
    // -----------------------------

    fprintf(stderr, "--- STARTING PRE-CACHE VERIFICATION (First Boot) ---\n");

    int totalExtracted = 0;

    for (uint32_t pId = 0; pId < dw->txtr.count; pId++) {
        Texture* txtr = &dw->txtr.textures[pId];

        // Проверяем, есть ли хотя бы один отсутствующий спрайт для этой страницы
        bool needsExtraction = false;
        for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
            TexturePageItem* item = &dw->tpag.items[tId];
            if (item->texturePageId == pId) {
                uint32_t hash = compute_tpag_hash(txtr, pId, tId);
                if (!nova_texture_cache_has(hash)) {
                    needsExtraction = true;
                    break;
                }
            }
        }

        if (needsExtraction) {
            fprintf(stderr, "Extracting missing sprites from Texture Page %u/%u...\n", pId + 1, dw->txtr.count);

            int origW = 0, origH = 0, channels = 0;
            uint8_t* pixels = NULL;

            if (txtr->blobData) {
                pixels = stbi_load_from_memory(txtr->blobData, (int) txtr->blobSize, &origW, &origH, &channels, 4);
            }

            if (!pixels) {
                fprintf(stderr, "WARNING: Failed to decode PNG for page %u (Out of Memory?)\n", pId);
            } else {
                for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
                    TexturePageItem* item = &dw->tpag.items[tId];
                    if (item->texturePageId == pId) {
                        uint32_t hash = compute_tpag_hash(txtr, pId, tId);
                        if (nova_texture_cache_has(hash)) continue;

                        int extractX = item->sourceX;
                        int extractY = item->sourceY;
                        int extW = item->sourceWidth;
                        int extH = item->sourceHeight;

                        if (extW <= 0) extW = 1;
                        if (extH <= 0) extH = 1;

                        float downscaleFactor = 1.0f;
                        if (extW > 1024 || extH > 1024) {
                            float scaleX = 1024.0f / (float) extW;
                            float scaleY = 1024.0f / (float) extH;
                            downscaleFactor = fminf(scaleX, scaleY);
                            if (downscaleFactor > 1.0f) downscaleFactor = 1.0f;
                        }

                        int scaledW = (int) ceilf((float) extW * downscaleFactor);
                        int scaledH = (int) ceilf((float) extH * downscaleFactor);
                        if (scaledW <= 0) scaledW = 1;
                        if (scaledH <= 0) scaledH = 1;
                        if (scaledW > 1024) scaledW = 1024;
                        if (scaledH > 1024) scaledH = 1024;

                        int potW = next_pot(scaledW);
                        int potH = next_pot(scaledH);

                        bool has_alpha = false;
                        for (int y = 0; y < extH; y++) {
                            for (int x = 0; x < extW; x++) {
                                int srcY = extractY + y;
                                int srcX = extractX + x;
                                if (srcY < origH && srcX < origW) {
                                    if (pixels[(srcY * origW + srcX) * 4 + 3] < 255) {
                                        has_alpha = true;
                                        goto alpha_found_cache;
                                    }
                                }
                            }
                        }
                        alpha_found_cache:;

                        uint16_t* pixels16 = malloc(potW * potH * sizeof(uint16_t));
                        if (pixels16) {
                            memset(pixels16, 0, potW * potH * sizeof(uint16_t));

                            float invScale = (downscaleFactor > 0.0f) ? (1.0f / downscaleFactor) : 1.0f;
                            for (int y = 0; y < scaledH; y++) {
                                int srcLocalY = (int) floorf(((float) y + 0.5f) * invScale);
                                if (srcLocalY < 0) srcLocalY = 0;
                                if (srcLocalY >= extH) srcLocalY = extH - 1;
                                int srcY = extractY + srcLocalY;

                                for (int x = 0; x < scaledW; x++) {
                                    int srcLocalX = (int) floorf(((float) x + 0.5f) * invScale);
                                    if (srcLocalX < 0) srcLocalX = 0;
                                    if (srcLocalX >= extW) srcLocalX = extW - 1;
                                    int srcX = extractX + srcLocalX;

                                    if (srcY < 0 || srcX < 0 || srcY >= origH || srcX >= origW) continue;

                                    int srcOffset = (srcY * origW + srcX) * 4;
                                    uint8_t r = pixels[srcOffset + 0];
                                    uint8_t g = pixels[srcOffset + 1];
                                    uint8_t b = pixels[srcOffset + 2];
                                    uint8_t a = pixels[srcOffset + 3];

                                    int dstOffset = y * potW + x;

                                    if (has_alpha) {
                                        pixels16[dstOffset] = pack_rgba4444(r, g, b, a);
                                    } else {
                                        pixels16[dstOffset] = pack_rgb565(r, g, b);
                                    }
                                }
                            }

                            GLenum format = has_alpha ? GL_RGBA : GL_RGB;
                            GLenum type   = has_alpha ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_SHORT_5_6_5;

                            GLuint tex;
                            glGenTextures(1, &tex);
                            glBindTexture(GL_TEXTURE_2D, tex);

                            while (glGetError() != GL_NO_ERROR);

                            glTexImage2D(GL_TEXTURE_2D, 0, format, potW, potH, 0, format, type, pixels16);

                            if (glGetError() == GL_NO_ERROR) {
                                nova_texture_cache_save(hash);
                                totalExtracted++;
                            }

                            glDeleteTextures(1, &tex);
                            free(pixels16);
                        }
                    }
                }
                stbi_image_free(pixels);
            }
        }

        if (txtr->blobData) {
            free(txtr->blobData);
            txtr->blobData = NULL;
        }
    }

    fprintf(stderr, "--- PRE-CACHE VERIFICATION COMPLETE (Extracted %d items) ---\n", totalExtracted);

    // --- ФИКС БЫСТРОЙ ЗАГРУЗКИ (Создание флага) ---
    // После успешного извлечения всего кэша создаем файл-флаг,
    // чтобы больше никогда не выполнять этот медленный процесс.
    FILE* outFlag = fopen(flagPath, "w");
    if (outFlag) {
        fputs("READY", outFlag);
        fclose(outFlag);
        fprintf(stderr, "--- Cache Flag saved! Next boot will be fast. ---\n");
    }
    // ----------------------------------------------
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

    ctrBuildFullTextureCache(gl);

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

    if (gl->pendingResidencyUpdate && g_frameCounter >= gl->pendingResidencyReadyFrame) {
        gl->pendingResidencyUpdate = false;

        for (uint32_t i = 0; i < gl->tpagCount; i++) {
            if (!gl->tpags[i].keepResident && gl->tpags[i].isLoaded) {
                unloadTpagTexture(gl, i);
            }
        }

        prefetchRoomTextures(gl);
    }

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

    gl->pendingResidencyUpdate = true;
    gl->pendingResidencyReadyFrame = g_frameCounter;
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

    // ПРИМЕНЯЕМ ФИКС АЛЬФЫ
    uint8_t a = clamp_alpha(alpha);
    if (a == 0) return; // Оптимизация: не тратим время на отрисовку невидимого

    if (angleDeg == 0.0f) {
        // ФИКС СЕТКИ БОЯ: Округляем финальные координаты, а не начальные!
        float x0 = roundf(x + lx0);
        float y0 = roundf(y + ly0);
        float x1 = roundf(x + lx1);
        float y1 = roundf(y + ly1);

        // Анти-схлопывание: если после округления ширина стала 0, спасаем её
        if ((lx1 - lx0) > 0.0f && x1 <= x0) x1 = x0 + 1.0f;
        if ((ly1 - ly0) > 0.0f && y1 <= y0) y1 = y0 + 1.0f;

        // Хак специально для Undertale/Deltarune:
        // Если это спрайт 1x1 пиксель (считай, линия), то на экране 3DS
        // мы принудительно делаем его толщиной 2 пикселя, иначе он потеряется.
        if (tpag->sourceWidth == 1 && tpag->sourceHeight == 1) {
            if (fabsf(xscale) > 0.0f && fabsf(xscale) <= 1.5f) x1 = x0 + 2.0f; // Вертикальная палка
            if (fabsf(yscale) > 0.0f && fabsf(yscale) <= 1.5f) y1 = y0 + 2.0f; // Горизонтальная палка
        }

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
    uint8_t a = clamp_alpha(alpha);
    if (a == 0) return;

    float cx = roundf(x);
    float cy = roundf(y);
    float x1 = cx + roundf((float)srcW * xscale);
    float y1 = cy + roundf((float)srcH * yscale);

    ctrDrawTpagRegion(gl, tpagIndex, (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH,
                      cx, cy, x1, cy, x1, y1, cx, y1, r, g, b, a);
}

static void emitColoredQuad(CtrRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    uint8_t cr = (uint8_t)(r * 255.0f); uint8_t cg = (uint8_t)(g * 255.0f);
    uint8_t cb = (uint8_t)(b * 255.0f);
    uint8_t ca = clamp_alpha(a); // ЗАМЕНИТЬ
    if (ca == 0) return; // ДОБАВИТЬ
    ctrPushQuad(gl, gl->whiteTexture, x0, y0, x1, y0, x1, y1, x0, y1, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    float left = roundf(fminf(x1, x2));
    float right = roundf(fmaxf(x1, x2));
    float top = roundf(fminf(y1, y2));
    float bottom = roundf(fmaxf(y1, y2));

    if (outline) {
        // ФИКС: Увеличиваем толщину рамки для 3DS до 2.0f пикселей
        // чтобы она не растворялась при масштабировании видов (views).
        float thick = 2.0f;

        // Рисуем 4 ровные планки
        emitColoredQuad(gl, left, top, right + 1.0f, top + thick, r, g, b, alpha); // Верх
        emitColoredQuad(gl, left, bottom - thick + 1.0f, right + 1.0f, bottom + 1.0f, r, g, b, alpha); // Низ
        emitColoredQuad(gl, left, top + thick, left + thick, bottom - thick + 1.0f, r, g, b, alpha); // Лево
        emitColoredQuad(gl, right - thick + 1.0f, top + thick, right + 1.0f, bottom - thick + 1.0f, r, g, b, alpha); // Право
    } else {
        emitColoredQuad(gl, left, top, right + 1.0f, bottom + 1.0f, r, g, b, alpha);
    }
}

static void ctrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t cr = BGR_R(color); uint8_t cg = BGR_G(color); uint8_t cb = BGR_B(color);
    uint8_t ca = (uint8_t)(alpha * 255.0f);

    // ФИКС: Задаем жесткий минимум толщины 2px для 3DS
    float actualWidth = (width < 2.0f) ? 2.0f : width;

    // Быстрый путь для прямых (горизонтальных/вертикальных) линий
    if (fabsf(x1 - x2) < 0.01f || fabsf(y1 - y2) < 0.01f) {
        float rx1 = roundf(fminf(x1, x2));
        float ry1 = roundf(fminf(y1, y2));
        float rx2 = roundf(fmaxf(x1, x2));
        float ry2 = roundf(fmaxf(y1, y2));

        if (fabsf(x1 - x2) < 0.01f) {
            // Вертикальная линия: расширяем по X
            rx1 -= actualWidth * 0.5f;
            rx2 += actualWidth * 0.5f;
            ry2 += 1.0f; // Закрываем длину
        } else {
            // Горизонтальная линия: расширяем по Y
            rx2 += 1.0f; // Закрываем длину
            ry1 -= actualWidth * 0.5f;
            ry2 += actualWidth * 0.5f;
        }

        ctrPushQuad(gl, gl->whiteTexture,
            rx1, ry1, rx2, ry1, rx2, ry2, rx1, ry2,
            0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
        return;
    }

    // Диагональные линии
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    // Вытягиваем края линии на полпикселя, чтобы избежать разрывов на углах рамок
    x2 += (dx / len) * 0.5f;
    y2 += (dy / len) * 0.5f;
    x1 -= (dx / len) * 0.5f;
    y1 -= (dy / len) * 0.5f;
    dx = x2 - x1; dy = y2 - y1; len = sqrtf(dx * dx + dy * dy);

    float halfW = actualWidth * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    ctrPushQuad(gl, gl->whiteTexture,
        x1 + px, y1 + py,
        x1 - px, y1 - py,
        x2 - px, y2 - py,
        x2 + px, y2 + py,
        0.5f, 0.5f, 0.5f, 0.5f,
        cr, cg, cb, ca);
}

static void ctrDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t c1r = BGR_R(color1); uint8_t c1g = BGR_G(color1); uint8_t c1b = BGR_B(color1);
    uint8_t c2r = BGR_R(color2); uint8_t c2g = BGR_G(color2); uint8_t c2b = BGR_B(color2);
    uint8_t ca = (uint8_t)(alpha * 255.0f);

    // ФИКС для 3DS
    float actualWidth = (width < 2.0f) ? 2.0f : width;

    if (fabsf(x1 - x2) < 0.01f || fabsf(y1 - y2) < 0.01f) {
        float rx1 = roundf(fminf(x1, x2));
        float ry1 = roundf(fminf(y1, y2));
        float rx2 = roundf(fmaxf(x1, x2));
        float ry2 = roundf(fmaxf(y1, y2));

        uint8_t r0, g0, b0, r1, g1, b1;
        if (x1 <= x2 && y1 <= y2) { r0=c1r; g0=c1g; b0=c1b; r1=c2r; g1=c2g; b1=c2b; }
        else { r0=c2r; g0=c2g; b0=c2b; r1=c1r; g1=c1g; b1=c1b; }

        if (fabsf(x1 - x2) < 0.01f) {
            // Вертикальная: градиент сверху вниз
            rx1 -= actualWidth * 0.5f; rx2 += actualWidth * 0.5f; ry2 += 1.0f;
            ctrPushQuadGradient(gl, gl->whiteTexture,
                rx1, ry1, rx2, ry1, rx2, ry2, rx1, ry2,
                0.5f, 0.5f, 0.5f, 0.5f,
                r0, g0, b0, ca, r0, g0, b0, ca, r1, g1, b1, ca, r1, g1, b1, ca);
        } else {
            // Горизонтальная: градиент слева направо
            rx2 += 1.0f; ry1 -= actualWidth * 0.5f; ry2 += actualWidth * 0.5f;
            ctrPushQuadGradient(gl, gl->whiteTexture,
                rx1, ry1, rx2, ry1, rx2, ry2, rx1, ry2,
                0.5f, 0.5f, 0.5f, 0.5f,
                r0, g0, b0, ca, r1, g1, b1, ca, r1, g1, b1, ca, r0, g0, b0, ca);
        }
        return;
    }

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    x2 += (dx / len) * 0.5f; y2 += (dy / len) * 0.5f;
    x1 -= (dx / len) * 0.5f; y1 -= (dy / len) * 0.5f;
    dx = x2 - x1; dy = y2 - y1; len = sqrtf(dx * dx + dy * dy);

    float halfW = actualWidth * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    ctrPushQuadGradient(gl, gl->whiteTexture,
        x1 + px, y1 + py,
        x1 - px, y1 - py,
        x2 - px, y2 - py,
        x2 + px, y2 + py,
        0.5f, 0.5f, 0.5f, 0.5f,
        c1r, c1g, c1b, ca,
        c1r, c1g, c1b, ca,
        c2r, c2g, c2b, ca,
        c2r, c2g, c2b, ca);
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
        uint8_t a = clamp_alpha(renderer->drawAlpha); // ЗАМЕНИТЬ

        ctrPushQuad(gl, gl->whiteTexture, x1, y1, x2, y2, x3, y3, x3, y3, 0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
    }
}

typedef struct {
    Font* font;
    uint32_t tpagIndex;
} CtrFontState;

static bool ctrResolveFontState(CtrRenderer* gl, DataWin* dw, Font* font, CtrFontState* state) {
    state->font = font;
    state->tpagIndex = 0;

    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (fontTpagIndex < 0 || fontTpagIndex >= (int32_t)gl->tpagCount) return false;

    if (!gl->tpags[fontTpagIndex].isLoaded) {
        loadDynamicSprite(gl, dw, fontTpagIndex);
        if (!gl->tpags[fontTpagIndex].isLoaded) return false;
    }
    state->tpagIndex = fontTpagIndex;

    return true;
}

static bool ctrResolveGlyph(CtrRenderer* gl, DataWin* dw, CtrFontState* state, FontGlyph* glyph, float cursorX, float cursorY, uint32_t* outTpagIndex, float* outSrcX, float* outSrcY, float* outSrcW, float* outSrcH, float* outLocalX0, float* outLocalY0) {
    *outTpagIndex = state->tpagIndex;
    *outSrcX = (float) glyph->sourceX;
    *outSrcY = (float) glyph->sourceY;
    *outSrcW = (float) glyph->sourceWidth;
    *outSrcH = (float) glyph->sourceHeight;
    *outLocalX0 = cursorX + glyph->offset;
    *outLocalY0 = cursorY;
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
    uint8_t a = clamp_alpha(renderer->drawAlpha); // ЗАМЕНИТЬ

    int32_t textLen = (int32_t) strlen(text); if (textLen == 0) return;
    int32_t lineCount = TextUtils_countLines(text, textLen);

    // ФИКС ВЫСОТЫ: Старые шрифты в GMS 1.4 часто имеют font->emSize равный 0.
    float lineStride = (float) font->emSize;
    if (lineStride <= 0.0f) {
        for (uint32_t i = 0; i < font->glyphCount; i++) {
            if (font->glyphs[i].sourceHeight > lineStride) {
                lineStride = (float) font->glyphs[i].sourceHeight;
            }
        }
        if (lineStride <= 0.0f) lineStride = 10.0f; // Безопасный фоллбэк
    }

    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;

    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;

    // ФИКС NaN МАТРИЦ: Вырезал font->scaleX и scaleY. В старом DataWin они не инициализируются!
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float cursorY = valignOffset;
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
static void ctrDrawTriangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3,
                                 uint32_t col1, uint32_t col2, uint32_t col3, float alpha, bool outline) {
    if (outline) {
        ctrDrawLine(renderer, x1, y1, x2, y2, 1.0f, col1, alpha);
        ctrDrawLine(renderer, x2, y2, x3, y3, 1.0f, col1, alpha);
        ctrDrawLine(renderer, x3, y3, x1, y1, 1.0f, col1, alpha);
        return;
    }

    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t r1, g1, b1, a1;
    uint8_t r2, g2, b2, a2;
    uint8_t r3, g3, b3, a3;
    ctrColorToBytes(col1, alpha, &r1, &g1, &b1, &a1);
    ctrColorToBytes(col2, alpha, &r2, &g2, &b2, &a2);
    ctrColorToBytes(col3, alpha, &r3, &g3, &b3, &a3);

    ctrPushTriangleGradient(gl, x1, y1, x2, y2, x3, y3,
        r1, g1, b1, a1,
        r2, g2, b2, a2,
        r3, g3, b3, a3);
}

static void ctrDrawEllipse(Renderer* renderer, float cx, float cy, float rx, float ry,
                           uint32_t color, float alpha, bool outline, int32_t precision) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx <= 0.0f || ry <= 0.0f) {
        ctrDrawRectangle(renderer, cx - rx, cy - ry, cx + rx, cy + ry, color, alpha, outline);
        return;
    }

    int32_t segments = ctrClampSegments(precision, 4, CTR_MAX_CIRCLE_SEGMENTS);
    float angleStep = (2.0f * (float)M_PI) / (float)segments;

    if (outline) {
        float prevX = cx + rx;
        float prevY = cy;
        for (int32_t i = 1; i <= segments; i++) {
            float angle = angleStep * (float)i;
            float nextX = cx + cosf(angle) * rx;
            float nextY = cy + sinf(angle) * ry;
            ctrDrawLine(renderer, prevX, prevY, nextX, nextY, 1.0f, color, alpha);
            prevX = nextX;
            prevY = nextY;
        }
        return;
    }

    uint8_t r, g, b, a;
    ctrColorToBytes(color, alpha, &r, &g, &b, &a);

    float prevX = cx + rx;
    float prevY = cy;
    for (int32_t i = 1; i <= segments; i++) {
        float angle = angleStep * (float)i;
        float nextX = cx + cosf(angle) * rx;
        float nextY = cy + sinf(angle) * ry;
        ctrPushTriangleSolid(gl, cx, cy, prevX, prevY, nextX, nextY, r, g, b, a);
        prevX = nextX;
        prevY = nextY;
    }
}

static void ctrDrawCircle(Renderer* renderer, float x, float y, float radius,
                          uint32_t color, float alpha, bool outline, int32_t precision) {
    float r = fabsf(radius);
    ctrDrawEllipse(renderer, x, y, r, r, color, alpha, outline, precision);
}

static void ctrDrawRoundrect(Renderer* renderer, float x1, float y1, float x2, float y2,
                             float radx, float rady, uint32_t color, float alpha, bool outline, int32_t precision) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    float left = fminf(x1, x2);
    float right = fmaxf(x1, x2);
    float top = fminf(y1, y2);
    float bottom = fmaxf(y1, y2);
    float width = right - left;
    float height = bottom - top;

    if (width <= 0.0f || height <= 0.0f) {
        ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline);
        return;
    }

    radx = fabsf(radx);
    rady = fabsf(rady);
    if (radx <= 0.0f || rady <= 0.0f) {
        ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline);
        return;
    }

    if (radx > width * 0.5f) radx = width * 0.5f;
    if (rady > height * 0.5f) rady = height * 0.5f;

    int32_t arcSegments = ctrClampSegments(precision / 4, 1, CTR_MAX_ROUNDRECT_CORNER_SEGMENTS);
    float pointsX[CTR_MAX_ROUNDRECT_POINTS];
    float pointsY[CTR_MAX_ROUNDRECT_POINTS];
    int32_t pointCount = 0;
    float halfPi = 0.5f * (float)M_PI;

    ctrAppendArcPoints(pointsX, pointsY, &pointCount, right - radx, top + rady, radx, rady, -halfPi, 0.0f, arcSegments, false);
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, right - radx, bottom - rady, radx, rady, 0.0f, halfPi, arcSegments, true);
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, left + radx, bottom - rady, radx, rady, halfPi, (float)M_PI, arcSegments, true);
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, left + radx, top + rady, radx, rady, (float)M_PI, (float)M_PI + halfPi, arcSegments, true);

    if (pointCount < 2) {
        ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline);
        return;
    }

    if (outline) {
        for (int32_t i = 0; i < pointCount; i++) {
            int32_t next = (i + 1) % pointCount;
            ctrDrawLine(renderer, pointsX[i], pointsY[i], pointsX[next], pointsY[next], 1.0f, color, alpha);
        }
        return;
    }

    uint8_t r, g, b, a;
    ctrColorToBytes(color, alpha, &r, &g, &b, &a);
    float centerX = (left + right) * 0.5f;
    float centerY = (top + bottom) * 0.5f;

    for (int32_t i = 0; i < pointCount; i++) {
        int32_t next = (i + 1) % pointCount;
        ctrPushTriangleSolid(gl, centerX, centerY, pointsX[i], pointsY[i], pointsX[next], pointsY[next], r, g, b, a);
    }
}

static void ctrDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    if (tile == nullptr) return;

    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tile);
    if (tpagIndex < 0) return;

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

    int32_t srcX = tile->sourceX;
    int32_t srcY = tile->sourceY;
    int32_t srcW = (int32_t) tile->width;
    int32_t srcH = (int32_t) tile->height;
    float drawX = (float) tile->x + offsetX;
    float drawY = (float) tile->y + offsetY;

    int32_t contentLeft = tpag->targetX;
    int32_t contentTop = tpag->targetY;
    if (contentLeft > srcX) {
        int32_t clip = contentLeft - srcX;
        drawX += (float) clip * tile->scaleX;
        srcW -= clip;
        srcX = contentLeft;
    }
    if (contentTop > srcY) {
        int32_t clip = contentTop - srcY;
        drawY += (float) clip * tile->scaleY;
        srcH -= clip;
        srcY = contentTop;
    }

    int32_t contentRight = tpag->targetX + tpag->sourceWidth;
    int32_t contentBottom = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > contentRight) srcW = contentRight - srcX;
    if (srcY + srcH > contentBottom) srcH = contentBottom - srcY;
    if (srcW <= 0 || srcH <= 0) return;

    int32_t atlasOffX = srcX - tpag->targetX;
    int32_t atlasOffY = srcY - tpag->targetY;

    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : ((float) alphaByte / 255.0f);
    uint32_t bgr = tile->color & 0x00FFFFFFu;

    ctrDrawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH,
                      drawX, drawY, tile->scaleX, tile->scaleY, bgr, alpha);
}
static void ctrDeleteSprite(Renderer* renderer, int32_t spriteIndex) {}

static RendererVtable ctrVtable = {
    .init = ctrInit, .destroy = ctrDestroy, .beginFrame = ctrBeginFrame, .endFrame = ctrEndFrame,
    .beginView = ctrBeginView, .endView = ctrEndView,
    .drawSprite = ctrDrawSprite, .drawSpritePart = ctrDrawSpritePart, .drawRectangle = ctrDrawRectangle,
    .drawLine = ctrDrawLine, .drawLineColor = ctrDrawLineColor, .drawTriangle = ctrDrawTriangle,
    .drawText = ctrDrawText, .drawTextColor = ctrDrawTextColor, .flush = ctrRendererFlush,
    .createSpriteFromSurface = ctrCreateSpriteFromSurface, .deleteSprite = ctrDeleteSprite,
    .drawTile = ctrDrawTile,
    // 🔥 ФИКС: Добавляем пропущенные функции в таблицу!
    .drawCircle = ctrDrawCircle,
    .drawRoundrect = ctrDrawRoundrect,
    .drawTriangleColor = ctrDrawTriangleColor,
    .drawEllipse = ctrDrawEllipse
};

Renderer* CtrRenderer_create(void) {
    CtrRenderer* gl = safeCalloc(1, sizeof(CtrRenderer));
    gl->base.vtable = &ctrVtable;
    gl->base.drawColor = 0xFFFFFF;
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    gl->base.circlePrecision = 12; // ФИКС: У PSP тоже стоит 36, синхронизируем
    return (Renderer*) gl;
}
// --- END OF FILE ctr_renderer.c ---