#include <3ds.h>
#include <citro3d.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>
#include <ctype.h>

#include "icon_parse.h"
#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "ctr_renderer.h"
#include "ctr_file_system.h"
#include "sdl12_audio_system.h"
#include "render2d_shader_shbin.h"

u32 __ctru_heap_size        = 0;
u32 __ctru_linear_heap_size = 25 * 1024 * 1024;
u32 __stacksize__           = 64 * 1024;

#define BASE_DIR  "sdmc:/3ds/butterscotch"
#define MAX_GAMES 64

char g_current_data_path[256];
char g_current_cache_dir[256];

typedef struct {
    char name[64];
    char path[256];
    char exe_path[256];
    bool icon_ready;
    int icon_w, icon_h;
    int icon_pot_w, icon_pot_h;
    C3D_Tex icon_tex;
} GameEntry;

char g_next_game_path[256] = "";
bool g_game_change_requested = false;

static GameEntry g_games[MAX_GAMES];
static int       g_game_count = 0;

#define LAUNCHER_W 400
#define LAUNCHER_H 240
#define LAUNCHER_VBUF_CAP (8192 * 3)
#define LAUNCHER_DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} LauncherVertex;

typedef struct {
    C3D_RenderTarget *target;
    DVLB_s *vshaderDvlb;
    shaderProgram_s shaderProg;
    int uLoc_projection;
    C3D_AttrInfo attrInfo;
    C3D_Tex whiteTex;
    LauncherVertex *vbuf;
    uint32_t vbufCap;
    uint32_t vbufHead;
    uint32_t batchStart;
    uint32_t batchVerts;
    C3D_Tex *batchTex;
    bool ready;
    bool inFrame;
} LauncherGfx;

static int launcher_next_pow2(int x) {
    if (x < 8) return 8;
    x--;
    x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16;
    return x + 1;
}

static inline uint16_t launcher_pack_rgba4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
}

static inline uint32_t launcher_morton_pos(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    r |= (x & 1u) << 0;
    r |= (y & 1u) << 1;
    r |= (x & 2u) << 1;
    r |= (y & 2u) << 2;
    r |= (x & 4u) << 2;
    r |= (y & 4u) << 3;
    return r;
}

static void launcher_tile_rgba4(const uint16_t *linear, uint16_t *tiled,
                                int linW, int linH, int potW, int potH) {
    int blocksX = potW >> 3;
    int blocksY = potH >> 3;
    for (int by = 0; by < blocksY; by++) {
        for (int bx = 0; bx < blocksX; bx++) {
            uint16_t *block = &tiled[(by * blocksX + bx) * 64];
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    int sx = bx * 8 + x;
                    int sy = (potH - 1) - (by * 8 + y);
                    uint16_t px = 0;
                    if (sx < linW && sy >= 0 && sy < linH) {
                        px = linear[sy * linW + sx];
                    }
                    block[launcher_morton_pos((uint32_t)x, (uint32_t)y)] = px;
                }
            }
        }
    }
}

static void free_game_icons(void) {
    for (int i = 0; i < g_game_count; i++) {
        if (g_games[i].icon_ready) {
            C3D_TexDelete(&g_games[i].icon_tex);
            memset(&g_games[i].icon_tex, 0, sizeof(g_games[i].icon_tex));
            g_games[i].icon_ready = false;
        }
    }
}

static bool launcher_upload_icon(GameEntry *game, const IconImage *img) {
    if (!game || !img || !img->pixels || img->width <= 0 || img->height <= 0) return false;

    if (game->icon_ready) {
        C3D_TexDelete(&game->icon_tex);
        memset(&game->icon_tex, 0, sizeof(game->icon_tex));
        game->icon_ready = false;
    }

    int potW = launcher_next_pow2(img->width);
    int potH = launcher_next_pow2(img->height);
    uint16_t *linear = calloc((size_t)potW * (size_t)potH, sizeof(uint16_t));
    if (!linear) return false;

    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            const uint8_t *p = &img->pixels[((size_t)y * (size_t)img->width + (size_t)x) * 4u];
            linear[y * potW + x] = launcher_pack_rgba4444(p[0], p[1], p[2], p[3]);
        }
    }

    if (!C3D_TexInit(&game->icon_tex, (u16)potW, (u16)potH, GPU_RGBA4)) {
        free(linear);
        return false;
    }

    uint16_t *tiled = linearAlloc((size_t)potW * (size_t)potH * sizeof(uint16_t));
    if (!tiled) {
        C3D_TexDelete(&game->icon_tex);
        memset(&game->icon_tex, 0, sizeof(game->icon_tex));
        free(linear);
        return false;
    }

    launcher_tile_rgba4(linear, tiled, potW, potH, potW, potH);
    C3D_TexLoadImage(&game->icon_tex, tiled, GPU_TEXFACE_2D, 0);
    C3D_TexFlush(&game->icon_tex);
    C3D_TexSetFilter(&game->icon_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&game->icon_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    linearFree(tiled);
    free(linear);

    game->icon_ready = true;
    game->icon_w = img->width;
    game->icon_h = img->height;
    game->icon_pot_w = potW;
    game->icon_pot_h = potH;
    return true;
}

static bool launcher_has_ext(const char *name, const char *ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n < e) return false;
    name += n - e;
    for (size_t i = 0; i < e; i++) {
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

static bool launcher_find_exe(const char *dir_path, char *out_path, size_t out_size) {
    DIR *dir = opendir(dir_path);
    if (!dir) return false;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!launcher_has_ext(ent->d_name, ".exe")) continue;
        snprintf(out_path, out_size, "%s/%s", dir_path, ent->d_name);
        closedir(dir);
        return true;
    }

    closedir(dir);
    return false;
}

