#include "ctr_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <3ds.h>
#include <malloc.h>
#include "stb_image.h"
#include "utils.h"

#define CTR_QUAD_BATCH_CAPACITY 2048
#define CTR_MAX_CIRCLE_SEGMENTS 128
#define CTR_MAX_ROUNDRECT_CORNER_SEGMENTS 64
#define CTR_MAX_ROUNDRECT_POINTS (CTR_MAX_ROUNDRECT_CORNER_SEGMENTS * 4 + 1)
#define ATLAS_MAGIC 0x534C5441

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
} AtlasHeader;

static uint32_t g_frameCounter = 0;
typedef struct { float x, y, z; float u, v; uint8_t r, g, b, a; } CtrPackedVertex;

static inline uint16_t pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static int next_pot(int x) {
    x--; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x++;
    return x < 8 ? 8 : x;
}

static uint8_t ctrAlphaToByte(float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return (uint8_t)(alpha * 255.0f);
}

static void ctrColorToBytes(uint32_t color, float alpha, uint8_t* outR, uint8_t* outG, uint8_t* outB, uint8_t* outA) {
    *outR = BGR_R(color); *outG = BGR_G(color); *outB = BGR_B(color); *outA = ctrAlphaToByte(alpha);
}

static int32_t ctrClampSegments(int32_t precision, int32_t minSegments, int32_t maxSegments) {
    if (precision < minSegments) return minSegments;
    if (precision > maxSegments) return maxSegments;
    return precision;
}

static uint8_t* ctrReadPngBlobFromFile(FILE* fp, uint32_t blobOffset, uint32_t blobSize) {
    if (!fp || blobSize == 0 || blobOffset == 0) return NULL;
    if (fseek(fp, (long) blobOffset, SEEK_SET) != 0) return NULL;
    uint8_t* buf = (uint8_t*) malloc(blobSize);
    if (!buf) return NULL;
    if (fread(buf, 1, blobSize, fp) != blobSize) { free(buf); return NULL; }
    return buf;
}

static void ctrBuildFullTextureCache(CtrRenderer* gl) {
    DataWin* dw = gl->base.dataWin;
    const char* flagPath = "sdmc:/3ds/butterscotch/cache/cache_ready.flag";

    FILE* flagFile = fopen(flagPath, "r");
    if (flagFile) { fclose(flagFile); return; }

    fprintf(stderr, "--- STARTING ATLAS GENERATION (First Boot) ---\n");
    FILE* dataWinFile = (dw->filePath != NULL) ? fopen(dw->filePath, "rb") : NULL;
    if (dataWinFile) setvbuf(dataWinFile, NULL, _IOFBF, 256 * 1024);

    for (uint32_t pId = 0; pId < dw->txtr.count; pId++) {
        char outPath[256];
        snprintf(outPath, sizeof(outPath), "sdmc:/3ds/butterscotch/cache/page_%u.atlas", pId);

        FILE* check = fopen(outPath, "r");
        if (check) { fclose(check); continue; }

        Texture* txtr = &dw->txtr.textures[pId];
        if (txtr->blobSize == 0) continue;

        uint8_t* streamedBlob = ctrReadPngBlobFromFile(dataWinFile, txtr->blobOffset, txtr->blobSize);
        if (!streamedBlob) continue;

        int w, h, ch;
        uint8_t* pixels = stbi_load_from_memory(streamedBlob, (int)txtr->blobSize, &w, &h, &ch, 4);
        free(streamedBlob);

        if (!pixels) continue;

        uint16_t* out16 = (uint16_t*) malloc(w * h * 2);
        if (out16) {
            for (int i = 0; i < w * h; i++) {
                int src = i * 4;
                out16[i] = pack_rgba4444(pixels[src], pixels[src+1], pixels[src+2], pixels[src+3]);
            }

            FILE* outF = fopen(outPath, "wb");
            if (outF) {
                setvbuf(outF, NULL, _IOFBF, 256 * 1024);
                AtlasHeader hdr = { ATLAS_MAGIC, (uint32_t)w, (uint32_t)h };
                fwrite(&hdr, sizeof(hdr), 1, outF);
                fwrite(out16, 1, w * h * 2, outF);
                fclose(outF);
            }
            free(out16);
        }
        stbi_image_free(pixels);
    }

    if (dataWinFile) fclose(dataWinFile);

    FILE* outFlag = fopen(flagPath, "w");
    if (outFlag) { fputs("READY", outFlag); fclose(outFlag); }
}
static void ctrFlushBatch(CtrRenderer* gl) {
    if (gl->quadBatchCount == 0 || gl->quadBatchTexture == 0) return;
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
    if (gl->quadBatchCount > 0 && gl->quadBatchTexture != textureId) ctrFlushBatch(gl);
    if (gl->quadBatchCount >= gl->quadBatchCapacity) ctrFlushBatch(gl);

    gl->quadBatchTexture = textureId;
    CtrPackedVertex* tri = (CtrPackedVertex*) gl->quadBatchVertices + gl->quadBatchCount * 6;

    tri[0] = (CtrPackedVertex) { x0, y0, 0, u0, v0, r0, g0, b0, a0 };
    tri[1] = (CtrPackedVertex) { x1, y1, 0, u1, v0, r1, g1, b1, a1 };
    tri[2] = (CtrPackedVertex) { x2, y2, 0, u1, v1, r2, g2, b2, a2 };
    tri[3] = tri[0];
    tri[4] = tri[2];
    tri[5] = (CtrPackedVertex) { x3, y3, 0, u0, v1, r3, g3, b3, a3 };

    gl->quadBatchCount++;
}

