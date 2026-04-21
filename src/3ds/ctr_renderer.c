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

// 3DS texture management:
//   * No persistent SD cache — every page is PNG-decoded fresh on first use.
//     (We used to have a NovaGL on-SD cache here; it kept paging in during play
//      and made hitches worse, so it's gone.)
//   * Lazy per-draw load: the first drawSprite that touches a page decodes +
//     uploads it. Subsequent frames in the same room are zero-cost.
//   * Per-room unload: on every room change we scan the new room's static
//     references (tiles, backgrounds, layer sprites, instance object sprites)
//     and free every GL texture that isn't on that list. Dynamically-set
//     sprites (GML changing spriteIndex at runtime) are still safe because
//     ensureTextureLoaded will lazily re-decode them on first draw.
//
// Net result: on OLD 3DS only the atlases actually visible in the current
// room live in linear-heap memory at any time.

#define CTR_QUAD_BATCH_CAPACITY 4096       // До 4096 спрайтов за 1 вызов (16k вершин)

#define LRU_SWEEP_INTERVAL 180
#define LRU_IDLE_THRESHOLD 450

// ===[ Vtable Implementations ]===

// Forward declarations — renderer-internal helpers used before their definitions.
static bool ensureTextureLoaded(CtrRenderer* gl, uint32_t pageId);
static void unloadPageTexture(CtrRenderer* gl, uint32_t pageId);

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} CtrPackedVertex;

// Выплевывает накопленные спрайты на GPU.
// Client-state enables and vertex-array pointers are set ONCE at renderer init
// (buffer base never moves for our re-used vertex scratch), so the hot path here
// is just bind + draw.
static void ctrFlushBatch(CtrRenderer* gl) {
    if (gl->quadBatchCount == 0 || gl->quadBatchTexture == 0 || gl->quadBatchVertices == nullptr) return;

    glBindTexture(GL_TEXTURE_2D, gl->quadBatchTexture);
    glDrawArrays(GL_QUADS, 0, (GLsizei) (gl->quadBatchCount * 4));

    gl->quadBatchCount = 0;
    gl->quadBatchTexture = 0;
}

// Закидывает спрайт в буфер
static void ctrPushQuad(CtrRenderer* gl, GLuint textureId, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float u0, float v0, float u1, float v1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (gl->quadBatchVertices == nullptr || textureId == 0) return;

    if (gl->quadBatchCount > 0 && gl->quadBatchTexture != textureId) {
        ctrFlushBatch(gl);
    }
    if (gl->quadBatchCount >= gl->quadBatchCapacity) {
        ctrFlushBatch(gl);
    }

    gl->quadBatchTexture = textureId;

    CtrPackedVertex* verts = (CtrPackedVertex*) gl->quadBatchVertices;
    CtrPackedVertex* quad = verts + gl->quadBatchCount * 4;

    quad[0] = (CtrPackedVertex) { .x = x0, .y = y0, .z = 0.0f, .u = u0, .v = v0, .r = r, .g = g, .b = b, .a = a };
    quad[1] = (CtrPackedVertex) { .x = x1, .y = y1, .z = 0.0f, .u = u1, .v = v0, .r = r, .g = g, .b = b, .a = a };
    quad[2] = (CtrPackedVertex) { .x = x2, .y = y2, .z = 0.0f, .u = u1, .v = v1, .r = r, .g = g, .b = b, .a = a };
    quad[3] = (CtrPackedVertex) { .x = x3, .y = y3, .z = 0.0f, .u = u0, .v = v1, .r = r, .g = g, .b = b, .a = a };

    gl->quadBatchCount++;
}