static void launcher_try_load_icon(GameEntry *game, const char *game_dir) {
    static const char *image_names[] = {
        "icon.png", "icon.jpg", "icon.jpeg", "cover.png", "cover.jpg", "cover.jpeg"
    };

    for (size_t i = 0; i < sizeof(image_names) / sizeof(image_names[0]); i++) {
        char image_path[256];
        snprintf(image_path, sizeof(image_path), "%s/%s", game_dir, image_names[i]);
        IconImage img;
        if (load_image_from_file(image_path, &img)) {
            bool ok = launcher_upload_icon(game, &img);
            IconImage_free(&img);
            if (ok) return;
        }
    }

    if (launcher_find_exe(game_dir, game->exe_path, sizeof(game->exe_path))) {
        IconImage img;
        if (extract_icon_from_exe_pe(game->exe_path, &img)) {
            bool ok = launcher_upload_icon(game, &img);
            IconImage_free(&img);
            if (ok) return;
        }
    }
}

static void launcher_gfx_flush(LauncherGfx *gfx) {
    if (!gfx->batchVerts || !gfx->batchTex || !gfx->inFrame) {
        gfx->batchVerts = 0;
        gfx->batchTex = NULL;
        return;
    }
    C3D_TexBind(0, gfx->batchTex);
    C3D_DrawArrays(GPU_TRIANGLES, gfx->batchStart, gfx->batchVerts);
    gfx->batchStart += gfx->batchVerts;
    gfx->batchVerts = 0;
    gfx->batchTex = NULL;
}

static LauncherVertex *launcher_gfx_reserve(LauncherGfx *gfx, uint32_t count, C3D_Tex *tex) {
    if (gfx->batchTex && gfx->batchTex != tex) launcher_gfx_flush(gfx);
    if (gfx->vbufHead + count > gfx->vbufCap) {
        launcher_gfx_flush(gfx);
        C3D_FrameSplit(0);
        gfx->vbufHead = 0;
        gfx->batchStart = 0;
    }
    gfx->batchTex = tex;
    LauncherVertex *v = gfx->vbuf + gfx->vbufHead;
    gfx->vbufHead += count;
    gfx->batchVerts += count;
    return v;
}

static void launcher_push_quad_grad(LauncherGfx *gfx, C3D_Tex *tex,
                                    float x0, float y0, float x1, float y1,
                                    float x2, float y2, float x3, float y3,
                                    float u0, float v0, float u1, float v1,
                                    const float c0[4], const float c1[4],
                                    const float c2[4], const float c3[4]) {
    if (!tex) tex = &gfx->whiteTex;
    LauncherVertex *v = launcher_gfx_reserve(gfx, 6, tex);
    v[0] = (LauncherVertex){x0, y0, 0.f, u0, v0, c0[0], c0[1], c0[2], c0[3]};
    v[1] = (LauncherVertex){x1, y1, 0.f, u1, v0, c1[0], c1[1], c1[2], c1[3]};
    v[2] = (LauncherVertex){x2, y2, 0.f, u1, v1, c2[0], c2[1], c2[2], c2[3]};
    v[3] = v[0];
    v[4] = v[2];
    v[5] = (LauncherVertex){x3, y3, 0.f, u0, v1, c3[0], c3[1], c3[2], c3[3]};
}

static void launcher_push_quad(LauncherGfx *gfx, C3D_Tex *tex,
                               float x, float y, float w, float h,
                               float u0, float v0, float u1, float v1,
                               const float col[4]) {
    launcher_push_quad_grad(gfx, tex, x, y, x + w, y, x + w, y + h, x, y + h,
                            u0, v0, u1, v1, col, col, col, col);
}

static void launcher_rect(LauncherGfx *gfx, float x, float y, float w, float h,
                          float r, float g, float b, float a) {
    float c[4] = {r, g, b, a};
    launcher_push_quad(gfx, &gfx->whiteTex, x, y, w, h, .5f, .5f, .5f, .5f, c);
}

static const uint8_t *launcher_glyph(char ch) {
    static const uint8_t sp[7] = {0,0,0,0,0,0,0};
    static const uint8_t q[7]  = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04};
    static const uint8_t A[7]  = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t B[7]  = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t C[7]  = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t D[7]  = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t E[7]  = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t F[7]  = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t G[7]  = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E};
    static const uint8_t H[7]  = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t I[7]  = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F};
    static const uint8_t J[7]  = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C};
    static const uint8_t K[7]  = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t L[7]  = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t M[7]  = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t N[7]  = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t O[7]  = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t P[7]  = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t Q[7]  = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
    static const uint8_t R[7]  = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t S[7]  = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t T[7]  = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t U[7]  = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t V[7]  = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04};
    static const uint8_t W[7]  = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11};
    static const uint8_t X[7]  = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    static const uint8_t Y[7]  = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
    static const uint8_t Z[7]  = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};
    static const uint8_t n0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t n1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t n2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t n3[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E};
    static const uint8_t n4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t n5[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E};
    static const uint8_t n6[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t n7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t n8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t n9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E};
    static const uint8_t dash[7] = {0,0,0,0x1F,0,0,0};
    static const uint8_t dot[7]  = {0,0,0,0,0,0x0C,0x0C};
    static const uint8_t slash[7]= {0x01,0x01,0x02,0x04,0x08,0x10,0x10};
    static const uint8_t colon[7]= {0,0x0C,0x0C,0,0x0C,0x0C,0};
    static const uint8_t under[7]= {0,0,0,0,0,0,0x1F};
    static const uint8_t bang[7] = {0x04,0x04,0x04,0x04,0x04,0,0x04};
    static const uint8_t pct[7]  = {0x19,0x19,0x02,0x04,0x08,0x13,0x13};

    ch = (char)toupper((unsigned char)ch);
    switch (ch) {
        case ' ': return sp; case '?': return q;
        case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
        case 'E': return E; case 'F': return F; case 'G': return G; case 'H': return H;
        case 'I': return I; case 'J': return J; case 'K': return K; case 'L': return L;
        case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P;
        case 'Q': return Q; case 'R': return R; case 'S': return S; case 'T': return T;
        case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X;
        case 'Y': return Y; case 'Z': return Z;
        case '0': return n0; case '1': return n1; case '2': return n2; case '3': return n3;
        case '4': return n4; case '5': return n5; case '6': return n6; case '7': return n7;
        case '8': return n8; case '9': return n9;
        case '-': return dash; case '.': return dot; case '/': return slash; case ':': return colon;
        case '_': return under; case '!': return bang; case '%': return pct;
        default: return q;
    }
}

