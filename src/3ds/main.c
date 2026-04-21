// 3DS entry point for Butterscotch running on NovaGL (OpenGL ES 1.1 -> Citro3D).
// Renders to the top screen (400x240). Bottom screen is cleared.
// data.win is loaded from sdmc:/3ds/butterscotch/data.win.

#include <3ds.h>
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
u32 __ctru_linear_heap_size = 35 * 1024 * 1024; // anything lower crashes on launch
u32 __stacksize__ = 64 * 1024;

#define DATA_WIN_PATH "sdmc:/3ds/butterscotch/data.win"
#define BUTTERSCOTCH_NOVA_CMD_BUF_SIZE      (1024 * 1024)
#define BUTTERSCOTCH_NOVA_CLIENT_BUF_SIZE   (8 * 1024 * 1024)
#define BUTTERSCOTCH_NOVA_INDEX_BUF_SIZE    (512 * 1024)
#define BUTTERSCOTCH_NOVA_TEX_STAGING_SIZE  (512 * 1024)

// Override devkitARM/ctrulib defaults so we have enough room to decode large TXTR pages.
// stb_image malloc's the full decoded RGBA buffer on the application heap, and GameMaker
// atlases can be 1024x2048 (8 MB each). The default heap fragments quickly once VM
// allocations come into play, causing late-loaded TXTR pages to fail to decode.
// The linear heap must also be large because every uploaded texture lives in linear
// memory for PICA200 DMA.
// These symbols are weakly defined by ctrulib; overriding them lets us size the heaps
// up front. Values picked to fit O3DS extended-memory mode (~96 MB user app RAM).
// Heap: stb_image decode of 1024x2048 RGBA = 8 MB transient. VM + DataWin take ~8-12 MB.
// Linear heap: each TXTR page uploaded via glTexImage2D lives here for the whole session,
// averaging ~2-4 MB each, plus NovaGL command/vertex buffers (~9 MB).
//u32 __ctru_heap_size = 32 * 1024 * 1024;         // 32 MB application heap
//u32 __ctru_linear_heap_size = 48 * 1024 * 1024;  // 48 MB linear heap for GPU textures

// 3DS HID button -> GML virtual key.
static void pushKey(RunnerKeyboardState* kb, int32_t gmlKey, bool pressed) {
    if (pressed) RunnerKeyboard_onKeyDown(kb, gmlKey);
    else RunnerKeyboard_onKeyUp(kb, gmlKey);
}

