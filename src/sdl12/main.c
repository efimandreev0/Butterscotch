#include "data_win.h"
#include "vm.h"

// Используем SDL 1.2
#include <SDL/SDL.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runner_keyboard.h"
#include "runner.h"
#include "input_recording.h"
#include "sdl12_renderer.h"
#include "glfw_file_system.h"
#include "sdl12_audio_system.h"
#include "stb_ds.h"
#include "stb_image_write.h"

#include "utils.h"

// В SDL 1.2 нет SDL_GetPerformanceCounter, используем SDL_GetTicks
static double get_time_sec(void) {
    return (double)SDL_GetTicks() / 1000.0;
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

    if (sdlRenderer->fboSurface == nullptr) return;

    // В SDL 1.2 читаем прямо из буфера FBO (переводим в 32 бита для STB)
    SDL_Surface* temp = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
                                             0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (temp) {
        SDL_Rect srcRect = {0, 0, width, height};
        SDL_BlitSurface(sdlRenderer->fboSurface, &srcRect, temp, NULL);
        
        stbi_write_png(filename, width, height, 4, temp->pixels, temp->pitch);
        
        SDL_FreeSurface(temp);
        printf("Screenshot saved: %s\n", filename);
    } else {
        fprintf(stderr, "Error: Failed to allocate memory for screenshot (%dx%d)\n", width, height);
    }
}