static float launcher_text_width(const char *text, float scale) {
    float w = 0.f;
    for (const char *p = text; p && *p; p++) w += 6.f * scale;
    return w > 0.f ? w - scale : 0.f;
}

static void launcher_draw_text(LauncherGfx *gfx, const char *text, float x, float y,
                               float scale, float r, float g, float b, float a) {
    float cx = x;
    for (const char *p = text; p && *p; p++) {
        const uint8_t *glyph = launcher_glyph(*p);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph[row] & (1u << (4 - col))) {
                    launcher_rect(gfx, cx + col * scale, y + row * scale,
                                  scale, scale, r, g, b, a);
                }
            }
        }
        cx += 6.f * scale;
    }
}

static void launcher_draw_centered_text(LauncherGfx *gfx, const char *text, float centerX, float y,
                                        float scale, float r, float g, float b, float a) {
    float width = launcher_text_width(text, scale);
    launcher_draw_text(gfx, text, centerX - width * 0.5f, y, scale, r, g, b, a);
}

static void launcher_gfx_destroy(LauncherGfx *gfx) {
    if (!gfx) return;
    if (gfx->whiteTex.data) C3D_TexDelete(&gfx->whiteTex);
    if (gfx->vbuf) linearFree(gfx->vbuf);
    if (gfx->target) C3D_RenderTargetDelete(gfx->target);
    if (gfx->vshaderDvlb) {
        shaderProgramFree(&gfx->shaderProg);
        DVLB_Free(gfx->vshaderDvlb);
    }
    memset(gfx, 0, sizeof(*gfx));
}