static void ctrInit(Renderer* renderer, DataWin* dataWin) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    renderer->dataWin = dataWin;

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE); // <--- ФИКС ПРОПАДАЮЩИХ ВРАГОВ (исправляет проблему с image_xscale = -1)
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    gl->textureCount = dataWin->txtr.count;
    gl->glTextures = safeMalloc(gl->textureCount * sizeof(GLuint));
    gl->textureWidths = safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureHeights = safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = safeMalloc(gl->textureCount * sizeof(bool));
    gl->lastUsedFrame = safeMalloc(gl->textureCount * sizeof(uint32_t));
    gl->keepResident = safeMalloc(gl->textureCount * sizeof(bool));

    // Инициализация мощного батчера
    gl->quadBatchCapacity = CTR_QUAD_BATCH_CAPACITY;
    gl->quadBatchCount = 0;
    gl->quadBatchTexture = 0;
    gl->quadBatchVertices = safeMalloc((size_t) gl->quadBatchCapacity * 4 * sizeof(CtrPackedVertex));

    // Sticky client-state: enabled once here, never toggled on the hot path.
    // The vertex buffer base pointer never moves (we reuse the same scratch), so
    // the per-attribute pointers are also set once.
    {
        CtrPackedVertex* verts = (CtrPackedVertex*) gl->quadBatchVertices;
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(CtrPackedVertex), &verts[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(CtrPackedVertex), &verts[0].u);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CtrPackedVertex), &verts[0].r);
    }

    // GL handles are generated lazily per-page inside ensureTextureLoaded so we
    // don't eat PICA linear-heap name slots for pages we may never touch.
    for (uint32_t i = 0; gl->textureCount > i; i++) {
        gl->glTextures[i] = 0;
        gl->textureWidths[i] = 0;
        gl->textureHeights[i] = 0;
        gl->textureLoaded[i] = false;
        gl->lastUsedFrame[i] = 0;
        gl->keepResident[i] = false;
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

    gl->originalTexturePageCount = gl->textureCount;
    gl->originalTpagCount = dataWin->tpag.count;
    gl->originalSpriteCount = dataWin->sprt.count;

    fprintf(stderr, "CTR: Renderer initialized (%u texture pages, lazy load)\n", gl->textureCount);
}

static void ctrDestroy(Renderer* renderer) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    glDeleteTextures(1, &gl->whiteTexture);
    // Only delete handles that were actually generated.
    for (uint32_t i = 0; i < gl->textureCount; i++) {
        if (gl->glTextures[i] != 0) {
            glDeleteTextures(1, &gl->glTextures[i]);
            gl->glTextures[i] = 0;
        }
    }

    free(gl->glTextures);
    free(gl->textureWidths);
    free(gl->textureHeights);
    free(gl->textureLoaded);
    free(gl->lastUsedFrame);
    free(gl->keepResident);
    free(gl->quadBatchVertices);
    free(gl);
}

// Per-frame diagnostic counters. Incremented by each draw path; dumped at endFrame.
static uint32_t g_drawSpriteCalls = 0;
static uint32_t g_drawSpritePartCalls = 0;
static uint32_t g_drawTextCalls = 0;
static uint32_t g_drawRectCalls = 0;
static uint32_t g_frameCounter = 0;

static void ctrBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Deferred residency update. ctrOnRoomChanged can't safely glDeleteTextures
    // during Runner_step — the previous frame's GPU commands may still reference
    // those handles via NovaGL's deferred command buffer, and deleting a handle
    // that was bound in the last frame occasionally hangs PICA200. Doing the
    // delete here is safe: we're past novaSwapBuffers, no texture is bound, and
    // the batcher is flushed and empty.
    if (gl->pendingResidencyUpdate && g_frameCounter >= gl->pendingResidencyReadyFrame) {
        gl->pendingResidencyUpdate = false;
        uint32_t warmed = 0;
        for (uint32_t i = 0; i < gl->textureCount; i++) {
            if (!gl->keepResident[i]) continue;
            bool wasLoaded = gl->textureLoaded[i] && gl->textureWidths[i] != 0;
            if (ensureTextureLoaded(gl, i) && !wasLoaded) warmed++;
        }
        if (warmed > 0) fprintf(stderr, "CTR: prefetched %u room texture pages\n", warmed);
    }

    gl->windowW = windowW;
    gl->windowH = windowH;
    gl->gameW = gameW;
    gl->gameH = gameH;

    g_drawSpriteCalls = 0;
    g_drawSpritePartCalls = 0;
    g_drawTextCalls = 0;
    g_drawRectCalls = 0;

    if (g_frameCounter > 0 && g_frameCounter % LRU_SWEEP_INTERVAL == 0) {
        uint32_t freed = 0;
        for (uint32_t i = 0; i < gl->textureCount; i++) {
            if (gl->keepResident[i]) continue;
            if (!gl->textureLoaded[i] || gl->textureWidths[i] == 0) continue;
            uint32_t age = g_frameCounter - gl->lastUsedFrame[i];
            if (age > LRU_IDLE_THRESHOLD) {
                unloadPageTexture(gl, i);
                freed++;
            }
        }
        if (freed > 0) fprintf(stderr, "CTR: LRU freed %u idle texture pages\n", freed);
    }
}

