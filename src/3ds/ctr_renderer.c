// --- START OF FILE ctr_renderer.c ---

#include "ctr_renderer.h"
#include "matrix_math.h"
#include "memlz.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

#define CTR_QUAD_BATCH_CAPACITY 1024
#define CTR_MAX_CIRCLE_SEGMENTS 128
#define CTR_MAX_ROUNDRECT_CORNER_SEGMENTS 64
#define CTR_MAX_ROUNDRECT_POINTS (CTR_MAX_ROUNDRECT_CORNER_SEGMENTS * 4 + 1)

// LRU кэш: очищает VRAM от неиспользуемых текстур через 10 секунд
#define LRU_SWEEP_INTERVAL 60
#define LRU_IDLE_THRESHOLD 600

static uint32_t g_frameCounter = 0;

typedef struct {
    uint8_t* compressedPixels;
    size_t compressedSize;
    size_t decompressedSize;
    int width;
    int height;
    bool isLoaded;
    bool keepResident;
    bool hasAlpha;
} RawAtlasData;

static int next_pot(int x) {
    x--; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x++;
    return x < 8 ? 8 : x;
}

static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
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
    if (gl->quadBatchCount > 0 && gl->quadBatchTexture != textureId) ctrFlushBatch(gl);
    if (gl->quadBatchCount >= gl->quadBatchCapacity) ctrFlushBatch(gl);

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

// =========================================================================
// ИДЕАЛЬНАЯ ЗАГРУЗКА АТЛАСОВ (LINEAR RAM)
// =========================================================================

// =========================================================================
// ИДЕАЛЬНАЯ ЗАГРУЗКА АТЛАСОВ (LINEAR RAM)
// =========================================================================

static void ctrBuildAtlasCache(CtrRenderer* gl) {
    DataWin* dw = gl->base.dataWin;
    mkdir("sdmc:/3ds/butterscotch", 0777);
    mkdir("sdmc:/3ds/butterscotch/cache", 0777);

    const char* flagPath = "sdmc:/3ds/butterscotch/cache/cache_ready.flag";
    FILE* flag = fopen(flagPath, "r");
    if (flag) {
        fclose(flag);
        fprintf(stderr, "--- FAST BOOT: Compressed Atlas Cache Ready! ---\n");
        for (uint32_t i = 0; i < dw->txtr.count; i++) {
            if (dw->txtr.textures[i].blobData) {
                free(dw->txtr.textures[i].blobData);
                dw->txtr.textures[i].blobData = NULL;
            }
        }
        return;
    }

    fprintf(stderr, "--- BUILDING MEMLZ COMPRESSED ATLAS CACHE ---\n");
    for (uint32_t i = 0; i < dw->txtr.count; i++) {
        Texture* txtr = &dw->txtr.textures[i];
        if (txtr->blobSize == 0) continue;

        fprintf(stderr, "Compressing Atlas %u/%u...\n", i+1, dw->txtr.count);

        uint8_t* pngData = txtr->blobData;
        bool freePng = false;

        if (!pngData && dw->filePath) {
            FILE* df = fopen(dw->filePath, "rb");
            if (df) {
                fseek(df, txtr->blobOffset, SEEK_SET);
                pngData = malloc(txtr->blobSize);
                if (pngData) fread(pngData, 1, txtr->blobSize, df);
                fclose(df);
                freePng = true;
            }
        }

        if (pngData) {
            int w, h, c;
            uint8_t* px = stbi_load_from_memory(pngData, (int)txtr->blobSize, &w, &h, &c, 4);
            if (px) {
                bool hasAlpha = false;
                for (int p = 0; p < w * h; p++) {
                    if (px[p * 4 + 3] < 255) { hasAlpha = true; break; }
                }

                size_t rawSize = w * h * sizeof(uint16_t);
                uint16_t* px16 = malloc(rawSize);
                if (px16) {
                    for (int p = 0; p < w * h; p++) {
                        if (hasAlpha) px16[p] = pack_rgba4444(px[p*4], px[p*4+1], px[p*4+2], px[p*4+3]);
                        else px16[p] = pack_rgb565(px[p*4], px[p*4+1], px[p*4+2]);
                    }

                    // СЖИМАЕМ!
                    size_t maxCompSize = memlz_max_compressed_len(rawSize);
                    uint8_t* compBuffer = linearAlloc(maxCompSize);
                    if (compBuffer) {
                        size_t compSize = memlz_compress(compBuffer, px16, rawSize);

                        char path[256];
                        snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/atlas_%u.lz", i);
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            int alphaFlag = hasAlpha ? 1 : 0;
                            // Пишем заголовок (Разрешение, Альфа, Размер сырой, Размер сжатый)
                            fwrite(&w, sizeof(int), 1, f);
                            fwrite(&h, sizeof(int), 1, f);
                            fwrite(&alphaFlag, sizeof(int), 1, f);
                            fwrite(&rawSize, sizeof(size_t), 1, f);
                            fwrite(&compSize, sizeof(size_t), 1, f);
                            // Пишем саму сжатую массу
                            fwrite(compBuffer, 1, compSize, f);
                            fclose(f);
                        }
                        linearFree(compBuffer);
                    }
                    free(px16);
                }
                stbi_image_free(px);
            }
            if (freePng) free(pngData);
            else if (txtr->blobData) { free(txtr->blobData); txtr->blobData = NULL; }
        }
    }

    flag = fopen(flagPath, "w");
    if (flag) { fputs("READY", flag); fclose(flag); }
    fprintf(stderr, "--- COMPRESSED CACHE COMPLETE! ---\n");
}

