#include <3ds.h>
#include <citro3d.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <malloc.h>

#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "ctr_renderer.h"
#include "ctr_texture_cache.h"
#include "ctr_file_system.h"
#include "sdl12_audio_system.h"
#include "render2d_shader_shbin.h"
#include "launcher.h"

u32 __ctru_heap_size        = 12 * 1024 * 1024; // keep main heap modest to avoid svc allocation failures
u32 __ctru_linear_heap_size = 48 * 1024 * 1024;
u32 __stacksize__           = 64 * 1024;

#define BASE_DIR  "sdmc:/3ds/butterscotch"
#ifndef CTR_OPT_DIAGNOSTICS
#define CTR_OPT_DIAGNOSTICS 0
#endif

char g_current_data_path[256];
char g_current_cache_dir[256];

DVLB_s          *g_vshaderDvlb = NULL;
shaderProgram_s  g_shaderProg;

char  g_next_game_path[256] = "";
char  g_next_game_args[512] = "";
bool  g_game_change_requested = false;
int   g_launch_arg_count = 0;
char  g_launch_args[16][64];

static void clear_launch_args(void) {
    g_launch_arg_count = 0;
    memset(g_launch_args, 0, sizeof(g_launch_args));
}

static void set_launch_args_from_string(const char *args) {
    clear_launch_args();
    if (!args || !args[0]) return;

    const char *p = args;
    while (*p && g_launch_arg_count < 16) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++;

        char token[64];
        size_t len = 0;
        while (*p) {
            if (quote) {
                if (*p == quote) {
                    p++;
                    break;
                }
            } else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
                break;
            }

            if (len + 1 < sizeof(token)) token[len++] = *p;
            p++;
        }
        token[len] = '\0';
        if (len > 0) {
            snprintf(g_launch_args[g_launch_arg_count], sizeof(g_launch_args[g_launch_arg_count]), "%s", token);
            g_launch_arg_count++;
        }
    }
}