static void ctrPushQuad(CtrRenderer* gl, GLuint textureId, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float u0, float v0, float u1, float v1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ctrPushQuadGradient(gl, textureId, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
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
        outX[*count] = cx + cosf(angle) * rx;
        outY[*count] = cy + sinf(angle) * ry;
        (*count)++;
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
    gl->tpags = (CtrTpagData*) calloc(gl->tpagCount, sizeof(CtrTpagData));

    gl->quadBatchCapacity = CTR_QUAD_BATCH_CAPACITY;
    gl->quadBatchVertices = linearAlloc(gl->quadBatchCapacity * 6 * sizeof(CtrPackedVertex));

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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ctrBuildFullTextureCache(gl);
}

static void ctrDestroy(Renderer* renderer) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    glDeleteTextures(1, &gl->whiteTexture);
    for (uint32_t i = 0; i < gl->tpagCount; i++) {
        if (gl->tpags[i].isLoaded) {
            for (int cx = 0; cx < gl->tpags[i].chunksX; cx++) {
                for (int cy = 0; cy < gl->tpags[i].chunksY; cy++) {
                    glDeleteTextures(1, &gl->tpags[i].chunks[cx][cy].tex);
                }
            }
        }
    }
    free(gl->tpags);
    if (gl->quadBatchVertices) linearFree(gl->quadBatchVertices);
    free(gl);
}

static void ctrBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);
    gl->windowW = windowW; gl->windowH = windowH;
    gl->gameW = gameW; gl->gameH = gameH;
}

static void ctrBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);
    float scaleX = (float)gl->windowW / (float)gl->gameW;
    float scaleY = (float)gl->windowH / (float)gl->gameH;
    float scale = fminf(scaleX, scaleY);

    int32_t vpW = (int32_t)(gl->gameW * scale);
    int32_t vpH = (int32_t)(gl->gameH * scale);
    int32_t vpX = (gl->windowW - vpW) / 2;
    int32_t vpY = (gl->windowH - vpH) / 2;
    glViewport(vpX, vpY, vpW, vpH);

    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, (float)viewX, (float)(viewX + viewW), (float)(viewY + viewH), (float)viewY, -1.0f, 1.0f);

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
static void ctrEndFrame(Renderer* renderer) { ctrFlushBatch((CtrRenderer*) renderer); g_frameCounter++; }
static void ctrRendererFlush(Renderer* renderer) { ctrFlushBatch((CtrRenderer*) renderer); }

static void extract_tpag_from_ram(CtrRenderer* gl, DataWin* dw, uint32_t tId, uint16_t* atlas_pixels, int atlas_w, int atlas_h) {
    TexturePageItem* item = &dw->tpag.items[tId];
    int extW = item->sourceWidth > 0 ? item->sourceWidth : 1;
    int extH = item->sourceHeight > 0 ? item->sourceHeight : 1;
    int extX = item->sourceX; int extY = item->sourceY;

    CtrTpagData* tpag = &gl->tpags[tId];
    tpag->origW = extW;
    tpag->origH = extH;

    tpag->chunksX = (extW + 1023) / 1024;
    tpag->chunksY = (extH + 1023) / 1024;
    if (tpag->chunksX > CTR_MAX_CHUNKS_X) tpag->chunksX = CTR_MAX_CHUNKS_X;
    if (tpag->chunksY > CTR_MAX_CHUNKS_Y) tpag->chunksY = CTR_MAX_CHUNKS_Y;

    for (int cy = 0; cy < tpag->chunksY; cy++) {
        for (int cx = 0; cx < tpag->chunksX; cx++) {
            CtrTpagChunk* chunk = &tpag->chunks[cx][cy];
            chunk->srcX = cx * 1024;
            chunk->srcY = cy * 1024;
            chunk->width = extW - chunk->srcX; if (chunk->width > 1024) chunk->width = 1024;
            chunk->height = extH - chunk->srcY; if (chunk->height > 1024) chunk->height = 1024;
            chunk->potW = next_pot(chunk->width);
            chunk->potH = next_pot(chunk->height);

            uint16_t* sprite_pixels = (uint16_t*) calloc(chunk->potW * chunk->potH, 2);
            if (!sprite_pixels) continue;

            for (int y = 0; y < chunk->height; y++) {
                int sy = extY + chunk->srcY + y;
                if (sy < 0 || sy >= atlas_h) continue;
                for (int x = 0; x < chunk->width; x++) {
                    int sx = extX + chunk->srcX + x;
                    if (sx < 0 || sx >= atlas_w) continue;
                    sprite_pixels[y * chunk->potW + x] = atlas_pixels[sy * atlas_w + sx];
                }
            }

            GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, chunk->potW, chunk->potH, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, sprite_pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            chunk->tex = tex;
            free(sprite_pixels);
        }
    }
    tpag->isLoaded = true;
}

#define CTR_LINEAR_LOW_THRESHOLD   (1024u * 1024u)
#define CTR_LINEAR_SAFE_TARGET     (2u * 1024u * 1024u)
#define CTR_EVICT_MAX_PER_CALL     32