static void ctrBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    (void) portX; (void) portY; (void) portW; (void) portH;

    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);

    // On 3DS we always render the game to the full top screen (400x240).
    // Map the game's port rect onto the whole top screen by scaling the ortho projection.
    glViewport(0, 0, gl->windowW, gl->windowH);
    glDisable(GL_SCISSOR_TEST);

    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, (float) viewX, (float) (viewX + viewW), (float) (viewY + viewH), (float) viewY, -1.0f, 1.0f);

    if (viewAngle != 0.0f) {
        float cx = (float) viewX + (float) viewW / 2.0f;
        float cy = (float) viewY + (float) viewH / 2.0f;
        Matrix4f rot;
        Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.0f);
        float angleRad = viewAngle * (float) M_PI / 180.0f;
        Matrix4f_rotateZ(&rot, -angleRad);
        Matrix4f_translate(&rot, -cx, -cy, 0.0f);
        Matrix4f result;
        Matrix4f_multiply(&result, &projection, &rot);
        projection = result;
    }

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glActiveTexture(GL_TEXTURE0);
}

static void ctrEndView(MAYBE_UNUSED Renderer* renderer) {}

static void ctrBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    (void) portX; (void) portY; (void) portW; (void) portH;

    ctrFlushBatch(gl);
    glBindTexture(GL_TEXTURE_2D, 0);

    glViewport(0, 0, gl->windowW, gl->windowH);
    glDisable(GL_SCISSOR_TEST);

    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, 0.0f, (float) guiW, (float) guiH, 0.0f, -1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(projection.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glActiveTexture(GL_TEXTURE0);
}

static void ctrEndGUI(MAYBE_UNUSED Renderer* renderer) {}

static void ctrEndFrame(Renderer* renderer) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    g_frameCounter++;
}

static void ctrRendererFlush(MAYBE_UNUSED Renderer* renderer) {}

// Lazily decode + upload a TXTR page on first use in the current room.
// No persistent cache — we always go PNG blob -> stb_image -> glTexImage2D.
static bool ensureTextureLoaded(CtrRenderer* gl, uint32_t pageId) {
    gl->lastUsedFrame[pageId] = g_frameCounter;
    if (gl->textureLoaded[pageId]) return (gl->textureWidths[pageId] != 0);
    gl->textureLoaded[pageId] = true;  // mark early; on failure we leave width=0 to signal "don't render"

    DataWin* dw = gl->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    if (gl->glTextures[pageId] == 0) {
        glGenTextures(1, &gl->glTextures[pageId]);
    }
    glBindTexture(GL_TEXTURE_2D, gl->glTextures[pageId]);

    int w, h, channels;
    uint8_t* pixels = stbi_load_from_memory(txtr->blobData, (int) txtr->blobSize, &w, &h, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        fprintf(stderr, "CTR: Failed to decode TXTR page %u (blobSize=%u): %s\n",
                pageId, txtr->blobSize, reason ? reason : "<null>");
        return false;
    }

    gl->textureWidths[pageId] = w;
    gl->textureHeights[pageId] = h;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    stbi_image_free(pixels);

    fprintf(stderr, "CTR: Decoded TXTR page %u (%dx%d)\n", pageId, w, h);
    return true;
}

static void unloadPageTexture(CtrRenderer* gl, uint32_t pageId) {
    if (!gl->textureLoaded[pageId] && gl->glTextures[pageId] == 0) return;
    if (gl->glTextures[pageId] != 0) {
        glDeleteTextures(1, &gl->glTextures[pageId]);
        gl->glTextures[pageId] = 0;
    }
    gl->textureLoaded[pageId] = false;
    gl->textureWidths[pageId] = 0;
    gl->textureHeights[pageId] = 0;
}

void CtrRenderer_prefetchSprite(Renderer* renderer, int32_t spriteIndex) {
    if (renderer == nullptr || renderer->dataWin == nullptr) return;

    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    for (uint32_t frame = 0; frame < sprite->textureCount; frame++) {
        int32_t tpagIndex = DataWin_resolveTPAG(dw, sprite->textureOffsets[frame]);
        if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) continue;
        int16_t pageId = dw->tpag.items[tpagIndex].texturePageId;
        if (pageId < 0 || (uint32_t) pageId >= gl->textureCount) continue;
        ensureTextureLoaded(gl, (uint32_t) pageId);
    }
}