static void loadRawAtlas(CtrRenderer* gl, DataWin* dw, uint32_t pId) {
    RawAtlasData* rawAtlases = (RawAtlasData*)gl->rawAtlases;
    if (pId >= gl->rawAtlasCount || rawAtlases[pId].isLoaded) return;

    char path[256];
    snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/atlas_%u.lz", pId);
    FILE* f = fopen(path, "rb");
    if (!f) return;

    int w, h, alphaFlag;
    size_t rawSize, compSize;
    fread(&w, sizeof(int), 1, f);
    fread(&h, sizeof(int), 1, f);
    fread(&alphaFlag, sizeof(int), 1, f);
    fread(&rawSize, sizeof(size_t), 1, f);
    fread(&compSize, sizeof(size_t), 1, f);

    // Выделяем память ОБЫЧНОМ HEAP'Е только под сжатый буфер (сущие копейки!)
    uint8_t* compPixels = linearAlloc(compSize);
    if (compPixels) {
        fread(compPixels, 1, compSize, f);
        rawAtlases[pId].compressedPixels = compPixels;
        rawAtlases[pId].compressedSize = compSize;
        rawAtlases[pId].decompressedSize = rawSize;
        rawAtlases[pId].width = w;
        rawAtlases[pId].height = h;
        rawAtlases[pId].hasAlpha = (alphaFlag != 0);
        rawAtlases[pId].isLoaded = true;
    }
    fclose(f);
}

void CtrRenderer_drainPrefetchQueue(Renderer* renderer, int maxItems) {
}

bool CtrRenderer_hasPendingPrefetch(Renderer* renderer) {
    return false;
}

static void ctrExtractTpag(CtrRenderer* gl, DataWin* dw, uint32_t tId) {
    if (tId >= gl->tpagCount || gl->tpags[tId].isLoaded) return;

    TexturePageItem* item = &dw->tpag.items[tId];
    uint32_t pId = item->texturePageId;
    RawAtlasData* rawAtlases = (RawAtlasData*)gl->rawAtlases;

    bool wasLoadedJustNow = false;
    if (!rawAtlases[pId].isLoaded) {
        loadRawAtlas(gl, dw, pId);
        wasLoadedJustNow = true;
    }

    if (!rawAtlases[pId].isLoaded) return;

    RawAtlasData* atlas = &rawAtlases[pId];

    // ВРЕМЕННЫЙ БУФЕР ДЛЯ РАСПАКОВКИ! Выделяется и умирает за миллисекунду
    uint16_t* uncompressedPixels = linearAlloc(atlas->decompressedSize);
    if (!uncompressedPixels) return; // Не хватило памяти для распаковки

    // Распаковываем
    memlz_decompress(uncompressedPixels, atlas->compressedPixels);

    int extW = item->sourceWidth > 0 ? item->sourceWidth : 1;
    int extH = item->sourceHeight > 0 ? item->sourceHeight : 1;
    int extractX = item->sourceX;
    int extractY = item->sourceY;

    float downscale = 1.0f;
    if (extW > 1024 || extH > 1024) downscale = fminf(1024.0f / extW, 1024.0f / extH);

    int scaledW = (int)ceilf(extW * downscale);
    int scaledH = (int)ceilf(extH * downscale);
    if (scaledW <= 0) scaledW = 1; if (scaledH <= 0) scaledH = 1;

    int potW = next_pot(scaledW);
    int potH = next_pot(scaledH);

    uint16_t* buffer = linearAlloc(potW * potH * sizeof(uint16_t));
    if (buffer) {
        memset(buffer, 0, potW * potH * sizeof(uint16_t));

        float invScale = (downscale > 0.0f) ? (1.0f / downscale) : 1.0f;

        for (int y = 0; y < scaledH; y++) {
            int srcY = extractY + (int)floorf(((float)y + 0.5f) * invScale);
            if (srcY < 0) srcY = 0; else if (srcY >= atlas->height) srcY = atlas->height - 1;

            for (int x = 0; x < scaledW; x++) {
                int srcX = extractX + (int)floorf(((float)x + 0.5f) * invScale);
                if (srcX < 0) srcX = 0; else if (srcX >= atlas->width) srcX = atlas->width - 1;

                // Читаем пиксели из временного РАСПАКОВАННОГО буфера
                buffer[y * potW + x] = uncompressedPixels[srcY * atlas->width + srcX];
            }
        }

        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);

        GLenum format = atlas->hasAlpha ? GL_RGBA : GL_RGB;
        GLenum type = atlas->hasAlpha ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_SHORT_5_6_5;

        while (glGetError() != GL_NO_ERROR);
        glTexImage2D(GL_TEXTURE_2D, 0, format, potW, potH, 0, format, type, buffer);

        if (glGetError() == GL_NO_ERROR) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            gl->tpags[tId].tex = tex;
            gl->tpags[tId].uvScaleX = 1.0f / (float)potW;
            gl->tpags[tId].uvScaleY = 1.0f / (float)potH;
            gl->tpags[tId].downscaleFactor = downscale;
        } else {
            glDeleteTextures(1, &tex);
            gl->tpags[tId].tex = 0;
        }

        gl->tpags[tId].isLoaded = true;
        gl->tpags[tId].lastUsedFrame = g_frameCounter;
        linearFree(buffer);
    }

    // 💥 УНИЧТОЖАЕМ временный распакованный буфер!
    linearFree(uncompressedPixels);

    // Удаляем даже СЖАТЫЙ атлас, если он был загружен чисто ради одного динамического чиха
    if (wasLoadedJustNow && !atlas->keepResident) {
        free(atlas->compressedPixels);
        atlas->compressedPixels = NULL;
        atlas->isLoaded = false;
    }
}