static void ctrEvictLruIfPressure(CtrRenderer* gl) {
    if (linearSpaceFree() >= CTR_LINEAR_LOW_THRESHOLD) return;

    bool flushedOnce = false;

    int evicted = 0;
    while (evicted < CTR_EVICT_MAX_PER_CALL && linearSpaceFree() < CTR_LINEAR_SAFE_TARGET) {
        uint32_t oldestFrame = UINT32_MAX;
        int32_t victim = -1;
        for (uint32_t i = 0; i < gl->tpagCount; i++) {
            if (!gl->tpags[i].isLoaded) continue;
            if (gl->tpags[i].keepResident) continue;
            if (gl->tpags[i].lastFrameUsed >= g_frameCounter) continue; // in-use this frame
            if (gl->tpags[i].lastFrameUsed < oldestFrame) {
                oldestFrame = gl->tpags[i].lastFrameUsed;
                victim = (int32_t) i;
            }
        }
        if (victim < 0) break;

        if (!flushedOnce) {
            ctrFlushBatch(gl);
            flushedOnce = true;
        }

        for (int cx = 0; cx < gl->tpags[victim].chunksX; cx++) {
            for (int cy = 0; cy < gl->tpags[victim].chunksY; cy++) {
                glDeleteTextures(1, &gl->tpags[victim].chunks[cx][cy].tex);
                gl->tpags[victim].chunks[cx][cy].tex = 0;
            }
        }
        gl->tpags[victim].isLoaded = false;
        evicted++;
    }
}

static void extract_tpag_direct_from_file(CtrRenderer* gl, DataWin* dw, uint32_t tId, FILE* f, int atlas_w, int atlas_h) {
    TexturePageItem* item = &dw->tpag.items[tId];
    int extW = item->sourceWidth > 0 ? item->sourceWidth : 1;
    int extH = item->sourceHeight > 0 ? item->sourceHeight : 1;
    int extX = item->sourceX; int extY = item->sourceY;

    CtrTpagData* tpag = &gl->tpags[tId];
    tpag->origW = extW;
    tpag->origH = extH;

    tpag->chunksX = (extW + 1023) / 1024;
    tpag->chunksY = (extH + 1023) / 1024;
    if (tpag->chunksX > CTR_MAX_CHUNKS_X) tpag->chunksX = CTR_MAX_CHUNKS_X;
    if (tpag->chunksY > CTR_MAX_CHUNKS_Y) tpag->chunksY = CTR_MAX_CHUNKS_Y;

    int header_size = sizeof(AtlasHeader);
    size_t strip_rows = (size_t)extH;
    if ((int)(extY + (int)strip_rows) > atlas_h) strip_rows = (size_t)(atlas_h - extY);
    size_t strip_bytes = (size_t)atlas_w * strip_rows * 2;
    uint16_t* strip = NULL;
    bool strip_is_linear = false;

    if (extY >= 0 && strip_rows > 0) {
        strip = (uint16_t*) linearAlloc(strip_bytes);
        if (strip) {
            strip_is_linear = true;
        }
        else if (strip_bytes <= 2500 * 1024) {
            strip = (uint16_t*) malloc(strip_bytes);
            strip_is_linear = false;
        }
    }

    if (strip) {
        fseek(f, header_size + (size_t)extY * (size_t)atlas_w * 2, SEEK_SET);
        if (fread(strip, 1, strip_bytes, f) != strip_bytes) {
            if (strip_is_linear) linearFree(strip);
            else free(strip);
            strip = NULL;
        }
    }

    for (int cy = 0; cy < tpag->chunksY; cy++) {
        for (int cx = 0; cx < tpag->chunksX; cx++) {
            CtrTpagChunk* chunk = &tpag->chunks[cx][cy];
            chunk->srcX = cx * 1024;
            chunk->srcY = cy * 1024;
            chunk->width = extW - chunk->srcX; if (chunk->width > 1024) chunk->width = 1024;
            chunk->height = extH - chunk->srcY; if (chunk->height > 1024) chunk->height = 1024;
            chunk->potW = next_pot(chunk->width);
            chunk->potH = next_pot(chunk->height);

            uint16_t* sprite_pixels = (uint16_t*) calloc(chunk->potW * chunk->potH, 2);
            if (!sprite_pixels) continue;

            if (strip) {
                for (int y = 0; y < chunk->height; y++) {
                    int local_y = chunk->srcY + y;
                    if (local_y < 0 || (size_t)local_y >= strip_rows) continue;
                    int src_x = extX + chunk->srcX;
                    if (src_x < 0) src_x = 0;
                    int copy_w = chunk->width;
                    if (src_x + copy_w > atlas_w) copy_w = atlas_w - src_x;
                    if (copy_w <= 0) continue;
                    memcpy(&sprite_pixels[y * chunk->potW],
                           &strip[local_y * atlas_w + src_x],
                           (size_t)copy_w * 2);
                }
            } else {
                for (int y = 0; y < chunk->height; y++) {
                    int sy = extY + chunk->srcY + y;
                    if (sy < 0 || sy >= atlas_h) continue;

                    fseek(f, header_size + (sy * atlas_w + extX + chunk->srcX) * 2, SEEK_SET);
                    fread(&sprite_pixels[y * chunk->potW], 2, chunk->width, f);
                }
            }

            GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, chunk->potW, chunk->potH, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, sprite_pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            chunk->tex = tex;
            free(sprite_pixels);
        }
    }
    if (strip) {
        if (strip_is_linear) linearFree(strip);
        else free(strip);
    }
    tpag->isLoaded = true;
}
static void ctrSet3DDepthOffset(Renderer* renderer, float gmDepth) {
    CtrRenderer* gl = (CtrRenderer*)renderer;
    ctrFlushBatch(gl);
    float z3D = 0.0f;

    if (gmDepth >= 1000000.0f) {
        z3D = 0.025f;
    } else if (gmDepth <= -100000.0f) {
        z3D = -0.04f;
    } else {
        z3D = gmDepth / 25000.0f;
        if (z3D >  0.025f) z3D =  0.025f;
        if (z3D < -0.025f) z3D = -0.025f;
    }

    novaSet3DDepth(z3D);
}
#define CTR_PAGE_BATCH_THRESHOLD 4
static __attribute__((aligned(8))) char g_dynamic_io_buf[64 * 1024];