// ===[ Per-room texture-page residency ]===

// Mark the texture page behind a TPAG offset as needed (if resolvable).
static void markTpagOffsetResident(CtrRenderer* gl, DataWin* dw, uint32_t tpagOffset) {
    int32_t tpagIdx = DataWin_resolveTPAG(dw, tpagOffset);
    if (tpagIdx < 0 || (uint32_t) tpagIdx >= dw->tpag.count) return;
    int16_t pageId = dw->tpag.items[tpagIdx].texturePageId;
    if (pageId < 0 || (uint32_t) pageId >= gl->textureCount) return;
    gl->keepResident[pageId] = true;
}

// All pages referenced by every frame of a sprite.
static void markSpriteResident(CtrRenderer* gl, DataWin* dw, int32_t spriteIndex) {
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return;
    Sprite* s = &dw->sprt.sprites[spriteIndex];
    for (uint32_t f = 0; f < s->textureCount; f++) {
        markTpagOffsetResident(gl, dw, s->textureOffsets[f]);
    }
}

static void markBackgroundResident(CtrRenderer* gl, DataWin* dw, int32_t bgndIndex) {
    if (bgndIndex < 0 || (uint32_t) bgndIndex >= dw->bgnd.count) return;
    Background* bg = &dw->bgnd.backgrounds[bgndIndex];
    markTpagOffsetResident(gl, dw, bg->textureOffset);
}

// Build the "keep" set from the new room's static references, then unload every
// page outside that set. Kept as pragmatic as possible — we don't try to solve
// the "GML changes spriteIndex at runtime" case (that's handled by lazy reload
// on next draw), but we cover everything visible from data.win up front.
static void ctrOnRoomChanged(Renderer* renderer, int32_t roomIndex) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (roomIndex < 0 || (uint32_t) roomIndex >= dw->room.count) return;
    Room* room = &dw->room.rooms[roomIndex];

    // Reset resident mask.
    for (uint32_t i = 0; i < gl->textureCount; i++) gl->keepResident[i] = false;

    // Pin every font atlas. Fonts are small and text can be drawn at any time
    // (debug overlays, UI). Re-decoding them every transition is pure waste.
    for (uint32_t i = 0; i < dw->font.count; i++) {
        markTpagOffsetResident(gl, dw, dw->font.fonts[i].textureOffset);
    }

    if (!room->payloadLoaded) {
        // Payload hasn't been loaded yet — nothing room-specific to pin. Bail
        // out and let lazy-load on first draw handle it.
        goto unload_stale;
    }

    // Room-level backgrounds (up to 8 layers on pre-GMS2 or GMS2 compat).
    if (room->backgrounds != nullptr) {
        for (int i = 0; i < 8; i++) {
            if (!room->backgrounds[i].enabled) continue;
            markBackgroundResident(gl, dw, room->backgrounds[i].backgroundDefinition);
        }
    }

    // Legacy (pre-GMS2) tiles on the room itself.
    for (uint32_t i = 0; i < room->tileCount; i++) {
        RoomTile* tile = &room->tiles[i];
        if (tile->useSpriteDefinition) {
            markSpriteResident(gl, dw, tile->backgroundDefinition);
        } else {
            markBackgroundResident(gl, dw, tile->backgroundDefinition);
        }
    }

    // Instances: mark the default sprite of the GameObject the instance spawns
    // from. If GML later swaps spriteIndex, lazy-load picks it up at draw time.
    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        int32_t objectIndex = room->gameObjects[i].objectDefinition;
        if (objectIndex < 0 || (uint32_t) objectIndex >= dw->objt.count) continue;
        markSpriteResident(gl, dw, dw->objt.objects[objectIndex].spriteId);
        // Cover parent-chain sprites too: child objects sometimes inherit visuals.
        int32_t parent = dw->objt.objects[objectIndex].parentId;
        int guard = 8;
        while (parent >= 0 && (uint32_t) parent < dw->objt.count && guard-- > 0) {
            markSpriteResident(gl, dw, dw->objt.objects[parent].spriteId);
            parent = dw->objt.objects[parent].parentId;
        }
    }

    // GMS2 layered data.
    for (uint32_t li = 0; li < room->layerCount; li++) {
        RoomLayer* layer = &room->layers[li];
        if (layer->backgroundData != nullptr) {
            markSpriteResident(gl, dw, layer->backgroundData->spriteIndex);
        }
        if (layer->tilesData != nullptr) {
            markBackgroundResident(gl, dw, layer->tilesData->backgroundIndex);
        }
        if (layer->assetsData != nullptr) {
            for (uint32_t i = 0; i < layer->assetsData->legacyTileCount; i++) {
                RoomTile* tile = &layer->assetsData->legacyTiles[i];
                if (tile->useSpriteDefinition) {
                    markSpriteResident(gl, dw, tile->backgroundDefinition);
                } else {
                    markBackgroundResident(gl, dw, tile->backgroundDefinition);
                }
            }
            for (uint32_t i = 0; i < layer->assetsData->spriteCount; i++) {
                markSpriteResident(gl, dw, layer->assetsData->sprites[i].spriteIndex);
            }
        }
    }