static void ensureTpagLoaded(CtrRenderer* gl, DataWin* dw, int32_t tpagIndex) {
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount || gl->tpags[tpagIndex].isLoaded) return;
    ctrExtractTpag(gl, dw, tpagIndex);
}

// =========================================================================

static void unloadTpagTexture(CtrRenderer* gl, uint32_t tpagIndex) {
    if (tpagIndex >= gl->tpagCount) return;
    if (gl->tpags[tpagIndex].isLoaded && gl->tpags[tpagIndex].tex != 0) {
        glDeleteTextures(1, &gl->tpags[tpagIndex].tex);
    }
    gl->tpags[tpagIndex].tex = 0;
    gl->tpags[tpagIndex].isLoaded = false;
}

static inline uint8_t clamp_alpha(float alpha) {
    if (alpha <= 0.0f) return 0;
    if (alpha >= 1.0f) return 255;
    return (uint8_t)(alpha * 255.0f + 0.5f);
}

static int32_t ctrClampSegments(int32_t precision, int32_t minSegments, int32_t maxSegments) {
    if (precision < minSegments) return minSegments;
    if (precision > maxSegments) return maxSegments;
    return precision;
}

static void ctrColorToBytes(uint32_t color, float alpha, uint8_t* outR, uint8_t* outG, uint8_t* outB, uint8_t* outA) {
    *outR = BGR_R(color); *outG = BGR_G(color); *outB = BGR_B(color);
    *outA = clamp_alpha(alpha);
}

static void ctrPushTriangleGradient(CtrRenderer* gl, float x1, float y1, float x2, float y2, float x3, float y3, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t a2, uint8_t r3, uint8_t g3, uint8_t b3, uint8_t a3) {
    ctrPushQuadGradient(gl, gl->whiteTexture, x1, y1, x2, y2, x3, y3, x3, y3, 0.5f, 0.5f, 0.5f, 0.5f, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3, r3, g3, b3, a3);
}

static void ctrPushTriangleSolid(CtrRenderer* gl, float x1, float y1, float x2, float y2, float x3, float y3, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ctrPushTriangleGradient(gl, x1, y1, x2, y2, x3, y3, r, g, b, a, r, g, b, a, r, g, b, a);
}

static void ctrAppendArcPoints(float* outX, float* outY, int32_t* count, float cx, float cy, float rx, float ry, float startAngle, float endAngle, int32_t segments, bool skipFirst) {
    for (int32_t i = 0; i <= segments; i++) {
        if (skipFirst && i == 0) continue;
        if (*count >= CTR_MAX_ROUNDRECT_POINTS) break;
        float t = (segments > 0) ? ((float)i / (float)segments) : 0.0f;
        float angle = startAngle + (endAngle - startAngle) * t;
        outX[*count] = cx + cosf(angle) * rx; outY[*count] = cy + sinf(angle) * ry;
        (*count)++;
    }
}

static void ctrDrawTpagRegion(CtrRenderer* gl, uint32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (tpagIndex >= gl->tpagCount || !gl->tpags[tpagIndex].isLoaded) return;
    CtrTpagData* tpagData = &gl->tpags[tpagIndex];
    tpagData->lastUsedFrame = g_frameCounter;

    float drawSrcX = srcOffX * tpagData->downscaleFactor;
    float drawSrcY = srcOffY * tpagData->downscaleFactor;
    float drawSrcW = srcW * tpagData->downscaleFactor;
    float drawSrcH = srcH * tpagData->downscaleFactor;
    if (drawSrcW <= 0.001f || drawSrcH <= 0.001f) return;

    float halfU = 0.5f * tpagData->uvScaleX;
    float halfV = 0.5f * tpagData->uvScaleY;
    float u0 = drawSrcX * tpagData->uvScaleX + halfU;
    float v0 = drawSrcY * tpagData->uvScaleY + halfV;
    float u1 = (drawSrcX + drawSrcW) * tpagData->uvScaleX - halfU;
    float v1 = (drawSrcY + drawSrcH) * tpagData->uvScaleY - halfV;

    ctrPushQuad(gl, tpagData->tex, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

static void ctrInit(Renderer* renderer, DataWin* dataWin) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    renderer->dataWin = dataWin;

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    gl->tpagCount = dataWin->tpag.count;
    gl->tpags = safeCalloc(gl->tpagCount, sizeof(CtrTpagData));

    gl->rawAtlasCount = dataWin->txtr.count;
    gl->rawAtlases = safeCalloc(gl->rawAtlasCount, sizeof(RawAtlasData));

    gl->quadBatchCapacity = CTR_QUAD_BATCH_CAPACITY;
    gl->quadBatchVertices = safeMalloc((size_t) gl->quadBatchCapacity * 6 * sizeof(CtrPackedVertex));

    CtrPackedVertex* verts = (CtrPackedVertex*) gl->quadBatchVertices;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(CtrPackedVertex), &verts[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(CtrPackedVertex), &verts[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CtrPackedVertex), &verts[0].r);

    glGenTextures(1, &gl->whiteTexture);
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, 0);

    ctrBuildAtlasCache(gl);
    fprintf(stderr, "CTR: Renderer initialized (Linear RAM Atlas Mode)\n");
}