static void updateKey(RunnerKeyboardState* kb, u32 kDown, u32 kUp, u32 btn, int32_t gmlKey) {
    if (kDown & btn) pushKey(kb, gmlKey, true);
    if (kUp & btn) pushKey(kb, gmlKey, false);
}
void initLogging() {
    freopen("sdmc:/3ds/butter_out.txt", "w", stdout);
    freopen("sdmc:/3ds/butter_err.txt", "w", stderr);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Logging initialized!\n");
    fprintf(stderr, "This goes to stderr!\n");
}
int main(int argc, char* argv[]) {
    (void) argc; (void) argv;
    initLogging();
    gfxInitDefault();
    // Note: we do NOT consoleInit(GFX_BOTTOM) — NovaGL owns both screens' framebuffers.
    // Diagnostic output goes to stderr (visible in Citra/debuggers but not on hardware).

    // Ask APT for a larger CPU slice (default is 30% for homebrew — bump to 30 max the OS
    // will grant without audio-priority churn) and enable N3DS 804 MHz + L2 cache.
    // On O3DS osSetSpeedupEnable is a no-op. Free ~2x CPU headroom on N3DS for the VM.
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
        // Only bring up the bottom-screen console here so the user can see the error
        // on real hardware. We haven't initialized NovaGL yet, so the bottom screen
        // framebuffer is still ours to use.
        consoleInit(GFX_BOTTOM, NULL);
        printf("Butterscotch 3DS: failed to parse data.win\n");
        printf("Expected at: %s\n", DATA_WIN_PATH);
        printf("\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            //if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    Gen8* gen8 = &dataWin->gen8;
    fprintf(stderr, "Loaded \"%s\" (%d) [BC%u]\n", gen8->name, gen8->gameID, gen8->bytecodeVersion);

    // Console must release the bottom screen before we can init NovaGL which uses it.
    // Current NovaGL sources implement nova_init() as nova_init_ex() with a 512 KB command
    // buffer. NovaGL clamps the client/index buffers to 8 MB / 512 KB respectively, and the
    // current tex-staging argument is ignored internally, so the only knob that materially
    // helps our sprite-heavy scenes is a slightly larger command buffer.
    //nova_init_ex(
    //    BUTTERSCOTCH_NOVA_CMD_BUF_SIZE,
    //    BUTTERSCOTCH_NOVA_CLIENT_BUF_SIZE,
    //    BUTTERSCOTCH_NOVA_INDEX_BUF_SIZE,
    //    BUTTERSCOTCH_NOVA_TEX_STAGING_SIZE
    //);
    nova_init();

    VMContext* vm = VM_create(dataWin);

    CtrFileSystem* fs = CtrFileSystem_create(DATA_WIN_PATH);
    Renderer* renderer = CtrRenderer_create();
    AudioSystem* audio = (AudioSystem*) SdlMixerAudioSystem_create();
    if (audio)
        audio->dataWin = dataWin;

    Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) fs, audio);
    runner->osType = OS_3DS;
    runner->nativeWindow = nullptr;
    runner->setWindowTitle = nullptr;

    Runner_initFirstRoom(runner);

    double targetFrameSec = 1.0 / 30.0; // 3DS defaults to 30 Hz for homebrew

    static u32 prevKeysHeld = 0;
    while (aptMainLoop() && !runner->shouldExit) {
        hidScanInput();

        u32 kHeld = hidKeysHeld();

        u32 kDown = (~prevKeysHeld) & kHeld;
        u32 kUp   = prevKeysHeld & (~kHeld);

        prevKeysHeld = kHeld;

        // --- input mapping ---
        RunnerKeyboard_beginFrame(runner->keyboard);

        // Circle Pad
        updateKey(runner->keyboard, kDown, kUp, KEY_CPAD_UP,     VK_UP);
        updateKey(runner->keyboard, kDown, kUp, KEY_CPAD_DOWN,   VK_DOWN);
        updateKey(runner->keyboard, kDown, kUp, KEY_CPAD_LEFT,   VK_LEFT);
        updateKey(runner->keyboard, kDown, kUp, KEY_CPAD_RIGHT,  VK_RIGHT);

        // D-Pad
        updateKey(runner->keyboard, kDown, kUp, KEY_DUP,     VK_UP);
        updateKey(runner->keyboard, kDown, kUp, KEY_DDOWN,   VK_DOWN);
        updateKey(runner->keyboard, kDown, kUp, KEY_DLEFT,   VK_LEFT);
        updateKey(runner->keyboard, kDown, kUp, KEY_DRIGHT,  VK_RIGHT);

        // Buttons
        updateKey(runner->keyboard, kDown, kUp, KEY_A,       'Z');
        updateKey(runner->keyboard, kDown, kUp, KEY_B,       'X');
        updateKey(runner->keyboard, kDown, kUp, KEY_X,       'C');
        updateKey(runner->keyboard, kDown, kUp, KEY_Y,       VK_SHIFT);
        updateKey(runner->keyboard, kDown, kUp, KEY_L,       VK_ENTER);
        updateKey(runner->keyboard, kDown, kUp, KEY_R,       VK_SPACE);
        updateKey(runner->keyboard, kDown, kUp, KEY_SELECT,  VK_ESCAPE);

        Runner_step(runner);
        runner->audioSystem->vtable->update(runner->audioSystem, (float) targetFrameSec);

        Room* activeRoom = runner->currentRoom;

        // Pick output (top screen) and clear.
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

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

        // On 3DS we always draw to the full 400x240 top screen.
        renderer->vtable->beginFrame(renderer, gameW, gameH, NOVA_SCREEN_W, NOVA_SCREEN_H);

        if (runner->drawBackgroundColor) {
            int rInt = BGR_R(runner->backgroundColor);
            int gInt = BGR_G(runner->backgroundColor);
            int bInt = BGR_B(runner->backgroundColor);
            glClearColor(rInt / 255.0f, gInt / 255.0f, bInt / 255.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        bool anyViewRendered = false;
        if (viewsEnabled) {
            for (int vi = 0; vi < 8; vi++) {
                if (!activeRoom->views[vi].enabled) continue;
                int32_t viewX = activeRoom->views[vi].viewX;
                int32_t viewY = activeRoom->views[vi].viewY;
                int32_t viewW = activeRoom->views[vi].viewWidth;
                int32_t viewH = activeRoom->views[vi].viewHeight;
                int32_t portX = activeRoom->views[vi].portX;
                int32_t portY = activeRoom->views[vi].portY;
                int32_t portW = activeRoom->views[vi].portWidth;
                int32_t portH = activeRoom->views[vi].portHeight;
                float viewAngle = runner->viewAngles[vi];

                runner->viewCurrent = vi;
                renderer->vtable->beginView(renderer, viewX, viewY, viewW, viewH, portX, portY, portW, portH, viewAngle);
                Runner_draw(runner);
                renderer->vtable->endView(renderer);

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
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            Runner_draw(runner);
            renderer->vtable->endView(renderer);

            int32_t guiW = runner->guiWidth > 0 ? runner->guiWidth : gameW;
            int32_t guiH = runner->guiHeight > 0 ? runner->guiHeight : gameH;
            renderer->vtable->beginGUI(renderer, guiW, guiH, 0, 0, gameW, gameH);
            Runner_drawGUI(runner);
            renderer->vtable->endGUI(renderer);
        }

        runner->viewCurrent = 0;
        renderer->vtable->endFrame(renderer);

        novaSwapBuffers();
    }

    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    renderer->vtable->destroy(renderer);

    Runner_free(runner);
    CtrFileSystem_destroy(fs);
    VM_free(vm);
    DataWin_free(dataWin);

    nova_fini();
    gfxExit();
    return 0;
}