unload_stale:
    {
        // Don't glDeleteTextures here — defer to the next ctrBeginFrame so the
        // PICA200 is guaranteed idle for these handles.
        uint32_t kept = 0, alreadyLoaded = 0;
        for (uint32_t i = 0; i < gl->textureCount; i++) {
            if (gl->keepResident[i]) {
                kept++;
                if (gl->textureLoaded[i] && gl->textureWidths[i] != 0) alreadyLoaded++;
            }
        }
        gl->pendingResidencyUpdate = true;
        gl->pendingResidencyMarkFrame = g_frameCounter;
        gl->pendingResidencyReadyFrame = g_frameCounter;
        fprintf(stderr, "CTR: Room %d residency: keep=%u, already_loaded=%u\n", roomIndex, kept, alreadyLoaded);
    }
}

static void ctrDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    g_drawSpriteCalls++;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

    const float insetEps = 0.01f;
    float u0 = ((float) tpag->sourceX + insetEps) / (float) texW;
    float v0 = ((float) tpag->sourceY + insetEps) / (float) texH;
    float u1 = ((float) (tpag->sourceX + tpag->sourceWidth) - insetEps) / (float) texW;
    float v1 = ((float) (tpag->sourceY + tpag->sourceHeight) - insetEps) / (float) texH;

    // БЫСТРАЯ МАТЕМАТИКА вместо тяжелых 4x4 матриц
    float lx0 = ((float) tpag->targetX - originX) * xscale;
    float ly0 = ((float) tpag->targetY - originY) * yscale;
    float lx1 = lx0 + ((float) tpag->sourceWidth) * xscale;
    float ly1 = ly0 + ((float) tpag->sourceHeight) * yscale;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    float c = cosf(angleRad);
    float s = sinf(angleRad);

    float x0 = lx0 * c - ly0 * s + x;
    float y0 = lx0 * s + ly0 * c + y;

    float x1 = lx1 * c - ly0 * s + x;
    float y1 = lx1 * s + ly0 * c + y;

    float x2 = lx1 * c - ly1 * s + x;
    float y2 = lx1 * s + ly1 * c + y;

    float x3 = lx0 * c - ly1 * s + x;
    float y3 = lx0 * s + ly1 * c + y;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    // Кидаем прямо в батчер! Никаких glBegin!
    ctrPushQuad(gl, texId, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

static void ctrDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    g_drawSpritePartCalls++;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;
    if (!ensureTextureLoaded(gl, (uint32_t) pageId)) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];

    const float insetEps = 0.01f;
    float u0 = ((float) (tpag->sourceX + srcOffX) + insetEps) / (float) texW;
    float v0 = ((float) (tpag->sourceY + srcOffY) + insetEps) / (float) texH;
    float u1 = ((float) (tpag->sourceX + srcOffX + srcW) - insetEps) / (float) texW;
    float v1 = ((float) (tpag->sourceY + srcOffY + srcH) - insetEps) / (float) texH;

    float x0 = x;
    float y0 = y;
    float x1 = x + (float) srcW * xscale;
    float y1 = y + (float) srcH * yscale;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    // Загоняем в батчер
    ctrPushQuad(gl, texId, x0, y0, x1, y0, x1, y1, x0, y1, u0, v0, u1, v1, r, g, b, a);
}