static void ctrDestroy(Renderer* renderer) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    glDeleteTextures(1, &gl->whiteTexture);
    for (uint32_t i = 0; i < gl->tpagCount; i++) unloadTpagTexture(gl, i);

    RawAtlasData* rawAtlases = (RawAtlasData*)gl->rawAtlases;
    for (uint32_t i = 0; i < gl->rawAtlasCount; i++) {
        if (rawAtlases[i].compressedPixels) linearFree(rawAtlases[i].compressedPixels);
    }

    free(gl->rawAtlases);
    free(gl->tpags);
    free(gl->quadBatchVertices);
    free(gl);
}

static void ctrBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);

    gl->windowW = windowW; gl->windowH = windowH;
    gl->gameW = gameW; gl->gameH = gameH;

    // Чистим старые неиспользуемые TPAG (VRAM)
    if (g_frameCounter > 0 && g_frameCounter % LRU_SWEEP_INTERVAL == 0) {
        for (uint32_t i = 0; i < gl->tpagCount; i++) {
            if (gl->tpags[i].keepResident || !gl->tpags[i].isLoaded) continue;
            if (g_frameCounter - gl->tpags[i].lastUsedFrame > LRU_IDLE_THRESHOLD) unloadTpagTexture(gl, i);
        }
    }
}

static void ctrBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, gl->windowW, gl->windowH);
    glDisable(GL_SCISSOR_TEST);

    Matrix4f projection; Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, (float)viewX, (float)(viewX + viewW), (float)(viewY + viewH), (float)viewY, -1.0f, 1.0f);

    if (viewAngle != 0.0f) {
        float cx = (float) viewX + (float) viewW / 2.0f;
        float cy = (float) viewY + (float) viewH / 2.0f;
        Matrix4f rot; Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.0f);
        Matrix4f_rotateZ(&rot, -viewAngle * (float) M_PI / 180.0f);
        Matrix4f_translate(&rot, -cx, -cy, 0.0f);
        Matrix4f result; Matrix4f_multiply(&result, &projection, &rot);
        projection = result;
    }
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static void ctrEndView(Renderer* renderer) {}
static void ctrEndFrame(Renderer* renderer) { ctrFlushBatch((CtrRenderer*) renderer); g_frameCounter++; }
static void ctrRendererFlush(Renderer* renderer) { ctrFlushBatch((CtrRenderer*) renderer); }

static void markAtlasResident(CtrRenderer* gl, DataWin* dw, uint32_t tpagOffset) {
    int32_t tpagIdx = DataWin_resolveTPAG(dw, tpagOffset);
    if (tpagIdx >= 0 && tpagIdx < (int32_t)gl->tpagCount) {
        uint32_t pId = dw->tpag.items[tpagIdx].texturePageId;
        if (pId < gl->rawAtlasCount) {
            ((RawAtlasData*)gl->rawAtlases)[pId].keepResident = true;
            gl->tpags[tpagIdx].keepResident = true;
        }
    }
}

static void ctrOnRoomChanged(Renderer* renderer, int32_t roomIndex) {
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;
    RawAtlasData* rawAtlases = (RawAtlasData*)gl->rawAtlases;

    if (roomIndex < 0 || (uint32_t) roomIndex >= dw->room.count) return;
    Room* room = &dw->room.rooms[roomIndex];

    for (uint32_t i = 0; i < gl->rawAtlasCount; i++) rawAtlases[i].keepResident = false;
    for (uint32_t i = 0; i < gl->tpagCount; i++) gl->tpags[i].keepResident = false;

    if (room->backgrounds) {
        for (int i = 0; i < 8; i++) {
            if (room->backgrounds[i].enabled) {
                int32_t bgId = room->backgrounds[i].backgroundDefinition;
                if (bgId >= 0 && bgId < dw->bgnd.count) markAtlasResident(gl, dw, dw->bgnd.backgrounds[bgId].textureOffset);
            }
        }
    }

    for (uint32_t i = 0; i < room->tileCount; i++) {
        int32_t bgId = room->tiles[i].backgroundDefinition;
        if (room->tiles[i].useSpriteDefinition) {
            if (bgId >= 0 && bgId < dw->sprt.count) markAtlasResident(gl, dw, dw->sprt.sprites[bgId].textureOffsets[0]);
        } else {
            if (bgId >= 0 && bgId < dw->bgnd.count) markAtlasResident(gl, dw, dw->bgnd.backgrounds[bgId].textureOffset);
        }
    }

    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int32_t objIdx = room->gameObjects[i].objectDefinition;
        if (objIdx >= 0 && (uint32_t) objIdx < dw->objt.count) {
            int32_t sprId = dw->objt.objects[objIdx].spriteId;
            if (sprId >= 0 && sprId < dw->sprt.count) markAtlasResident(gl, dw, dw->sprt.sprites[sprId].textureOffsets[0]);
        }
    }
    for (uint32_t i = 0; i < dw->font.count; i++) markAtlasResident(gl, dw, dw->font.fonts[i].textureOffset);

    // Удаляем из Linear RAM атласы, которые не нужны в новой комнате (защита от OOM)
    for (uint32_t i = 0; i < gl->rawAtlasCount; i++) {
        if (!rawAtlases[i].keepResident && rawAtlases[i].isLoaded) {
            linearFree(rawAtlases[i].compressedPixels);
            rawAtlases[i].compressedPixels = NULL;
            rawAtlases[i].isLoaded = false;
        }
    }

    // Загружаем нужные атласы из .bin, чтобы избежать статтеров во время игры
    for (uint32_t i = 0; i < gl->rawAtlasCount; i++) {
        if (rawAtlases[i].keepResident && !rawAtlases[i].isLoaded) {
            loadRawAtlas(gl, dw, i);
        }
    }
}