static bool launcher_gfx_init(LauncherGfx *gfx) {
    memset(gfx, 0, sizeof(*gfx));

    gfx->target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (!gfx->target) goto fail;
    C3D_RenderTargetSetOutput(gfx->target, GFX_TOP, GFX_LEFT, LAUNCHER_DISPLAY_TRANSFER_FLAGS);

    gfx->vshaderDvlb = DVLB_ParseFile((u32 *)render2d_shader_shbin, render2d_shader_shbin_size);
    if (!gfx->vshaderDvlb) goto fail;
    shaderProgramInit(&gfx->shaderProg);
    shaderProgramSetVsh(&gfx->shaderProg, &gfx->vshaderDvlb->DVLE[0]);
    gfx->uLoc_projection = shaderInstanceGetUniformLocation(gfx->shaderProg.vertexShader, "projection");

    AttrInfo_Init(&gfx->attrInfo);
    AttrInfo_AddLoader(&gfx->attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&gfx->attrInfo, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&gfx->attrInfo, 2, GPU_FLOAT, 4);

    gfx->vbufCap = LAUNCHER_VBUF_CAP;
    gfx->vbuf = linearAlloc(gfx->vbufCap * sizeof(LauncherVertex));
    if (!gfx->vbuf) goto fail;

    if (!C3D_TexInit(&gfx->whiteTex, 8, 8, GPU_RGBA4)) goto fail;
    uint16_t *white = linearAlloc(8 * 8 * sizeof(uint16_t));
    if (!white) goto fail;
    for (int i = 0; i < 64; i++) white[i] = 0xFFFF;
    C3D_TexLoadImage(&gfx->whiteTex, white, GPU_TEXFACE_2D, 0);
    C3D_TexFlush(&gfx->whiteTex);
    C3D_TexSetFilter(&gfx->whiteTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&gfx->whiteTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    linearFree(white);

    gfx->ready = true;
    return true;

fail:
    launcher_gfx_destroy(gfx);
    return false;
}

static bool launcher_begin_frame(LauncherGfx *gfx) {
    if (!gfx->ready) return false;
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return false;
    gfx->inFrame = true;
    gfx->vbufHead = 0;
    gfx->batchStart = 0;
    gfx->batchVerts = 0;
    gfx->batchTex = NULL;

    C3D_FrameDrawOn(gfx->target);
    C3D_BindProgram(&gfx->shaderProg);
    C3D_SetAttrInfo(&gfx->attrInfo);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, gfx->vbuf, sizeof(LauncherVertex), 3, 0x210);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    C3D_DepthTest(false, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    C3D_RenderTargetClear(gfx->target, C3D_CLEAR_ALL, 0x070914FF, 0);
    C3D_SetViewport(0, 0, 240, 400);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

    C3D_Mtx proj;
    Mtx_Identity(&proj);
    Mtx_OrthoTilt(&proj, 0.f, (float)LAUNCHER_W, (float)LAUNCHER_H, 0.f, -1.f, 1.f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gfx->uLoc_projection, &proj);
    return true;
}

static void launcher_end_frame(LauncherGfx *gfx) {
    launcher_gfx_flush(gfx);
    C3D_FrameEnd(0);
    gfx->inFrame = false;
}

static void launcher_draw_background(LauncherGfx *gfx, float time) {
    float pulse = sinf(time * 0.035f) * 0.04f;
    float top[4] = {0.11f + pulse, 0.08f, 0.20f + pulse, 1.f};
    float bot[4] = {0.015f, 0.025f, 0.055f, 1.f};
    launcher_push_quad_grad(gfx, &gfx->whiteTex,
                            0, 0, LAUNCHER_W, 0, LAUNCHER_W, LAUNCHER_H, 0, LAUNCHER_H,
                            .5f, .5f, .5f, .5f, top, top, bot, bot);

    for (int i = 0; i < 52; i++) {
        float seed = (float)i * 19.73f;
        float x = fmodf(seed * 13.1f + sinf(time * 0.025f + seed) * 18.f, (float)LAUNCHER_W + 40.f) - 20.f;
        float y = fmodf(seed * 7.7f + time * (0.35f + (float)(i & 3) * 0.07f), (float)LAUNCHER_H + 32.f) - 16.f;
        float s = 1.4f + (float)(i % 5) * 0.55f;
        float a = 0.16f + (sinf(time * 0.12f + seed) + 1.f) * 0.09f;
        float hue = (float)(i % 3);
        if (hue < 1.f) launcher_rect(gfx, x, y, s, s, 0.95f, 0.70f, 0.22f, a);
        else if (hue < 2.f) launcher_rect(gfx, x, y, s, s, 0.36f, 0.82f, 1.00f, a);
        else launcher_rect(gfx, x, y, s, s, 0.92f, 0.44f, 0.88f, a);
    }

    launcher_rect(gfx, 0, 0, LAUNCHER_W, 24, 0.045f, 0.040f, 0.090f, 0.88f);
    launcher_rect(gfx, 0, 24, LAUNCHER_W, 1, 1.00f, 0.72f, 0.22f, 0.92f);
    launcher_rect(gfx, 0, 218, LAUNCHER_W, 22, 0.035f, 0.032f, 0.070f, 0.90f);
    launcher_rect(gfx, 0, 218, LAUNCHER_W, 1, 1.00f, 0.72f, 0.22f, 0.92f);
    launcher_draw_text(gfx, "BUTTERSCOTCH", 10, 8, 1.35f, 1.f, 0.78f, 0.28f, 1.f);
}

static void launcher_draw_scrollbar(LauncherGfx *gfx, float cam_y, float max_y) {
    if (max_y <= 0.f) return;
    float trackX = 392.f, trackY = 34.f, trackW = 4.f, trackH = 174.f;
    launcher_rect(gfx, trackX, trackY, trackW, trackH, 0.08f, 0.07f, 0.14f, 0.75f);
    float thumbH = trackH * (trackH / (trackH + max_y));
    if (thumbH < 12.f) thumbH = 12.f;
    float thumbY = trackY + (trackH - thumbH) * (cam_y / max_y);
    launcher_rect(gfx, trackX, thumbY, trackW, thumbH, 1.0f, 0.74f, 0.25f, 1.f);
}

static void launcher_draw_icon_or_placeholder(LauncherGfx *gfx, const GameEntry *game,
                                              float x, float y, float size, int index) {
    if (game->icon_ready) {
        float u1 = (float)game->icon_w / (float)game->icon_pot_w;
        float v1 = (float)game->icon_h / (float)game->icon_pot_h;
        launcher_push_quad(gfx, (C3D_Tex *)&game->icon_tex, x, y, size, size, 0.f, 0.f, u1, v1,
                           (float[4]){1.f, 1.f, 1.f, 1.f});
        return;
    }

    static const float colors[6][3] = {
        {0.34f, 0.64f, 1.00f}, {0.42f, 0.86f, 0.46f}, {0.98f, 0.36f, 0.36f},
        {0.94f, 0.76f, 0.26f}, {0.82f, 0.42f, 0.95f}, {0.36f, 0.88f, 0.86f}
    };
    const float *c = colors[index % 6];
    launcher_rect(gfx, x, y, size, size, c[0] * 0.35f, c[1] * 0.35f, c[2] * 0.35f, 1.f);
    for (int yy = 0; yy < 8; yy++) {
        for (int xx = 0; xx < 8; xx++) {
            if (((xx + yy) & 1) == 0) {
                launcher_rect(gfx, x + xx * size / 8.f, y + yy * size / 8.f,
                              size / 8.f, size / 8.f, c[0], c[1], c[2], 0.22f);
            }
        }
    }
    char initial[2] = {game->name[0] ? game->name[0] : '?', '\0'};
    launcher_draw_centered_text(gfx, initial, x + size * 0.5f, y + size * 0.34f,
                                4.4f, 1.f, 1.f, 1.f, 0.90f);
}

static void launcher_render(LauncherGfx *gfx, int selected, float time, float cam_y, float select_anim) {
    if (!launcher_begin_frame(gfx)) return;

    const float tileSize = 76.f;
    const float gapX = 88.f;
    const float gapY = 92.f;
    const float gridX = 30.f;
    const float gridY = 36.f;
    const int cols = 4;
    const float visibleH = (float)LAUNCHER_H - gridY - 28.f;
    int rows = (g_game_count + cols - 1) / cols;
    float maxY = rows * gapY - visibleH;
    if (maxY < 0.f) maxY = 0.f;

    launcher_draw_background(gfx, time);

    for (int i = 0; i < g_game_count; i++) {
        int col = i % cols;
        int row = i / cols;
        float x = gridX + (float)col * gapX;
        float y = gridY + (float)row * gapY - cam_y;
        if (y < -tileSize || y > (float)LAUNCHER_H) continue;

        bool sel = (i == selected);
        launcher_rect(gfx, x + 4, y + 5, tileSize, tileSize, 0.f, 0.f, 0.f, 0.28f);

        if (sel) {
            float pulse = sinf(time * 0.20f) * 2.2f + 4.6f;
            float grow = (1.f - select_anim) * 7.f;
            launcher_rect(gfx, x - 6 - pulse - grow, y - 6 - pulse - grow,
                          tileSize + 12 + pulse * 2 + grow * 2,
                          tileSize + 12 + pulse * 2 + grow * 2,
                          1.f, 0.72f, 0.20f, 0.26f);
            launcher_rect(gfx, x - 3, y - 3, tileSize + 6, tileSize + 6,
                          1.f, 0.78f, 0.25f, 1.f);
        } else {
            launcher_rect(gfx, x - 2, y - 2, tileSize + 4, tileSize + 4,
                          0.17f, 0.14f, 0.28f, 0.82f);
        }

        launcher_rect(gfx, x, y, tileSize, tileSize, 0.065f, 0.055f, 0.12f, 1.f);
        launcher_draw_icon_or_placeholder(gfx, &g_games[i], x + 6, y + 6, tileSize - 12, i);
    }

    launcher_draw_scrollbar(gfx, cam_y, maxY);

    char title[48];
    snprintf(title, sizeof(title), "%.36s", g_games[selected].name);
    float scale = 2.0f;
    float width = launcher_text_width(title, scale);
    if (width > 360.f) scale *= 360.f / width;
    launcher_draw_centered_text(gfx, title, 200.f, 224.f, scale, 1.f, 0.90f, 0.56f, 1.f);

    launcher_end_frame(gfx);
}

typedef struct {
    LauncherGfx *gfx;
    const char *gameName;
    float basePercent;
    float spanPercent;
} LoadingScreenState;

static void launcher_render_loading(LauncherGfx *gfx, const char *gameName, const char *stage,
                                    int page, int total, float percent) {
    if (!gfx || !gfx->ready || !launcher_begin_frame(gfx)) return;
    if (percent < 0.f) percent = 0.f;
    if (percent > 100.f) percent = 100.f;

    launcher_draw_background(gfx, percent * 0.12f);
    launcher_draw_centered_text(gfx, "BUTTERSCOTCH", 200.f, 48.f, 2.0f, 1.f, 0.80f, 0.34f, 1.f);

    char title[48];
    snprintf(title, sizeof(title), "%.36s", gameName ? gameName : "GAME");
    float titleScale = 1.8f;
    float titleW = launcher_text_width(title, titleScale);
    if (titleW > 350.f) titleScale *= 350.f / titleW;
    launcher_draw_centered_text(gfx, title, 200.f, 82.f, titleScale, 0.72f, 0.88f, 1.f, 0.95f);

    launcher_draw_centered_text(gfx, stage ? stage : "LOADING", 200.f, 112.f, 1.55f, 1.f, 1.f, 1.f, 0.95f);

    float barX = 42.f, barY = 148.f, barW = 316.f, barH = 14.f;
    launcher_rect(gfx, barX - 2, barY - 2, barW + 4, barH + 4, 0.06f, 0.055f, 0.10f, 0.95f);
    launcher_rect(gfx, barX, barY, barW, barH, 0.12f, 0.11f, 0.18f, 1.f);
    launcher_rect(gfx, barX, barY, barW * (percent / 100.f), barH, 1.f, 0.69f, 0.18f, 1.f);
    launcher_rect(gfx, barX, barY, barW, 1.f, 1.f, 0.93f, 0.55f, 0.65f);

    char status[48];
    snprintf(status, sizeof(status), "%d%%", (int)(percent + 0.5f));
    launcher_draw_centered_text(gfx, status, 200.f, 174.f, 1.7f, 1.f, 0.86f, 0.40f, 1.f);

    if (total > 0) {
        char pageText[48];
        snprintf(pageText, sizeof(pageText), "PAGE %d / %d", page, total);
        launcher_draw_centered_text(gfx, pageText, 200.f, 198.f, 1.25f, 0.70f, 0.86f, 1.f, 0.88f);
    }

    launcher_end_frame(gfx);
}

static void launcher_cache_progress(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user) {
    (void)pagePath;
    LoadingScreenState *state = (LoadingScreenState *)user;
    if (!state || !state->gfx) return;
    float cachePct = pageCount ? ((float)pageIndex / (float)pageCount) * 100.f : 100.f;
    float overall = state->basePercent + (cachePct / 100.f) * state->spanPercent;
    int page = pageCount ? (int)pageIndex + 1 : 0;
    if (page > (int)pageCount) page = (int)pageCount;
    launcher_render_loading(state->gfx, state->gameName, "BUILDING TEXTURE CACHE", page, (int)pageCount, overall);
}

static void resolve_new_game_path(const char *request, char *out_path) {
    printf("Try resolve path: %s\n", request);
    if (strncmp(request, "sdmc:/", 6) == 0) {
        strncpy(out_path, request, 255);
        return;
    }
    char base_dir[256];
    char *slash = strrchr(g_current_data_path, '/');
    size_t baselen = slash ? (size_t)(slash - g_current_data_path) : 0;
    if (baselen >= sizeof(base_dir)) baselen = sizeof(base_dir) - 1;
    memcpy(base_dir, g_current_data_path, baselen);
    base_dir[baselen] = '\0';

    char test_path[512];
    snprintf(test_path, sizeof(test_path), "%s/%s/data.win", base_dir, request);
    FILE *f = fopen(test_path, "rb");
    if (f) { fclose(f); strncpy(out_path, test_path, 255); return; }

    snprintf(test_path, sizeof(test_path), "%s/%s/data.win", BASE_DIR, request);
    f = fopen(test_path, "rb");
    if (f) { fclose(f); strncpy(out_path, test_path, 255); return; }

    snprintf(test_path, sizeof(test_path), "%s/%s", BASE_DIR, request);
    strncpy(out_path, test_path, 255);
}

static void scan_games(void) {
    free_game_icons();
    memset(g_games, 0, sizeof(g_games));
    g_game_count = 0;
    DIR *dir = opendir(BASE_DIR);
    if (!dir) { mkdir(BASE_DIR, 0777); return; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && g_game_count < MAX_GAMES) {
        if (ent->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", BASE_DIR, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;

        char win_path[256];
        snprintf(win_path, sizeof(win_path), "%s/data.win", path);
        FILE *f = fopen(win_path, "rb");
        if (!f) continue;
        fclose(f);

        strncpy(g_games[g_game_count].name, ent->d_name, 63);
        g_games[g_game_count].name[63] = '\0';
        strncpy(g_games[g_game_count].path, win_path, 255);
        g_games[g_game_count].path[255] = '\0';
        launcher_try_load_icon(&g_games[g_game_count], path);
        g_game_count++;
    }
    closedir(dir);
}

static int run_launcher(void) {
    scan_games();

    LauncherGfx gfx;
    bool gfx_ready = launcher_gfx_init(&gfx);

    if (g_game_count == 0) {
        consoleClear();
        printf("\x1b[2;1H Butterscotch 3DS \n");
        printf("\x1b[15;5H No games found.\n");
        printf("\x1b[16;3H Place folders with data.win\n");
        printf("\x1b[17;5H into %s\n", BASE_DIR);
        printf("\x1b[28;5H [START] -- quit");
        float time = 0.f;
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                if (gfx_ready) launcher_gfx_destroy(&gfx);
                free_game_icons();
                return -1;
            }
            if (gfx_ready && launcher_begin_frame(&gfx)) {
                launcher_draw_background(&gfx, time);
                launcher_draw_centered_text(&gfx, "NO GAMES FOUND", 200.f, 98.f, 2.25f, 1.f, 0.86f, 0.34f, 1.f);
                launcher_draw_centered_text(&gfx, "SDMC:/3DS/BUTTERSCOTCH", 200.f, 132.f, 1.45f, 0.70f, 0.86f, 1.f, 0.9f);
                launcher_end_frame(&gfx);
            }
            time += 0.16f;
            gspWaitForVBlank();
        }
        if (gfx_ready) launcher_gfx_destroy(&gfx);
        free_game_icons();
        return -1;
    }

    int selected = 0;
    int last_render = -1;
    float time = 0.f;
    float cam_y = 0.f;
    float select_anim = 1.f;

    const float tile_gap_y = 92.f;
    const float grid_y = 36.f;
    const float visible_h = (float)LAUNCHER_H - grid_y - 28.f;
    const int cols = 4;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) {
            if (gfx_ready) launcher_gfx_destroy(&gfx);
            free_game_icons();
            return -1;
        }
        if (kDown & (KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT)) { selected++; select_anim = 0.f; }
        if (kDown & (KEY_LEFT  | KEY_DLEFT  | KEY_CPAD_LEFT))  { selected--; select_anim = 0.f; }
        if (kDown & (KEY_DOWN  | KEY_DDOWN  | KEY_CPAD_DOWN))  { selected += cols; select_anim = 0.f; }
        if (kDown & (KEY_UP    | KEY_DUP    | KEY_CPAD_UP))    { selected -= cols; select_anim = 0.f; }
        if (kDown & KEY_R)                                     { selected += cols * 2; select_anim = 0.f; }
        if (kDown & KEY_L)                                     { selected -= cols * 2; select_anim = 0.f; }
        if (selected < 0)              selected = 0;
        if (selected >= g_game_count)  selected = g_game_count - 1;
        if (kDown & KEY_A) {
            int ret = selected;
            if (gfx_ready) launcher_gfx_destroy(&gfx);
            free_game_icons();
            return ret;
        }

        int sel_row = selected / cols;
        float target_y = (float)sel_row * tile_gap_y - visible_h * 0.40f;
        int max_rows = (g_game_count + cols - 1) / cols;
        float max_y = (float)max_rows * tile_gap_y - visible_h;
        if (max_y < 0.f) max_y = 0.f;
        if (target_y < 0.f) target_y = 0.f;
        if (target_y > max_y) target_y = max_y;
        cam_y += (target_y - cam_y) * 0.22f;
        select_anim += (1.f - select_anim) * 0.18f;

        if (gfx_ready) launcher_render(&gfx, selected, time, cam_y, select_anim);
        time += 0.16f;

        if (selected != last_render) {
            consoleClear();
            printf("\x1b[1;1H\x1b[44;37;1m         Butterscotch Launcher          \x1b[0m\n\n");
            printf("  \x1b[36mGame %d / %d\x1b[0m\n\n", selected + 1, g_game_count);
            printf("  \x1b[36mTitle:\x1b[0m \x1b[32;1m%.36s\x1b[0m\n", g_games[selected].name);
            printf("\n  \x1b[36mPath:\x1b[0m %.40s\n", g_games[selected].path);
            if (g_games[selected].exe_path[0]) {
                printf("\n  \x1b[36mIcon:\x1b[0m %.40s\n", g_games[selected].exe_path);
            }
            printf("\x1b[28;1H\x1b[47;30m  [A] Launch   [D-Pad] Move   [L/R] Page   [START] Quit  \x1b[0m");
            last_render = selected;
        }
        gspWaitForVBlank();
    }
    if (gfx_ready) launcher_gfx_destroy(&gfx);
    free_game_icons();
    return -1;
}

