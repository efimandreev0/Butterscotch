// 3DS entry point for Butterscotch running on NovaGL (OpenGL ES 1.1 -> Citro3D).
// Renders to the top screen (400x240). Bottom screen is cleared.
// data.win is loaded from sdmc:/3ds/butterscotch/data.win.

#include <3ds.h>
#include <malloc.h>
#include <NovaGL.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "ctr_renderer.h"
#include "ctr_file_system.h"
#include "sdl12_audio_system.h"
#include "utils.h"

u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 25 * 1024 * 1024;
u32 __stacksize__ = 64 * 1024;

#define DATA_WIN_PATH "sdmc:/3ds/butterscotch/data.win"
#define BUTTERSCOTCH_NOVA_CMD_BUF_SIZE      (1024 * 1024)
#define BUTTERSCOTCH_NOVA_CLIENT_BUF_SIZE   (8 * 1024 * 1024)
#define BUTTERSCOTCH_NOVA_INDEX_BUF_SIZE    (512 * 1024)
#define BUTTERSCOTCH_NOVA_TEX_STAGING_SIZE  (512 * 1024)

// Умный помощник для ввода
static void processCombinedKey(RunnerKeyboardState* kb, u32 kDown, u32 kUp, u32 kHeld, u32 mask, int32_t gmlKey) {
    if (kDown & mask) {
        RunnerKeyboard_onKeyDown(kb, gmlKey);
    } else if ((kUp & mask) && !(kHeld & mask)) {
        RunnerKeyboard_onKeyUp(kb, gmlKey);
    }
}

