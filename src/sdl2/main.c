#include "data_win.h"
#include "vm.h"

#include <SDL2/SDL.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>

#include "runner_keyboard.h"
#include "runner.h"
#include "input_recording.h"
#include "sdl_renderer.h"
#include "glfw_file_system.h"
#include "ma_audio_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "utils.h"

static double get_time_sec(void) {
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

// ===[ COMMAND LINE ARGUMENTS ]===
typedef struct {
    int key;
    bool value;
} FrameSetEntry;

typedef struct {
    const char* dataWinPath;
    const char* screenshotPattern;
    FrameSetEntry* screenshotFrames;
    FrameSetEntry* dumpFrames;
    FrameSetEntry* dumpJsonFrames;
    const char* dumpJsonFilePattern;
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* disassemble;
    StringBooleanEntry* tilesToBeTraced;
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printDeclaredFunctions;
    int exitAtFrame;
    double speedMultiplier;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char* recordInputsPath;
    const char* playbackInputsPath;
} CommandLineArgs;

static void parseCommandLineArgs(CommandLineArgs* args, int argc, char* argv[]) {
    memset(args, 0, sizeof(CommandLineArgs));

    static struct option longOptions[] = {
        {"screenshot",          required_argument, nullptr, 's'},
        {"screenshot-at-frame", required_argument, nullptr, 'f'},
        {"headless",            no_argument,       nullptr, 'h'},
        {"print-rooms", no_argument,               nullptr, 'r'},
        {"print-declared-functions", no_argument,  nullptr, 'p'},
        {"trace-variable-reads", required_argument,  nullptr, 'R'},
        {"trace-variable-writes", required_argument, nullptr, 'W'},
        {"trace-function-calls", required_argument,         nullptr, 'c'},
        {"trace-alarms", required_argument,         nullptr, 'a'},
        {"trace-instance-lifecycles", required_argument,         nullptr, 'l'},
        {"trace-events", required_argument,         nullptr, 'e'},
        {"trace-event-inherited", no_argument, nullptr, 'E'},
        {"trace-tiles", required_argument, nullptr, 'T'},
        {"trace-opcodes", required_argument,       nullptr, 'o'},
        {"trace-stack", required_argument,         nullptr, 'S'},
        {"trace-frames", no_argument, nullptr, 'k'},
        {"exit-at-frame", required_argument, nullptr, 'x'},
        {"dump-frame", required_argument, nullptr, 'd'},
        {"dump-frame-json", required_argument, nullptr, 'j'},
        {"dump-frame-json-file", required_argument, nullptr, 'J'},
        {"speed", required_argument, nullptr, 'M'},
        {"seed", required_argument, nullptr, 'Z'},
        {"debug", no_argument, nullptr, 'D'},
        {"disassemble", required_argument, nullptr, 'A'},
        {"record-inputs", required_argument, nullptr, 'I'},
        {"playback-inputs", required_argument, nullptr, 'P'},
        {nullptr,               0,                 nullptr,  0 }
    };

    args->screenshotFrames = nullptr;
    args->exitAtFrame = -1;
    args->speedMultiplier = 1.0;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 's': args->screenshotPattern = optarg; break;
            case 'f': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s'\n", optarg);
                    exit(1);
                }
                hmput(args->screenshotFrames, (int) frame, true);
                break;
            }
            case 'h': args->headless = true; break;
            case 'r': args->printRooms = true; break;
            case 'p': args->printDeclaredFunctions = true; break;
            case 'R': shput(args->varReadsToBeTraced, optarg, true); break;
            case 'W': shput(args->varWritesToBeTraced, optarg, true); break;
            case 'c': shput(args->functionCallsToBeTraced, optarg, true); break;
            case 'a': shput(args->alarmsToBeTraced, optarg, true); break;
            case 'l': shput(args->instanceLifecyclesToBeTraced, optarg, true); break;
            case 'e': shput(args->eventsToBeTraced, optarg, true); break;
            case 'o': shput(args->opcodesToBeTraced, optarg, true); break;
            case 'S': shput(args->stackToBeTraced, optarg, true); break;
            case 'k': args->traceFrames = true; break;
            case 'x': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --exit-at-frame\n", optarg);
                    exit(1);
                }
                args->exitAtFrame = (int) frame;
                break;
            }
            case 'd': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame\n", optarg);
                    exit(1);
                }
                hmput(args->dumpFrames, (int) frame, true);
                break;
            }
            case 'j': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame-json\n", optarg);
                    exit(1);
                }
                hmput(args->dumpJsonFrames, (int) frame, true);
                break;
            }
            case 'J': args->dumpJsonFilePattern = optarg; break;
            case 'M': {
                char* endPtr;
                double speed = strtod(optarg, &endPtr);
                if (*endPtr != '\0' || speed <= 0.0) {
                    fprintf(stderr, "Error: Invalid speed multiplier '%s' for --speed (must be > 0)\n", optarg);
                    exit(1);
                }
                args->speedMultiplier = speed;
                break;
            }
            case 'D': args->debug = true; break;
            case 'A': shput(args->disassemble, optarg, true); break;
            case 'T': shput(args->tilesToBeTraced, optarg, true); break;
            case 'E': args->traceEventInherited = true; break;
            case 'Z': {
                char* endPtr;
                long seedVal = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0') {
                    fprintf(stderr, "Error: Invalid seed value '%s' for --seed\n", optarg);
                    exit(1);
                }
                args->seed = (int) seedVal;
                args->hasSeed = true;
                break;
            }
            case 'I': args->recordInputsPath = optarg; break;
            case 'P': args->playbackInputsPath = optarg; break;
            default:
                fprintf(stderr, "Usage: %s [--headless] [--screenshot=PATTERN] [--screenshot-at-frame=N ...] <path to data.win or game.unx>\n", argv[0]);
                exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [--headless] [--screenshot=PATTERN] [--screenshot-at-frame=N ...] <path to data.win or game.unx>\n", argv[0]);
        exit(1);
    }

    args->dataWinPath = argv[optind];

    if (hmlen(args->screenshotFrames) > 0 && args->screenshotPattern == nullptr) {
        fprintf(stderr, "Error: --screenshot-at-frame requires --screenshot to be set\n");
        exit(1);
    }

    if (args->headless && args->speedMultiplier != 1.0) {
        fprintf(stderr, "You can't set the speed multiplier while running in headless mode! Headless mode always run in real time\n");
        exit(1);
    }
}