void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex) {
    if (!renderer || !renderer->dataWin) return;
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;

    if (spriteIndex >= 0 && (uint32_t) spriteIndex < dw->sprt.count) {
        Sprite* s = &dw->sprt.sprites[spriteIndex];
        for (uint32_t f = 0; f < s->textureCount; f++) markAtlasResident(gl, dw, s->textureOffsets[f]);
    }

    RawAtlasData* rawAtlases = (RawAtlasData*)gl->rawAtlases;
    for (uint32_t i = 0; i < gl->rawAtlasCount; i++) {
        if (rawAtlases[i].keepResident && !rawAtlases[i].isLoaded) loadRawAtlas(gl, dw, i);
    }
}

static void ctrDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount) return;

    ensureTpagLoaded(gl, dw, tpagIndex);
    if (!gl->tpags[tpagIndex].isLoaded) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float lx0 = ((float) tpag->targetX - originX) * xscale;
    float ly0 = ((float) tpag->targetY - originY) * yscale;
    float lx1 = lx0 + ((float) tpag->sourceWidth) * xscale;
    float ly1 = ly0 + ((float) tpag->sourceHeight) * yscale;

    uint8_t r = BGR_R(color); uint8_t g = BGR_G(color); uint8_t b = BGR_B(color);
    uint8_t a = clamp_alpha(alpha);
    if (a == 0) return;

    if (angleDeg == 0.0f) {
        float x0 = roundf(x + lx0); float y0 = roundf(y + ly0);
        float x1 = roundf(x + lx1); float y1 = roundf(y + ly1);
        if ((lx1 - lx0) > 0.0f && x1 <= x0) x1 = x0 + 1.0f;
        if ((ly1 - ly0) > 0.0f && y1 <= y0) y1 = y0 + 1.0f;

        if (tpag->sourceWidth == 1 && tpag->sourceHeight == 1) {
            if (fabsf(xscale) > 0.0f && fabsf(xscale) <= 1.5f) x1 = x0 + 2.0f;
            if (fabsf(yscale) > 0.0f && fabsf(yscale) <= 1.5f) y1 = y0 + 2.0f;
        }

        ctrDrawTpagRegion(gl, tpagIndex, 0.0f, 0.0f, (float)tpag->sourceWidth, (float)tpag->sourceHeight, x0, y0, x1, y0, x1, y1, x0, y1, r, g, b, a);
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float c = cosf(angleRad); float s = sinf(angleRad);
        float x0 = lx0 * c - ly0 * s + x; float y0 = lx0 * s + ly0 * c + y;
        float x1 = lx1 * c - ly0 * s + x; float y1 = lx1 * s + ly0 * c + y;
        float x2 = lx1 * c - ly1 * s + x; float y2 = lx1 * s + ly1 * c + y;
        float x3 = lx0 * c - ly1 * s + x; float y3 = lx0 * s + ly1 * c + y;
        ctrDrawTpagRegion(gl, tpagIndex, 0.0f, 0.0f, (float)tpag->sourceWidth, (float)tpag->sourceHeight, x0, y0, x1, y1, x2, y2, x3, y3, r, g, b, a);
    }
}

static void ctrDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount) return;

    ensureTpagLoaded(gl, dw, tpagIndex);
    if (!gl->tpags[tpagIndex].isLoaded) return;

    uint8_t r = BGR_R(color); uint8_t g = BGR_G(color); uint8_t b = BGR_B(color);
    uint8_t a = clamp_alpha(alpha);
    if (a == 0) return;

    float cx = roundf(x); float cy = roundf(y);
    float x1 = cx + roundf((float)srcW * xscale); float y1 = cy + roundf((float)srcH * yscale);

    ctrDrawTpagRegion(gl, tpagIndex, (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH, cx, cy, x1, cy, x1, y1, cx, y1, r, g, b, a);
}