static void loadDynamicSprite(CtrRenderer* gl, DataWin* dw, int32_t tpagIndex) {
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount || gl->tpags[tpagIndex].isLoaded) return;

    ctrEvictLruIfPressure(gl);

    uint32_t pageId = dw->tpag.items[tpagIndex].texturePageId;

    int pendingOnPage = 0;
    for (uint32_t i = 0; i < gl->tpagCount; i++) {
        if (dw->tpag.items[i].texturePageId != pageId) continue;
        if (!gl->tpags[i].isLoaded && gl->tpags[i].keepResident) pendingOnPage++;
    }

    char path[256];
    snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/page_%u.atlas", pageId);

    FILE* f = fopen(path, "rb");
    if (!f) return;
    setvbuf(f, g_dynamic_io_buf, _IOFBF, sizeof(g_dynamic_io_buf));

    AtlasHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != ATLAS_MAGIC) { fclose(f); return; }

    if (pendingOnPage >= CTR_PAGE_BATCH_THRESHOLD) {
        size_t data_size = (size_t)hdr.width * (size_t)hdr.height * 2;
        uint16_t* atlas_pixels = (uint16_t*) linearAlloc(data_size);

        if (atlas_pixels) {
            if (fread(atlas_pixels, 1, data_size, f) == data_size) {
                for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
                    if (dw->tpag.items[tId].texturePageId != pageId) continue;
                    if (gl->tpags[tId].isLoaded) continue;
                    if (!gl->tpags[tId].keepResident && (int32_t)tId != tpagIndex) continue;
                    extract_tpag_from_ram(gl, dw, tId, atlas_pixels, hdr.width, hdr.height);
                }
            }
            linearFree(atlas_pixels);
        } else {
            extract_tpag_direct_from_file(gl, dw, tpagIndex, f, hdr.width, hdr.height);
        }
    } else {
        extract_tpag_direct_from_file(gl, dw, tpagIndex, f, hdr.width, hdr.height);
    }

    fclose(f);
}

static void markTpagResident(CtrRenderer* gl, int32_t tpagIndex) {
    if (tpagIndex >= 0 && (uint32_t)tpagIndex < gl->tpagCount) gl->tpags[tpagIndex].keepResident = true;
}
static void markTpagOffsetResident(CtrRenderer* gl, DataWin* dw, uint32_t tpagOffset) {
    markTpagResident(gl, DataWin_resolveTPAG(dw, tpagOffset));
}
static void markSpriteResident(CtrRenderer* gl, DataWin* dw, int32_t spriteIndex) {
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return;
    Sprite* s = &dw->sprt.sprites[spriteIndex];
    for (uint32_t f = 0; f < s->textureCount; f++) markTpagOffsetResident(gl, dw, s->textureOffsets[f]);
}