static void freeCommandLineArgs(CommandLineArgs* args) {
    hmfree(args->screenshotFrames);
    hmfree(args->dumpFrames);
    hmfree(args->dumpJsonFrames);
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
    shfree(args->disassemble);
    shfree(args->tilesToBeTraced);
}

// ===[ SCREENSHOT ]===
static void captureScreenshot(SDLRenderer* sdlRenderer, const char* filenamePattern, int frameNumber, int width, int height) {
    char filename[512];
    snprintf(filename, sizeof(filename), filenamePattern, frameNumber);

    int stride = width * 4;
    unsigned char* pixels = safeMalloc(stride * height);
    if (pixels == nullptr) {
        fprintf(stderr, "Error: Failed to allocate memory for screenshot (%dx%d)\n", width, height);
        return;
    }

    SDL_SetRenderTarget(sdlRenderer->sdlRenderer, sdlRenderer->fboTexture);
    SDL_Rect rect = {0, 0, width, height};
    SDL_RenderReadPixels(sdlRenderer->sdlRenderer, &rect, SDL_PIXELFORMAT_ABGR8888, pixels, stride);
    SDL_SetRenderTarget(sdlRenderer->sdlRenderer, NULL);

    stbi_write_png(filename, width, height, 4, pixels, stride);

    free(pixels);
    printf("Screenshot saved: %s\n", filename);
}

