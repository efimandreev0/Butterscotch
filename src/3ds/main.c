#include <3ds.h>
#include <citro3d.h>

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
//#include "n3ds_audio_system.h"
#include "ctr_audio_system.h"
#include "render2d_shader_shbin.h"
#include "launcher.h"

//u32 __ctru_heap_size        = 35 * 1024 * 1024;
u32 __ctru_linear_heap_size = 48 * 1024 * 1024;
u32 __stacksize__           = 64 * 1024;

#define BASE_DIR  "sdmc:/3ds/butterscotch"
char g_current_data_path[256];
char g_current_cache_dir[256];

DVLB_s          *g_vshaderDvlb = NULL;
shaderProgram_s  g_shaderProg;

char  g_next_game_path[256] = "";
bool  g_game_change_requested = false;

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
    gfxSet3D(true);

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

    printf("[BOOT] Entering launcher_run_menu...\n");
    int selected_game = launcher_run_menu(&gfx);
    printf("[BOOT] launcher_run_menu returned: %d\n", selected_game);

    if (selected_game < 0) {
        printf("[BOOT] Exiting from menu...\n");
        if (gfx_ready) launcher_gfx_destroy(&gfx);
        launcher_free_game_icons();

        shaderProgramFree(&g_shaderProg);
        DVLB_Free(g_vshaderDvlb);

        C3D_Fini();
        cfguExit();
        gfxExit();
        return 0;
    }

    strncpy(g_current_data_path, launcher_game(selected_game)->path, 255);
    g_current_data_path[255] = '\0';

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

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name, "LAUNCHING GAME!", 0, 0, 100.f);
        }

        if (gfx_ready) { launcher_gfx_destroy(&gfx); gfx_ready = false; }

        VMContext      *vm  = VM_create(dw);
        N3dsFileSystem *fs  = N3dsFileSystem_create(g_current_data_path);
        Renderer       *ren = CtrRenderer_create();
        AudioSystem    *snd =(AudioSystem*)CtrAudioSystem_create();
        if (snd) snd->dataWin = dw;

        Runner *run = Runner_create(dw, vm, ren, (FileSystem *)fs, snd);
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
                    action = launcher_run_pause(&pauseGfx);
                    launcher_gfx_destroy(&pauseGfx);
                }
                launcher_apply_settings(launcher_get_settings());
                run->osType = (YoYoOperatingSystem)launcher_get_settings()->os_type;

                if (action == LAUNCHER_PAUSE_QUIT_TO_LAUNCHER) {
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
                                               cp.dx, cp.dy, cs.dx, cs.dy);
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
                default:
                    launcher_apply_3ds_input(run->keyboard, d, u, h);
                    break;
            }
            RunnerMouse_endFrame(run->mouse);

            Runner_step(run);
            if (run->audioSystem)
                run->audioSystem->vtable->update(run->audioSystem, 1.f / 30.f);

            Room *rm = run->currentRoom;
            int gw = dw->gen8.defaultWindowWidth;
            int gh = dw->gen8.defaultWindowHeight;
            bool views_en = rm->flags & 1;

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

            float depthSliderState = osGet3DSliderState();

            int numEyes = (depthSliderState > 0.01f && launcher_get_settings()->game_screen == LAUNCHER_GAME_SCREEN_TOP) ? 2 : 1;

            ren->vtable->beginFrame(ren, gw, gh, winW, 240);

            // Stereoscopic Dual Rendering Engine Main Cycle Injection!
            for(int eye = 0; eye < numEyes; eye++) {

                CtrRenderer_beginEye(ren, eye, depthSliderState);

                if (run->drawBackgroundColor) {
                    ren->vtable->clearTarget(ren, run->backgroundColor, 1.f);
                }

                bool drawn = false;
                if (views_en) {
                    for (int i = 0; i < MAX_VIEWS; i++) {
                        RuntimeView *v = &run->views[i];
                        if (!v->enabled) continue;
                        run->viewCurrent = i;

                        ren->vtable->beginView(ren, v->viewX, v->viewY, v->viewWidth, v->viewHeight,
                                            v->portX, v->portY, v->portWidth, v->portHeight, v->viewAngle);
                        Runner_draw(run);
                        ren->vtable->endView(ren);

                        ren->vtable->beginGUI(ren,
                                            run->guiWidth  > 0 ? run->guiWidth  : v->portWidth,
                                            run->guiHeight > 0 ? run->guiHeight : v->portHeight,
                                            v->portX, v->portY, v->portWidth, v->portHeight);
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
            } // END DUAL/MONO EYE RENDERING FOR CYCLE!

            run->viewCurrent = 0;
            ren->vtable->endFrame(ren);
            if (frameCounter % 600 == 0) printMemoryStats();
            frameCounter++;
            logPerfSample(&perfFrameCount, &perfWindowStart);

            while (osGetTime() - t_start < 33) gspWaitForVBlank();
        }

        run->audioSystem->vtable->destroy(run->audioSystem);
        ren->vtable->destroy(ren);
        Runner_free(run);
        N3dsFileSystem_destroy(fs);
        VM_free(vm);
        DataWin_free(dw);

        if (!gfx_ready) gfx_ready = launcher_gfx_init(&gfx);

        if (g_game_change_requested && !quit_to_launcher) {
            char resolved[256];
            launcher_resolve_new_game_path(g_next_game_path, resolved, sizeof(resolved));
            FILE *probe = fopen(resolved, "rb");
            if (probe) {
                fclose(probe);
                strncpy(g_current_data_path, resolved, sizeof(g_current_data_path) - 1);
                g_current_data_path[sizeof(g_current_data_path) - 1] = '\0';
            } else {
                fprintf(stderr, "[GAME_CHANGE] cannot open '%s' (request '%s'); back to menu\n",
                        resolved, g_next_game_path);
                fprintf(stderr, "[GAME_CHANGE] tip: place a sibling folder containing data.win next to '%s'\n",
                        g_current_data_path);
                g_game_change_requested = false;
                int new_selection = launcher_run_menu(&gfx);
                if (new_selection < 0) {
                    keep_playing = false;
                } else {
                    strncpy(g_current_data_path, launcher_game(new_selection)->path, 255);
                    g_current_data_path[255] = '\0';
                }
            }
        } else {
            int new_selection = launcher_run_menu(&gfx);
            if (new_selection < 0) {
                keep_playing = false;
            } else {
                strncpy(g_current_data_path, launcher_game(new_selection)->path, 255);
                g_current_data_path[255] = '\0';
            }
        }
    }

    if (gfx_ready) launcher_gfx_destroy(&gfx);
    launcher_free_game_icons();

    shaderProgramFree(&g_shaderProg);
    DVLB_Free(g_vshaderDvlb);

    C3D_Fini();
    cfguExit();
    gfxExit();
    return 0;
}