static void markBackgroundResident(CtrRenderer* gl, DataWin* dw, int32_t bgndIndex) {
    if (bgndIndex >= 0 && (uint32_t) bgndIndex < dw->bgnd.count) markTpagOffsetResident(gl, dw, dw->bgnd.backgrounds[bgndIndex].textureOffset);
}
void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex) {
    if (!renderer || !renderer->dataWin) return;
    CtrRenderer* gl = (CtrRenderer*) renderer;
    markSpriteResident(gl, renderer->dataWin, spriteIndex);
}
static void ctrOnRoomChanged(Renderer* renderer, int32_t roomIndex) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (roomIndex < 0 || (uint32_t) roomIndex >= dw->room.count) return;
    Room* room = &dw->room.rooms[roomIndex];

    for (uint32_t i = 0; i < gl->tpagCount; i++) gl->tpags[i].keepResident = false;

    for (uint32_t i = 0; i < dw->font.count; i++) markTpagOffsetResident(gl, dw, dw->font.fonts[i].textureOffset);

    if (room->backgrounds) {
        for (int i = 0; i < 8; i++) {
            if (room->backgrounds[i].enabled && room->backgrounds[i].backgroundDefinition >= 0) {
                markBackgroundResident(gl, dw, room->backgrounds[i].backgroundDefinition);
            }
        }
    }
    for (uint32_t i = 0; i < room->tileCount; i++) {
        int32_t bgIdx = room->tiles[i].backgroundDefinition;
        if (room->tiles[i].useSpriteDefinition && bgIdx >= 0) markSpriteResident(gl, dw, bgIdx);
        else if (bgIdx >= 0) markBackgroundResident(gl, dw, bgIdx);
    }
    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int32_t objIdx = room->gameObjects[i].objectDefinition;
        if (objIdx >= 0 && (uint32_t) objIdx < dw->objt.count) {
            markSpriteResident(gl, dw, dw->objt.objects[objIdx].spriteId);
            int32_t parent = dw->objt.objects[objIdx].parentId;
            if (parent >= 0 && (uint32_t)parent < dw->objt.count) {
                markSpriteResident(gl, dw, dw->objt.objects[parent].spriteId);
            }
        }
    }
    bool pageNeedsLoad[256] = {false};
    for (uint32_t i = 0; i < gl->tpagCount; i++) {
        if (gl->tpags[i].keepResident && !gl->tpags[i].isLoaded) {
            uint16_t pid = dw->tpag.items[i].texturePageId;
            if (pid < 256) pageNeedsLoad[pid] = true;
        }
    }

    for (int pid = 0; pid < 256; pid++) {
        if (!pageNeedsLoad[pid]) continue;

        int pendingCount = 0;
        for (uint32_t i = 0; i < gl->tpagCount; i++) {
            if (dw->tpag.items[i].texturePageId == pid && gl->tpags[i].keepResident && !gl->tpags[i].isLoaded) pendingCount++;
        }

        char path[256];
        snprintf(path, sizeof(path), "sdmc:/3ds/butterscotch/cache/page_%d.atlas", pid);

        FILE* f = fopen(path, "rb");
        if (!f) continue;

        setvbuf(f, g_dynamic_io_buf, _IOFBF, sizeof(g_dynamic_io_buf));

        AtlasHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ATLAS_MAGIC) {
            bool batchLoaded = false;
            if (pendingCount >= 2) {
                size_t data_size = (size_t)hdr.width * (size_t)hdr.height * 2;
                uint16_t* atlas_pixels = (uint16_t*) linearAlloc(data_size);
                if (atlas_pixels) {
                    if (fread(atlas_pixels, 1, data_size, f) == data_size) {
                        for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
                            if (dw->tpag.items[tId].texturePageId == pid && gl->tpags[tId].keepResident && !gl->tpags[tId].isLoaded) {
                                extract_tpag_from_ram(gl, dw, tId, atlas_pixels, hdr.width, hdr.height);
                            }
                        }
                        batchLoaded = true;
                    }
                    linearFree(atlas_pixels);
                }
            }
            if (!batchLoaded) {
                for (uint32_t tId = 0; tId < gl->tpagCount; tId++) {
                    if (dw->tpag.items[tId].texturePageId == pid && gl->tpags[tId].keepResident && !gl->tpags[tId].isLoaded) {
                        extract_tpag_direct_from_file(gl, dw, tId, f, hdr.width, hdr.height);
                    }
                }
            }
        }
        fclose(f);
    }
}
static void lerp2D_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float tx, float ty, float* outX, float* outY) {
    float topX = x0 + (x1 - x0) * tx;
    float topY = y0 + (y1 - y0) * tx;
    float botX = x3 + (x2 - x3) * tx;
    float botY = y3 + (y2 - y3) * tx;
    *outX = topX + (botX - topX) * ty;
    *outY = topY + (botY - topY) * ty;
}

static void ctrDrawTpagRegion(CtrRenderer* gl, uint32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (tpagIndex >= gl->tpagCount) return;

    if (!gl->tpags[tpagIndex].isLoaded) loadDynamicSprite(gl, gl->base.dataWin, tpagIndex);
    if (!gl->tpags[tpagIndex].isLoaded) return;

    gl->tpags[tpagIndex].lastFrameUsed = g_frameCounter;
    CtrTpagData* tpagData = &gl->tpags[tpagIndex];

    if (srcW <= 0.0f || srcH <= 0.0f) return;
    float reqL = srcOffX;
    float reqT = srcOffY;
    float reqR = srcOffX + srcW;
    float reqB = srcOffY + srcH;

    for (int cy = 0; cy < tpagData->chunksY; cy++) {
        for (int cx = 0; cx < tpagData->chunksX; cx++) {
            CtrTpagChunk* chunk = &tpagData->chunks[cx][cy];

            float chunkL = (float)chunk->srcX;
            float chunkT = (float)chunk->srcY;
            float chunkR = chunkL + (float)chunk->width;
            float chunkB = chunkT + (float)chunk->height;
            float drawL = fmaxf(reqL, chunkL);
            float drawT = fmaxf(reqT, chunkT);
            float drawR = fminf(reqR, chunkR);
            float drawB = fminf(reqB, chunkB);
            if (drawL >= drawR || drawT >= drawB) continue;
            float u0 = (drawL - chunkL) / (float)chunk->potW;
            float v0 = (drawT - chunkT) / (float)chunk->potH;
            float u1 = (drawR - chunkL) / (float)chunk->potW;
            float v1 = (drawB - chunkT) / (float)chunk->potH;
            float tL = (drawL - reqL) / srcW;
            float tR = (drawR - reqL) / srcW;
            float tT = (drawT - reqT) / srcH;
            float tB = (drawB - reqT) / srcH;
            float px0, py0, px1, py1, px2, py2, px3, py3;
            lerp2D_quad(x0,y0, x1,y1, x2,y2, x3,y3, tL, tT, &px0, &py0);
            lerp2D_quad(x0,y0, x1,y1, x2,y2, x3,y3, tR, tT, &px1, &py1);
            lerp2D_quad(x0,y0, x1,y1, x2,y2, x3,y3, tR, tB, &px2, &py2);
            lerp2D_quad(x0,y0, x1,y1, x2,y2, x3,y3, tL, tB, &px3, &py3);

            ctrPushQuad(gl, chunk->tex, px0, py0, px1, py1, px2, py2, px3, py3, u0, v0, u1, v1, r, g, b, a);
        }
    }
}

