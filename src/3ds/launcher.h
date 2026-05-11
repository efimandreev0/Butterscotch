// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#ifndef BUTTERSCOTCH_LAUNCHER_H
#define BUTTERSCOTCH_LAUNCHER_H

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>

#include "data_win.h"
#include "runner_keyboard.h"
#include "runner_gamepad.h"
#include "runner_mouse.h"

#define LAUNCHER_TOP_W   400
#define LAUNCHER_TOP_H   240
#define LAUNCHER_BOT_W   320
#define LAUNCHER_BOT_H   240

#define LAUNCHER_MAX_GAMES 64

typedef struct {
    char name[64];
    char path[256];
    char exe_path[256];
    bool icon_ready;
    int icon_w, icon_h;
    int icon_pot_w, icon_pot_h;
    C3D_Tex icon_tex;
} LauncherGameEntry;

typedef enum {
    LAUNCHER_GAME_SCREEN_TOP = 0,
    LAUNCHER_GAME_SCREEN_BOTTOM = 1
} LauncherGameScreen;

typedef enum {
    LAUNCHER_BACKDROP_GRADIENT = 0,
    LAUNCHER_BACKDROP_BLUR,
    LAUNCHER_BACKDROP_BLACK,
    LAUNCHER_BACKDROP_STRETCH,
} LauncherBackdropMode;

typedef enum {
    LAUNCHER_CONTROL_CPAD_UP = 0,
    LAUNCHER_CONTROL_CPAD_DOWN,
    LAUNCHER_CONTROL_CPAD_LEFT,
    LAUNCHER_CONTROL_CPAD_RIGHT,
    LAUNCHER_CONTROL_DPAD_UP,
    LAUNCHER_CONTROL_DPAD_DOWN,
    LAUNCHER_CONTROL_DPAD_LEFT,
    LAUNCHER_CONTROL_DPAD_RIGHT,
    LAUNCHER_CONTROL_A,
    LAUNCHER_CONTROL_B,
    LAUNCHER_CONTROL_X,
    LAUNCHER_CONTROL_Y,
    LAUNCHER_CONTROL_L,
    LAUNCHER_CONTROL_R,
    LAUNCHER_CONTROL_ZL,
    LAUNCHER_CONTROL_ZR,
    LAUNCHER_CONTROL_START,
    LAUNCHER_CONTROL_SELECT,
    LAUNCHER_CONTROL_CSTICK_UP,
    LAUNCHER_CONTROL_CSTICK_DOWN,
    LAUNCHER_CONTROL_CSTICK_LEFT,
    LAUNCHER_CONTROL_CSTICK_RIGHT,
    LAUNCHER_CONTROL_COUNT
} LauncherControl;

#define LAUNCHER_CONTROL_MAP_MAGIC   0x4D545243u
#define LAUNCHER_CONTROL_MAP_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t vk[LAUNCHER_CONTROL_COUNT];
    uint8_t _reserved[42];
} LauncherControlMap;

typedef struct {
    char id[12];
    char display[20];
    float bg_top[3];
    float bg_mid[3];
    float bg_bot[3];
    float accent[3];
    float accent_dim[3];
    float particle_a[3];
    float particle_b[3];
    float particle_c[3];
    float text_main[3];
    float text_title[3];
    float text_subtle[3];
    float side_blur_alpha;
    float side_particle_alpha;
} LauncherTheme;

typedef enum {
    LAUNCHER_INPUT_KEYBOARD = 0,
    LAUNCHER_INPUT_GAMEPAD,
    LAUNCHER_INPUT_TOUCH,
    LAUNCHER_INPUT_MODE_COUNT
} LauncherInputMode;

#define LAUNCHER_SETTINGS_VERSION 4u

typedef enum {
    LAUNCHER_LETTERBOX_CONTAIN = 0,
    LAUNCHER_LETTERBOX_COVER = 1,
} LauncherLetterboxMode;

typedef struct {
    uint32_t magic;
    uint32_t version;
    int theme_index;
    LauncherGameScreen game_screen;
    int show_side_particles;
    int show_side_blur;
    LauncherBackdropMode backdrop_mode;
    int os_type;
    LauncherInputMode input_mode;
    int letterbox_mode;
    int _reserved[4];
    LauncherControlMap global_controls;
} LauncherSettings;

typedef struct {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} LauncherVertex;

typedef struct {
    C3D_RenderTarget *target;
    int logicalW;
    int logicalH;
    bool owns;
    bool ready;
} LauncherScreen;

typedef struct {
    int uLoc_projection;
    C3D_AttrInfo attrInfo;
    C3D_Tex whiteTex;
    LauncherVertex *vbuf;
    uint32_t vbufCap;
    uint32_t vbufHead;
    uint32_t batchStart;
    uint32_t batchVerts;
    C3D_Tex *batchTex;

    LauncherScreen topScreen;
    LauncherScreen bottomScreen;

    LauncherScreen *currentScreen;

    bool ready;
    bool inFrame;
} LauncherGfx;

typedef enum {
    LAUNCHER_PAUSE_RESUME = 0,
    LAUNCHER_PAUSE_QUIT_TO_LAUNCHER,
    LAUNCHER_PAUSE_QUIT_APP
} LauncherPauseAction;

const LauncherTheme *launcher_theme_at(int index);

int launcher_theme_count(void);

const LauncherTheme *launcher_current_theme(void);

const LauncherSettings *launcher_get_settings(void);

const LauncherControlMap *launcher_get_global_controls(void);

const LauncherControlMap *launcher_get_active_controls(void);

void launcher_apply_theme_index(int index);

void launcher_apply_settings(const LauncherSettings *s);

void launcher_save_settings(void);

void launcher_load_settings(void);

void launcher_load_active_controls(const char *data_win_path);

void launcher_save_active_controls(void);

void launcher_apply_3ds_input(RunnerKeyboardState *kb, u32 down, u32 up, u32 held);

void launcher_apply_3ds_gamepad(RunnerGamepadState *gp,
                                u32 down, u32 up, u32 held,
                                int circleX, int circleY,
                                int cstickX, int cstickY);

void launcher_apply_3ds_touch(RunnerMouseState *mouse,
                              bool touched, int touchX, int touchY,
                              int gameW, int gameH,
                              LauncherGameScreen gameScreen);

const char *launcher_os_type_label(int osType);

int launcher_os_type_count(void);

int launcher_os_type_at(int index);

int launcher_os_type_index_of(int osType);

const char *launcher_input_mode_label(LauncherInputMode mode);

bool launcher_gfx_init(LauncherGfx *gfx);

bool launcher_gfx_init_borrowed(LauncherGfx *gfx,
                                C3D_RenderTarget *topTarget, int topW, int topH,
                                C3D_RenderTarget *bottomTarget, int bottomW, int bottomH);

void launcher_gfx_destroy(LauncherGfx *gfx);

void launcher_scan_games(void);

int launcher_game_count(void);

const LauncherGameEntry *launcher_game(int index);

void launcher_free_game_icons(void);

int launcher_run_menu(LauncherGfx *gfx);

void launcher_render_loading(LauncherGfx *gfx, const char *gameName, const char *stage,
                             int page, int total, float percent);

LauncherPauseAction launcher_run_pause(LauncherGfx *gfx);

float launcher_anim_seconds(void);

void launcher_resolve_new_game_path(const char *request, char *out_path, size_t out_size);

#endif