// ===[ KEYBOARD INPUT ]===
static int32_t sdlKeyToGml(SDL_Keycode key) {
    if (key >= SDLK_a && key <= SDLK_z) return key - 32;
    if (key >= SDLK_0 && key <= SDLK_9) return key;
    switch (key) {
        case SDLK_ESCAPE:        return VK_ESCAPE;
        case SDLK_RETURN:        return VK_ENTER;
        case SDLK_TAB:           return VK_TAB;
        case SDLK_BACKSPACE:     return VK_BACKSPACE;
        case SDLK_SPACE:         return VK_SPACE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:        return VK_SHIFT;
        case SDLK_LCTRL:
        case SDLK_RCTRL:         return VK_CONTROL;
        case SDLK_LALT:
        case SDLK_RALT:          return VK_ALT;
        case SDLK_UP:            return VK_UP;
        case SDLK_DOWN:          return VK_DOWN;
        case SDLK_LEFT:          return VK_LEFT;
        case SDLK_RIGHT:         return VK_RIGHT;
        case SDLK_F1:            return VK_F1;
        case SDLK_F2:            return VK_F2;
        case SDLK_F3:            return VK_F3;
        case SDLK_F4:            return VK_F4;
        case SDLK_F5:            return VK_F5;
        case SDLK_F6:            return VK_F6;
        case SDLK_F7:            return VK_F7;
        case SDLK_F8:            return VK_F8;
        case SDLK_F9:            return VK_F9;
        case SDLK_F10:           return VK_F10;
        case SDLK_F11:           return VK_F11;
        case SDLK_F12:           return VK_F12;
        case SDLK_INSERT:        return VK_INSERT;
        case SDLK_DELETE:        return VK_DELETE;
        case SDLK_HOME:          return VK_HOME;
        case SDLK_END:           return VK_END;
        case SDLK_PAGEUP:        return VK_PAGEUP;
        case SDLK_PAGEDOWN:      return VK_PAGEDOWN;
        default:                 return -1; // Unknown
    }
}