static inline void ctrDrawSpriteFastSingleChunk(CtrRenderer* gl, CtrTpagData* tpagData,
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    CtrTpagChunk* chunk = &tpagData->chunks[0][0];

    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = (float)chunk->width  / (float)chunk->potW;
    float v1 = (float)chunk->height / (float)chunk->potH;
    ctrPushQuad(gl, chunk->tex, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

static void ctrDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer; DataWin* dw = renderer->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= gl->tpagCount) return;

    if (!gl->tpags[tpagIndex].isLoaded) loadDynamicSprite(gl, dw, tpagIndex);
    if (!gl->tpags[tpagIndex].isLoaded) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float lx0 = ((float) tpag->targetX - originX) * xscale; float ly0 = ((float) tpag->targetY - originY) * yscale;
    float lx1 = lx0 + ((float) tpag->sourceWidth) * xscale; float ly1 = ly0 + ((float) tpag->sourceHeight) * yscale;

    uint8_t a = ctrAlphaToByte(alpha); if (a == 0) return;

    CtrTpagData* tpagData = &gl->tpags[tpagIndex];
    bool singleChunk = (tpagData->chunksX == 1 && tpagData->chunksY == 1);
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);

    if (angleDeg == 0.0f) {
        float x0 = roundf(x + lx0); float y0 = roundf(y + ly0);
        float x1 = roundf(x + lx1); float y1 = roundf(y + ly1);
        if (singleChunk) {
            tpagData->lastFrameUsed = g_frameCounter;
            ctrDrawSpriteFastSingleChunk(gl, tpagData, x0, y0, x1, y0, x1, y1, x0, y1, r, g, b, a);
        } else {
            ctrDrawTpagRegion(gl, tpagIndex, 0.0f, 0.0f, (float)tpag->sourceWidth, (float)tpag->sourceHeight, x0, y0, x1, y0, x1, y1, x0, y1, r, g, b, a);
        }
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float c = cosf(angleRad); float s = sinf(angleRad);
        float x0 = lx0 * c - ly0 * s + x; float y0 = lx0 * s + ly0 * c + y;
        float x1 = lx1 * c - ly0 * s + x; float y1 = lx1 * s + ly0 * c + y;
        float x2 = lx1 * c - ly1 * s + x; float y2 = lx1 * s + ly1 * c + y;
        float x3 = lx0 * c - ly1 * s + x; float y3 = lx0 * s + ly1 * c + y;
        if (singleChunk) {
            tpagData->lastFrameUsed = g_frameCounter;
            ctrDrawSpriteFastSingleChunk(gl, tpagData, x0, y0, x1, y1, x2, y2, x3, y3, r, g, b, a);
        } else {
            ctrDrawTpagRegion(gl, tpagIndex, 0.0f, 0.0f, (float)tpag->sourceWidth, (float)tpag->sourceHeight, x0, y0, x1, y1, x2, y2, x3, y3, r, g, b, a);
        }
    }
}

static void ctrDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t a = ctrAlphaToByte(alpha); if (a == 0) return;
    float cx = roundf(x); float cy = roundf(y);
    float x1 = cx + roundf((float)srcW * xscale); float y1 = cy + roundf((float)srcH * yscale);

    ctrDrawTpagRegion(gl, tpagIndex, (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH, cx, cy, x1, cy, x1, y1, cx, y1, BGR_R(color), BGR_G(color), BGR_B(color), a);
}

static void ctrDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    if (tile == NULL) return;
    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tile);
    if (tpagIndex < 0) return;

    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];
    int32_t srcX = tile->sourceX; int32_t srcY = tile->sourceY;
    int32_t srcW = (int32_t) tile->width; int32_t srcH = (int32_t) tile->height;
    float drawX = (float) tile->x + offsetX; float drawY = (float) tile->y + offsetY;

    if (tpag->targetX > srcX) {
        int32_t clip = tpag->targetX - srcX; drawX += (float)clip * tile->scaleX; srcW -= clip; srcX = tpag->targetX;
    }
    if (tpag->targetY > srcY) {
        int32_t clip = tpag->targetY - srcY; drawY += (float)clip * tile->scaleY; srcH -= clip; srcY = tpag->targetY;
    }

    int32_t cR = tpag->targetX + tpag->sourceWidth; int32_t cB = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > cR) srcW = cR - srcX;
    if (srcY + srcH > cB) srcH = cB - srcY;
    if (srcW <= 0 || srcH <= 0) return;

    int32_t atlasOffX = srcX - tpag->targetX; int32_t atlasOffY = srcY - tpag->targetY;
    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : ((float) alphaByte / 255.0f);

    ctrDrawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH, drawX, drawY, tile->scaleX, tile->scaleY, tile->color & 0x00FFFFFFu, alpha);
}