// ===[ KEYBOARD INPUT ]===
static int32_t sdlKeyToGml(SDLKey key) {
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


void abort_handler(int sig) {
    fprintf(stderr, "Caught SIGABRT!\n");
    fflush(stderr);

    // можно поставить breakpoint тут
    __builtin_trap(); // или raise(SIGTRAP);
}


int main(int argc, char* argv[]) {
    signal(SIGABRT, abort_handler);

    CommandLineArgs args;

    char* my_argv[] = {
        "butterscotch.exe",
        "--trace-function-calls", "mouse_check_button",
        "--trace-function-calls", "mouse_check_button_pressed",
        "C:\\Users\\Pugemon\\Desktop\\DevMops\\Personal\\Projects\\C\\Butterscotch-3ds-src\\data.win"
    };
    int my_argc = 6;

    // TODO: Закомментируйте эти две строки для реального релиза, если нужно
    argc = my_argc;
    argv = my_argv;
    // -------------------------------------

    parseCommandLineArgs(&args, argc, argv);

    printf("Loading %s...\n", args.dataWinPath);

    DataWin* dataWin = DataWin_parse(
        args.dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true, .parseOptn = true, .parseLang = true, .parseExtn = true,
            .parseSond = true, .parseAgrp = true, .parseSprt = true, .parseBgnd = true,
            .parsePath = true, .parseScpt = true, .parseGlob = true, .parseShdr = true,
            .parseFont = true, .parseTmln = true, .parseObjt = true, .parseRoom = true,
            .parseTpag = true, .parseCode = true, .parseVari = true, .parseFunc = true,
            .parseStrg = true, .parseTxtr = true, .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true
        }
    );

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully!\n", gen8->name, gen8->gameID);

    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    VMContext* vm = VM_create(dataWin);

    if (args.hasSeed) {
        srand((unsigned int) args.seed);
        vm->hasFixedSeed = true;
        printf("Using fixed RNG seed: %d\n", args.seed);
    }

    if (args.printRooms) {
        // Логика печати комнат
        VM_free(vm); DataWin_free(dataWin); return 0;
    }

    if (args.printDeclaredFunctions) {
        // Логика печати функций
        VM_free(vm); DataWin_free(dataWin); return 0;
    }

    if (shlen(args.disassemble) > 0) {
        // Логика дизассемблирования
        VM_free(vm); DataWin_free(dataWin); freeCommandLineArgs(&args); return 0;
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

    // ===[ INIT SDL 1.2 ]===
    
    if (args.headless) {
        // Эмуляция headless режима в SDL 1.2
        #ifdef _WIN32
        _putenv("SDL_VIDEODRIVER=dummy");
        #else
        putenv("SDL_VIDEODRIVER=dummy");
        #endif
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    SDL_WM_SetCaption(windowTitle, NULL);

    int winFlags = SDL_HWSURFACE | SDL_DOUBLEBUF;
    SDL_Surface* screen = SDL_SetVideoMode((int)gen8->defaultWindowWidth, (int)gen8->defaultWindowHeight, 32, winFlags);

    if (screen == NULL) {
        fprintf(stderr, "Failed to set SDL video mode: %s\n", SDL_GetError());
        SDL_Quit();
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Инициализация кастомного рендерера SDL1
    Renderer* renderer = SDLRenderer_create(screen);
    renderer->vtable->init(renderer, dataWin);
    runner->renderer = renderer;

    AudioSystem* maAudio = nullptr;
    if (!args.headless) {
        maAudio = SdlMixerAudioSystem_create();
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
            } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                if (!InputRecording_isPlaybackActive(globalInputRecording)) {
                    int32_t btn = -1;
                    if      (event.button.button == SDL_BUTTON_LEFT)   btn = 1;
                    else if (event.button.button == SDL_BUTTON_RIGHT)  btn = 2;
                    else if (event.button.button == SDL_BUTTON_MIDDLE) btn = 3;
                    if (btn >= 1) {
                        if (event.type == SDL_MOUSEBUTTONDOWN)
                            RunnerKeyboard_onMouseDown(runner->keyboard, btn);
                        else
                            RunnerKeyboard_onMouseUp(runner->keyboard, btn);
                    }
                }
            }
        }
        {
            int mx, my;
            SDL_GetMouseState(&mx, &my);

            int winW = screen->w;
            int winH = screen->h;

            float gx = (winW > 0) ? (float)mx * (float)gen8->defaultWindowWidth  / winW : (float)mx;
            float gy = (winH > 0) ? (float)my * (float)gen8->defaultWindowHeight / winH : (float)my;

            Room* room = runner->currentRoom;
            if (room != NULL && (room->flags & 1) != 0) {
                repeat(8, vi) {
                    if (!room->views[vi].enabled) continue;
                    int portW = room->views[vi].portWidth;
                    int portH = room->views[vi].portHeight;
                    if (portW > 0 && portH > 0) {
                        gx = room->views[vi].viewX + (gx - room->views[vi].portX)
                             * room->views[vi].viewWidth  / portW;
                        gy = room->views[vi].viewY + (gy - room->views[vi].portY)
                             * room->views[vi].viewHeight / portH;
                    }
                    break;
                }
            }

            runner->keyboard->mouseX = gx;
            runner->keyboard->mouseY = gy;
        }
        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);

        // Debug key bindings (Осталось без изменений)
        if (runner->debugMode) {
            if (RunnerKeyboard_checkPressed(runner->keyboard, 'P')) { debugPaused = !debugPaused; }
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) { /* ... */ }
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) { /* ... */ }
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) { Runner_dumpState(runner); }
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) { /* json dump */ }
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) { /* global var override */ }
        }

        bool shouldStep = true;
        if (runner->debugMode && debugPaused) {
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
        }

        double frameStartTime = 0;
        Room* previousRoom = runner->currentRoom;

        if (shouldStep) {
            if (args.traceFrames) frameStartTime = get_time_sec();
            Runner_step(runner);
            if (previousRoom != runner->currentRoom && previousRoom != nullptr) {
                runner->renderer->vtable->flush(runner->renderer);
            }
            if (runner->audioSystem != nullptr) {
                float dt = (float) (get_time_sec() - lastFrameTime);
                if (0.0f > dt) dt = 0.0f;
                if (dt > 0.1f) dt = 0.1f;
                runner->audioSystem->vtable->update(runner->audioSystem, dt);
            }
            if (hmget(args.dumpFrames, runner->frameCount)) Runner_dumpState(runner);
        }

        Room* activeRoom = runner->currentRoom;

        int fbWidth = screen->w;
        int fbHeight = screen->h;

        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;

        // В SDL 1.2 рендерере beginFrame очищает FBO 
        // (SDL_SetRenderTarget/RenderClear удалены, так как они специфичны для SDL2)
        renderer->vtable->beginFrame(renderer, gameW, gameH, fbWidth, fbHeight);

        if (runner->drawBackgroundColor) {
            renderer->vtable->beginView(renderer, 0, 0, gameW, gameH, 0, 0, gameW, gameH, 0.0f);
            renderer->vtable->drawRectangle(renderer, 0, 0, gameW, gameH, runner->backgroundColor, 1.0f, false);
            renderer->vtable->endView(renderer);
        }

        // Если игра требует цвет фона, закрасим fboSurface (это должно быть внутри вашего sdl12_renderer)
        // В данном случае мы просто полагаемся на drawBackgroundColor внутри Runner_draw
        
        bool viewsEnabled = (activeRoom->flags & 1) != 0;
        bool anyViewRendered = false;

        if (viewsEnabled) {
            repeat(8, vi) {
                if (!activeRoom->views[vi].enabled) continue;
                runner->viewCurrent = vi;
                renderer->vtable->beginView(renderer, activeRoom->views[vi].viewX, activeRoom->views[vi].viewY, 
                    activeRoom->views[vi].viewWidth, activeRoom->views[vi].viewHeight, activeRoom->views[vi].portX, 
                    activeRoom->views[vi].portY, activeRoom->views[vi].portWidth, activeRoom->views[vi].portHeight, runner->viewAngles[vi]);

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

        // endFrame в sdl12_renderer.c автоматически вызывает SDL_Flip(screen)
        renderer->vtable->endFrame(renderer);

        bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);
        if (shouldScreenshot) {
            captureScreenshot((SDLRenderer*)renderer, args.screenshotPattern, runner->frameCount, (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight);
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) runner->shouldExit = true;

        // Frame rate limiting
        if (!args.headless && runner->currentRoom->speed > 0) {
            double targetFrameTime = 1.0 / (runner->currentRoom->speed * args.speedMultiplier);
            double nextFrameTime = lastFrameTime + targetFrameTime;

            double remaining = nextFrameTime - get_time_sec();
            if (remaining > 0.002) {
                SDL_Delay((uint32_t)((remaining - 0.001) * 1000.0));
            }
            while (get_time_sec() < nextFrameTime) {}
            lastFrameTime = nextFrameTime;
        } else {
            lastFrameTime = get_time_sec();
        }
    }

    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) InputRecording_save(globalInputRecording);
        InputRecording_free(globalInputRecording);
    }

    if (runner->audioSystem != nullptr) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
    }

    renderer->vtable->destroy(renderer);
    SDL_Quit();

    Runner_free(runner);
    GlfwFileSystem_destroy(fileSystem);
    VM_free(vm);
    DataWin_free(dataWin);

    freeCommandLineArgs(&args);

    printf("Bye! :3\n");
    return 0;
}