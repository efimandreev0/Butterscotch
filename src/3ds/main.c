#include <3ds.h>
#include <malloc.h>
#include <NovaGL.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>

#include "icon_parse.h"
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

#define BASE_DIR "sdmc:/3ds/butterscotch"
#define MAX_GAMES 64

char g_current_data_path[256];
char g_current_cache_dir[256];

typedef struct {
    char name[64];
    char path[256];
    char exe_path[256];
    GLuint icon_tex;
} GameEntry;

static GameEntry g_games[MAX_GAMES];
static int g_game_count = 0;

static void draw_ui_quad(GLuint tex, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    typedef struct {
        float x, y, z;
        float u, v;
        uint8_t r, g, b, a;
    } UIVertex;

    UIVertex verts[6] = {
        {x,     y,     0,  0.f, 0.f,  r, g, b, a},
        {x + w, y,     0,  1.f, 0.f,  r, g, b, a},
        {x + w, y + h, 0,  1.f, 1.f,  r, g, b, a},
        {x,     y,     0,  0.f, 0.f,  r, g, b, a},
        {x + w, y + h, 0,  1.f, 1.f,  r, g, b, a},
        {x,     y + h, 0,  0.f, 1.f,  r, g, b, a}
    };

    if (tex != 0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
    } else {
        glDisable(GL_TEXTURE_2D);
    }

    glVertexPointer(3, GL_FLOAT, sizeof(UIVertex), &verts[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(UIVertex), &verts[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(UIVertex), &verts[0].r);

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static GLuint generate_default_icon(int index) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    uint32_t pixels[64 * 64];
    uint32_t colors[] = {0xFF4444FF, 0xFF44FF44, 0xFFFF4444, 0xFFFFFF44, 0xFFFF44FF, 0xFF44FFFF};
    uint32_t col = colors[index % 6];

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            if (x < 4 || x > 59 || y < 4 || y > 59) pixels[y * 64 + x] = 0xFFFFFFFF;
            else pixels[y * 64 + x] = col;
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

static void scan_games() {
    DIR *dir = opendir(BASE_DIR);
    if (!dir) {
        mkdir(BASE_DIR, 0777);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && g_game_count < MAX_GAMES) {
        if (ent->d_name[0] == '.') continue;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", BASE_DIR, ent->d_name);

        struct stat st;
        stat(path, &st);
        if (S_ISDIR(st.st_mode)) {
            char win_path[256];
            snprintf(win_path, sizeof(win_path), "%s/data.win", path);

            FILE *f = fopen(win_path, "rb");
            if (f) {
                fclose(f);
                strncpy(g_games[g_game_count].name, ent->d_name, 63);
                strncpy(g_games[g_game_count].path, win_path, 255);

                g_games[g_game_count].icon_tex = 0;

                char icon_path[256];
                snprintf(icon_path, sizeof(icon_path), "%s/icon.png", path);
                g_games[g_game_count].icon_tex = load_texture_from_file(icon_path);

                if (g_games[g_game_count].icon_tex == 0) {
                    DIR *gdir = opendir(path);
                    char exe_path[256] = "";
                    if (gdir) {
                        struct dirent *gent;
                        while ((gent = readdir(gdir)) != NULL) {
                            if (strstr(gent->d_name, ".exe")) {
                                snprintf(exe_path, sizeof(exe_path), "%s/%s", path, gent->d_name);
                                break;
                            }
                        }
                        closedir(gdir);
                    }
                    if (strlen(exe_path) > 0) {
                        g_games[g_game_count].icon_tex = extract_icon_from_exe_pe(exe_path);
                    }
                }

                if (g_games[g_game_count].icon_tex == 0) {
                    g_games[g_game_count].icon_tex = generate_default_icon(g_game_count);
                }

                g_game_count++;
            }
        }
    }
    closedir(dir);
}

static int run_launcher() {
    scan_games();

    if (g_game_count == 0) {
        consoleClear();
        printf("\x1b[15;10HNo games found.\n");
        printf("\x1b[16;5HPlace folders with data.win\n");
        printf("\x1b[17;7Hinto %s\n", BASE_DIR);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) return -1;
            gspWaitForVBlank();
        }
        return -1;
    }

    int selected = 0;
    float time = 0.0f;

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 400.0, 240.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    int last_selected = -1;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) return -1;
        if (kDown & KEY_RIGHT) selected++;
        if (kDown & KEY_LEFT) selected--;
        if (kDown & KEY_UP) selected -= 4;
        if (kDown & KEY_DOWN) selected += 4;

        if (selected < 0) selected = 0;
        if (selected >= g_game_count) selected = g_game_count - 1;

        if (kDown & KEY_A) return selected;

        time += 0.15f;

        novaBeginEye(0);
        glViewport(0, 0, 400, 240);

        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int start_idx = (selected / 8) * 8;
        for (int i = 0; i < 8; i++) {
            int g_idx = start_idx + i;
            if (g_idx >= g_game_count) break;

            int col = i % 4;
            int row = i / 4;
            float x = 20 + col * 90;
            float y = 30 + row * 100;
            float size = 70;

            if (g_idx == selected) {
                float pulse = sinf(time) * 3.0f;
                draw_ui_quad(0, x - 4 - pulse/2, y - 4 - pulse/2, size + 8 + pulse, size + 8 + pulse, 255, 200, 50, 255);
            } else {
                draw_ui_quad(0, x - 2, y - 2, size + 4, size + 4, 80, 80, 100, 255);
            }

            draw_ui_quad(g_games[g_idx].icon_tex, x, y, size, size, 255, 255, 255, 255);
        }

        novaSwapBuffers();

        if (selected != last_selected) {
            consoleClear();
            printf("\x1b[1;1H\x1b[36mButterscotch Launcher\x1b[0m\n");
            printf("--------------------------------------\n");
            printf("Selected: \x1b[32m%s\x1b[0m\n", g_games[selected].name);
            printf("\n\x1b[30;1mPath:\n%s\x1b[0m\n", g_games[selected].path);
            printf("\x1b[25;1H[A] Launch  [D-Pad] Select  [START] Quit");
            last_selected = selected;
        }

        gspWaitForVBlank();
    }
    return -1;
}

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

    PrintConsole bottomConsole;
    consoleInit(GFX_BOTTOM, &bottomConsole);

    APT_SetAppCpuTimeLimit(30);
    //osSetSpeedupEnable(true); //uncoment for n3ds clock

    nova_init_ex(512*1024, 1024*1024, 256*1024, 256*1024);

    int selected_game = run_launcher();
    if (selected_game < 0) {
        nova_fini();
        gfxExit();
        return 0;
    }

    strncpy(g_current_data_path, g_games[selected_game].path, 255);

    char base_game_dir[256];
    strncpy(base_game_dir, g_current_data_path, strrchr(g_current_data_path, '/') - g_current_data_path);
    base_game_dir[strrchr(g_current_data_path, '/') - g_current_data_path] = '\0';

    snprintf(g_current_cache_dir, sizeof(g_current_cache_dir), "%s/cache", base_game_dir);
    mkdir(g_current_cache_dir, 0777);

    nova_texture_cache_set_directory(g_current_cache_dir);
    char code_cache_path[256];
    snprintf(code_cache_path, sizeof(code_cache_path), "%s/code.cache", g_current_cache_dir);

    char cache_flag_path[256];
    snprintf(cache_flag_path, sizeof(cache_flag_path), "%s/cache_ready.flag", g_current_cache_dir);

    for (int i = 0; i < g_game_count; i++) {
        if (g_games[i].icon_tex) glDeleteTextures(1, &g_games[i].icon_tex);
    }
    consoleClear();
    printf("\x1b[10;10H\x1b[32mLoading %s...\x1b[0m\n", g_games[selected_game].name);

    FILE *flag = fopen(cache_flag_path, "r");
    bool cached = flag != NULL;
    if (flag) fclose(flag);

    if (!cached) {
        printf("\n\nGenerating texture cache...\nThis may take a minute.");
        DataWinParserOptions opt = { .parseGen8=1, .parseTpag=1, .parseTxtr=1, .skipTextureBlobData=1 };
        DataWin *dw = DataWin_parse(g_current_data_path, opt);
        if (dw) {
            Renderer *r = CtrRenderer_create();
            r->vtable->init(r, dw);
            r->vtable->destroy(r);
            DataWin_free(dw);
        }

        flag = fopen(cache_flag_path, "r");
        cached = flag != NULL;
        if (flag) fclose(flag);
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

    DataWin *dw = DataWin_parse(g_current_data_path, full_opt);

    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0 || !dw) {
        printf("\nBoot failed.\nCheck data.win at %s\nPress START to quit.\n", g_current_data_path);
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        if (dw) DataWin_free(dw);
        nova_fini();
        gfxExit();
        return 1;
    }

    fprintf(stderr, "Loaded \"%s\" (ID: %d)\n", dw->gen8.name, dw->gen8.gameID);

    VMContext *vm = VM_create(dw);
    N3dsFileSystem *fs = N3dsFileSystem_create(g_current_data_path);
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
                displayScaleX = (float) gw / (float) (maxR - minL);
                displayScaleY = (float) gh / (float) (maxB - minT);
            }
        }

        ren->vtable->beginFrame(ren, gw, gh, NOVA_SCREEN_W, NOVA_SCREEN_H);
        int eyes = novaGetEyeCount();

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
                    int portX = (int) ((float) v->portX * displayScaleX + 0.5f);
                    int portY = (int) ((float) v->portY * displayScaleY + 0.5f);
                    int portW = (int) ((float) v->portWidth * displayScaleX + 0.5f);
                    int portH = (int) ((float) v->portHeight * displayScaleY + 0.5f);

                    ren->vtable->beginView(ren, v->viewX, v->viewY, v->viewWidth, v->viewHeight, portX, portY, portW, portH, v->viewAngle);
                    Runner_draw(run);
                    ren->vtable->endView(ren);

                    ren->vtable->beginGUI(ren, run->guiWidth > 0 ? run->guiWidth : portW, run->guiHeight > 0 ? run->guiHeight : portH, portX, portY, portW, portH);
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