static void emitColoredQuad(CtrRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    uint8_t cr = (uint8_t)(r * 255.0f); uint8_t cg = (uint8_t)(g * 255.0f); uint8_t cb = (uint8_t)(b * 255.0f);
    uint8_t ca = clamp_alpha(a);
    if (ca == 0) return;
    ctrPushQuad(gl, gl->whiteTexture, x0, y0, x1, y0, x1, y1, x0, y1, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    float r = (float) BGR_R(color) / 255.0f; float g = (float) BGR_G(color) / 255.0f; float b = (float) BGR_B(color) / 255.0f;
    float left = roundf(fminf(x1, x2)); float right = roundf(fmaxf(x1, x2));
    float top = roundf(fminf(y1, y2)); float bottom = roundf(fmaxf(y1, y2));

    if (outline) {
        float thick = 2.0f;
        emitColoredQuad(gl, left, top, right + 1.0f, top + thick, r, g, b, alpha);
        emitColoredQuad(gl, left, bottom - thick + 1.0f, right + 1.0f, bottom + 1.0f, r, g, b, alpha);
        emitColoredQuad(gl, left, top + thick, left + thick, bottom - thick + 1.0f, r, g, b, alpha);
        emitColoredQuad(gl, right - thick + 1.0f, top + thick, right + 1.0f, bottom - thick + 1.0f, r, g, b, alpha);
    } else {
        emitColoredQuad(gl, left, top, right + 1.0f, bottom + 1.0f, r, g, b, alpha);
    }
}

static void ctrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t cr = BGR_R(color); uint8_t cg = BGR_G(color); uint8_t cb = BGR_B(color); uint8_t ca = clamp_alpha(alpha);
    float actualWidth = (width < 2.0f) ? 2.0f : width;

    if (fabsf(x1 - x2) < 0.01f || fabsf(y1 - y2) < 0.01f) {
        float rx1 = roundf(fminf(x1, x2)); float ry1 = roundf(fminf(y1, y2));
        float rx2 = roundf(fmaxf(x1, x2)); float ry2 = roundf(fmaxf(y1, y2));
        if (fabsf(x1 - x2) < 0.01f) { rx1 -= actualWidth * 0.5f; rx2 += actualWidth * 0.5f; ry2 += 1.0f; }
        else { rx2 += 1.0f; ry1 -= actualWidth * 0.5f; ry2 += actualWidth * 0.5f; }
        ctrPushQuad(gl, gl->whiteTexture, rx1, ry1, rx2, ry1, rx2, ry2, rx1, ry2, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
        return;
    }

    float dx = x2 - x1; float dy = y2 - y1; float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;
    x2 += (dx / len) * 0.5f; y2 += (dy / len) * 0.5f; x1 -= (dx / len) * 0.5f; y1 -= (dy / len) * 0.5f;
    dx = x2 - x1; dy = y2 - y1; len = sqrtf(dx * dx + dy * dy);
    float halfW = actualWidth * 0.5f; float px = (-dy / len) * halfW; float py = (dx / len) * halfW;
    ctrPushQuad(gl, gl->whiteTexture, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t c1r = BGR_R(color1); uint8_t c1g = BGR_G(color1); uint8_t c1b = BGR_B(color1);
    uint8_t c2r = BGR_R(color2); uint8_t c2g = BGR_G(color2); uint8_t c2b = BGR_B(color2);
    uint8_t ca = clamp_alpha(alpha);
    float actualWidth = (width < 2.0f) ? 2.0f : width;

    if (fabsf(x1 - x2) < 0.01f || fabsf(y1 - y2) < 0.01f) {
        float rx1 = roundf(fminf(x1, x2)); float ry1 = roundf(fminf(y1, y2));
        float rx2 = roundf(fmaxf(x1, x2)); float ry2 = roundf(fmaxf(y1, y2));
        uint8_t r0, g0, b0, r1, g1, b1;
        if (x1 <= x2 && y1 <= y2) { r0=c1r; g0=c1g; b0=c1b; r1=c2r; g1=c2g; b1=c2b; }
        else { r0=c2r; g0=c2g; b0=c2b; r1=c1r; g1=c1g; b1=c1b; }
        if (fabsf(x1 - x2) < 0.01f) {
            rx1 -= actualWidth * 0.5f; rx2 += actualWidth * 0.5f; ry2 += 1.0f;
            ctrPushQuadGradient(gl, gl->whiteTexture, rx1, ry1, rx2, ry1, rx2, ry2, rx1, ry2, 0.5f, 0.5f, 0.5f, 0.5f, r0, g0, b0, ca, r0, g0, b0, ca, r1, g1, b1, ca, r1, g1, b1, ca);
        } else {
            rx2 += 1.0f; ry1 -= actualWidth * 0.5f; ry2 += actualWidth * 0.5f;
            ctrPushQuadGradient(gl, gl->whiteTexture, rx1, ry1, rx2, ry1, rx2, ry2, rx1, ry2, 0.5f, 0.5f, 0.5f, 0.5f, r0, g0, b0, ca, r1, g1, b1, ca, r1, g1, b1, ca, r0, g0, b0, ca);
        }
        return;
    }
    float dx = x2 - x1; float dy = y2 - y1; float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;
    x2 += (dx / len) * 0.5f; y2 += (dy / len) * 0.5f; x1 -= (dx / len) * 0.5f; y1 -= (dy / len) * 0.5f;
    dx = x2 - x1; dy = y2 - y1; len = sqrtf(dx * dx + dy * dy);
    float halfW = actualWidth * 0.5f; float px = (-dy / len) * halfW; float py = (dx / len) * halfW;
    ctrPushQuadGradient(gl, gl->whiteTexture, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, 0.5f, 0.5f, 0.5f, 0.5f, c1r, c1g, c1b, ca, c1r, c1g, c1b, ca, c2r, c2g, c2b, ca, c2r, c2g, c2b, ca);
}

static void ctrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    if (outline) {
        ctrDrawLine(renderer, x1, y1, x2, y2, 1, renderer->drawColor, 1.0f);
        ctrDrawLine(renderer, x2, y2, x3, y3, 1, renderer->drawColor, 1.0f);
        ctrDrawLine(renderer, x3, y3, x1, y1, 1, renderer->drawColor, 1.0f);
    } else {
        uint8_t r = BGR_R(renderer->drawColor); uint8_t g = BGR_G(renderer->drawColor); uint8_t b = BGR_B(renderer->drawColor); uint8_t a = clamp_alpha(renderer->drawAlpha);
        ctrPushQuad(gl, gl->whiteTexture, x1, y1, x2, y2, x3, y3, x3, y3, 0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
    }
}

typedef struct { Font* font; uint32_t tpagIndex; } CtrFontState;

static bool ctrResolveFontState(CtrRenderer* gl, DataWin* dw, Font* font, CtrFontState* state) {
    state->font = font; state->tpagIndex = 0;
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (fontTpagIndex < 0 || fontTpagIndex >= (int32_t)gl->tpagCount) return false;
    ensureTpagLoaded(gl, dw, fontTpagIndex);
    if (!gl->tpags[fontTpagIndex].isLoaded) return false;
    state->tpagIndex = fontTpagIndex;
    return true;
}

static bool ctrResolveGlyph(CtrRenderer* gl, DataWin* dw, CtrFontState* state, FontGlyph* glyph, float cursorX, float cursorY, uint32_t* outTpagIndex, float* outSrcX, float* outSrcY, float* outSrcW, float* outSrcH, float* outLocalX0, float* outLocalY0) {
    *outTpagIndex = state->tpagIndex; *outSrcX = (float) glyph->sourceX; *outSrcY = (float) glyph->sourceY;
    *outSrcW = (float) glyph->sourceWidth; *outSrcH = (float) glyph->sourceHeight;
    *outLocalX0 = cursorX + glyph->offset; *outLocalY0 = cursorY;
    return true;
}

static void ctrDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    CtrFontState fontState;
    if (!ctrResolveFontState(gl, dw, font, &fontState)) return;

    uint8_t r = BGR_R(renderer->drawColor); uint8_t g = BGR_G(renderer->drawColor); uint8_t b = BGR_B(renderer->drawColor); uint8_t a = clamp_alpha(renderer->drawAlpha);
    int32_t textLen = (int32_t) strlen(text); if (textLen == 0) return;
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float lineStride = (float) font->emSize;
    if (lineStride <= 0.0f) {
        for (uint32_t i = 0; i < font->glyphCount; i++) if (font->glyphs[i].sourceHeight > lineStride) lineStride = (float) font->glyphs[i].sourceHeight;
        if (lineStride <= 0.0f) lineStride = 10.0f;
    }

    float totalHeight = (float) lineCount * lineStride; float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform; Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float cursorY = valignOffset; int32_t lineStart = 0;
    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset; int32_t pos = 0;
        while (lineLen > pos) {
            int32_t oldPos = pos;
            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            if (pos == oldPos) { pos++; continue; }

            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) { cursorX += glyph->shift; continue; }

            float srcX, srcY, srcW, srcH, localX0, localY0; uint32_t tpagIndex;
            if (!ctrResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &tpagIndex, &srcX, &srcY, &srcW, &srcH, &localX0, &localY0)) { cursorX += glyph->shift; continue; }

            float localX1 = localX0 + (float) glyph->sourceWidth; float localY1 = localY0 + (float) glyph->sourceHeight;
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0); Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2); Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

            ctrDrawTpagRegion(gl, tpagIndex, srcX, srcY, srcW, srcH, px0, py0, px1, py1, px2, py2, px3, py3, r, g, b, a);
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos; uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos; cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }
        cursorY += lineStride; lineStart = (textLen > lineEnd) ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }
}