static InputRecording* globalInputRecording = nullptr;
#ifdef main
#undef main
#endif
// ===[ MAIN ]===
int main(int argc, char* argv[]) {
    CommandLineArgs args;

    char* my_argv[] = {
        "butterscotch.exe",
        "C:\\Users\\Notebook\\CLionProjects\\Butterscotch\\under\\data.win"
    };
    int my_argc = 2;

    argc = my_argc;
    argv = my_argv;
    // -------------------------------------

    parseCommandLineArgs(&args, argc, argv);

    printf("Loading %s...\n", args.dataWinPath);

    DataWin* dataWin = DataWin_parse(
        args.dataWinPath,
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
            .skipLoadingPreciseMasksForNonPreciseSprites = true
        }
    );

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully!\n", gen8->name, gen8->gameID);

    //{
    //    struct mallinfo2 mi = mallinfo2();
    //    printf("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
    //}

    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    VMContext* vm = VM_create(dataWin);

    if (args.hasSeed) {
        srand((unsigned int) args.seed);
        vm->hasFixedSeed = true;
        printf("Using fixed RNG seed: %d\n", args.seed);
    }

    if (args.printRooms) {
        forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
            printf("[%d] %s ()\n", idx, room->name);
            forEachIndexed(RoomGameObject, roomGameObject, idx2, room->gameObjects, room->gameObjectCount) {
                GameObject* gameObject = &dataWin->objt.objects[roomGameObject->objectDefinition];
                printf(
                    "  [%d] %s (x=%d,y=%d,persistent=%d,solid=%d,spriteId=%d,preCreateCode=%d,creationCode=%d)\n",
                    idx2, gameObject->name, roomGameObject->x, roomGameObject->y,
                    gameObject->persistent, gameObject->solid, gameObject->spriteId,
                    roomGameObject->preCreateCode, roomGameObject->creationCode
                );
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        return 0;
    }

    if (args.printDeclaredFunctions) {
        repeat(hmlen(vm->funcMap), i) {
            printf("[%d] %s\n", vm->funcMap[i].value, vm->funcMap[i].key);
        }
        VM_free(vm);
        DataWin_free(dataWin);
        return 0;
    }

    if (shlen(args.disassemble) > 0) {
        VM_buildCrossReferences(vm);
        if (shgeti(args.disassemble, "*") >= 0) {
            repeat(dataWin->code.count, i) {
                VM_disassemble(vm, (int32_t) i);
            }
        } else {
            for (ptrdiff_t i = 0; shlen(args.disassemble) > i; i++) {
                const char* name = args.disassemble[i].key;
                ptrdiff_t idx = shgeti(vm->funcMap, (char*) name);
                if (idx >= 0) {
                    VM_disassemble(vm, vm->funcMap[idx].value);
                } else {
                    fprintf(stderr, "Error: Script '%s' not found in funcMap\n", name);
                }
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 0;
    }

    GlfwFileSystem* fileSystem = GlfwFileSystem_create(args.dataWinPath);

    Runner* runner = Runner_create(dataWin, vm, (FileSystem*) fileSystem);
    runner->debugMode = args.debug;

    if (args.playbackInputsPath != nullptr) {
        globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
    } else if (args.recordInputsPath != nullptr) {
        globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
    }

    shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
    shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
    shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
    shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
    shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
    shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
    shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
    shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);
    shcopyFromTo(args.tilesToBeTraced, runner->vmContext->tilesToBeTraced);
    runner->vmContext->traceEventInherited = args.traceEventInherited;

    // ===[ INIT SDL2 ]===
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    uint32_t windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (args.headless) {
        windowFlags |= SDL_WINDOW_HIDDEN;
    }

    SDL_Window* window = SDL_CreateWindow(
        windowTitle, 
        SDL_WINDOWPOS_CENTERED, 
        SDL_WINDOWPOS_CENTERED, 
        (int) gen8->defaultWindowWidth, 
        (int) gen8->defaultWindowHeight, 
        windowFlags
    );

    if (window == nullptr) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    uint32_t rendererFlags = SDL_RENDERER_ACCELERATED;
    if (!args.headless && args.speedMultiplier == 1.0) {
        // rendererFlags |= SDL_RENDERER_PRESENTVSYNC;
    }

    SDL_Renderer* sdlRenderer = SDL_CreateRenderer(window, -1, rendererFlags);
    if (sdlRenderer == nullptr) {
        fprintf(stderr, "Failed to create SDL renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    Renderer* renderer = SDLRenderer_create(window, sdlRenderer);
    renderer->vtable->init(renderer, dataWin);
    runner->renderer = renderer;

    MaAudioSystem* maAudio = nullptr;
    if (!args.headless) {
        maAudio = MaAudioSystem_create();
        AudioSystem* audioSystem = (AudioSystem*) maAudio;
        audioSystem->vtable->init(audioSystem, dataWin, (FileSystem*) fileSystem);
        runner->audioSystem = audioSystem;
    }

    Runner_initFirstRoom(runner);

    bool debugPaused = false;
    double lastFrameTime = get_time_sec();

    // MAIN LOOP
    while (!runner->shouldExit) {
        RunnerKeyboard_beginFrame(runner->keyboard);
        
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                runner->shouldExit = true;
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                if (!InputRecording_isPlaybackActive(globalInputRecording)) {
                    int32_t gmlKey = sdlKeyToGml(event.key.keysym.sym);
                    if (gmlKey >= 0) {
                        if (event.type == SDL_KEYDOWN) {
                            RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                        } else {
                            RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
                        }
                    }
                }
            }
        }

        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);

        // Debug key bindings
        if (runner->debugMode) {
            if (RunnerKeyboard_checkPressed(runner->keyboard, 'P')) {
                debugPaused = !debugPaused;
                fprintf(stderr, "Debug: %s\n", debugPaused ? "Paused" : "Resumed");
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
                DataWin* dw = runner->dataWin;
                if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                    int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                    runner->pendingRoom = nextIdx;
                    if (runner->audioSystem) runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                }
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
                DataWin* dw = runner->dataWin;
                if (runner->currentRoomOrderPosition > 0) {
                    int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                    runner->pendingRoom = prevIdx;
                    fprintf(stderr, "Debug: Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                }
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                Runner_dumpState(runner);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                char* json = Runner_dumpStateJson(runner);

                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }
                free(json);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");
                runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
                printf("Changed global.interact [%d] value!\n", interactVarId);
            }
        }

        bool shouldStep = true;
        if (runner->debugMode && debugPaused) {
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
            if (shouldStep) fprintf(stderr, "Debug: Frame advance (frame %d)\n", runner->frameCount);
        }

        double frameStartTime = 0;

        if (shouldStep) {
            if (args.traceFrames) {
                frameStartTime = get_time_sec();
                fprintf(stderr, "Frame %d (Start)\n", runner->frameCount);
            }

            Runner_step(runner);

            if (runner->audioSystem != nullptr) {
                float dt = (float) (get_time_sec() - lastFrameTime);
                if (0.0f > dt) dt = 0.0f;
                if (dt > 0.1f) dt = 0.1f;
                runner->audioSystem->vtable->update(runner->audioSystem, dt);
            }

            if (hmget(args.dumpFrames, runner->frameCount)) {
                Runner_dumpState(runner);
            }

            if (hmget(args.dumpJsonFrames, runner->frameCount)) {
                char* json = Runner_dumpStateJson(runner);
                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }
                free(json);
            }
        }

        Room* activeRoom = runner->currentRoom;

        int fbWidth, fbHeight;
        SDL_GetWindowSize(window, &fbWidth, &fbHeight);

        SDL_SetRenderTarget(sdlRenderer, NULL);
        SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
        SDL_RenderClear(sdlRenderer);

        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;

        renderer->vtable->beginFrame(renderer, gameW, gameH, fbWidth, fbHeight);

        if (runner->drawBackgroundColor) {
            uint8_t r = BGR_R(runner->backgroundColor);
            uint8_t g = BGR_G(runner->backgroundColor);
            uint8_t b = BGR_B(runner->backgroundColor);
            SDL_SetRenderDrawColor(sdlRenderer, r, g, b, 255);
        } else {
            SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
        }
        SDL_RenderClear(sdlRenderer);

        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        bool anyViewRendered = false;

        if (viewsEnabled) {
            repeat(8, vi) {
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
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered) {
            runner->viewCurrent = 0;
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            Runner_draw(runner);
            renderer->vtable->endView(renderer);
        }

        runner->viewCurrent = 0;

        renderer->vtable->endFrame(renderer);

        bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);
        if (shouldScreenshot) {
            captureScreenshot((SDLRenderer*)renderer, args.screenshotPattern, runner->frameCount, (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight);
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
            printf("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
            runner->shouldExit = true;
        }

        if (shouldStep && args.traceFrames) {
            double frameElapsedMs = (get_time_sec() - frameStartTime) * 1000.0;
            fprintf(stderr, "Frame %d (End, %.2f ms)\n", runner->frameCount, frameElapsedMs);
        }

        SDL_RenderPresent(sdlRenderer);

        // Frame rate limiting
        if (!args.headless && runner->currentRoom->speed > 0) {
            double targetFrameTime = 1.0 / (runner->currentRoom->speed * args.speedMultiplier);
            double nextFrameTime = lastFrameTime + targetFrameTime;

            double remaining = nextFrameTime - get_time_sec();
            if (remaining > 0.002) {
                SDL_Delay((uint32_t)((remaining - 0.001) * 1000.0));
            }
            while (get_time_sec() < nextFrameTime) {
            }
            lastFrameTime = nextFrameTime;
        } else {
            lastFrameTime = get_time_sec();
        }
    }

    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) {
            InputRecording_save(globalInputRecording);
        }
        InputRecording_free(globalInputRecording);
        globalInputRecording = nullptr;
    }

    if (runner->audioSystem != nullptr) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = nullptr;
    }

    renderer->vtable->destroy(renderer);
    SDL_DestroyRenderer(sdlRenderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    Runner_free(runner);
    GlfwFileSystem_destroy(fileSystem);
    VM_free(vm);
    DataWin_free(dataWin);

    freeCommandLineArgs(&args);

    printf("Bye! :3\n");
    return 0;
}