static void map_key(RunnerKeyboardState *kb, u32 down, u32 up, u32 held, u32 mask, int gml) {
    if (down & mask)             RunnerKeyboard_onKeyDown(kb, gml);
    else if ((up & mask) && !(held & mask)) RunnerKeyboard_onKeyUp(kb, gml);
}

static void setup_logging(void) {
    freopen("sdmc:/3ds/butter_out.txt", "w", stdout);
    freopen("sdmc:/3ds/butter_err.txt", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    setup_logging();
    cfguInit();
    gfxInitDefault();
    gfxSet3D(false);

    PrintConsole bottomConsole;
    consoleInit(GFX_BOTTOM, &bottomConsole);

    APT_SetAppCpuTimeLimit(30);

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        printf("C3D_Init failed!\n");
        gfxExit();
        return 1;
    }

    int selected_game = run_launcher();
    if (selected_game < 0) {
        C3D_Fini();
        cfguExit();
        gfxExit();
        return 0;
    }

    strncpy(g_current_data_path, g_games[selected_game].path, 255);
    g_current_data_path[255] = '\0';

    bool keep_playing = true;

    while (keep_playing && aptMainLoop()) {
        g_game_change_requested = false;

        char base_game_dir[256];
        char *slash = strrchr(g_current_data_path, '/');
        size_t baselen = slash ? (size_t)(slash - g_current_data_path) : 0;
        if (baselen >= sizeof(base_game_dir)) baselen = sizeof(base_game_dir) - 1;
        memcpy(base_game_dir, g_current_data_path, baselen);
        base_game_dir[baselen] = '\0';

        snprintf(g_current_cache_dir, sizeof(g_current_cache_dir), "%s/cache", base_game_dir);
        mkdir(g_current_cache_dir, 0777);

        char code_cache_path[256];
        snprintf(code_cache_path, sizeof(code_cache_path), "%s/code.cache", g_current_cache_dir);

        char cache_flag_path[256];
        snprintf(cache_flag_path, sizeof(cache_flag_path), "%s/cache_ready.flag", g_current_cache_dir);

        consoleClear();
        const char *display_name = strrchr(base_game_dir, '/');
        display_name = display_name ? display_name + 1 : base_game_dir;
        printf("\x1b[10;5H\x1b[32mLoading %s...\x1b[0m\n", display_name);

        LauncherGfx loadingGfx;
        bool loadingGfxReady = launcher_gfx_init(&loadingGfx);
        if (loadingGfxReady) {
            launcher_render_loading(&loadingGfx, display_name, "PREPARING", 0, 0, 2.f);
        }

        FILE *flag = fopen(cache_flag_path, "r");
        bool cached = flag != NULL;
        if (flag) fclose(flag);

        if (!cached) {
            printf("\n\nGenerating texture cache...\nThis may take a minute.");
            if (loadingGfxReady) {
                launcher_render_loading(&loadingGfx, display_name, "READING TEXTURE TABLE", 0, 0, 6.f);
            }
            DataWinParserOptions opt = { .parseGen8=1, .parseTpag=1, .parseTxtr=1, .skipTextureBlobData=1 };
            DataWin *dw = DataWin_parse(g_current_data_path, opt);
            if (dw) {
                LoadingScreenState loadingState = {
                    .gfx = loadingGfxReady ? &loadingGfx : NULL,
                    .gameName = display_name,
                    .basePercent = 8.f,
                    .spanPercent = 76.f
                };
                CtrRenderer_setCacheProgressCallback(loadingGfxReady ? launcher_cache_progress : NULL, &loadingState);
                Renderer *r = CtrRenderer_create();
                r->vtable->init(r, dw);
                r->vtable->destroy(r);
                CtrRenderer_setCacheProgressCallback(NULL, NULL);
                DataWin_free(dw);
            }
            flag = fopen(cache_flag_path, "r");
            cached = flag != NULL;
            if (flag) fclose(flag);
        }

        if (loadingGfxReady) {
            launcher_render_loading(&loadingGfx, display_name,
                                    cached ? "CACHE READY" : "CACHE SKIPPED", 0, 0,
                                    cached ? 84.f : 12.f);
        }

        DataWinParserOptions full_opt = {
            .parseGen8=1, .parseOptn=1, .parseLang=1, .parseExtn=1, .parseSond=1,
            .parseAgrp=1, .parseSprt=1, .parseBgnd=1, .parsePath=1, .parseScpt=1,
            .parseGlob=1, .parseShdr=1, .parseFont=1, .parseTmln=1, .parseObjt=1,
            .parseRoom=1, .parseTpag=1, .parseCode=1, .parseVari=1, .parseFunc=1,
            .parseStrg=1, .parseTxtr=1, .parseAudo=1,
            .skipLoadingPreciseMasksForNonPreciseSprites=1,
            .skipTextureBlobData=cached, .skipAudioBlobData=1,
            .codeCachePath=code_cache_path
        };

        if (loadingGfxReady) {
            launcher_render_loading(&loadingGfx, display_name, "LOADING DATA.WIN", 0, 0, 88.f);
        }
        DataWin *dw = DataWin_parse(g_current_data_path, full_opt);
        if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0 || !dw) {
            if (loadingGfxReady) launcher_gfx_destroy(&loadingGfx);
            printf("\nBoot failed.\nCheck data.win at %s\nPress START to quit.\n", g_current_data_path);
            while (aptMainLoop()) {
                hidScanInput();
                if (hidKeysDown() & KEY_START) break;
                gspWaitForVBlank();
            }
            if (dw) DataWin_free(dw);
            break;
        }

        fprintf(stderr, "Loaded \"%s\" (ID: %d)\n", dw->gen8.name, dw->gen8.gameID);

        if (loadingGfxReady) {
            launcher_render_loading(&loadingGfx, display_name, "LAUNCHING GAME!", 0, 0, 100.f);
            launcher_gfx_destroy(&loadingGfx);
        }

        VMContext      *vm  = VM_create(dw);
        N3dsFileSystem *fs  = N3dsFileSystem_create(g_current_data_path);
        Renderer       *ren = CtrRenderer_create();
        AudioSystem    *snd = SdlMixerAudioSystem_create();
        if (snd) snd->dataWin = dw;

        Runner *run = Runner_create(dw, vm, ren, (FileSystem *)fs, snd);
        run->osType = OS_WINDOWS;

        snd->vtable->init(snd, dw, (FileSystem *)fs);
        ren->vtable->init(ren, dw);
        Runner_initFirstRoom(run);

        while (aptMainLoop() && !run->shouldExit) {
            u64 t_start = osGetTime();
            hidScanInput();
            u32 d = hidKeysDown(), u = hidKeysUp(), h = hidKeysHeld();

            if ((h & KEY_START) && (h & KEY_SELECT) && (h & KEY_A)) {
                printf("Ret to launcher\n");
                g_game_change_requested = false;
                run->shouldExit = true;
            }

            RunnerKeyboard_beginFrame(run->keyboard);
            map_key(run->keyboard, d, u, h, KEY_CPAD_UP    | KEY_DUP,    VK_UP);
            map_key(run->keyboard, d, u, h, KEY_CPAD_DOWN  | KEY_DDOWN,  VK_DOWN);
            map_key(run->keyboard, d, u, h, KEY_CPAD_LEFT  | KEY_DLEFT,  VK_LEFT);
            map_key(run->keyboard, d, u, h, KEY_CPAD_RIGHT | KEY_DRIGHT, VK_RIGHT);
            map_key(run->keyboard, d, u, h, KEY_A,          'Z');
            map_key(run->keyboard, d, u, h, KEY_B,          'X');
            map_key(run->keyboard, d, u, h, KEY_X,          'C');
            map_key(run->keyboard, d, u, h, KEY_Y,          VK_SHIFT);
            map_key(run->keyboard, d, u, h, KEY_L,          VK_ENTER);
            map_key(run->keyboard, d, u, h, KEY_R,          VK_SPACE);
            map_key(run->keyboard, d, u, h, KEY_SELECT,     VK_ESCAPE);

            Runner_step(run);
            if (run->audioSystem)
                run->audioSystem->vtable->update(run->audioSystem, 1.f / 30.f);

            Room *rm = run->currentRoom;
            int gw = dw->gen8.defaultWindowWidth;
            int gh = dw->gen8.defaultWindowHeight;
            bool views_en = rm->flags & 1;
            float displayScaleX = 1.f, displayScaleY = 1.f;

            if (views_en) {
                int minL = 0x7fffffff, minT = 0x7fffffff;
                int maxR = -0x7fffffff, maxB = -0x7fffffff;
                for (int i = 0; i < MAX_VIEWS; i++) {
                    if (!run->views[i].enabled) continue;
                    if (run->views[i].portX < minL) minL = run->views[i].portX;
                    if (run->views[i].portY < minT) minT = run->views[i].portY;
                    int r = run->views[i].portX + run->views[i].portWidth;
                    int b = run->views[i].portY + run->views[i].portHeight;
                    if (r > maxR) maxR = r;
                    if (b > maxB) maxB = b;
                }
                if (maxR > minL && maxB > minT) {
                    displayScaleX = (float)gw / (float)(maxR - minL);
                    displayScaleY = (float)gh / (float)(maxB - minT);
                }
            }

            ren->vtable->beginFrame(ren, gw, gh, 400, 240);
            if (run->drawBackgroundColor) {
                ren->vtable->clearTarget(ren, run->backgroundColor, 1.f);
            }

            bool drawn = false;
            if (views_en) {
                for (int i = 0; i < MAX_VIEWS; i++) {
                    RuntimeView *v = &run->views[i];
                    if (!v->enabled) continue;
                    run->viewCurrent = i;
                    int portX = (int)((float)v->portX     * displayScaleX + 0.5f);
                    int portY = (int)((float)v->portY     * displayScaleY + 0.5f);
                    int portW = (int)((float)v->portWidth * displayScaleX + 0.5f);
                    int portH = (int)((float)v->portHeight* displayScaleY + 0.5f);

                    ren->vtable->beginView(ren, v->viewX, v->viewY, v->viewWidth, v->viewHeight,
                                           portX, portY, portW, portH, v->viewAngle);
                    Runner_draw(run);
                    ren->vtable->endView(ren);

                    ren->vtable->beginGUI(ren,
                                          run->guiWidth  > 0 ? run->guiWidth  : portW,
                                          run->guiHeight > 0 ? run->guiHeight : portH,
                                          portX, portY, portW, portH);
                    Runner_drawGUI(run);
                    ren->vtable->endGUI(ren);
                    ren->vtable->flush(ren);
                    drawn = true;
                }
            }

            if (!drawn) {
                run->viewCurrent = 0;
                ren->vtable->beginView(ren, 0, 0, gw, gh, 0, 0, gw, gh, 0.f);
                Runner_draw(run);
                ren->vtable->endView(ren);

                ren->vtable->beginGUI(ren,
                                      run->guiWidth  > 0 ? run->guiWidth  : gw,
                                      run->guiHeight > 0 ? run->guiHeight : gh,
                                      0, 0, gw, gh);
                Runner_drawGUI(run);
                ren->vtable->endGUI(ren);
                ren->vtable->flush(ren);
            }

            run->viewCurrent = 0;
            ren->vtable->endFrame(ren);

            while (osGetTime() - t_start < 33) gspWaitForVBlank();
        }

        run->audioSystem->vtable->destroy(run->audioSystem);
        ren->vtable->destroy(ren);
        Runner_free(run);
        N3dsFileSystem_destroy(fs);
        VM_free(vm);
        DataWin_free(dw);
        SDL_Quit();

        if (g_game_change_requested) {
            resolve_new_game_path(g_next_game_path, g_current_data_path);
        } else {
            int new_selection = run_launcher();
            if (new_selection < 0) {
                keep_playing = false;
            } else {
                strncpy(g_current_data_path, g_games[new_selection].path, 255);
                g_current_data_path[255] = '\0';
            }
        }
    }

    C3D_Fini();
    cfguExit();
    gfxExit();
    return 0;
}