static void ctrDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float floatAlpha) {
    float savedAlpha = renderer->drawAlpha; renderer->drawAlpha = floatAlpha;
    ctrDrawText(renderer, text, x, y, xscale, yscale, angleDeg); renderer->drawAlpha = savedAlpha;
}

static int32_t ctrCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) { return -1; }

static void ctrDrawTriangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t col1, uint32_t col2, uint32_t col3, float alpha, bool outline) {
    if (outline) {
        ctrDrawLine(renderer, x1, y1, x2, y2, 1.0f, col1, alpha);
        ctrDrawLine(renderer, x2, y2, x3, y3, 1.0f, col1, alpha);
        ctrDrawLine(renderer, x3, y3, x1, y1, 1.0f, col1, alpha);
        return;
    }
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3;
    ctrColorToBytes(col1, alpha, &r1, &g1, &b1, &a1); ctrColorToBytes(col2, alpha, &r2, &g2, &b2, &a2); ctrColorToBytes(col3, alpha, &r3, &g3, &b3, &a3);
    ctrPushTriangleGradient(gl, x1, y1, x2, y2, x3, y3, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3);
}

static void ctrDrawEllipse(Renderer* renderer, float cx, float cy, float rx, float ry, uint32_t color, float alpha, bool outline, int32_t precision) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    rx = fabsf(rx); ry = fabsf(ry);
    if (rx <= 0.0f || ry <= 0.0f) { ctrDrawRectangle(renderer, cx - rx, cy - ry, cx + rx, cy + ry, color, alpha, outline); return; }
    int32_t segments = ctrClampSegments(precision, 4, CTR_MAX_CIRCLE_SEGMENTS); float angleStep = (2.0f * (float)M_PI) / (float)segments;
    if (outline) {
        float prevX = cx + rx; float prevY = cy;
        for (int32_t i = 1; i <= segments; i++) {
            float angle = angleStep * (float)i; float nextX = cx + cosf(angle) * rx; float nextY = cy + sinf(angle) * ry;
            ctrDrawLine(renderer, prevX, prevY, nextX, nextY, 1.0f, color, alpha); prevX = nextX; prevY = nextY;
        }
        return;
    }
    uint8_t r, g, b, a; ctrColorToBytes(color, alpha, &r, &g, &b, &a);
    float prevX = cx + rx; float prevY = cy;
    for (int32_t i = 1; i <= segments; i++) {
        float angle = angleStep * (float)i; float nextX = cx + cosf(angle) * rx; float nextY = cy + sinf(angle) * ry;
        ctrPushTriangleSolid(gl, cx, cy, prevX, prevY, nextX, nextY, r, g, b, a); prevX = nextX; prevY = nextY;
    }
}

static void ctrDrawCircle(Renderer* renderer, float x, float y, float radius, uint32_t color, float alpha, bool outline, int32_t precision) { ctrDrawEllipse(renderer, x, y, radius, radius, color, alpha, outline, precision); }