static void setup_logging(void) {
    freopen("sdmc:/3ds/butter_out.txt", "w", stdout);
    freopen("sdmc:/3ds/butter_err.txt", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void printMemoryStats(void) {
    struct mallinfo mi = mallinfo();
    u32 linearFree = linearSpaceFree();
    float heapUsedMB   = (float)mi.uordblks / 1024.0f / 1024.0f;
    float linearFreeMB = (float)linearFree  / 1024.0f / 1024.0f;
    printf("[MEMORY] Heap Used: %.2f MB | LINEAR RAM FREE: %.2f MB\n", heapUsedMB, linearFreeMB);
}

// Per-second FPS + memory snapshot, written to sdmc:/3ds/butter_out.txt via the
// stdout redirect set up in setup_logging(). We accumulate frame count between
// stamps instead of computing instantaneous FPS so the number is stable.
static void logPerfSample(int *frames, u64 *windowStart) {
#if CTR_OPT_DIAGNOSTICS
    (*frames)++;
    u64 now = osGetTime();
    u64 elapsed = now - *windowStart;
    if (elapsed >= 1000) {
        float fps = (float)(*frames) * 1000.0f / (float)elapsed;
        struct mallinfo mi = mallinfo();
        u32 linearFree = linearSpaceFree();
        printf("[PERF] FPS=%.1f  Heap=%.2fMB  LinearFree=%.2fMB\n",
               fps,
               (float)mi.uordblks / 1024.0f / 1024.0f,
               (float)linearFree / 1024.0f / 1024.0f);
        *frames = 0;
        *windowStart = now;
    }
#else
    (void)frames;
    (void)windowStart;
#endif
}

typedef struct {
    LauncherGfx *gfx;
    const char  *gameName;
    float        basePercent;
    float        spanPercent;
} LoadingScreenState;

static void cache_progress_cb(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user) {
    (void)pagePath;
    LoadingScreenState *state = (LoadingScreenState *)user;
    if (!state || !state->gfx) return;
    float cachePct = pageCount ? ((float)pageIndex / (float)pageCount) * 100.f : 100.f;
    float overall  = state->basePercent + (cachePct / 100.f) * state->spanPercent;
    int page = pageCount ? (int)pageIndex + 1 : 0;
    if (page > (int)pageCount) page = (int)pageCount;
    launcher_render_loading(state->gfx, state->gameName, "BUILDING TEXTURE CACHE",
                            page, (int)pageCount, overall);
}

static void datawin_progress_cb(const char *chunkName, int chunkIndex, int totalChunks,
                                DataWin *dw, void *user) {
    (void)dw;
    LoadingScreenState *state = (LoadingScreenState *)user;
    if (!state || !state->gfx) return;
    float parsePct = (totalChunks > 0) ? ((float)chunkIndex / (float)totalChunks) * 100.f : 100.f;
    float overall  = state->basePercent + (parsePct / 100.f) * state->spanPercent;
    char stage[40];
    snprintf(stage, sizeof(stage), "PARSING %.4s", chunkName ? chunkName : "DATA");
    launcher_render_loading(state->gfx, state->gameName, stage,
                            chunkIndex + 1, totalChunks, overall);
}

static bool atlas_index_is_current(const char *path) {
    return CtrTextureCache_indexIsCurrentPath(path);
}

static char lower_ascii_char(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool contains_ignore_case(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               lower_ascii_char(p[i]) == lower_ascii_char(needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static bool is_deltarune_root_data_path(const char *path) {
    return contains_ignore_case(path, "deltarune/data.win") &&
           !contains_ignore_case(path, "chapter1_windows") &&
           !contains_ignore_case(path, "chapter2_windows") &&
           !contains_ignore_case(path, "chapter3_windows") &&
           !contains_ignore_case(path, "chapter4_windows");
}

static void apply_initial_launch_args_for_path(const char *path) {
    if (is_deltarune_root_data_path(path)) {
        // The main DELTARUNE launcher skips the "start Chapter 1?" prompt when
        // it sees a returning_* launch parameter, and opens chapter select.
        set_launch_args_from_string("launcher switch_-1 returning_1");
    } else {
        clear_launch_args();
    }
}

static GameProfile detect_game_profile(const char *data_path, const DataWin *dw) {
    const char *game_name = (dw && dw->gen8.name) ? dw->gen8.name : "";

    if (contains_ignore_case(data_path, "deltarune") ||
        contains_ignore_case(data_path, "chapter1_windows") ||
        contains_ignore_case(data_path, "chapter2_windows") ||
        contains_ignore_case(data_path, "chapter3_windows") ||
        contains_ignore_case(data_path, "chapter4_windows") ||
        contains_ignore_case(game_name, "deltarune")) {
        return GAME_PROFILE_DELTARUNE;
    }

    if (contains_ignore_case(data_path, "undertale") ||
        contains_ignore_case(game_name, "undertale")) {
        return GAME_PROFILE_UNDERTALE;
    }

    return GAME_PROFILE_GENERIC;
}

static const char *game_profile_name(GameProfile profile) {
    switch (profile) {
        case GAME_PROFILE_UNDERTALE: return "UNDERTALE";
        case GAME_PROFILE_DELTARUNE: return "DELTARUNE";
        case GAME_PROFILE_GENERIC:
        default: return "GENERIC";
    }
}

static const char *dump_profile_dir_name(GameProfile profile) {
    switch (profile) {
        case GAME_PROFILE_UNDERTALE: return "Undertale";
        case GAME_PROFILE_DELTARUNE: return "Deltarune";
        case GAME_PROFILE_GENERIC:
        default: return "Generic";
    }
}

static void sanitize_dump_name(const char *in, char *out, size_t out_size) {
    if (out_size == 0) return;
    size_t j = 0;
    if (in == NULL || in[0] == '\0') in = "unknown";
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; i++) {
        char c = in[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        out[j++] = ok ? c : '_';
    }
    out[j] = '\0';
}

static bool dump_framebuffer_ppm(const char *path, gfxScreen_t screen) {
    u16 fbW = 0, fbH = 0;
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &fbW, &fbH);
    if (fb == NULL || fbW == 0 || fbH == 0) return false;

    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;

    int outW = (int)fbH;
    int outH = (int)fbW;
    fprintf(f, "P6\n%d %d\n255\n", outW, outH);
    for (int y = 0; y < outH; y++) {
        for (int x = 0; x < outW; x++) {
            size_t offset = ((size_t)x * (size_t)fbW + (size_t)y) * 3u;
            u8 rgb[3] = { fb[offset + 2], fb[offset + 1], fb[offset + 0] };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

static void dump_runner_state_to_sd(Runner *run, GameProfile profile) {
    if (run == NULL) return;

    char root[256];
    char profileDir[256];
    snprintf(root, sizeof(root), "%s/Dumps", BASE_DIR);
    snprintf(profileDir, sizeof(profileDir), "%s/%s", root, dump_profile_dir_name(profile));
    mkdir(BASE_DIR, 0777);
    mkdir(root, 0777);
    mkdir(profileDir, 0777);

    const char *roomName = (run->currentRoom && run->currentRoom->name) ? run->currentRoom->name : "no_room";
    char safeRoom[96];
    sanitize_dump_name(roomName, safeRoom, sizeof(safeRoom));

    u64 now = osGetTime();
    char txtPath[256];
    char jsonPath[256];
    char topPath[256];
    char bottomPath[256];
    snprintf(txtPath, sizeof(txtPath), "%s/%llu_%s_frame_%d.txt",
             profileDir, (unsigned long long)now, safeRoom, run->frameCount);
    snprintf(jsonPath, sizeof(jsonPath), "%s/%llu_%s_frame_%d.json",
             profileDir, (unsigned long long)now, safeRoom, run->frameCount);
    snprintf(topPath, sizeof(topPath), "%s/%llu_%s_frame_%d_top.ppm",
             profileDir, (unsigned long long)now, safeRoom, run->frameCount);
    snprintf(bottomPath, sizeof(bottomPath), "%s/%llu_%s_frame_%d_bottom.ppm",
             profileDir, (unsigned long long)now, safeRoom, run->frameCount);

    FILE *txt = fopen(txtPath, "wb");
    if (txt != NULL) {
        Runner_dumpDiagnostics(run, txt);
        CtrRenderer_dumpTextureDiagnostics(run->renderer, txt, run->currentRoom);
        fclose(txt);
        printf("[DUMP] wrote %s\n", txtPath);
    } else {
        printf("[DUMP] failed to write %s\n", txtPath);
    }

    char *json = Runner_dumpStateJson(run);
    if (json != NULL) {
        FILE *jf = fopen(jsonPath, "wb");
        if (jf != NULL) {
            fputs(json, jf);
            fclose(jf);
            printf("[DUMP] wrote %s\n", jsonPath);
        } else {
            printf("[DUMP] failed to write %s\n", jsonPath);
        }
        free(json);
    }

    if (dump_framebuffer_ppm(topPath, GFX_TOP)) {
        printf("[DUMP] wrote %s\n", topPath);
    } else {
        printf("[DUMP] failed to write %s\n", topPath);
    }
    if (dump_framebuffer_ppm(bottomPath, GFX_BOTTOM)) {
        printf("[DUMP] wrote %s\n", bottomPath);
    } else {
        printf("[DUMP] failed to write %s\n", bottomPath);
    }
}

static bool compute_wide_render_layout(int baseW, int baseH, int screenW, int screenH,
                                       int *outW, int *outExtraW) {
    if (outW) *outW = baseW;
    if (outExtraW) *outExtraW = 0;
    if (baseW <= 0 || baseH <= 0 || screenW <= 0 || screenH <= 0) return false;
    if (launcher_get_settings()->display_mode != LAUNCHER_DISPLAY_3DS_WIDE) return false;
    if (baseW < 600 || baseH < 400) return false;

    int targetW = (baseH * screenW + screenH / 2) / screenH;
    if (targetW <= baseW) return false;

    int extraW = targetW - baseW;
    if (outW) *outW = targetW;
    if (outExtraW) *outExtraW = extraW;
    return true;
}

static int clamp_view_x_to_room(Runner *run, int viewX, int viewW) {
    if (!run || !run->currentRoom || viewW <= 0) return viewX;
    int roomW = (int)run->currentRoom->width;
    if (roomW <= 0) return viewX;
    if (roomW <= viewW) return (roomW - viewW) / 2;
    if (viewX < 0) return 0;
    if (viewX + viewW > roomW) return roomW - viewW;
    return viewX;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    setup_logging();
    printf("\n\n========================================\n");
    printf("[BOOT] setup_logging OK\n");
    printMemoryStats();

    printf("[BOOT] Calling cfguInit...\n");
    cfguInit();

    printf("[BOOT] Calling gfxInitDefault...\n");
    gfxInitDefault();
    gfxSet3D(false);

    printf("[BOOT] Calling APT_SetAppCpuTimeLimit & osSetSpeedupEnable...\n");
    APT_SetAppCpuTimeLimit(30);
    osSetSpeedupEnable(1);

    printf("[BOOT] Calling C3D_Init...\n");
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) {
        printf("[FATAL] C3D_Init failed! OOM in Linear RAM?\n");
        gfxExit();
        return 1;
    }
    printf("[BOOT] C3D_Init OK\n");
    printMemoryStats();

    printf("[BOOT] Parsing DVLB shader...\n");
    g_vshaderDvlb = DVLB_ParseFile((u32 *)render2d_shader_shbin, render2d_shader_shbin_size);
    if (!g_vshaderDvlb) {
        printf("[FATAL] g_vshaderDvlb is NULL! malloc failed?\n");
    }
    printf("[BOOT] DVLB_ParseFile OK\n");

    printf("[BOOT] shaderProgramInit...\n");
    shaderProgramInit(&g_shaderProg);
    shaderProgramSetVsh(&g_shaderProg, &g_vshaderDvlb->DVLE[0]);
    printf("[BOOT] Shader setup OK\n");

    printf("[BOOT] Initializing LauncherGfx...\n");
    LauncherGfx gfx;
    bool gfx_ready = launcher_gfx_init(&gfx);
    printf("[BOOT] launcher_gfx_init OK. gfx_ready = %d\n", gfx_ready);
    printMemoryStats();

    printf("[BOOT] Calling launcher_load_settings...\n");
    launcher_load_settings();
    printf("[BOOT] launcher_load_settings OK\n");

    // Init SDL+mixer once for the whole app. Cycling Mix_OpenAudio/Mix_CloseAudio
    // between game launches deadlocks/crashes on 3DS, so we keep them alive.
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        printf("[BOOT] SDL_Init failed: %s\n", SDL_GetError());
    } else if (!SdlMixer_globalInit()) {
        printf("[BOOT] SdlMixer_globalInit failed (audio disabled)\n");
    }

    printf("[BOOT] Entering launcher_run_menu...\n");
    int selected_game = launcher_run_menu(&gfx);
    printf("[BOOT] launcher_run_menu returned: %d\n", selected_game);

    if (selected_game < 0) {
        printf("[BOOT] Exiting from menu...\n");
        if (gfx_ready) launcher_gfx_destroy(&gfx);
        launcher_free_game_icons();

        shaderProgramFree(&g_shaderProg);
        DVLB_Free(g_vshaderDvlb);

        SdlMixer_globalShutdown();
        SDL_Quit();

        C3D_Fini();
        cfguExit();
        gfxExit();
        return 0;
    }

    strncpy(g_current_data_path, launcher_game(selected_game)->path, 255);
    g_current_data_path[255] = '\0';
    apply_initial_launch_args_for_path(g_current_data_path);

    bool keep_playing = true;

    while (keep_playing && aptMainLoop()) {
        g_game_change_requested = false;
        int frameCounter = 0;
        int perfFrameCount = 0;
        u64 perfWindowStart = osGetTime();
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
        snprintf(cache_flag_path, sizeof(cache_flag_path), "%s/%s", g_current_cache_dir, CTR_TEXTURE_CACHE_READY_FLAG);
        char atlas_index_path[256];
        snprintf(atlas_index_path, sizeof(atlas_index_path), "%s/atlas.bin", g_current_cache_dir);

        const char *display_name = strrchr(base_game_dir, '/');
        display_name = display_name ? display_name + 1 : base_game_dir;
        printf("Loading %s...\n", display_name);

        launcher_load_active_controls(g_current_data_path);
        launcher_free_game_icons();

        if (gfx_ready) launcher_render_loading(&gfx, display_name, "PREPARING", 0, 0, 2.f);

        FILE *flag = fopen(cache_flag_path, "r");
        bool cached = atlas_index_is_current(atlas_index_path);
        if (flag) fclose(flag);

        if (!cached) {
            printf("\nGenerating texture cache...\nThis may take a minute.");
            if (gfx_ready) launcher_render_loading(&gfx, display_name, "READING TEXTURE TABLE", 0, 0, 4.f);

            LoadingScreenState prePassState = {
                .gfx = gfx_ready ? &gfx : NULL,
                .gameName = display_name,
                .basePercent = 4.f,
                .spanPercent = 4.f
            };
            DataWinParserOptions opt = {
                .parseGen8=1, .parseSprt=1, .parseBgnd=1, .parseFont=1,
                .parseTpag=1, .parseTxtr=1, .parseStrg=1,
                .skipLoadingPreciseMasksForNonPreciseSprites=1,
                .skipTextureBlobData=1,
                .progressCallback = gfx_ready ? datawin_progress_cb : NULL,
                .progressCallbackUserData = &prePassState
            };
            DataWin *dw = DataWin_parse(g_current_data_path, opt);
            if (dw) {
                LoadingScreenState cacheState = {
                    .gfx = gfx_ready ? &gfx : NULL,
                    .gameName = display_name,
                    .basePercent = 8.f,
                    .spanPercent = 76.f
                };
                CtrRenderer_setCacheProgressCallback(gfx_ready ? cache_progress_cb : NULL, &cacheState);
                CtrRenderer_prepareTextureCache(dw);
                CtrRenderer_setCacheProgressCallback(NULL, NULL);
                DataWin_free(dw);
                malloc_trim(0);
                CtrRenderer_resetSessionState();
            }
            flag = fopen(cache_flag_path, "r");
            cached = atlas_index_is_current(atlas_index_path);
            if (flag) fclose(flag);
        }

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name,
                                    cached ? "CACHE READY" : "CACHE SKIPPED", 0, 0,
                                    cached ? 84.f : 12.f);
        }

        float fullBase = cached ? 84.f : 12.f;
        LoadingScreenState fullParseState = {
            .gfx = gfx_ready ? &gfx : NULL,
            .gameName = display_name,
            .basePercent = fullBase,
            .spanPercent = 99.f - fullBase
        };
        DataWinParserOptions full_opt = {
            .parseGen8=1, .parseOptn=1, .parseLang=1, .parseExtn=0, .parseSond=1,
            .parseAgrp=1, .parseSprt=1, .parseBgnd=1, .parsePath=1, .parseScpt=1,
            .parseGlob=1, .parseShdr=1, .parseFont=1, .parseTmln=1, .parseObjt=1,
            .parseRoom=1, .parseTpag=1, .parseCode=1, .parseVari=1, .parseFunc=1,
            .parseStrg=1, .parseTxtr=1, .parseAudo=1,
            .skipLoadingPreciseMasksForNonPreciseSprites=1,
            .skipTextureBlobData=cached, .skipAudioBlobData=1,

            .lazyLoadRooms=1,
            .codeCachePath=code_cache_path,
            .progressCallback = gfx_ready ? datawin_progress_cb : NULL,
            .progressCallbackUserData = &fullParseState
        };

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name, "LOADING DATA.WIN", 0, 0, fullBase);
        }
        DataWin *dw = DataWin_parse(g_current_data_path, full_opt);
        if (!dw) {
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
        GameProfile profile = detect_game_profile(g_current_data_path, dw);
        fprintf(stderr, "[COMPAT] game profile: %s\n", game_profile_name(profile));

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name, "LAUNCHING GAME!", 0, 0, 100.f);
        }

        CtrRenderer_resetSessionState();
        if (gfx_ready) { launcher_gfx_destroy(&gfx); gfx_ready = false; }

        VMContext      *vm  = VM_create(dw);
        N3dsFileSystem *fs  = N3dsFileSystem_create(g_current_data_path);
        Renderer       *ren = CtrRenderer_create();
        AudioSystem    *snd = (AudioSystem*)SdlMixerAudioSystem_create();//AlAudioSystem_create();//SdlMixerAudioSystem_create();
        if (snd) snd->dataWin = dw;

        Runner *run = Runner_create(dw, vm, ren, (FileSystem *)fs, snd, profile);
        run->osType = (YoYoOperatingSystem)launcher_get_settings()->os_type;

        Runner_initFirstRoom(run);

        launcher_apply_settings(launcher_get_settings());

        bool quit_to_launcher = false;

        while (aptMainLoop() && !run->shouldExit) {
            u64 t_start = osGetTime();
            hidScanInput();
            u32 d = hidKeysDown(), u = hidKeysUp(), h = hidKeysHeld();

            if ((h & KEY_L) && (h & KEY_R) && (h & KEY_A)) {
                LauncherGfx pauseGfx;
                bool pauseReady = launcher_gfx_init_borrowed(
                    &pauseGfx,
                    CtrRenderer_getTopTarget(ren),    LAUNCHER_TOP_W, LAUNCHER_TOP_H,
                    CtrRenderer_getBottomTarget(ren), LAUNCHER_BOT_W, LAUNCHER_BOT_H);

                LauncherPauseAction action = LAUNCHER_PAUSE_RESUME;
                if (pauseReady) {
                    action = launcher_run_pause(&pauseGfx,
                                               profile == GAME_PROFILE_DELTARUNE);
                    launcher_gfx_destroy(&pauseGfx);
                }
                // Apply any layout/theme tweaks the user made.
                launcher_apply_settings(launcher_get_settings());
                run->osType = (YoYoOperatingSystem)launcher_get_settings()->os_type;
                RunnerKeyboard_clear(run->keyboard, VK_ANYKEY);
                RunnerGamepad_clear(run->gamepads);
                RunnerMouse_beginFrame(run->mouse);

                if (action == LAUNCHER_PAUSE_QUIT_TO_LAUNCHER) {
                    quit_to_launcher = true;
                    g_game_change_requested = false;
                    run->shouldExit = true;
                    break;
                }
                continue; // resume — don't process input from the chord this frame
            }

            if (profile == GAME_PROFILE_DELTARUNE &&
                launcher_get_settings()->debug_mode && (d & KEY_SELECT)) {
                dump_runner_state_to_sd(run, profile);
                RunnerKeyboard_clear(run->keyboard, VK_ANYKEY);
                RunnerGamepad_clear(run->gamepads);
                RunnerMouse_beginFrame(run->mouse);
                continue;
            }

            if (profile == GAME_PROFILE_DELTARUNE &&
                launcher_get_settings()->debug_mode && (d & KEY_START)) {
                LauncherGfx confirmGfx;
                bool confirmReady = launcher_gfx_init_borrowed(
                    &confirmGfx,
                    CtrRenderer_getTopTarget(ren),    LAUNCHER_TOP_W, LAUNCHER_TOP_H,
                    CtrRenderer_getBottomTarget(ren), LAUNCHER_BOT_W, LAUNCHER_BOT_H);

                bool confirmed = false;
                if (confirmReady) {
                    confirmed = launcher_confirm_quit_to_launcher(&confirmGfx);
                    launcher_gfx_destroy(&confirmGfx);
                }

                launcher_apply_settings(launcher_get_settings());
                run->osType = (YoYoOperatingSystem)launcher_get_settings()->os_type;
                RunnerKeyboard_clear(run->keyboard, VK_ANYKEY);
                RunnerGamepad_clear(run->gamepads);
                RunnerMouse_beginFrame(run->mouse);

                if (confirmed) {
                    quit_to_launcher = true;
                    g_game_change_requested = false;
                    run->shouldExit = true;
                    break;
                }
                continue;
            }

            RunnerKeyboard_beginFrame(run->keyboard);
            RunnerGamepad_beginFrame(run->gamepads);
            RunnerMouse_beginFrame(run->mouse);

            LauncherInputMode mode = launcher_get_settings()->input_mode;
            switch (mode) {
                case LAUNCHER_INPUT_GAMEPAD: {
                    circlePosition cp; hidCircleRead(&cp);
                    circlePosition cs; hidCstickRead(&cs);
                    launcher_apply_3ds_gamepad(run->gamepads, d, u, h,
                                               cp.dx, cp.dy,
                                               profile == GAME_PROFILE_DELTARUNE ? cs.dx : 0,
                                               profile == GAME_PROFILE_DELTARUNE ? cs.dy : 0);
                    break;
                }
                case LAUNCHER_INPUT_TOUCH: {
                    touchPosition tp; hidTouchRead(&tp);
                    bool touched = (h & KEY_TOUCH) != 0;
                    launcher_apply_3ds_touch(run->mouse, touched, tp.px, tp.py,
                                             dw->gen8.defaultWindowWidth,
                                             dw->gen8.defaultWindowHeight);
                    break;
                }
                case LAUNCHER_INPUT_KEYBOARD:
                default: {
                    circlePosition cs; hidCstickRead(&cs);
                    launcher_apply_3ds_input(run->keyboard, d, u, h,
                                             profile == GAME_PROFILE_DELTARUNE ? cs.dx : 0,
                                             profile == GAME_PROFILE_DELTARUNE ? cs.dy : 0);
                    break;
                }
            }
            RunnerMouse_endFrame(run->mouse);

            Runner_step(run);
            if (run->audioSystem)
                run->audioSystem->vtable->update(run->audioSystem, 1.f / 30.f);

            int gw = dw->gen8.defaultWindowWidth;
            int gh = dw->gen8.defaultWindowHeight;
            bool views_en = run->viewsEnabled;

            if (views_en) {
                int maxR = 0, maxB = 0;
                for (int i = 0; i < MAX_VIEWS; i++) {
                    if (!run->views[i].enabled) continue;
                    int r = run->views[i].portX + run->views[i].portWidth;
                    int b = run->views[i].portY + run->views[i].portHeight;
                    if (r > maxR) maxR = r;
                    if (b > maxB) maxB = b;
                }
                if (maxR > 0 && maxB > 0) { gw = maxR; gh = maxB; }
            }

            int winW = (launcher_get_settings()->game_screen == LAUNCHER_GAME_SCREEN_BOTTOM) ? 320 : 400;
            int renderW = gw;
            int renderH = gh;
            int wideExtraW = 0;
            bool wideMode = compute_wide_render_layout(gw, gh, winW, 240, &renderW, &wideExtraW);
            ren->vtable->beginFrame(ren, renderW, renderH, winW, 240);
            if (run->drawBackgroundColor) {
                ren->vtable->clearTarget(ren, run->backgroundColor, 1.f);
            }

            bool drawn = false;
            if (views_en) {
                for (int i = 0; i < MAX_VIEWS; i++) {
                    RuntimeView *v = &run->views[i];
                    if (!v->enabled) continue;
                    run->viewCurrent = i;
                    GMLCamera *camera = Runner_getCameraForView(run, i);
                    int viewX = camera ? camera->viewX : v->viewX;
                    int viewY = camera ? camera->viewY : v->viewY;
                    int viewW = camera ? camera->viewWidth : v->viewWidth;
                    int viewH = camera ? camera->viewHeight : v->viewHeight;
                    float angle = camera ? camera->viewAngle : v->viewAngle;

                    int portX = v->portX;
                    int portY = v->portY;
                    int portW = v->portWidth;
                    int portH = v->portHeight;
                    bool fullScreenPort = portX == 0 && portY == 0 &&
                                          portW == gw && portH == gh;
                    bool widenThisView = wideMode && wideExtraW > 0 && fullScreenPort && angle == 0.f;
                    if (widenThisView) {
                        int viewExtraW = (portW > 0)
                                             ? (wideExtraW * viewW + portW / 2) / portW
                                             : wideExtraW;
                        viewW += viewExtraW;
                        viewX = clamp_view_x_to_room(run, viewX - viewExtraW / 2, viewW);
                        portW = renderW;
                    }

                    ren->vtable->beginView(ren, viewX, viewY, viewW, viewH,
                                           portX, portY, portW, portH, angle);
                    Runner_draw(run);
                    ren->vtable->endView(ren);

                    int guiPortX = portX;
                    int guiPortW = portW;
                    if (widenThisView) {
                        guiPortX = wideExtraW / 2;
                        guiPortW = v->portWidth;
                    }

                    ren->vtable->beginGUI(ren,
                                          run->guiWidth  > 0 ? run->guiWidth  : v->portWidth,
                                          run->guiHeight > 0 ? run->guiHeight : v->portHeight,
                                          guiPortX, v->portY, guiPortW, v->portHeight);
                    Runner_drawGUI(run);
                    ren->vtable->endGUI(ren);
                    ren->vtable->flush(ren);
                    drawn = true;
                }
            }

            if (!drawn) {
                run->viewCurrent = 0;
                int viewX = 0;
                int viewW = gw;
                if (wideMode && wideExtraW > 0) {
                    viewW = renderW;
                    viewX = clamp_view_x_to_room(run, -wideExtraW / 2, viewW);
                }
                ren->vtable->beginView(ren, viewX, 0, viewW, gh, 0, 0, renderW, renderH, 0.f);
                Runner_draw(run);
                ren->vtable->endView(ren);

                int guiPortX = wideMode ? wideExtraW / 2 : 0;
                int guiPortW = wideMode ? gw : renderW;
                ren->vtable->beginGUI(ren,
                                      run->guiWidth  > 0 ? run->guiWidth  : gw,
                                      run->guiHeight > 0 ? run->guiHeight : gh,
                                      guiPortX, 0, guiPortW, gh);
                Runner_drawGUI(run);
                ren->vtable->endGUI(ren);
                ren->vtable->flush(ren);
            }

            run->viewCurrent = 0;
            ren->vtable->endFrame(ren);
            if (frameCounter % 600 == 0) printMemoryStats();
            frameCounter++;
            logPerfSample(&perfFrameCount, &perfWindowStart);

            while (osGetTime() - t_start < 33) gspWaitForVBlank();
        }

        CtrRenderer_resetSessionState();
        run->audioSystem->vtable->destroy(run->audioSystem);
        ren->vtable->destroy(ren);
        Runner_free(run);
        N3dsFileSystem_destroy(fs);
        VM_free(vm);
        DataWin_free(dw);
        malloc_trim(0);
        CtrRenderer_resetSessionState();
        // SDL/Mix stays alive — see SdlMixer_globalInit().

        if (!gfx_ready) gfx_ready = launcher_gfx_init(&gfx);

        if (g_game_change_requested && !quit_to_launcher) {
            char resolved[256];
            launcher_resolve_new_game_path(g_next_game_path, resolved, sizeof(resolved));
            // Bail out to the menu if the requested path is bogus — better than
            // running DataWin_parse on a missing file and aborting.
            FILE *probe = fopen(resolved, "rb");
            if (probe) {
                fclose(probe);
                strncpy(g_current_data_path, resolved, sizeof(g_current_data_path) - 1);
                g_current_data_path[sizeof(g_current_data_path) - 1] = '\0';
                set_launch_args_from_string(g_next_game_args);
            } else {
                fprintf(stderr, "[GAME_CHANGE] cannot open '%s' (request '%s'); back to menu\n",
                        resolved, g_next_game_path);
                fprintf(stderr, "[GAME_CHANGE] tip: place a sibling folder containing data.win next to '%s'\n",
                        g_current_data_path);
                g_game_change_requested = false;
                clear_launch_args();
                int new_selection = launcher_run_menu(&gfx);
                if (new_selection < 0) {
                    keep_playing = false;
                } else {
                    strncpy(g_current_data_path, launcher_game(new_selection)->path, 255);
                    g_current_data_path[255] = '\0';
                    apply_initial_launch_args_for_path(g_current_data_path);
                }
            }
        } else {
            int new_selection = launcher_run_menu(&gfx);
            if (new_selection < 0) {
                keep_playing = false;
            } else {
                strncpy(g_current_data_path, launcher_game(new_selection)->path, 255);
                g_current_data_path[255] = '\0';
                apply_initial_launch_args_for_path(g_current_data_path);
            }
        }
    }

    if (gfx_ready) launcher_gfx_destroy(&gfx);
    launcher_free_game_icons();

    shaderProgramFree(&g_shaderProg);
    DVLB_Free(g_vshaderDvlb);

    SdlMixer_globalShutdown();
    SDL_Quit();

    C3D_Fini();
    cfguExit();
    gfxExit();
    return 0;
}