static void emitColoredQuad(CtrRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    uint8_t ca = ctrAlphaToByte(a); if (ca == 0) return;
    ctrPushQuad(gl, gl->whiteTexture, x0, y0, x1, y0, x1, y1, x0, y1, 0.5f, 0.5f, 0.5f, 0.5f, (uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f), ca);
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
    uint8_t cr = BGR_R(color); uint8_t cg = BGR_G(color); uint8_t cb = BGR_B(color); uint8_t ca = ctrAlphaToByte(alpha);
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
    if (len < 0.0001f) return;
    x2 += (dx / len) * 0.5f; y2 += (dy / len) * 0.5f; x1 -= (dx / len) * 0.5f; y1 -= (dy / len) * 0.5f;
    dx = x2 - x1; dy = y2 - y1; len = sqrtf(dx * dx + dy * dy);
    float halfW = actualWidth * 0.5f; float px = (-dy / len) * halfW; float py = (dx / len) * halfW;
    ctrPushQuad(gl, gl->whiteTexture, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    uint8_t c1r = BGR_R(color1); uint8_t c1g = BGR_G(color1); uint8_t c1b = BGR_B(color1);
    uint8_t c2r = BGR_R(color2); uint8_t c2g = BGR_G(color2); uint8_t c2b = BGR_B(color2);
    uint8_t ca = ctrAlphaToByte(alpha);
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
    if (len < 0.0001f) return;
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
        ctrPushQuad(gl, gl->whiteTexture, x1, y1, x2, y2, x3, y3, x3, y3, 0.5f, 0.5f, 0.5f, 0.5f, BGR_R(renderer->drawColor), BGR_G(renderer->drawColor), BGR_B(renderer->drawColor), ctrAlphaToByte(renderer->drawAlpha));
    }
}

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

    if (rx <= 2.5f && ry <= 2.5f) {
        ctrDrawRectangle(renderer, cx - rx, cy - ry, cx + rx, cy + ry, color, alpha, outline);
        return;
    }

    float maxRadius = fmaxf(rx, ry);
    int32_t segments = (int32_t)(sqrtf(maxRadius) * 3.5f);
    if (segments < 8) segments = 8;
    if (segments > 32) segments = 32;

    float angleStep = (2.0f * (float)M_PI) / (float)segments;
    uint8_t r, g, b, a; ctrColorToBytes(color, alpha, &r, &g, &b, &a);

    float prevX = cx + rx; float prevY = cy;
    for (int32_t i = 1; i <= segments; i++) {
        float angle = angleStep * (float)i;
        float nextX = cx + cosf(angle) * rx;
        float nextY = cy + sinf(angle) * ry;

        if (outline) {
            ctrDrawLine(renderer, prevX, prevY, nextX, nextY, 1.0f, color, alpha);
        } else {
            ctrPushTriangleSolid(gl, cx, cy, prevX, prevY, nextX, nextY, r, g, b, a);
        }
        prevX = nextX; prevY = nextY;
    }
}

static void ctrDrawCircle(Renderer* renderer, float x, float y, float radius, uint32_t color, float alpha, bool outline, int32_t precision) {
    ctrDrawEllipse(renderer, x, y, fabsf(radius), fabsf(radius), color, alpha, outline, precision);
}

static void ctrDrawRoundrect(Renderer* renderer, float x1, float y1, float x2, float y2, float radx, float rady, uint32_t color, float alpha, bool outline, int32_t precision) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    float left = fminf(x1, x2); float right = fmaxf(x1, x2); float top = fminf(y1, y2); float bottom = fmaxf(y1, y2);
    float width = right - left; float height = bottom - top;

    if (width <= 0.0f || height <= 0.0f || radx <= 0.0f || rady <= 0.0f) { ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline); return; }
    radx = fabsf(radx); rady = fabsf(rady);
    if (radx > width * 0.5f) radx = width * 0.5f; if (rady > height * 0.5f) rady = height * 0.5f;

    int32_t arcSegments = (int32_t)(sqrtf(fmaxf(radx, rady)) * 0.8f);
    if (arcSegments < 2) arcSegments = 2;
    if (arcSegments > 8) arcSegments = 8;

    float pX[CTR_MAX_ROUNDRECT_POINTS]; float pY[CTR_MAX_ROUNDRECT_POINTS]; int32_t count = 0; float halfPi = 0.5f * (float)M_PI;

    ctrAppendArcPoints(pX, pY, &count, right - radx, top + rady, radx, rady, -halfPi, 0.0f, arcSegments, false);
    ctrAppendArcPoints(pX, pY, &count, right - radx, bottom - rady, radx, rady, 0.0f, halfPi, arcSegments, true);
    ctrAppendArcPoints(pX, pY, &count, left + radx, bottom - rady, radx, rady, halfPi, (float)M_PI, arcSegments, true);
    ctrAppendArcPoints(pX, pY, &count, left + radx, top + rady, radx, rady, (float)M_PI, (float)M_PI + halfPi, arcSegments, true);

    if (count < 2) { ctrDrawRectangle(renderer, left, top, right, bottom, color, alpha, outline); return; }

    uint8_t r, g, b, a; ctrColorToBytes(color, alpha, &r, &g, &b, &a);
    if (outline) {
        for (int32_t i = 0; i < count; i++) {
            int32_t next = (i + 1) % count;
            ctrDrawLine(renderer, pX[i], pY[i], pX[next], pY[next], 1.0f, color, alpha);
        }
    } else {
        float centerX = (left + right) * 0.5f; float centerY = (top + bottom) * 0.5f;
        for (int32_t i = 0; i < count; i++) {
            int32_t next = (i + 1) % count;
            ctrPushTriangleSolid(gl, centerX, centerY, pX[i], pY[i], pX[next], pY[next], r, g, b, a);
        }
    }
}
typedef struct { Font* font; uint32_t tpagIndex; } CtrFontState;
static bool ctrResolveFontState(CtrRenderer* gl, DataWin* dw, Font* font, CtrFontState* state) {
    state->font = font; state->tpagIndex = 0;
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (fontTpagIndex < 0 || fontTpagIndex >= (int32_t)gl->tpagCount) return false;

    if (!gl->tpags[fontTpagIndex].isLoaded) loadDynamicSprite(gl, dw, fontTpagIndex);
    if (!gl->tpags[fontTpagIndex].isLoaded) return false;

    state->tpagIndex = fontTpagIndex;
    return true;
}