static void ctrDrawRoundrect(Renderer* renderer, float x1, float y1, float x2, float y2, float radx, float rady, uint32_t color, float alpha, bool outline, int32_t precision) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    float left = fminf(x1, x2); float right = fmaxf(x1, x2); float top = fminf(y1, y2); float bottom = fmaxf(y1, y2);
    float width = right - left; float height = bottom - top;
    if (width <= 0.0f || height <= 0.0f) { ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline); return; }
    radx = fabsf(radx); rady = fabsf(rady);
    if (radx <= 0.0f || rady <= 0.0f) { ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline); return; }
    if (radx > width * 0.5f) radx = width * 0.5f; if (rady > height * 0.5f) rady = height * 0.5f;
    int32_t arcSegments = ctrClampSegments(precision / 4, 1, CTR_MAX_ROUNDRECT_CORNER_SEGMENTS);
    float pointsX[CTR_MAX_ROUNDRECT_POINTS]; float pointsY[CTR_MAX_ROUNDRECT_POINTS]; int32_t pointCount = 0; float halfPi = 0.5f * (float)M_PI;
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, right - radx, top + rady, radx, rady, -halfPi, 0.0f, arcSegments, false);
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, right - radx, bottom - rady, radx, rady, 0.0f, halfPi, arcSegments, true);
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, left + radx, bottom - rady, radx, rady, halfPi, (float)M_PI, arcSegments, true);
    ctrAppendArcPoints(pointsX, pointsY, &pointCount, left + radx, top + rady, radx, rady, (float)M_PI, (float)M_PI + halfPi, arcSegments, true);
    if (pointCount < 2) { ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline); return; }
    if (outline) {
        for (int32_t i = 0; i < pointCount; i++) {
            int32_t next = (i + 1) % pointCount; ctrDrawLine(renderer, pointsX[i], pointsY[i], pointsX[next], pointsY[next], 1.0f, color, alpha);
        }
        return;
    }
    uint8_t r, g, b, a; ctrColorToBytes(color, alpha, &r, &g, &b, &a);
    float centerX = (left + right) * 0.5f; float centerY = (top + bottom) * 0.5f;
    for (int32_t i = 0; i < pointCount; i++) {
        int32_t next = (i + 1) % pointCount; ctrPushTriangleSolid(gl, centerX, centerY, pointsX[i], pointsY[i], pointsX[next], pointsY[next], r, g, b, a);
    }
}

static void ctrDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    if (tile == nullptr) return;
    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tile); if (tpagIndex < 0) return;
    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];
    int32_t srcX = tile->sourceX; int32_t srcY = tile->sourceY; int32_t srcW = (int32_t) tile->width; int32_t srcH = (int32_t) tile->height;
    float drawX = (float) tile->x + offsetX; float drawY = (float) tile->y + offsetY;
    int32_t contentLeft = tpag->targetX; int32_t contentTop = tpag->targetY;
    if (contentLeft > srcX) { int32_t clip = contentLeft - srcX; drawX += (float) clip * tile->scaleX; srcW -= clip; srcX = contentLeft; }
    if (contentTop > srcY) { int32_t clip = contentTop - srcY; drawY += (float) clip * tile->scaleY; srcH -= clip; srcY = contentTop; }
    int32_t contentRight = tpag->targetX + tpag->sourceWidth; int32_t contentBottom = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > contentRight) srcW = contentRight - srcX; if (srcY + srcH > contentBottom) srcH = contentBottom - srcY;
    if (srcW <= 0 || srcH <= 0) return;
    int32_t atlasOffX = srcX - tpag->targetX; int32_t atlasOffY = srcY - tpag->targetY;
    uint8_t alphaByte = (tile->color >> 24) & 0xFF; float alpha = (alphaByte == 0) ? 1.0f : ((float) alphaByte / 255.0f);
    uint32_t bgr = tile->color & 0x00FFFFFFu;
    ctrDrawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH, drawX, drawY, tile->scaleX, tile->scaleY, bgr, alpha);
}
static void ctrDeleteSprite(Renderer* renderer, int32_t spriteIndex) {}

static RendererVtable ctrVtable = {
    .init = ctrInit, .destroy = ctrDestroy, .beginFrame = ctrBeginFrame, .endFrame = ctrEndFrame,
    .beginView = ctrBeginView, .endView = ctrEndView,
    .drawSprite = ctrDrawSprite, .drawSpritePart = ctrDrawSpritePart, .drawRectangle = ctrDrawRectangle,
    .drawLine = ctrDrawLine, .drawLineColor = ctrDrawLineColor, .drawTriangle = ctrDrawTriangle,
    .drawText = ctrDrawText, .drawTextColor = ctrDrawTextColor, .flush = ctrRendererFlush,
    .createSpriteFromSurface = ctrCreateSpriteFromSurface, .deleteSprite = ctrDeleteSprite,
    .drawTile = ctrDrawTile, .drawCircle = ctrDrawCircle, .drawRoundrect = ctrDrawRoundrect,
    .drawTriangleColor = ctrDrawTriangleColor, .drawEllipse = ctrDrawEllipse, .onRoomChanged = ctrOnRoomChanged
};

Renderer* CtrRenderer_create(void) {
    CtrRenderer* gl = safeCalloc(1, sizeof(CtrRenderer));
    gl->base.vtable = &ctrVtable;
    gl->base.drawColor = 0xFFFFFF; gl->base.drawAlpha = 1.0f; gl->base.drawFont = -1;
    gl->base.circlePrecision = 12;
    return (Renderer*) gl;
}
// --- END OF FILE ctr_renderer.c ---