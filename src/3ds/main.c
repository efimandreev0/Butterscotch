#include <3ds.h>
#include <malloc.h>
#include <NovaGL.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "ctr_renderer.h"
#include "ctr_file_system.h"
#include "sdl12_audio_system.h"

//ctru ram setup
u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 25 * 1024 * 1024;
u32 __stacksize__ = 64 * 1024;

#define DATA_PATH "sdmc:/3ds/butterscotch/data.win"
#define CACHE_DIR "sdmc:/3ds/butterscotch/cache"
#define CODE_CACHE "sdmc:/3ds/butterscotch/cache/code.cache"

static void map_key(RunnerKeyboardState *kb, u32 down, u32 up, u32 held, u32 mask, int gml) {
    if (down & mask) RunnerKeyboard_onKeyDown(kb, gml);
    else if ((up & mask) && !(held & mask)) RunnerKeyboard_onKeyUp(kb, gml);
}

static void setup_logging() {
    freopen("sdmc:/3ds/butter_out.txt", "w", stdout);
    freopen("sdmc:/3ds/butter_err.txt", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

int main(int argc, char **argv) {
    setup_logging();
    cfguInit();
    gfxInitDefault();
    gfxSet3D(true);

    APT_SetAppCpuTimeLimit(30);
    //osSetSpeedupEnable(true); //uncoment for n3ds clock

    nova_init_ex(512*1024, 1024*1024, 256*1024, 256*1024);
    mkdir(CACHE_DIR, 0777);
    nova_texture_cache_set_directory(CACHE_DIR);

    FILE *flag = fopen(CACHE_DIR "/cache_ready.flag", "r");
    bool cached = flag != NULL;
    if (flag) fclose(flag);

    if (!cached) {
        fprintf(stderr, "=== STAGE 1: TEX PRECACHE ===\n");
        DataWinParserOptions opt = { .parseGen8=1, .parseTpag=1, .parseTxtr=1, .skipTextureBlobData=1 };
        DataWin *dw = DataWin_parse(DATA_PATH, opt);
        if (dw) {
            Renderer *r = CtrRenderer_create();
            r->vtable->init(r, dw);
            r->vtable->destroy(r);
            DataWin_free(dw);
        }

        flag = fopen(CACHE_DIR "/cache_ready.flag", "r");
        cached = flag != NULL;
        if (flag) fclose(flag);
    }

    fprintf(stderr, "=== STAGE 2: FULL BOOT ===\n");
    DataWinParserOptions full_opt = {
        .parseGen8=1, .parseOptn=1, .parseLang=1, .parseExtn=1, .parseSond=1,
        .parseAgrp=1, .parseSprt=1, .parseBgnd=1, .parsePath=1, .parseScpt=1,
        .parseGlob=1, .parseShdr=1, .parseFont=1, .parseTmln=1, .parseObjt=1,
        .parseRoom=1, .parseTpag=1, .parseCode=1, .parseVari=1, .parseFunc=1,
        .parseStrg=1, .parseTxtr=1, .parseAudo=1,
        .skipLoadingPreciseMasksForNonPreciseSprites=1,
        .skipTextureBlobData=cached, .skipAudioBlobData=1,
        .codeCachePath=CODE_CACHE
    };

    DataWin *dw = DataWin_parse(DATA_PATH, full_opt);

    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0 || !dw) {
        consoleInit(GFX_BOTTOM, NULL);
        printf("Boot failed.\nCheck data.win at %s\nPress START to quit.\n", DATA_PATH);
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        gfxExit();
        if (dw) DataWin_free(dw);
        return 1;
    }

    fprintf(stderr, "Loaded \"%s\" (ID: %d) [BC%u]\n", dw->gen8.name, dw->gen8.gameID, dw->gen8.bytecodeVersion);

    VMContext *vm = VM_create(dw);
    N3dsFileSystem *fs = N3dsFileSystem_create(DATA_PATH);
    Renderer *ren = CtrRenderer_create();
    AudioSystem *snd = SdlMixerAudioSystem_create();
    if (snd) snd->dataWin = dw;

    Runner *run = Runner_create(dw, vm, ren, (FileSystem*)fs, snd);
    run->osType = OS_3DS;

    snd->vtable->init(snd, dw, (FileSystem*)fs);
    ren->vtable->init(ren, dw);
    Runner_initFirstRoom(run);

    u32 frames = 0;

    while (aptMainLoop() && !run->shouldExit) {
        u64 t_start = osGetTime();
        hidScanInput();
        u32 d = hidKeysDown(), u = hidKeysUp(), h = hidKeysHeld();

        RunnerKeyboard_beginFrame(run->keyboard);
        map_key(run->keyboard, d, u, h, KEY_CPAD_UP | KEY_DUP, VK_UP);
        map_key(run->keyboard, d, u, h, KEY_CPAD_DOWN | KEY_DDOWN, VK_DOWN);
        map_key(run->keyboard, d, u, h, KEY_CPAD_LEFT | KEY_DLEFT, VK_LEFT);
        map_key(run->keyboard, d, u, h, KEY_CPAD_RIGHT | KEY_DRIGHT, VK_RIGHT);
        map_key(run->keyboard, d, u, h, KEY_A, 'Z');
        map_key(run->keyboard, d, u, h, KEY_B, 'X');
        map_key(run->keyboard, d, u, h, KEY_X, 'C');
        map_key(run->keyboard, d, u, h, KEY_Y, VK_SHIFT);
        map_key(run->keyboard, d, u, h, KEY_L, VK_ENTER);
        map_key(run->keyboard, d, u, h, KEY_R, VK_SPACE);
        map_key(run->keyboard, d, u, h, KEY_SELECT, VK_ESCAPE);

        Runner_step(run);
        if (run->audioSystem) run->audioSystem->vtable->update(run->audioSystem, 1.f/30.f);

        Room *rm = run->currentRoom;
        int gw = dw->gen8.defaultWindowWidth, gh = dw->gen8.defaultWindowHeight;
        bool views_en = rm->flags & 1;

        if (views_en) {
            int mr = 0, mb = 0;
            for (int i = 0; i < MAX_VIEWS; i++) {
                if (!run->views[i].enabled) continue;
                int r = run->views[i].portX + run->views[i].portWidth;
                int b = run->views[i].portY + run->views[i].portHeight;
                if (r > mr) mr = r;
                if (b > mb) mb = b;
            }
            if (mr > 0 && mb > 0) { gw = mr; gh = mb; }
        }

        ren->vtable->beginFrame(ren, gw, gh, NOVA_SCREEN_W, NOVA_SCREEN_H);
        int eyes = novaGetEyeCount();

        //backups for 3d slider rendering
        RunnerKeyboardState kb_bak = *run->keyboard;

        for (int eye = 0; eye < eyes; eye++) {
            novaBeginEye(eye);
            if (eye == 1) {
                memset(run->keyboard->keyPressed, 0, sizeof(run->keyboard->keyPressed));
                memset(run->keyboard->keyReleased, 0, sizeof(run->keyboard->keyReleased));
            }

            if (run->drawBackgroundColor) {
                glClearColor(BGR_R(run->backgroundColor)/255.f, BGR_G(run->backgroundColor)/255.f, BGR_B(run->backgroundColor)/255.f, 1.f);
            } else {
                glClearColor(0.f, 0.f, 0.f, 1.f);
            }
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            bool drawn = false;
            if (views_en) {
                for (int i = 0; i < MAX_VIEWS; i++) {
                    RuntimeView *v = &run->views[i];
                    if (!v->enabled) continue;

                    run->viewCurrent = i;
                    novaSet3DDepth(0.05f);
                    ren->vtable->beginView(ren, v->viewX, v->viewY, v->viewWidth, v->viewHeight, v->portX, v->portY, v->portWidth, v->portHeight, v->viewAngle);
                    Runner_draw(run);
                    ren->vtable->endView(ren);

                    ren->vtable->beginGUI(ren, run->guiWidth > 0 ? run->guiWidth : v->portWidth, run->guiHeight > 0 ? run->guiHeight : v->portHeight, v->portX, v->portY, v->portWidth, v->portHeight);
                    Runner_drawGUI(run);
                    ren->vtable->endGUI(ren);
                    ren->vtable->flush(ren);
                    drawn = true;
                }
            }

            if (!drawn) {
                run->viewCurrent = 0;
                novaSet3DDepth(0.05f);
                ren->vtable->beginView(ren, 0, 0, gw, gh, 0, 0, gw, gh, 0.f);
                Runner_draw(run);
                ren->vtable->endView(ren);

                ren->vtable->beginGUI(ren, run->guiWidth > 0 ? run->guiWidth : gw, run->guiHeight > 0 ? run->guiHeight : gh, 0, 0, gw, gh);
                Runner_drawGUI(run);
                ren->vtable->endGUI(ren);
                ren->vtable->flush(ren);
            }

            run->viewCurrent = 0;
            ren->vtable->flush(ren);
        }

        *run->keyboard = kb_bak;
        ren->vtable->endFrame(ren);
        novaSwapBuffers();

        u64 ftime = osGetTime() - t_start;
        //statter warning nahui
        if (ftime > 200) fprintf(stderr, "lag spike: %llu ms\n", ftime);

        if (frames++ % 300 == 0) {
            struct mallinfo mi = mallinfo();
            fprintf(stderr, "MEM | Heap: %.2f MB | Linear: %.2f MB\n", mi.uordblks / 1048576.f, linearSpaceFree() / 1048576.f);
        }

        //limit to ~30fps
        while (osGetTime() - t_start < 33) gspWaitForVBlank();
    }

    run->audioSystem->vtable->destroy(run->audioSystem);
    ren->vtable->destroy(ren);
    Runner_free(run);
    N3dsFileSystem_destroy(fs);
    VM_free(vm);
    DataWin_free(dw);

    cfguExit();
    nova_fini();
    gfxExit();
    return 0;
}