static bool ctrResolveGlyph(CtrRenderer* gl, DataWin* dw, CtrFontState* state, FontGlyph* glyph, float cursorX, float cursorY, uint32_t* outTpagIndex, float* outSrcX, float* outSrcY, float* outSrcW, float* outSrcH, float* outLocalX0, float* outLocalY0) {
    *outTpagIndex = state->tpagIndex;
    *outSrcX = (float) glyph->sourceX; *outSrcY = (float) glyph->sourceY;
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

    uint8_t a = ctrAlphaToByte(renderer->drawAlpha); if (a == 0) return;

    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);
    if (textLen == 0) { free(processed); return; }
    x = roundf(x);
    y = roundf(y);

    int32_t lineCount = TextUtils_countLines(processed, textLen);

    float lineStride = (float) font->emSize;
    if (lineStride <= 0.0f) {
        for (uint32_t i = 0; i < font->glyphCount; i++)
            if (font->glyphs[i].sourceHeight > lineStride) lineStride = (float) font->glyphs[i].sourceHeight;
        if (lineStride <= 0.0f) lineStride = 10.0f;
    }

    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -(float)lineCount * lineStride / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -(float)lineCount * lineStride;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;

    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset; int32_t pos = 0;
        while (lineLen > pos) {
            int32_t oldPos = pos;
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            if (pos == oldPos) { pos++; continue; }

            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) { cursorX += glyph->shift; continue; }

            float srcX, srcY, srcW, srcH, localX0, localY0;
            uint32_t tpagIndex;
            if (!ctrResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &tpagIndex, &srcX, &srcY, &srcW, &srcH, &localX0, &localY0)) {
                cursorX += glyph->shift; continue;
            }

            float localX1 = localX0 + (float) glyph->sourceWidth; float localY1 = localY0 + (float) glyph->sourceHeight;
            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);
            ctrDrawTpagRegion(gl, tpagIndex,
                srcX,
                srcY,
                srcW, srcH,
                px0, py0, px1, py1, px2, py2, px3, py3,
                BGR_R(renderer->drawColor), BGR_G(renderer->drawColor), BGR_B(renderer->drawColor), a);

            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos; cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }
        cursorY += lineStride;
        lineStart = (textLen > lineEnd) ? TextUtils_skipNewline(processed, lineEnd, textLen) : lineEnd;
    }

    free(processed);
}

static void ctrDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float floatAlpha) {
    float savedAlpha = renderer->drawAlpha; renderer->drawAlpha = floatAlpha;
    ctrDrawText(renderer, text, x, y, xscale, yscale, angleDeg);
    renderer->drawAlpha = savedAlpha;
}

static int32_t ctrCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) { return -1; }
static void ctrDeleteSprite(Renderer* renderer, int32_t spriteIndex) {}

static RendererVtable ctrVtable = {
    .init = ctrInit, .destroy = ctrDestroy, .beginFrame = ctrBeginFrame, .endFrame = ctrEndFrame,
    .beginView = ctrBeginView, .endView = ctrEndView,
    .drawSprite = ctrDrawSprite, .drawSpritePart = ctrDrawSpritePart, .drawRectangle = ctrDrawRectangle,
    .drawLine = ctrDrawLine, .drawLineColor = ctrDrawLineColor, .drawTriangle = ctrDrawTriangle,
    .drawText = ctrDrawText, .drawTextColor = ctrDrawTextColor, .flush = ctrRendererFlush,
    .createSpriteFromSurface = ctrCreateSpriteFromSurface, .deleteSprite = ctrDeleteSprite,
    .drawTile = ctrDrawTile, .drawCircle = ctrDrawCircle, .drawRoundrect = ctrDrawRoundrect,
    .drawTriangleColor = ctrDrawTriangleColor, .drawEllipse = ctrDrawEllipse, .onRoomChanged = ctrOnRoomChanged,
    .set3DDepthOffset = ctrSet3DDepthOffset
};

Renderer* CtrRenderer_create(void) {
    CtrRenderer* gl = safeCalloc(1, sizeof(CtrRenderer));
    gl->base.vtable = &ctrVtable;
    gl->base.drawColor = 0xFFFFFF; gl->base.drawAlpha = 1.0f; gl->base.drawFont = -1;
    return (Renderer*) gl;
}