void initLogging() {
    freopen("sdmc:/3ds/butter_out.txt", "w", stdout);
    freopen("sdmc:/3ds/butter_err.txt", "w", stderr);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Logging initialized!\n");
    fprintf(stderr, "This goes to stderr!\n");
}
void printMemoryStats() {
    //struct mallinfo mi = mallinfo();

    //u32 linearFree = linearSpaceFree();

    //float heapUsedMB = (float)mi.uordblks / 1024.0f / 1024.0f;
    //float linearFreeMB = (float)linearFree / 1024.0f / 1024.0f;

    //fprintf(stderr, "[MEMORY] Heap Used: %.2f MB | LINEAR RAM FREE: %.2f MB\n",
    //       heapUsedMB, linearFreeMB);
}
int main(int argc, char* argv[]) {
    (void) argc; (void) argv;
    initLogging();
    cfguInit();
    gfxInitDefault();

    APT_SetAppCpuTimeLimit(30);
    osSetSpeedupEnable(true);

    fprintf(stderr, "Butterscotch 3DS booting...\n");
    fprintf(stderr, "Loading %s\n", DATA_WIN_PATH);

    DataWin* dataWin = DataWin_parse(
        DATA_WIN_PATH,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .lazyLoadRooms = true,
            .eagerlyLoadedRooms = nullptr
        }
    );

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        DataWin_free(dataWin);
        return 1;
    }

    if (dataWin == nullptr) {
        consoleInit(GFX_BOTTOM, NULL);
        printf("Butterscotch 3DS: failed to parse data.win\n");
        printf("Expected at: %s\n", DATA_WIN_PATH);
        printf("\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    Gen8* gen8 = &dataWin->gen8;
    fprintf(stderr, "Loaded \"%s\" (%d) [BC%u]\n", gen8->name, gen8->gameID, gen8->bytecodeVersion);

    nova_init();

    nova_texture_cache_set_directory("sdmc:/3ds/butterscotch/cache");

    VMContext* vm = VM_create(dataWin);

    N3dsFileSystem* fs = N3dsFileSystem_create(DATA_WIN_PATH);
    Renderer* renderer = CtrRenderer_create();
    AudioSystem* audio = (AudioSystem*) SdlMixerAudioSystem_create();
    if (audio)
        audio->dataWin = dataWin;

    Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) fs, audio);
    runner->osType = OS_3DS;
    runner->nativeWindow = nullptr;
    runner->setWindowTitle = nullptr;

    Runner_initFirstRoom(runner);

    double targetFrameSec = 1.0 / 30.0;

    int frameCounter = 0;

    while (aptMainLoop() && !runner->shouldExit) {
        u64 frameStart = osGetTime();

        hidScanInput();

        u32 kHeld = hidKeysHeld();
        u32 kDown = hidKeysDown();
        u32 kUp   = hidKeysUp();

        RunnerKeyboard_beginFrame(runner->keyboard);

        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_CPAD_UP | KEY_DUP, VK_UP);
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_CPAD_DOWN | KEY_DDOWN, VK_DOWN);
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_CPAD_LEFT | KEY_DLEFT, VK_LEFT);
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_CPAD_RIGHT | KEY_DRIGHT, VK_RIGHT);

        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_A, 'Z');
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_B, 'X');
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_X, 'C');
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_Y, VK_SHIFT);
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_L, VK_ENTER);
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_R, VK_SPACE);
        processCombinedKey(runner->keyboard, kDown, kUp, kHeld, KEY_SELECT, VK_ESCAPE);

        Runner_step(runner);
        runner->audioSystem->vtable->update(runner->audioSystem, (float) targetFrameSec);

        Room* activeRoom = runner->currentRoom;

        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;

        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        if (viewsEnabled) {
            int32_t maxRight = 0, maxBottom = 0;
            for (int vi = 0; vi < 8; vi++) {
                if (!activeRoom->views[vi].enabled) continue;
                int32_t right = activeRoom->views[vi].portX + activeRoom->views[vi].portWidth;
                int32_t bottom = activeRoom->views[vi].portY + activeRoom->views[vi].portHeight;
                if (right > maxRight) maxRight = right;
                if (bottom > maxBottom) maxBottom = bottom;
            }
            if (maxRight > 0 && maxBottom > 0) {
                gameW = maxRight;
                gameH = maxBottom;
            }
        }

        renderer->vtable->beginFrame(renderer, gameW, gameH, NOVA_SCREEN_W, NOVA_SCREEN_H);

        // ===[ НАЧАЛО 3D ЦИКЛА (1 или 2 прохода) ]===
        int eyes = novaGetEyeCount();

        // ФИКС 3D: Сохраняем состояние клавиатуры!
        // GML скрипты будут вызываться дважды, если включен 3D.
        RunnerKeyboardState kb_backup = *(runner->keyboard);

        for (int eye = 0; eye < eyes; eye++) {
            novaBeginEye(eye);

            // ФИКС 3D: Если это второй проход (правый глаз), прячем импульсные нажатия от игры!
            // Зажатые кнопки (keyDown) оставляем, чтобы анимации бега не мерцали.
            if (eye == 1) {
                memset(runner->keyboard->keyPressed, 0, sizeof(runner->keyboard->keyPressed));
                memset(runner->keyboard->keyReleased, 0, sizeof(runner->keyboard->keyReleased));
            }

            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            if (runner->drawBackgroundColor) {
                int rInt = BGR_R(runner->backgroundColor);
                int gInt = BGR_G(runner->backgroundColor);
                int bInt = BGR_B(runner->backgroundColor);
                glClearColor(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }

            bool anyViewRendered = false;
            if (viewsEnabled) {
                for (int vi = 0; vi < 8; vi++) {
                    RuntimeView* view = &runner->views[vi];

                    if (!view->enabled) continue;

                    int32_t viewX = view->viewX;
                    int32_t viewY = view->viewY;
                    int32_t viewW = view->viewWidth;
                    int32_t viewH = view->viewHeight;
                    int32_t portX = view->portX;
                    int32_t portY = view->portY;
                    int32_t portW = view->portWidth;
                    int32_t portH = view->portHeight;
                    float viewAngle = view->viewAngle;

                    runner->viewCurrent = vi;

                    novaSet3DDepth(0.05f);
                    renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);
                    Runner_draw(runner);
                    renderer->vtable->endView(renderer);

                    novaSet3DDepth(0.0f);
                    int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : portW;
                    int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : portH;
                    renderer->vtable->beginGUI(renderer, guiW, guiH, portX, portY, portW, portH);
                    Runner_drawGUI(runner);
                    renderer->vtable->endGUI(renderer);

                    anyViewRendered = true;
                }
            }

            if (!anyViewRendered) {
                runner->viewCurrent = 0;

                novaSet3DDepth(0.05f);
                renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
                Runner_draw(runner);
                renderer->vtable->endView(renderer);

                novaSet3DDepth(0.0f);
                int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : gameW;
                int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : gameH;
                renderer->vtable->beginGUI(renderer, guiW, guiH, 0, 0, gameW, gameH);
                Runner_drawGUI(runner);
                renderer->vtable->endGUI(renderer);
            }

            runner->viewCurrent = 0;
            renderer->vtable->flush(renderer);
        }

        // ФИКС 3D: Возвращаем состояние клавиш обратно для следующего шага игры
        *(runner->keyboard) = kb_backup;

        // ===[ КОНЕЦ 3D ЦИКЛА ]===

        renderer->vtable->endFrame(renderer);

        novaSwapBuffers();

        if (frameCounter % 60 == 0) {
            printMemoryStats();
        }
        frameCounter++;
        // 30 FPS Limiter
        while (osGetTime() - frameStart < 33) {
            gspWaitForVBlank();
        }
    }

    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    renderer->vtable->destroy(renderer);

    Runner_free(runner);
    N3dsFileSystem_destroy(fs);
    VM_free(vm);
    DataWin_free(dataWin);
    cfguExit();

    nova_fini();
    gfxExit();
    return 0;
}