static void emitColoredQuad(CtrRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    uint8_t cr = (uint8_t)(r * 255.0f);
    uint8_t cg = (uint8_t)(g * 255.0f);
    uint8_t cb = (uint8_t)(b * 255.0f);
    uint8_t ca = (uint8_t)(a * 255.0f);

    // Прямоугольники теперь тоже рисуются пачками!
    ctrPushQuad(gl, gl->whiteTexture, x0, y0, x1, y0, x1, y1, x0, y1, 0.5f, 0.5f, 0.5f, 0.5f, cr, cg, cb, ca);
}

static void ctrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;

    g_drawRectCalls++;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    if (outline) {
        emitColoredQuad(gl, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha);
        emitColoredQuad(gl, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha);
        emitColoredQuad(gl, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha);
        emitColoredQuad(gl, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha);
    } else {
        emitColoredQuad(gl, x1, y1, x2 + 1, y2 + 1, r, g, b, alpha);
    }
}

static void ctrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

    // Strip order: A, B, D, C (so tri1=A-B-D, tri2=B-D-C cover the rect).
    glBegin(GL_TRIANGLE_STRIP);
        glColor4f(r, g, b, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x1 + px, y1 + py); // A
        glColor4f(r, g, b, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x1 - px, y1 - py); // B
        glColor4f(r, g, b, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x2 + px, y2 + py); // D
        glColor4f(r, g, b, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x2 - px, y2 - py); // C
    glEnd();
}

static void ctrDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;

    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);

    glBegin(GL_TRIANGLE_STRIP);
        glColor4f(r1, g1, b1, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x1 + px, y1 + py); // A
        glColor4f(r1, g1, b1, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x1 - px, y1 - py); // B
        glColor4f(r2, g2, b2, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x2 + px, y2 + py); // D
        glColor4f(r2, g2, b2, alpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x2 - px, y2 - py); // C
    glEnd();
}

static void ctrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, bool outline) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    ctrFlushBatch(gl);
    if (outline) {
        ctrDrawLine(renderer, x1, y1, x2, y2, 1, renderer->drawColor, 1.0f);
        ctrDrawLine(renderer, x2, y2, x3, y3, 1, renderer->drawColor, 1.0f);
        ctrDrawLine(renderer, x3, y3, x1, y1, 1, renderer->drawColor, 1.0f);
    } else {
        float r = (float) BGR_R(renderer->drawColor) / 255.0f;
        float g = (float) BGR_G(renderer->drawColor) / 255.0f;
        float b = (float) BGR_B(renderer->drawColor) / 255.0f;

        glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
        glBegin(GL_TRIANGLES);
            glColor4f(r, g, b, renderer->drawAlpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x1, y1);
            glColor4f(r, g, b, renderer->drawAlpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x2, y2);
            glColor4f(r, g, b, renderer->drawAlpha); glTexCoord2f(0.5f, 0.5f); glVertex2f(x3, y3);
        glEnd();
    }
}

// ===[ Text Drawing ]===

typedef struct {
    Font* font;
    TexturePageItem* fontTpag;
    GLuint texId;
    int32_t texW, texH;
    Sprite* spriteFontSprite;
} CtrFontState;

static bool ctrResolveFontState(CtrRenderer* gl, DataWin* dw, Font* font, CtrFontState* state) {
    state->font = font;
    state->fontTpag = nullptr;
    state->texId = 0;
    state->texW = 0;
    state->texH = 0;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
        if (0 > fontTpagIndex) return false;

        state->fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= gl->textureCount) return false;
        if (!ensureTextureLoaded(gl, (uint32_t) pageId)) return false;

        state->texId = gl->glTextures[pageId];
        state->texW = gl->textureWidths[pageId];
        state->texH = gl->textureHeights[pageId];
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    return true;
}

static bool ctrResolveGlyph(CtrRenderer* gl, DataWin* dw, CtrFontState* state, FontGlyph* glyph, float cursorX, float cursorY, GLuint* outTexId, float* outU0, float* outV0, float* outU1, float* outV1, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex || glyphIndex >= (int32_t) sprite->textureCount) return false;

        uint32_t tpagOffset = sprite->textureOffsets[glyphIndex];
        int32_t tpagIdx = DataWin_resolveTPAG(dw, tpagOffset);
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= gl->textureCount) return false;
        if (!ensureTextureLoaded(gl, (uint32_t) pid)) return false;

        *outTexId = gl->glTextures[pid];
        int32_t tw = gl->textureWidths[pid];
        int32_t th = gl->textureHeights[pid];

        const float insetEps = 0.01f;
        *outU0 = ((float) glyphTpag->sourceX + insetEps) / (float) tw;
        *outV0 = ((float) glyphTpag->sourceY + insetEps) / (float) th;
        *outU1 = ((float) (glyphTpag->sourceX + glyphTpag->sourceWidth) - insetEps) / (float) tw;
        *outV1 = ((float) (glyphTpag->sourceY + glyphTpag->sourceHeight) - insetEps) / (float) th;

        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) ((int32_t) glyphTpag->targetY - sprite->originY);
    } else {
        *outTexId = state->texId;
        const float insetEps = 0.01f;
        *outU0 = ((float) (state->fontTpag->sourceX + glyph->sourceX) + insetEps) / (float) state->texW;
        *outV0 = ((float) (state->fontTpag->sourceY + glyph->sourceY) + insetEps) / (float) state->texH;
        *outU1 = ((float) (state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) - insetEps) / (float) state->texW;
        *outV1 = ((float) (state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) - insetEps) / (float) state->texH;

        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void ctrDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    CtrRenderer* gl = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    g_drawTextCalls++;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    CtrFontState fontState;
    if (!ctrResolveFontState(gl, dw, font, &fontState)) return;

    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;
    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t) ((alpha <= 0.0f) ? 0 : (alpha >= 1.0f ? 255 : (alpha * 255.0f + 0.5f)));

    int32_t textLen = (int32_t) strlen(text);
    if (textLen == 0) return;

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
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
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

            if (pos == oldPos) {
                pos++;
                continue;
            }

            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float u0, v0, u1, v1;
            float localX0, localY0;
            GLuint glyphTexId;

            if (!ctrResolveGlyph(gl, dw, &fontState, glyph, cursorX, cursorY, &glyphTexId, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                cursorX += glyph->shift;
                continue;
            }

            float localX1 = localX0 + (float) glyph->sourceWidth;
            float localY1 = localY0 + (float) glyph->sourceHeight;

            float px0, py0, px1, py1, px2, py2, px3, py3;
            Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
            Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
            Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
            Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

            ctrPushQuad(gl, glyphTexId, px0, py0, px1, py1, px2, py2, px3, py3, u0, v0, u1, v1, r, g, b, a);

            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += lineStride;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }

    ctrFlushBatch(gl);
}

static void ctrDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha) {
    // For simplicity on 3DS we ignore the 4-corner gradient coloring and render as a flat-colored text.
    (void) _c1; (void) _c2; (void) _c3; (void) _c4;
    float savedAlpha = renderer->drawAlpha;
    renderer->drawAlpha = alpha;
    ctrDrawText(renderer, text, x, y, xscale, yscale, angleDeg);
    renderer->drawAlpha = savedAlpha;
}

// ===[ Dynamic sprite creation/deletion ]===
// createSpriteFromSurface is not supported on 3DS (readPixels from PICA200 targets is not implemented in NovaGL).
static int32_t ctrCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    (void) renderer; (void) x; (void) y; (void) w; (void) h; (void) removeback; (void) smooth; (void) xorig; (void) yorig;
    fprintf(stderr, "CTR: createSpriteFromSurface not supported\n");
    return -1;
}

static void ctrDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    (void) renderer; (void) spriteIndex;
}

// ===[ Vtable ]===

static RendererVtable ctrVtable = {
    .init = ctrInit,
    .destroy = ctrDestroy,
    .beginFrame = ctrBeginFrame,
    .endFrame = ctrEndFrame,
    .beginView = ctrBeginView,
    .endView = ctrEndView,
    .beginGUI = ctrBeginGUI,
    .endGUI = ctrEndGUI,
    .drawSprite = ctrDrawSprite,
    .drawSpritePart = ctrDrawSpritePart,
    .drawRectangle = ctrDrawRectangle,
    .drawLine = ctrDrawLine,
    .drawLineColor = ctrDrawLineColor,
    .drawTriangle = ctrDrawTriangle,
    .drawText = ctrDrawText,
    .drawTextColor = ctrDrawTextColor,
    .flush = ctrRendererFlush,
    .createSpriteFromSurface = ctrCreateSpriteFromSurface,
    .deleteSprite = ctrDeleteSprite,
    .drawTile = nullptr,
    .onRoomChanged = ctrOnRoomChanged,
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
