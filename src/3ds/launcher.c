//
// Created by Notebook on 03.05.2026.
//

#include "launcher.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <stddef.h>

#include "icon_parse.h"
#include "image_decoder.h"
#include "ctr_renderer.h"
#include "runner.h"

#define BASE_DIR  "sdmc:/3ds/butterscotch"
#define SETTINGS_PATH BASE_DIR "/launcher_settings.bin"
#define LAUNCHER_SETTINGS_MAGIC 0x4253544Cu // 'LTSB'
#define GAME_CONTROL_MAP_NAME "controls.bin"
#define LAUNCHER_APP_TITLE "BUTTERSCOTCH - v7.2 EPdN"

// Each glyph in the bitmap font expands to ~17 6-vertex rects, so a single full footer
// line can burn ~4k verts. Three footer lines + gradient + particles + chrome easily blew
// past the old 24k cap mid-frame, which forced an in-frame buffer wraparound that we
// could not safely combine with citro3d's draw-on switching. 96k gives a comfortable
// margin (~6 MiB linear) for both screens worth of UI in one batch.
#define LAUNCHER_VBUF_CAP (96 * 1024)
#define LAUNCHER_DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

extern shaderProgram_s g_shaderProg;

extern char g_current_data_path[256];

// ---------------------------------------------------------------------------
// Theme catalogue
// ---------------------------------------------------------------------------

static const LauncherTheme g_themes[] = {
    {
        .id = "butter", .display = "BUTTERSCOTCH",
        .bg_top = {0.180f, 0.095f, 0.260f},
        .bg_mid = {0.105f, 0.075f, 0.205f},
        .bg_bot = {0.020f, 0.030f, 0.075f},
        .accent     = {1.00f, 0.74f, 0.24f},
        .accent_dim = {0.55f, 0.30f, 0.10f},
        .particle_a = {0.97f, 0.72f, 0.24f},
        .particle_b = {0.38f, 0.84f, 1.00f},
        .particle_c = {0.94f, 0.46f, 0.90f},
        .text_main  = {1.00f, 0.90f, 0.56f},
        .text_title = {1.00f, 0.78f, 0.28f},
        .text_subtle= {0.70f, 0.86f, 1.00f},
        .side_blur_alpha = 0.35f,
        .side_particle_alpha = 1.00f,
    },
    {
        .id = "midnight", .display = "MIDNIGHT",
        .bg_top = {0.060f, 0.085f, 0.180f},
        .bg_mid = {0.030f, 0.050f, 0.130f},
        .bg_bot = {0.010f, 0.020f, 0.060f},
        .accent     = {0.42f, 0.66f, 1.00f},
        .accent_dim = {0.18f, 0.30f, 0.55f},
        .particle_a = {0.42f, 0.66f, 1.00f},
        .particle_b = {0.78f, 0.84f, 1.00f},
        .particle_c = {0.36f, 0.96f, 0.90f},
        .text_main  = {0.86f, 0.92f, 1.00f},
        .text_title = {0.62f, 0.84f, 1.00f},
        .text_subtle= {0.62f, 0.74f, 0.96f},
        .side_blur_alpha = 0.40f,
        .side_particle_alpha = 0.90f,
    },
    {
        .id = "forest", .display = "FOREST",
        .bg_top = {0.060f, 0.155f, 0.085f},
        .bg_mid = {0.035f, 0.090f, 0.060f},
        .bg_bot = {0.010f, 0.030f, 0.020f},
        .accent     = {0.62f, 0.96f, 0.38f},
        .accent_dim = {0.20f, 0.45f, 0.18f},
        .particle_a = {0.62f, 0.96f, 0.38f},
        .particle_b = {1.00f, 0.92f, 0.40f},
        .particle_c = {0.36f, 0.90f, 0.86f},
        .text_main  = {0.92f, 1.00f, 0.86f},
        .text_title = {0.74f, 1.00f, 0.46f},
        .text_subtle= {0.66f, 0.92f, 0.66f},
        .side_blur_alpha = 0.34f,
        .side_particle_alpha = 0.85f,
    },
    {
        .id = "rose", .display = "ROSE",
        .bg_top = {0.220f, 0.080f, 0.180f},
        .bg_mid = {0.135f, 0.045f, 0.115f},
        .bg_bot = {0.040f, 0.015f, 0.050f},
        .accent     = {1.00f, 0.46f, 0.78f},
        .accent_dim = {0.55f, 0.18f, 0.34f},
        .particle_a = {1.00f, 0.46f, 0.78f},
        .particle_b = {0.96f, 0.78f, 0.46f},
        .particle_c = {0.62f, 0.48f, 1.00f},
        .text_main  = {1.00f, 0.86f, 0.94f},
        .text_title = {1.00f, 0.66f, 0.88f},
        .text_subtle= {0.96f, 0.74f, 0.86f},
        .side_blur_alpha = 0.36f,
        .side_particle_alpha = 0.95f,
    },
    {
        .id = "carbon", .display = "CARBON",
        .bg_top = {0.110f, 0.110f, 0.115f},
        .bg_mid = {0.060f, 0.060f, 0.065f},
        .bg_bot = {0.015f, 0.015f, 0.020f},
        .accent     = {0.92f, 0.92f, 0.96f},
        .accent_dim = {0.40f, 0.40f, 0.45f},
        .particle_a = {0.92f, 0.92f, 0.96f},
        .particle_b = {0.62f, 0.62f, 0.66f},
        .particle_c = {0.78f, 0.78f, 0.82f},
        .text_main  = {0.94f, 0.94f, 0.96f},
        .text_title = {1.00f, 1.00f, 1.00f},
        .text_subtle= {0.74f, 0.74f, 0.78f},
        .side_blur_alpha = 0.30f,
        .side_particle_alpha = 0.70f,
    },
};
#define THEME_COUNT ((int)(sizeof(g_themes) / sizeof(g_themes[0])))

static LauncherSettings g_settings = {
    .magic = LAUNCHER_SETTINGS_MAGIC,
    .version = LAUNCHER_SETTINGS_VERSION,
    .theme_index = 0,
    .game_screen = LAUNCHER_GAME_SCREEN_TOP,
    .show_side_particles = 1,
    .show_side_blur = 1,
    .backdrop_mode = LAUNCHER_BACKDROP_GRADIENT,
    .display_mode = LAUNCHER_DISPLAY_ORIGINAL,
    .app_filter = LAUNCHER_APP_FILTER_LINEAR,
    .os_type = OS_WINDOWS,
    .input_mode = LAUNCHER_INPUT_KEYBOARD,
    .debug_mode = 0,
};

// Subset of YoYoOperatingSystem the user can pick from in settings. Order
// roughly matches what real GameMaker games branch on.
static const struct {
    int          value;
    const char  *label;
} g_os_options[] = {
    { OS_WINDOWS,  "WINDOWS"  },
    { OS_LINUX,    "LINUX"    },
    { OS_MACOSX,   "MACOSX"   },
    { OS_ANDROID,  "ANDROID"  },
    { OS_IOS,      "IOS"      },
    { OS_3DS,      "3DS"      },
    { OS_SWITCH,   "SWITCH"   },
    { OS_PSVITA,   "PSVITA"   },
    { OS_PS4,      "PS4"      },
    { OS_PS3,      "PS3"      },
    { OS_XBOXONE,  "XBOXONE"  },
    { OS_XBOX360,  "XBOX360"  },
    { OS_UWP,      "UWP"      },
    { OS_WIIU,     "WIIU"     },
};
#define OS_OPTION_COUNT ((int)(sizeof(g_os_options) / sizeof(g_os_options[0])))

const char *launcher_os_type_label(int osType) {
    for (int i = 0; i < OS_OPTION_COUNT; i++) {
        if (g_os_options[i].value == osType) return g_os_options[i].label;
    }
    return "WINDOWS";
}

int launcher_os_type_count(void) { return OS_OPTION_COUNT; }

int launcher_os_type_at(int index) {
    if (index < 0) index = 0;
    if (index >= OS_OPTION_COUNT) index = OS_OPTION_COUNT - 1;
    return g_os_options[index].value;
}

int launcher_os_type_index_of(int osType) {
    for (int i = 0; i < OS_OPTION_COUNT; i++) {
        if (g_os_options[i].value == osType) return i;
    }
    return -1;
}

const char *launcher_input_mode_label(LauncherInputMode mode) {
    switch (mode) {
        case LAUNCHER_INPUT_KEYBOARD: return "KEYBOARD";
        case LAUNCHER_INPUT_GAMEPAD:  return "GAMEPAD";
        case LAUNCHER_INPUT_TOUCH:    return "TOUCH";
        default:                      return "KEYBOARD";
    }
}

typedef struct {
    const char *label;
    u32         mask;
    uint8_t     default_vk;
} LauncherControlDef;

static const LauncherControlDef g_control_defs[LAUNCHER_CONTROL_COUNT] = {
    [LAUNCHER_CONTROL_CPAD_UP]      = {"CPAD UP",      KEY_CPAD_UP,      VK_UP},
    [LAUNCHER_CONTROL_CPAD_DOWN]    = {"CPAD DOWN",    KEY_CPAD_DOWN,    VK_DOWN},
    [LAUNCHER_CONTROL_CPAD_LEFT]    = {"CPAD LEFT",    KEY_CPAD_LEFT,    VK_LEFT},
    [LAUNCHER_CONTROL_CPAD_RIGHT]   = {"CPAD RIGHT",   KEY_CPAD_RIGHT,   VK_RIGHT},
    [LAUNCHER_CONTROL_DPAD_UP]      = {"DPAD UP",      KEY_DUP,          VK_UP},
    [LAUNCHER_CONTROL_DPAD_DOWN]    = {"DPAD DOWN",    KEY_DDOWN,        VK_DOWN},
    [LAUNCHER_CONTROL_DPAD_LEFT]    = {"DPAD LEFT",    KEY_DLEFT,        VK_LEFT},
    [LAUNCHER_CONTROL_DPAD_RIGHT]   = {"DPAD RIGHT",   KEY_DRIGHT,       VK_RIGHT},
    [LAUNCHER_CONTROL_A]            = {"A",            KEY_A,            'Z'},
    [LAUNCHER_CONTROL_B]            = {"B",            KEY_B,            'X'},
    [LAUNCHER_CONTROL_X]            = {"X",            KEY_X,            'C'},
    [LAUNCHER_CONTROL_Y]            = {"Y",            KEY_Y,            VK_SHIFT},
    [LAUNCHER_CONTROL_L]            = {"L",            KEY_L,            VK_ENTER},
    [LAUNCHER_CONTROL_R]            = {"R",            KEY_R,            VK_SPACE},
    [LAUNCHER_CONTROL_ZL]           = {"ZL",           KEY_ZL,           VK_NOKEY},
    [LAUNCHER_CONTROL_ZR]           = {"ZR",           KEY_ZR,           VK_NOKEY},
    [LAUNCHER_CONTROL_START]        = {"START",        KEY_START,        VK_NOKEY},
    [LAUNCHER_CONTROL_SELECT]       = {"SELECT",       KEY_SELECT,       VK_NOKEY},
    [LAUNCHER_CONTROL_CSTICK_UP]    = {"CSTICK UP",    KEY_CSTICK_UP,    VK_NOKEY},
    [LAUNCHER_CONTROL_CSTICK_DOWN]  = {"CSTICK DOWN",  KEY_CSTICK_DOWN,  VK_NOKEY},
    [LAUNCHER_CONTROL_CSTICK_LEFT]  = {"CSTICK LEFT",  KEY_CSTICK_LEFT,  VK_NOKEY},
    [LAUNCHER_CONTROL_CSTICK_RIGHT] = {"CSTICK RIGHT", KEY_CSTICK_RIGHT, VK_NOKEY},
};

static LauncherControlMap g_active_controls;
static char               g_active_controls_path[256];

const LauncherTheme *launcher_theme_at(int index) {
    if (index < 0) index = 0;
    if (index >= THEME_COUNT) index = THEME_COUNT - 1;
    return &g_themes[index];
}

int launcher_theme_count(void) { return THEME_COUNT; }

const LauncherTheme *launcher_current_theme(void) {
    return launcher_theme_at(g_settings.theme_index);
}

const LauncherSettings *launcher_get_settings(void) { return &g_settings; }
const LauncherControlMap *launcher_get_global_controls(void) { return &g_settings.global_controls; }
const LauncherControlMap *launcher_get_active_controls(void) { return &g_active_controls; }

static void launcher_reset_control_map(LauncherControlMap *map) {
    if (!map) return;
    memset(map, 0, sizeof(*map));
    map->magic = LAUNCHER_CONTROL_MAP_MAGIC;
    map->version = LAUNCHER_CONTROL_MAP_VERSION;
    for (int i = 0; i < LAUNCHER_CONTROL_COUNT; i++) {
        map->vk[i] = g_control_defs[i].default_vk;
    }
}

static void launcher_normalize_control_map(LauncherControlMap *map) {
    if (!map || map->magic != LAUNCHER_CONTROL_MAP_MAGIC ||
        map->version != LAUNCHER_CONTROL_MAP_VERSION) {
        launcher_reset_control_map(map);
        return;
    }
    map->magic = LAUNCHER_CONTROL_MAP_MAGIC;
    map->version = LAUNCHER_CONTROL_MAP_VERSION;
}

static bool launcher_game_dir_from_data_path(const char *data_win_path, char *out, size_t out_size) {
    if (!data_win_path || !out || out_size == 0) return false;
    const char *slash = strrchr(data_win_path, '/');
    if (!slash) return false;
    size_t len = (size_t)(slash - data_win_path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, data_win_path, len);
    out[len] = '\0';
    return len > 0;
}

static bool launcher_control_path_for_game(const char *data_win_path, char *out, size_t out_size) {
    char game_dir[256];
    if (!launcher_game_dir_from_data_path(data_win_path, game_dir, sizeof(game_dir))) return false;
    snprintf(out, out_size, "%s/%s", game_dir, GAME_CONTROL_MAP_NAME);
    return true;
}

void launcher_load_active_controls(const char *data_win_path) {
    g_active_controls = g_settings.global_controls;
    launcher_normalize_control_map(&g_active_controls);
    g_active_controls_path[0] = '\0';

    char path[256];
    if (!launcher_control_path_for_game(data_win_path, path, sizeof(path))) return;
    strncpy(g_active_controls_path, path, sizeof(g_active_controls_path) - 1);
    g_active_controls_path[sizeof(g_active_controls_path) - 1] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return;
    LauncherControlMap tmp = g_active_controls;
    size_t r = fread(&tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (r >= offsetof(LauncherControlMap, vk) + LAUNCHER_CONTROL_COUNT &&
        tmp.magic == LAUNCHER_CONTROL_MAP_MAGIC) {
        g_active_controls = tmp;
        launcher_normalize_control_map(&g_active_controls);
    }
}

void launcher_save_active_controls(void) {
    launcher_normalize_control_map(&g_active_controls);
    if (g_active_controls_path[0] == '\0') return;
    FILE *f = fopen(g_active_controls_path, "wb");
    if (!f) return;
    fwrite(&g_active_controls, sizeof(g_active_controls), 1, f);
    fclose(f);
}

static u32 launcher_cstick_as_cpad_mask(int cstickX, int cstickY) {
    const int deadzone = 45;
    u32 mask = 0;
    if (cstickX <= -deadzone) mask |= KEY_CPAD_LEFT;
    if (cstickX >=  deadzone) mask |= KEY_CPAD_RIGHT;
    if (cstickY <= -deadzone) mask |= KEY_CPAD_DOWN;
    if (cstickY >=  deadzone) mask |= KEY_CPAD_UP;
    return mask;
}

static void launcher_apply_vk_edge(RunnerKeyboardState *kb, int vk, bool down, bool up, bool held) {
    if (!kb || vk < 0 || vk >= GML_KEY_COUNT || vk == VK_NOKEY) return;
    if (down) {
        RunnerKeyboard_onKeyDown(kb, vk);
    } else if (up && !held) {
        RunnerKeyboard_onKeyUp(kb, vk);
    }
}

void launcher_apply_3ds_input(RunnerKeyboardState *kb, u32 down, u32 up, u32 held,
                              int cstickX, int cstickY) {
    static u32 cstickHeldPrev = 0;
    u32 cstickHeld = launcher_cstick_as_cpad_mask(cstickX, cstickY);
    down |= cstickHeld & ~cstickHeldPrev;
    up |= cstickHeldPrev & ~cstickHeld;
    held |= cstickHeld;
    cstickHeldPrev = cstickHeld;

    bool vk_down[GML_KEY_COUNT];
    bool vk_up[GML_KEY_COUNT];
    bool vk_held[GML_KEY_COUNT];
    memset(vk_down, 0, sizeof(vk_down));
    memset(vk_up, 0, sizeof(vk_up));
    memset(vk_held, 0, sizeof(vk_held));

    launcher_normalize_control_map(&g_active_controls);
    for (int i = 0; i < LAUNCHER_CONTROL_COUNT; i++) {
        int vk = g_active_controls.vk[i];
        if (vk == VK_NOKEY || vk >= GML_KEY_COUNT) continue;
        u32 mask = g_control_defs[i].mask;
        if (down & mask) vk_down[vk] = true;
        if (up & mask) vk_up[vk] = true;
        if (held & mask) vk_held[vk] = true;
    }

    for (int vk = 1; vk < GML_KEY_COUNT; vk++) {
        launcher_apply_vk_edge(kb, vk, vk_down[vk], vk_up[vk], vk_held[vk]);
    }
}

// Indices in GamepadSlot.buttonDown[] (see runner_gamepad.c gmlButtonToIndex).
enum {
    GP_IDX_FACE1     = 0,
    GP_IDX_FACE2     = 1,
    GP_IDX_FACE3     = 2,
    GP_IDX_FACE4     = 3,
    GP_IDX_SHOULDERL = 4,
    GP_IDX_SHOULDERR = 5,
    GP_IDX_TRIGGERL  = 6,
    GP_IDX_TRIGGERR  = 7,
    GP_IDX_SELECT    = 8,
    GP_IDX_START     = 9,
    GP_IDX_PADU      = 12,
    GP_IDX_PADD      = 13,
    GP_IDX_PADL      = 14,
    GP_IDX_PADR      = 15,
};

static float circle_axis_normalize(int v) {
    // 3DS circle pad raw range is roughly -156..+156. Clamp & normalize.
    if (v >  156) v =  156;
    if (v < -156) v = -156;
    return (float)v / 156.0f;
}

static float deadzone(float v, float dz) {
    if (v >  0.0f) return (v <  dz) ? 0.0f : (v - dz) / (1.0f - dz);
    if (v <  0.0f) return (v > -dz) ? 0.0f : (v + dz) / (1.0f - dz);
    return 0.0f;
}

void launcher_apply_3ds_gamepad(RunnerGamepadState *gp,
                                u32 down, u32 up, u32 held,
                                int circleX, int circleY,
                                int cstickX, int cstickY) {
    (void)down; (void)up;
    if (!gp) return;
    GamepadSlot *slot = &gp->slots[0];

    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown,     0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed,  0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue,    0, sizeof(slot->buttonValue));
    memset(slot->axisValue,      0, sizeof(slot->axisValue));

    // Map 3DS face buttons to GameMaker's xinput-style face indices:
    //   GP_FACE1 = bottom = A on 3DS  (matches "A" on Xbox = bottom)
    //   GP_FACE2 = right  = B on 3DS
    //   GP_FACE3 = left   = Y on 3DS
    //   GP_FACE4 = top    = X on 3DS
    if (held & KEY_A) slot->buttonDown[GP_IDX_FACE1] = true;
    if (held & KEY_B) slot->buttonDown[GP_IDX_FACE2] = true;
    if (held & KEY_Y) slot->buttonDown[GP_IDX_FACE3] = true;
    if (held & KEY_X) slot->buttonDown[GP_IDX_FACE4] = true;

    if (held & KEY_L)  slot->buttonDown[GP_IDX_SHOULDERL] = true;
    if (held & KEY_R)  slot->buttonDown[GP_IDX_SHOULDERR] = true;
    if (held & KEY_ZL) slot->buttonDown[GP_IDX_TRIGGERL]  = true;
    if (held & KEY_ZR) slot->buttonDown[GP_IDX_TRIGGERR]  = true;

    if (held & KEY_SELECT) slot->buttonDown[GP_IDX_SELECT] = true;
    if (held & KEY_START)  slot->buttonDown[GP_IDX_START]  = true;

    if (held & KEY_DUP)    slot->buttonDown[GP_IDX_PADU] = true;
    if (held & KEY_DDOWN)  slot->buttonDown[GP_IDX_PADD] = true;
    if (held & KEY_DLEFT)  slot->buttonDown[GP_IDX_PADL] = true;
    if (held & KEY_DRIGHT) slot->buttonDown[GP_IDX_PADR] = true;

    // 3DS doesn't have analog triggers, so trigger buttons only carry 0/1.
    for (int i = 0; i < GP_BUTTON_COUNT; i++) {
        slot->buttonValue[i] = slot->buttonDown[i] ? 1.0f : 0.0f;
    }

    // Circle pad -> left stick. C-stick mirrors left-stick movement too, so New
    // 3DS users can move with either nub without changing per-game controls.
    float lh = circle_axis_normalize(circleX);
    float lv = -circle_axis_normalize(circleY);
    float rh = circle_axis_normalize(cstickX);
    float rv = -circle_axis_normalize(cstickY);
    float leftH = deadzone(lh, slot->deadzone);
    float leftV = deadzone(lv, slot->deadzone);
    float cstickH = deadzone(rh, slot->deadzone);
    float cstickV = deadzone(rv, slot->deadzone);
    if (fabsf(cstickH) > fabsf(leftH)) leftH = cstickH;
    if (fabsf(cstickV) > fabsf(leftV)) leftV = cstickV;
    slot->axisValue[0] = leftH;
    slot->axisValue[1] = leftV;
    slot->axisValue[2] = deadzone(rh, slot->deadzone);
    slot->axisValue[3] = deadzone(rv, slot->deadzone);

    for (int i = 0; i < GP_BUTTON_COUNT; i++) {
        bool now = slot->buttonDown[i];
        bool was = slot->buttonDownPrev[i];
        if (now && !was) slot->buttonPressed[i]  = true;
        if (!now && was) slot->buttonReleased[i] = true;
    }

    snprintf(slot->description, sizeof(slot->description), "Nintendo 3DS");
    snprintf(slot->guid,        sizeof(slot->guid),        "n3ds-builtin");
    slot->connectedPrev = slot->connected;
    slot->connected = true;
    slot->jid = 0;
    gp->connectedCount = 1;
}

void launcher_apply_3ds_touch(RunnerMouseState *mouse,
                              bool touched, int touchX, int touchY,
                              int gameW, int gameH) {
    if (!mouse) return;
    if (touched) {
        // 3DS bottom screen is 320x240. Scale to game logical size so that
        // mouse_x/mouse_y match what the game expects in room space. If gameW/H
        // are zero (no room loaded yet), fall back to raw touch coords.
        if (gameW <= 0) gameW = 320;
        if (gameH <= 0) gameH = 240;
        int x = (touchX * gameW) / 320;
        int y = (touchY * gameH) / 240;
        RunnerMouse_setPosition(mouse, x, y);
    }
    RunnerMouse_setButton(mouse, MB_LEFT, touched);
    // 3DS has no separate right/middle buttons; leave them clear.
    RunnerMouse_setButton(mouse, MB_RIGHT,  false);
    RunnerMouse_setButton(mouse, MB_MIDDLE, false);
}

static void launcher_push_theme_to_renderer(void) {
    const LauncherTheme *t = launcher_current_theme();
    CtrRenderer_setLetterboxTheme(t->bg_top[0], t->bg_top[1], t->bg_top[2],
                                  t->bg_bot[0], t->bg_bot[1], t->bg_bot[2],
                                  t->accent[0], t->accent[1], t->accent[2],
                                  g_settings.show_side_blur ? t->side_blur_alpha : 0.f,
                                  g_settings.show_side_particles ? t->side_particle_alpha : 0.f);
    CtrRenderer_setGameScreen((CtrGameScreen)g_settings.game_screen);
    CtrRenderer_setBackdropMode((CtrBackdropMode)g_settings.backdrop_mode);
    CtrRenderer_setDisplayMode((CtrDisplayMode)g_settings.display_mode);
    CtrRenderer_setAppFilterMode((CtrAppFilterMode)g_settings.app_filter);
}

static const char *launcher_backdrop_label(LauncherBackdropMode mode) {
    switch (mode) {
        case LAUNCHER_BACKDROP_GRADIENT: return "GRADIENT";
        case LAUNCHER_BACKDROP_BLUR:     return "BLUR";
        case LAUNCHER_BACKDROP_BLACK:    return "BLACK";
        case LAUNCHER_BACKDROP_STRETCH:  return "STRETCH";
        default:                         return "GRADIENT";
    }
}

static LauncherBackdropMode launcher_next_backdrop(LauncherBackdropMode mode, int dir) {
    int next = (int)mode + (dir >= 0 ? 1 : -1);
    int count = 3;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    return (LauncherBackdropMode)next;
}

static const char *launcher_display_mode_label(LauncherDisplayMode mode) {
    switch (mode) {
        case LAUNCHER_DISPLAY_ORIGINAL: return "ORIGINAL";
        case LAUNCHER_DISPLAY_STRETCH:  return "STRETCH";
        case LAUNCHER_DISPLAY_3DS_WIDE: return "3DS WIDE";
        default:                        return "ORIGINAL";
    }
}

static LauncherDisplayMode launcher_next_display_mode(LauncherDisplayMode mode, int dir) {
    int next = (int)mode + (dir >= 0 ? 1 : -1);
    int count = (int)LAUNCHER_DISPLAY_MODE_COUNT;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    return (LauncherDisplayMode)next;
}

static const char *launcher_app_filter_label(LauncherAppFilterMode mode) {
    switch (mode) {
        case LAUNCHER_APP_FILTER_LINEAR:  return "LINEAR";
        case LAUNCHER_APP_FILTER_NEAREST: return "NEAREST";
        default:                          return "LINEAR";
    }
}

static LauncherAppFilterMode launcher_next_app_filter(LauncherAppFilterMode mode, int dir) {
    int next = (int)mode + (dir >= 0 ? 1 : -1);
    int count = (int)LAUNCHER_APP_FILTER_COUNT;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    return (LauncherAppFilterMode)next;
}

void launcher_apply_theme_index(int index) {
    if (index < 0) index = 0;
    if (index >= THEME_COUNT) index = THEME_COUNT - 1;
    g_settings.theme_index = index;
    launcher_push_theme_to_renderer();
}

void launcher_apply_settings(const LauncherSettings *s) {
    if (!s) return;
    g_settings.theme_index = s->theme_index;
    if (g_settings.theme_index < 0 || g_settings.theme_index >= THEME_COUNT)
        g_settings.theme_index = 0;
    g_settings.game_screen = s->game_screen;
    g_settings.show_side_blur = s->show_side_blur;
    g_settings.show_side_particles = s->show_side_particles;
    g_settings.backdrop_mode = s->backdrop_mode;
    g_settings.display_mode = s->display_mode;
    g_settings.app_filter = s->app_filter;
    g_settings.os_type = s->os_type;
    g_settings.input_mode = s->input_mode;
    g_settings.debug_mode = s->debug_mode ? 1 : 0;
    g_settings.global_controls = s->global_controls;
    launcher_normalize_control_map(&g_settings.global_controls);
    int backdropValue = (int)g_settings.backdrop_mode;
    if (backdropValue < (int)LAUNCHER_BACKDROP_GRADIENT ||
        backdropValue > (int)LAUNCHER_BACKDROP_BLACK) {
        g_settings.backdrop_mode = LAUNCHER_BACKDROP_GRADIENT;
    }
    if (g_settings.display_mode < 0 || g_settings.display_mode >= LAUNCHER_DISPLAY_MODE_COUNT) {
        g_settings.display_mode = LAUNCHER_DISPLAY_ORIGINAL;
    }
    if (g_settings.app_filter < 0 || g_settings.app_filter >= LAUNCHER_APP_FILTER_COUNT) {
        g_settings.app_filter = LAUNCHER_APP_FILTER_LINEAR;
    }
    g_settings.frame_pacing = LAUNCHER_FRAME_PACING_30;
    if (g_settings.input_mode < 0 || g_settings.input_mode >= LAUNCHER_INPUT_MODE_COUNT) {
        g_settings.input_mode = LAUNCHER_INPUT_KEYBOARD;
    }
    if (launcher_os_type_index_of(g_settings.os_type) < 0) {
        g_settings.os_type = OS_WINDOWS;
    }
    launcher_push_theme_to_renderer();
}

void launcher_save_settings(void) {
    mkdir(BASE_DIR, 0777);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) return;
    g_settings.magic = LAUNCHER_SETTINGS_MAGIC;
    g_settings.version = LAUNCHER_SETTINGS_VERSION;
    launcher_normalize_control_map(&g_settings.global_controls);
    fwrite(&g_settings, sizeof(g_settings), 1, f);
    fclose(f);
}

void launcher_load_settings(void) {
    launcher_normalize_control_map(&g_settings.global_controls);
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) { launcher_push_theme_to_renderer(); return; }
    LauncherSettings tmp = g_settings;
    size_t r = fread(&tmp, 1, sizeof(tmp), f);
    fclose(f);
    if (r >= offsetof(LauncherSettings, theme_index) + sizeof(tmp.theme_index) &&
        tmp.magic == LAUNCHER_SETTINGS_MAGIC) {
        if (tmp.theme_index < 0 || tmp.theme_index >= THEME_COUNT) tmp.theme_index = 0;
        if (tmp.game_screen != LAUNCHER_GAME_SCREEN_TOP &&
            tmp.game_screen != LAUNCHER_GAME_SCREEN_BOTTOM) {
            tmp.game_screen = LAUNCHER_GAME_SCREEN_TOP;
        }
        int backdropValue = (int)tmp.backdrop_mode;
        if (backdropValue < (int)LAUNCHER_BACKDROP_GRADIENT ||
            backdropValue > (int)LAUNCHER_BACKDROP_STRETCH) {
            tmp.backdrop_mode = LAUNCHER_BACKDROP_GRADIENT;
        }
        if (tmp.version < 5u) {
            tmp.display_mode = (tmp.backdrop_mode == LAUNCHER_BACKDROP_STRETCH)
                                   ? LAUNCHER_DISPLAY_STRETCH
                                   : LAUNCHER_DISPLAY_ORIGINAL;
            if (tmp.backdrop_mode == LAUNCHER_BACKDROP_STRETCH)
                tmp.backdrop_mode = LAUNCHER_BACKDROP_GRADIENT;
        }
        if ((int)tmp.backdrop_mode < (int)LAUNCHER_BACKDROP_GRADIENT ||
            (int)tmp.backdrop_mode > (int)LAUNCHER_BACKDROP_BLACK) {
            tmp.backdrop_mode = LAUNCHER_BACKDROP_GRADIENT;
        }
        if (tmp.display_mode < 0 || tmp.display_mode >= LAUNCHER_DISPLAY_MODE_COUNT) {
            tmp.display_mode = LAUNCHER_DISPLAY_ORIGINAL;
        }
        if (tmp.version < 6u || tmp.app_filter < 0 || tmp.app_filter >= LAUNCHER_APP_FILTER_COUNT) {
            tmp.app_filter = LAUNCHER_APP_FILTER_LINEAR;
        }
        if (tmp.version < 7u || tmp.frame_pacing < 0 ||
            tmp.frame_pacing >= LAUNCHER_FRAME_PACING_COUNT) {
            tmp.frame_pacing = LAUNCHER_FRAME_PACING_30;
        }
        tmp.frame_pacing = LAUNCHER_FRAME_PACING_30;
        if (tmp.version < 8u) {
            tmp.debug_mode = 0;
        } else {
            tmp.debug_mode = tmp.debug_mode ? 1 : 0;
        }
        // Older settings files (version 3) don't have os_type / input_mode —
        // their bytes will be whatever happened to live in the previous
        // _reserved[] padding. Fall back to defaults if they're out of range.
        if (tmp.version < 4u || launcher_os_type_index_of(tmp.os_type) < 0) {
            tmp.os_type = OS_WINDOWS;
        }
        if (tmp.version < 4u || tmp.input_mode < 0 ||
            tmp.input_mode >= LAUNCHER_INPUT_MODE_COUNT) {
            tmp.input_mode = LAUNCHER_INPUT_KEYBOARD;
        }
        launcher_normalize_control_map(&tmp.global_controls);
        g_settings = tmp;
    }
    g_settings.version = LAUNCHER_SETTINGS_VERSION;
    launcher_push_theme_to_renderer();
}

// ---------------------------------------------------------------------------
// Game list
// ---------------------------------------------------------------------------

static LauncherGameEntry g_games[LAUNCHER_MAX_GAMES];
static int               g_game_count = 0;

int launcher_game_count(void) { return g_game_count; }
const LauncherGameEntry *launcher_game(int index) {
    if (index < 0 || index >= g_game_count) return NULL;
    return &g_games[index];
}

// ---------------------------------------------------------------------------
// Texture helpers
// ---------------------------------------------------------------------------

#define LAUNCHER_ICON_CACHE_MAGIC   0x49435442u // 'BTCI'
#define LAUNCHER_ICON_CACHE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  width;
    int32_t  height;
    int32_t  pot_w;
    int32_t  pot_h;
    uint64_t source_size;
    int64_t  source_mtime;
    uint32_t data_bytes;
    char     source_name[96];
} LauncherIconCacheHeader;

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

static const char *launcher_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

static void launcher_icon_cache_path(const char *game_dir, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/cache/launcher_icon.bin", game_dir);
}

static bool launcher_write_icon_cache(const char *cache_path, const char *source_path,
                                      const struct stat *source_st,
                                      const LauncherGameEntry *game,
                                      const uint16_t *tiled) {
    if (!cache_path || !source_path || !source_st || !game || !tiled) return false;

    char cache_dir[256];
    strncpy(cache_dir, cache_path, sizeof(cache_dir) - 1);
    cache_dir[sizeof(cache_dir) - 1] = '\0';
    char *slash = strrchr(cache_dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(cache_dir, 0777);
    }

    FILE *f = fopen(cache_path, "wb");
    if (!f) return false;

    LauncherIconCacheHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = LAUNCHER_ICON_CACHE_MAGIC;
    hdr.version = LAUNCHER_ICON_CACHE_VERSION;
    hdr.width = game->icon_w;
    hdr.height = game->icon_h;
    hdr.pot_w = game->icon_pot_w;
    hdr.pot_h = game->icon_pot_h;
    hdr.source_size = (uint64_t)source_st->st_size;
    hdr.source_mtime = (int64_t)source_st->st_mtime;
    hdr.data_bytes = (uint32_t)((size_t)hdr.pot_w * (size_t)hdr.pot_h * sizeof(uint16_t));
    snprintf(hdr.source_name, sizeof(hdr.source_name), "%s", launcher_basename(source_path));

    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(tiled, 1, hdr.data_bytes, f) == hdr.data_bytes;
    fclose(f);
    return ok;
}

static bool launcher_load_icon_cache(LauncherGameEntry *game, const char *cache_path,
                                     const char *source_path, const struct stat *source_st) {
    if (!game || !cache_path || !source_path || !source_st) return false;

    FILE *f = fopen(cache_path, "rb");
    if (!f) return false;

    LauncherIconCacheHeader hdr;
    bool ok = fread(&hdr, sizeof(hdr), 1, f) == 1;
    char source_name[96];
    memset(source_name, 0, sizeof(source_name));
    snprintf(source_name, sizeof(source_name), "%s", launcher_basename(source_path));
    if (ok) {
        size_t expected = (size_t)hdr.pot_w * (size_t)hdr.pot_h * sizeof(uint16_t);
        ok = hdr.magic == LAUNCHER_ICON_CACHE_MAGIC &&
             hdr.version == LAUNCHER_ICON_CACHE_VERSION &&
             hdr.width > 0 && hdr.height > 0 &&
             hdr.pot_w >= hdr.width && hdr.pot_h >= hdr.height &&
             hdr.pot_w <= 512 && hdr.pot_h <= 512 &&
             hdr.source_size == (uint64_t)source_st->st_size &&
             hdr.source_mtime == (int64_t)source_st->st_mtime &&
             hdr.data_bytes == expected &&
             memcmp(hdr.source_name, source_name, sizeof(hdr.source_name)) == 0;
    }

    uint16_t *tiled = NULL;
    if (ok) {
        tiled = linearAlloc(hdr.data_bytes);
        ok = tiled && fread(tiled, 1, hdr.data_bytes, f) == hdr.data_bytes;
    }
    fclose(f);
    if (!ok) {
        if (tiled) linearFree(tiled);
        return false;
    }

    if (game->icon_ready) {
        C3D_TexDelete(&game->icon_tex);
        memset(&game->icon_tex, 0, sizeof(game->icon_tex));
        game->icon_ready = false;
    }

    if (!C3D_TexInit(&game->icon_tex, (u16)hdr.pot_w, (u16)hdr.pot_h, GPU_RGBA4)) {
        linearFree(tiled);
        return false;
    }

    C3D_TexLoadImage(&game->icon_tex, tiled, GPU_TEXFACE_2D, 0);
    C3D_TexFlush(&game->icon_tex);
    // GPU_NEAREST so pixel-art game icons stay crisp at the launcher's upscale factor.
    C3D_TexSetFilter(&game->icon_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&game->icon_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    linearFree(tiled);

    game->icon_ready = true;
    game->icon_w = hdr.width;
    game->icon_h = hdr.height;
    game->icon_pot_w = hdr.pot_w;
    game->icon_pot_h = hdr.pot_h;
    return true;
}

void launcher_free_game_icons(void) {
    for (int i = 0; i < g_game_count; i++) {
        if (g_games[i].icon_ready) {
            C3D_TexDelete(&g_games[i].icon_tex);
            memset(&g_games[i].icon_tex, 0, sizeof(g_games[i].icon_tex));
            g_games[i].icon_ready = false;
        }
    }
}

static bool launcher_upload_icon(LauncherGameEntry *game, const IconImage *img,
                                 const char *cache_path, const char *source_path,
                                 const struct stat *source_st) {
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
    // GPU_NEAREST so pixel-art game icons stay crisp at the launcher's upscale factor.
    C3D_TexSetFilter(&game->icon_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&game->icon_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    game->icon_ready = true;
    game->icon_w = img->width;
    game->icon_h = img->height;
    game->icon_pot_w = potW;
    game->icon_pot_h = potH;

    if (cache_path && source_path && source_st) {
        launcher_write_icon_cache(cache_path, source_path, source_st, game, tiled);
    }

    linearFree(tiled);
    free(linear);
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

static void launcher_try_load_icon(LauncherGameEntry *game, const char *game_dir) {
    static const char *image_names[] = {
        "icon.png", "icon.jpg", "icon.jpeg", "cover.png", "cover.jpg", "cover.jpeg"
    };

    char cache_path[256];
    launcher_icon_cache_path(game_dir, cache_path, sizeof(cache_path));

    for (size_t i = 0; i < sizeof(image_names) / sizeof(image_names[0]); i++) {
        char image_path[256];
        snprintf(image_path, sizeof(image_path), "%s/%s", game_dir, image_names[i]);
        struct stat st;
        if (stat(image_path, &st) != 0) continue;
        if (launcher_load_icon_cache(game, cache_path, image_path, &st)) return;

        IconImage img;
        if (load_image_from_file(image_path, &img)) {
            bool ok = launcher_upload_icon(game, &img, cache_path, image_path, &st);
            IconImage_free(&img);
            if (ok) return;
        }
    }

    if (launcher_find_exe(game_dir, game->exe_path, sizeof(game->exe_path))) {
        struct stat st;
        if (stat(game->exe_path, &st) != 0) return;
        if (launcher_load_icon_cache(game, cache_path, game->exe_path, &st)) return;

        IconImage img;
        if (extract_icon_from_exe_pe(game->exe_path, &img)) {
            bool ok = launcher_upload_icon(game, &img, cache_path, game->exe_path, &st);
            IconImage_free(&img);
            if (ok) return;
        }
    }
}

void launcher_scan_games(void) {
    launcher_free_game_icons();
    memset(g_games, 0, sizeof(g_games));
    g_game_count = 0;
    DIR *dir = opendir(BASE_DIR);
    if (!dir) { mkdir(BASE_DIR, 0777); return; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && g_game_count < LAUNCHER_MAX_GAMES) {
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

// Case-insensitive substring search. We avoid strcasestr (GNU extension; newlib
// doesn't expose it on devkitARM by default) and roll a tiny one inline.
static const char *ci_strstr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nlen) {
            unsigned char a = (unsigned char)hay[i];
            unsigned char b = (unsigned char)needle[i];
            if (!a) return NULL;
            if (tolower(a) != tolower(b)) break;
            i++;
        }
        if (i == nlen) return hay;
    }
    return NULL;
}

// Try `path`; if it points at a directory, append "/data.win". Returns true if
// the resulting file exists and is openable.
static bool try_resolve_candidate(const char *path, char *out_path, size_t out_size) {
    if (!path || !*path) return false;

    char buf[512];
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(buf, sizeof(buf), "%s/data.win", path);
    } else {
        snprintf(buf, sizeof(buf), "%s", path);
    }

    FILE *f = fopen(buf, "rb");
    if (!f) return false;
    fclose(f);

    strncpy(out_path, buf, out_size - 1);
    out_path[out_size - 1] = '\0';
    return true;
}

static void launcher_normalize_path(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    out[0] = '\0';

    char prefix[16] = "";
    const char *rest = path;
    if (strncmp(path, "sdmc:/", 6) == 0) {
        snprintf(prefix, sizeof(prefix), "sdmc:/");
        rest = path + 6;
    } else if (path[0] == '/') {
        snprintf(prefix, sizeof(prefix), "/");
        while (*rest == '/') rest++;
    }

    char work[512];
    snprintf(work, sizeof(work), "%s", rest);

    char parts[32][64];
    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(work, "/\\", &save); tok != NULL; tok = strtok_r(NULL, "/\\", &save)) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') continue;
        if (strcmp(tok, "..") == 0) {
            if (count > 0) count--;
            continue;
        }
        if (count < 32) {
            snprintf(parts[count], sizeof(parts[count]), "%s", tok);
            count++;
        }
    }

    snprintf(out, out_size, "%s", prefix);
    size_t len = strlen(out);
    for (int i = 0; i < count; i++) {
        bool needSlash = len > 0 && out[len - 1] != '/';
        int written = snprintf(out + len, out_size - len, "%s%s", needSlash ? "/" : "", parts[i]);
        if (written < 0) break;
        len += (size_t)written;
        if (len >= out_size) {
            out[out_size - 1] = '\0';
            break;
        }
    }
}

static bool try_resolve_joined_candidate(const char *base, const char *rel,
                                         char *out_path, size_t out_size) {
    if (!base || !base[0] || !rel) return false;
    char joined[512];
    char normalized[512];
    snprintf(joined, sizeof(joined), "%s/%s", base, rel);
    launcher_normalize_path(joined, normalized, sizeof(normalized));
    return try_resolve_candidate(normalized, out_path, out_size);
}

void launcher_resolve_new_game_path(const char *request, char *out_path, size_t out_size) {
    printf("Try resolve path: %s\n", request ? request : "(null)");
    if (!request || !out_path || out_size == 0) {
        if (out_path && out_size) out_path[0] = '\0';
        return;
    }

    // Drop a leading slash — Deltarune & co. pass paths like "/chapter3_windows"
    // expecting them to be sibling-directory names. If we leave the slash in,
    // path joins produce "BASE_DIR//chapter3_windows" which several SDMC
    // implementations treat as invalid.
    const char *clean = request;
    while (*clean == '/' || *clean == '\\') clean++;

    // Absolute sdmc:/ path — accept verbatim (well, after directory probe).
    if (strncmp(clean, "sdmc:/", 6) == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", clean);
        if (try_resolve_candidate(buf, out_path, out_size)) return;
        // Fall through and let caller report failure.
        strncpy(out_path, buf, out_size - 1);
        out_path[out_size - 1] = '\0';
        return;
    }

    // Compute the directory the current game lives in (parent of data.win).
    char cur_dir[256];
    {
        const char *slash = strrchr(g_current_data_path, '/');
        size_t baselen = slash ? (size_t)(slash - g_current_data_path) : 0;
        if (baselen >= sizeof(cur_dir)) baselen = sizeof(cur_dir) - 1;
        memcpy(cur_dir, g_current_data_path, baselen);
        cur_dir[baselen] = '\0';
    }

    // Compute the directory ABOVE the current game (where sibling chapters live).
    char parent_dir[256];
    {
        const char *slash = strrchr(cur_dir, '/');
        size_t baselen = slash ? (size_t)(slash - cur_dir) : 0;
        if (baselen >= sizeof(parent_dir)) baselen = sizeof(parent_dir) - 1;
        memcpy(parent_dir, cur_dir, baselen);
        parent_dir[baselen] = '\0';
    }

    // Strip any trailing data.win the request might already carry, so we can
    // probe both as a directory and as an explicit data.win path.
    char clean_req[256];
    snprintf(clean_req, sizeof(clean_req), "%s", clean);
    {
        size_t n = strlen(clean_req);
        const char *suffix = "/data.win";
        size_t s = strlen(suffix);
        if (n >= s && strcmp(clean_req + n - s, suffix) == 0) {
            clean_req[n - s] = '\0';
        }
    }

    char buf[512];

    // 1) Sibling of the current game: <parent>/chapter3_windows[/data.win]
    if (parent_dir[0]) {
        if (try_resolve_joined_candidate(parent_dir, clean_req, out_path, out_size)) return;
    }

    // 2) Subdir of the current game: <current>/chapter3_windows[/data.win]
    if (cur_dir[0]) {
        if (try_resolve_joined_candidate(cur_dir, clean_req, out_path, out_size)) return;
    }

    // 3) Top-level games root: <BASE_DIR>/chapter3_windows[/data.win]
    if (try_resolve_joined_candidate(BASE_DIR, clean_req, out_path, out_size)) return;

    // 4) Last resort: scan the registered launcher games for a name match
    //    (case-insensitive substring). Lets the user keep their own folder
    //    naming (e.g. "deltarune_ch3") even if the script asks for
    //    "chapter3_windows" — as long as one substring matches the other we
    //    accept it.
    for (int i = 0; i < g_game_count; i++) {
        const char *gname = g_games[i].name;
        if (!gname || !*gname) continue;
        bool match = false;
        if (ci_strstr(gname, clean_req) || ci_strstr(clean_req, gname)) match = true;
        if (match && try_resolve_candidate(g_games[i].path, out_path, out_size)) {
            return;
        }
    }

    // Nothing matched. Return a "best guess" path with /data.win appended so
    // the caller's open probe + error message at least show a meaningful path.
    snprintf(buf, sizeof(buf), "%s/%s/data.win", BASE_DIR, clean_req);
    strncpy(out_path, buf, out_size - 1);
    out_path[out_size - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Vertex batch primitives
// ---------------------------------------------------------------------------

static void launcher_gfx_flush(LauncherGfx *gfx) {
    if (!gfx->batchVerts || !gfx->batchTex || !gfx->inFrame) {
        gfx->batchVerts = 0;
        gfx->batchTex   = NULL;
        return;
    }
    GSPGPU_FlushDataCache(gfx->vbuf + gfx->batchStart,
                          gfx->batchVerts * sizeof(LauncherVertex));
    C3D_TexBind(0, gfx->batchTex);
    C3D_DrawArrays(GPU_TRIANGLES, gfx->batchStart, gfx->batchVerts);
    gfx->batchStart += gfx->batchVerts;
    gfx->batchVerts = 0;
    gfx->batchTex   = NULL;
}

static LauncherVertex *launcher_gfx_reserve(LauncherGfx *gfx, uint32_t count, C3D_Tex *tex) {
    if (gfx->batchTex && gfx->batchTex != tex) launcher_gfx_flush(gfx);
    if (gfx->vbufHead + count > gfx->vbufCap) {
        launcher_gfx_flush(gfx);
        C3D_FrameSplit(0);
        gfx->vbufHead   = 0;
        gfx->batchStart = 0;

        // citro3d does NOT preserve render target / pipeline state across FrameSplit.
        // Re-attach the current target and re-bind program/attrInfo/bufInfo/texenv/proj
        // so the next draw call lands on the right screen instead of bleeding into
        // whichever target citro3d last had bound.
        if (gfx->currentScreen && gfx->currentScreen->target) {
            C3D_FrameDrawOn(gfx->currentScreen->target);

            C3D_BindProgram(&g_shaderProg);
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

            C3D_SetViewport(0, 0,
                            (u32) gfx->currentScreen->logicalH,
                            (u32) gfx->currentScreen->logicalW);
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

            C3D_Mtx proj;
            Mtx_Identity(&proj);
            Mtx_OrthoTilt(&proj, 0.f, (float) gfx->currentScreen->logicalW,
                          (float) gfx->currentScreen->logicalH, 0.f, -1.f, 1.f, true);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gfx->uLoc_projection, &proj);
        }
    }
    gfx->batchTex = tex;
    LauncherVertex *v = gfx->vbuf + gfx->vbufHead;
    gfx->vbufHead   += count;
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

// ---------------------------------------------------------------------------
// Pixel font
// ---------------------------------------------------------------------------

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
    static const uint8_t lt[7]   = {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
    static const uint8_t gt[7]   = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
    static const uint8_t plus[7] = {0,0x04,0x04,0x1F,0x04,0x04,0};
    static const uint8_t lpar[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
    static const uint8_t rpar[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
    static const uint8_t lbr[7]  = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E};
    static const uint8_t rbr[7]  = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E};
    static const uint8_t eq[7]   = {0,0,0x1F,0,0x1F,0,0};
    static const uint8_t hash[7] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A};

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
        case '<': return lt;   case '>': return gt;   case '+': return plus;
        case '(': return lpar; case ')': return rpar; case '[': return lbr; case ']': return rbr;
        case '=': return eq;   case '#': return hash;
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

static void launcher_draw_text_rgb(LauncherGfx *gfx, const char *text, float x, float y,
                                   float scale, const float c[3], float a) {
    launcher_draw_text(gfx, text, x, y, scale, c[0], c[1], c[2], a);
}

static void launcher_draw_centered_text_rgb(LauncherGfx *gfx, const char *text, float centerX, float y,
                                            float scale, const float c[3], float a) {
    launcher_draw_centered_text(gfx, text, centerX, y, scale, c[0], c[1], c[2], a);
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void launcher_gfx_destroy(LauncherGfx *gfx) {
    if (!gfx) return;
    if (gfx->inFrame) {
        launcher_gfx_flush(gfx);
        C3D_FrameEnd(0);
        gfx->inFrame = false;
    }
    if (gfx->whiteTex.data) C3D_TexDelete(&gfx->whiteTex);
    if (gfx->vbuf) linearFree(gfx->vbuf);
    if (gfx->topScreen.owns && gfx->topScreen.target) C3D_RenderTargetDelete(gfx->topScreen.target);
    if (gfx->bottomScreen.owns && gfx->bottomScreen.target) C3D_RenderTargetDelete(gfx->bottomScreen.target);
    memset(gfx, 0, sizeof(*gfx));
}

static bool launcher_gfx_init_common(LauncherGfx *gfx) {
    gfx->uLoc_projection = shaderInstanceGetUniformLocation(g_shaderProg.vertexShader, "projection");

    AttrInfo_Init(&gfx->attrInfo);
    AttrInfo_AddLoader(&gfx->attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&gfx->attrInfo, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&gfx->attrInfo, 2, GPU_FLOAT, 4);

    gfx->vbufCap = LAUNCHER_VBUF_CAP;
    gfx->vbuf    = linearAlloc(gfx->vbufCap * sizeof(LauncherVertex));
    if (!gfx->vbuf) return false;

    if (!C3D_TexInit(&gfx->whiteTex, 8, 8, GPU_RGBA4)) return false;
    uint16_t *white = linearAlloc(8 * 8 * sizeof(uint16_t));
    if (!white) return false;
    for (int i = 0; i < 64; i++) white[i] = 0xFFFF;
    C3D_TexLoadImage(&gfx->whiteTex, white, GPU_TEXFACE_2D, 0);
    C3D_TexFlush(&gfx->whiteTex);
    C3D_TexSetFilter(&gfx->whiteTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&gfx->whiteTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    linearFree(white);

    gfx->ready = true;
    return true;
}

bool launcher_gfx_init(LauncherGfx *gfx) {
    memset(gfx, 0, sizeof(*gfx));

    gfx->topScreen.target = C3D_RenderTargetCreate(LAUNCHER_TOP_H, LAUNCHER_TOP_W,
                                                   GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (!gfx->topScreen.target) goto fail;
    C3D_RenderTargetSetOutput(gfx->topScreen.target, GFX_TOP, GFX_LEFT, LAUNCHER_DISPLAY_TRANSFER_FLAGS);
    gfx->topScreen.owns     = true;
    gfx->topScreen.ready    = true;
    gfx->topScreen.logicalW = LAUNCHER_TOP_W;
    gfx->topScreen.logicalH = LAUNCHER_TOP_H;

    gfx->bottomScreen.target = C3D_RenderTargetCreate(LAUNCHER_BOT_H, LAUNCHER_BOT_W,
                                                      GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (!gfx->bottomScreen.target) goto fail;
    C3D_RenderTargetSetOutput(gfx->bottomScreen.target, GFX_BOTTOM, GFX_LEFT, LAUNCHER_DISPLAY_TRANSFER_FLAGS);
    gfx->bottomScreen.owns     = true;
    gfx->bottomScreen.ready    = true;
    gfx->bottomScreen.logicalW = LAUNCHER_BOT_W;
    gfx->bottomScreen.logicalH = LAUNCHER_BOT_H;

    if (!launcher_gfx_init_common(gfx)) goto fail;
    return true;

fail:
    launcher_gfx_destroy(gfx);
    return false;
}

bool launcher_gfx_init_borrowed(LauncherGfx *gfx,
                                C3D_RenderTarget *topTarget, int topW, int topH,
                                C3D_RenderTarget *bottomTarget, int bottomW, int bottomH) {
    memset(gfx, 0, sizeof(*gfx));

    if (topTarget) {
        gfx->topScreen.target   = topTarget;
        gfx->topScreen.owns     = false;
        gfx->topScreen.ready    = true;
        gfx->topScreen.logicalW = topW;
        gfx->topScreen.logicalH = topH;
    }
    if (bottomTarget) {
        gfx->bottomScreen.target   = bottomTarget;
        gfx->bottomScreen.owns     = false;
        gfx->bottomScreen.ready    = true;
        gfx->bottomScreen.logicalW = bottomW;
        gfx->bottomScreen.logicalH = bottomH;
    }

    if (!launcher_gfx_init_common(gfx)) {
        launcher_gfx_destroy(gfx);
        return false;
    }
    return true;
}

static bool launcher_begin_frame(LauncherGfx *gfx) {
    if (!gfx->ready) return false;
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return false;
    gfx->inFrame    = true;
    gfx->vbufHead   = 0;
    gfx->batchStart = 0;
    gfx->batchVerts = 0;
    gfx->batchTex   = NULL;
    gfx->currentScreen = NULL;

    C3D_BindProgram(&g_shaderProg);
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
    return true;
}

static void launcher_bind_screen(LauncherGfx *gfx, LauncherScreen *scr,
                                 bool clear, uint32_t clearColor) {
    if (!scr || !scr->ready || !scr->target) return;
    bool switchingTarget = gfx->currentScreen && gfx->currentScreen != scr;
    launcher_gfx_flush(gfx);
    if (switchingTarget) C3D_FrameSplit(0);

    gfx->currentScreen = scr;
    gfx->batchStart   = gfx->vbufHead;

    if (clear) {
        C3D_RenderTargetClear(scr->target, C3D_CLEAR_ALL, clearColor, 0);
    }
    C3D_FrameDrawOn(scr->target);

    // Re-bind the full pipeline state — citro3d does NOT preserve all of this
    // across C3D_FrameDrawOn switches, so without this the second screen renders
    // through a half-configured pipeline (corrupt colours, wrong UVs).
    C3D_BindProgram(&g_shaderProg);
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

    C3D_SetViewport(0, 0, (u32)scr->logicalH, (u32)scr->logicalW);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

    C3D_Mtx proj;
    Mtx_Identity(&proj);
    Mtx_OrthoTilt(&proj, 0.f, (float)scr->logicalW, (float)scr->logicalH, 0.f, -1.f, 1.f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gfx->uLoc_projection, &proj);
}

static void launcher_end_frame(LauncherGfx *gfx) {
    launcher_gfx_flush(gfx);
    C3D_FrameEnd(0);
    gfx->inFrame = false;
    gfx->currentScreen = NULL;
}

// ---------------------------------------------------------------------------
// Animation timer
// ---------------------------------------------------------------------------

static u64 g_launcher_t0_ms = 0;
float launcher_anim_seconds(void) {
    if (g_launcher_t0_ms == 0) g_launcher_t0_ms = osGetTime();
    return (float)(osGetTime() - g_launcher_t0_ms) * 0.001f;
}

// ---------------------------------------------------------------------------
// Themed background (works on any screen size)
// ---------------------------------------------------------------------------

static void launcher_draw_themed_background(LauncherGfx *gfx, float t,
                                            float w, float h,
                                            const LauncherTheme *th) {
    float warm = 0.5f + 0.5f * sinf(t * 0.35f);
    float deep = 0.5f + 0.5f * sinf(t * 0.27f + 1.1f);

    float top[4] = { th->bg_top[0] + 0.06f * warm,
                     th->bg_top[1] + 0.04f * deep,
                     th->bg_top[2] + 0.06f * warm, 1.f };
    float mid[4] = { th->bg_mid[0] + 0.04f * deep,
                     th->bg_mid[1] + 0.03f * warm,
                     th->bg_mid[2] + 0.04f * deep, 1.f };
    float bot[4] = { th->bg_bot[0],
                     th->bg_bot[1] + 0.01f * warm,
                     th->bg_bot[2] + 0.03f * deep, 1.f };

    launcher_push_quad_grad(gfx, &gfx->whiteTex,
                            0, 0, w, 0, w, h * 0.55f, 0, h * 0.55f,
                            .5f, .5f, .5f, .5f, top, top, mid, mid);
    launcher_push_quad_grad(gfx, &gfx->whiteTex,
                            0, h * 0.55f, w, h * 0.55f,
                            w, h, 0, h,
                            .5f, .5f, .5f, .5f, mid, mid, bot, bot);

    if (g_settings.show_side_blur) {
        for (int i = 0; i < 6; i++) {
            float band_y = fmodf(t * 8.f + (float)i * 67.f, h + 80.f) - 40.f;
            float a = 0.030f + 0.020f * sinf(t * 0.6f + (float)i);
            launcher_push_quad(gfx, &gfx->whiteTex,
                               -20.f, band_y, w + 40.f, 18.f,
                               .5f, .5f, .5f, .5f,
                               (float[4]){th->accent[0], th->accent[1], th->accent[2], a});
        }
    }

    if (g_settings.show_side_particles) {
        const float *hues[3] = { th->particle_a, th->particle_b, th->particle_c };
        for (int i = 0; i < 64; i++) {
            float seed  = (float)i * 12.91f;
            float orbit = 18.f + (float)(i % 5) * 9.f;
            float ang   = t * (0.18f + (float)(i & 3) * 0.07f) + seed;
            float baseX = fmodf(seed * 17.3f, w);
            float baseY = fmodf(seed * 11.7f + t * (3.f + (float)(i % 4) * 1.2f), h + 40.f) - 20.f;
            float x = baseX + cosf(ang) * orbit;
            float y = baseY + sinf(ang * 1.3f) * orbit * 0.45f;
            float s = 1.4f + (float)(i % 6) * 0.55f;
            float a = 0.16f + (sinf(t * 1.4f + seed) + 1.f) * 0.10f;
            const float *c = hues[i % 3];
            launcher_rect(gfx, x, y, s, s, c[0], c[1], c[2], a);
        }
    }
}

static void launcher_draw_top_chrome(LauncherGfx *gfx, float w, const LauncherTheme *th, float t,
                                     const char *title) {
    launcher_rect(gfx, 0, 0,   w, 26, 0.04f, 0.035f, 0.085f, 0.92f);
    launcher_rect(gfx, 0, 26,  w, 1,
                  th->accent[0], th->accent[1], th->accent[2], 0.95f);
    launcher_rect(gfx, 0, 27,  w, 1,
                  th->accent_dim[0], th->accent_dim[1], th->accent_dim[2], 0.55f);

    launcher_rect(gfx, 0, 215, w, 25, 0.030f, 0.028f, 0.065f, 0.93f);
    launcher_rect(gfx, 0, 215, w, 1,
                  th->accent[0], th->accent[1], th->accent[2], 0.95f);
    launcher_rect(gfx, 0, 216, w, 1,
                  th->accent_dim[0], th->accent_dim[1], th->accent_dim[2], 0.55f);

    float titlePulse = 0.85f + 0.15f * (0.5f + 0.5f * sinf(t * 1.6f));
    launcher_draw_text(gfx, title, 10, 9, 1.35f,
                       th->text_title[0] * titlePulse,
                       th->text_title[1] * titlePulse,
                       th->text_title[2] * titlePulse,
                       1.f);
}

// ---------------------------------------------------------------------------
// Top-screen menu
// ---------------------------------------------------------------------------

static void launcher_draw_scrollbar(LauncherGfx *gfx, float cam_y, float max_y, const LauncherTheme *th) {
    if (max_y <= 0.f) return;
    float trackX = 392.f, trackY = 34.f, trackW = 4.f, trackH = 174.f;
    launcher_rect(gfx, trackX, trackY, trackW, trackH, 0.08f, 0.07f, 0.14f, 0.75f);
    float thumbH = trackH * (trackH / (trackH + max_y));
    if (thumbH < 12.f) thumbH = 12.f;
    float thumbY = trackY + (trackH - thumbH) * (cam_y / max_y);
    launcher_rect(gfx, trackX, thumbY, trackW, thumbH,
                  th->accent[0], th->accent[1], th->accent[2], 1.f);
}

static void launcher_draw_icon_or_placeholder(LauncherGfx *gfx, const LauncherGameEntry *game,
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

static void launcher_render_grid(LauncherGfx *gfx, int selected, float t, float cam_y, float select_anim) {
    const LauncherTheme *th = launcher_current_theme();
    const float W = (float)LAUNCHER_TOP_W;
    const float H = (float)LAUNCHER_TOP_H;

    launcher_bind_screen(gfx, &gfx->topScreen, true, 0x070914FF);
    launcher_draw_themed_background(gfx, t, W, H, th);

    const float tileSize = 76.f;
    const float gapX = 88.f;
    const float gapY = 92.f;
    const float gridX = 30.f;
    const float gridY = 36.f;
    const int cols = 4;
    const float visibleH = H - gridY - 28.f;
    int rows = (g_game_count + cols - 1) / cols;
    float maxY = rows * gapY - visibleH;
    if (maxY < 0.f) maxY = 0.f;

    for (int i = 0; i < g_game_count; i++) {
        int col = i % cols;
        int row = i / cols;
        float x = gridX + (float)col * gapX;
        float y = gridY + (float)row * gapY - cam_y;
        if (y < -tileSize - 12.f || y > H + 12.f) continue;

        bool sel = (i == selected);

        launcher_rect(gfx, x + 4, y + 5, tileSize, tileSize, 0.f, 0.f, 0.f, 0.30f);

        if (sel) {
            float breath = 0.5f + 0.5f * sinf(t * 2.4f);
            float halo = 4.f + breath * 3.f + (1.f - select_anim) * 6.f;
            launcher_rect(gfx, x - 8 - halo, y - 8 - halo,
                          tileSize + 16 + halo * 2, tileSize + 16 + halo * 2,
                          th->accent[0], th->accent[1], th->accent[2], 0.10f + 0.10f * breath);
            launcher_rect(gfx, x - 4 - halo * 0.5f, y - 4 - halo * 0.5f,
                          tileSize + 8 + halo, tileSize + 8 + halo,
                          th->accent[0], th->accent[1], th->accent[2], 0.20f + 0.10f * breath);
            launcher_rect(gfx, x - 3, y - 3, tileSize + 6, tileSize + 6,
                          th->accent[0], th->accent[1], th->accent[2], 1.f);
            launcher_rect(gfx, x - 2, y - 2, tileSize + 4, tileSize + 4,
                          th->accent_dim[0], th->accent_dim[1], th->accent_dim[2], 1.f);
        } else {
            launcher_rect(gfx, x - 2, y - 2, tileSize + 4, tileSize + 4,
                          0.17f, 0.14f, 0.28f, 0.82f);
            launcher_rect(gfx, x - 1, y - 1, tileSize + 2, tileSize + 2,
                          0.05f, 0.04f, 0.10f, 0.85f);
        }

        launcher_rect(gfx, x, y, tileSize, tileSize, 0.065f, 0.055f, 0.12f, 1.f);
        launcher_draw_icon_or_placeholder(gfx, &g_games[i], x + 6, y + 6, tileSize - 12, i);

        if (sel) {
            float gleam = 0.18f + 0.10f * (0.5f + 0.5f * sinf(t * 3.1f));
            launcher_rect(gfx, x, y, tileSize, 2.f, 1.0f, 0.94f, 0.62f, gleam);
        }
    }

    launcher_draw_scrollbar(gfx, cam_y, maxY, th);

    char title[48];
    snprintf(title, sizeof(title), "%.36s",
             (selected >= 0 && selected < g_game_count) ? g_games[selected].name : "");
    float scale = 2.0f;
    float width = launcher_text_width(title, scale);
    if (width > 360.f) scale *= 360.f / width;
    launcher_draw_centered_text_rgb(gfx, title, 200.f, 224.f, scale, th->text_main, 1.f);

    launcher_draw_top_chrome(gfx, W, th, t, LAUNCHER_APP_TITLE);
}

// ---------------------------------------------------------------------------
// Bottom-screen detail panel
// ---------------------------------------------------------------------------

static void launcher_draw_bottom_panel(LauncherGfx *gfx, int selected, float t,
                                       const char *footer1, const char *footer2) {
    const LauncherTheme *th = launcher_current_theme();
    const float W = (float)LAUNCHER_BOT_W;
    const float H = (float)LAUNCHER_BOT_H;

    if (!gfx->bottomScreen.ready) return;

    launcher_bind_screen(gfx, &gfx->bottomScreen, true, 0x050711FF);
    launcher_draw_themed_background(gfx, t * 0.7f, W, H, th);

    // Header band
    launcher_rect(gfx, 0, 0, W, 22, 0.04f, 0.035f, 0.085f, 0.92f);
    launcher_rect(gfx, 0, 22, W, 1,
                  th->accent[0], th->accent[1], th->accent[2], 0.85f);
    launcher_rect(gfx, 0, 23, W, 1,
                  th->accent_dim[0], th->accent_dim[1], th->accent_dim[2], 0.55f);
    launcher_draw_text_rgb(gfx, "GAME INFO", 10, 7, 1.20f, th->text_title, 1.f);

    // Footer band
    launcher_rect(gfx, 0, H - 22, W, 22, 0.030f, 0.028f, 0.065f, 0.93f);
    launcher_rect(gfx, 0, H - 23, W, 1,
                  th->accent[0], th->accent[1], th->accent[2], 0.85f);

    const LauncherGameEntry *game = (selected >= 0 && selected < g_game_count) ? &g_games[selected] : NULL;

    // Big icon
    float iconSize = 96.f;
    float ix = 16.f;
    float iy = 38.f;
    launcher_rect(gfx, ix - 3, iy - 3, iconSize + 6, iconSize + 6,
                  th->accent[0], th->accent[1], th->accent[2], 0.95f);
    launcher_rect(gfx, ix - 2, iy - 2, iconSize + 4, iconSize + 4, 0.05f, 0.04f, 0.10f, 1.f);
    launcher_rect(gfx, ix, iy, iconSize, iconSize, 0.065f, 0.055f, 0.12f, 1.f);
    if (game) {
        launcher_draw_icon_or_placeholder(gfx, game, ix + 6, iy + 6, iconSize - 12, selected);
    }

    // Text column
    float tx = ix + iconSize + 14.f;
    float ty = iy;
    char buf[64];

    if (game) {
        snprintf(buf, sizeof(buf), "%.20s", game->name);
        launcher_draw_text_rgb(gfx, buf, tx, ty, 1.30f, th->text_main, 1.f);
        ty += 22.f;
        snprintf(buf, sizeof(buf), "#%d / %d", selected + 1, g_game_count);
        launcher_draw_text_rgb(gfx, buf, tx, ty, 1.10f, th->text_subtle, 0.95f);
        ty += 18.f;
        launcher_draw_text_rgb(gfx, "DATA.WIN OK", tx, ty, 1.00f, th->text_subtle, 0.85f);
        ty += 16.f;
        launcher_draw_text_rgb(gfx, game->icon_ready ? "ICON LOADED" : "DEFAULT ICON",
                               tx, ty, 1.00f, th->text_subtle, 0.85f);
        ty += 22.f;
    } else {
        launcher_draw_text_rgb(gfx, "NO GAME", tx, ty, 1.40f, th->text_main, 1.f);
        ty += 24.f;
    }

    launcher_draw_text_rgb(gfx, "THEME:", tx, ty, 1.00f, th->text_subtle, 0.80f);
    launcher_draw_text_rgb(gfx, th->display, tx + 50.f, ty, 1.00f, th->text_main, 1.f);
    ty += 16.f;
    launcher_draw_text_rgb(gfx, g_settings.game_screen == LAUNCHER_GAME_SCREEN_TOP
                                  ? "PLAY ON: TOP" : "PLAY ON: BOTTOM",
                           tx, ty, 1.00f, th->text_subtle, 0.80f);

    // Footer hint text
    if (footer1) launcher_draw_text_rgb(gfx, footer1, 6, H - 17, 0.95f, th->text_main, 0.95f);
    if (footer2) launcher_draw_text_rgb(gfx, footer2, 6, H - 9, 0.95f, th->text_subtle, 0.85f);
}

// ---------------------------------------------------------------------------
// Cache clearing
// ---------------------------------------------------------------------------

static void launcher_dirname(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char *slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) : 0;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static bool launcher_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool launcher_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int launcher_remove_tree(const char *path) {
    if (!launcher_path_exists(path)) return 0;
    if (!launcher_is_dir(path)) return remove(path) == 0 ? 1 : -1;

    DIR *dir = opendir(path);
    if (!dir) return -1;

    int removed = 0;
    int errors = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        int rc = launcher_remove_tree(child);
        if (rc < 0) errors++;
        else removed += rc;
    }
    closedir(dir);

    if (rmdir(path) != 0) errors++;
    else removed++;
    return errors ? -1 : removed;
}

static void launcher_clear_cache_scan(const char *dir_path, int depth, int *cleared, int *errors) {
    if (!dir_path || depth > 6 || !launcher_is_dir(dir_path)) return;

    char data_win[512];
    snprintf(data_win, sizeof(data_win), "%s/data.win", dir_path);
    if (launcher_path_exists(data_win)) {
        char cache_path[512];
        snprintf(cache_path, sizeof(cache_path), "%s/cache", dir_path);
        if (launcher_path_exists(cache_path)) {
            int rc = launcher_remove_tree(cache_path);
            if (rc < 0) (*errors)++;
            else (*cleared)++;
        }
    }

    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, "cache") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name);
        if (launcher_is_dir(child)) launcher_clear_cache_scan(child, depth + 1, cleared, errors);
    }
    closedir(dir);
}

static void launcher_clear_cache_for_game(const LauncherGameEntry *game, int *cleared, int *errors) {
    if (cleared) *cleared = 0;
    if (errors) *errors = 0;
    if (!game) return;

    char game_dir[512];
    launcher_dirname(game->path, game_dir, sizeof(game_dir));
    int local_cleared = 0;
    int local_errors = 0;
    launcher_clear_cache_scan(game_dir, 0, &local_cleared, &local_errors);
    if (cleared) *cleared = local_cleared;
    if (errors) *errors = local_errors;
}

static void launcher_draw_cache_message(LauncherGfx *gfx, const char *title,
                                        const char *line1, const char *line2,
                                        const char *footer, float t) {
    const LauncherTheme *th = launcher_current_theme();

    launcher_bind_screen(gfx, &gfx->topScreen, true, 0x050711FF);
    launcher_draw_themed_background(gfx, t, LAUNCHER_TOP_W, LAUNCHER_TOP_H, th);
    launcher_draw_top_chrome(gfx, LAUNCHER_TOP_W, th, t, LAUNCHER_APP_TITLE);
    launcher_draw_centered_text_rgb(gfx, title, LAUNCHER_TOP_W * 0.5f, 92.f, 1.7f, th->text_title, 1.f);
    if (line1) launcher_draw_centered_text_rgb(gfx, line1, LAUNCHER_TOP_W * 0.5f, 124.f, 1.15f, th->text_main, 0.95f);
    if (line2) launcher_draw_centered_text_rgb(gfx, line2, LAUNCHER_TOP_W * 0.5f, 144.f, 1.0f, th->text_subtle, 0.9f);

    launcher_bind_screen(gfx, &gfx->bottomScreen, true, 0x050711FF);
    launcher_draw_themed_background(gfx, t * 0.7f, LAUNCHER_BOT_W, LAUNCHER_BOT_H, th);
    launcher_rect(gfx, 16, 56, LAUNCHER_BOT_W - 32, 92, 0.04f, 0.035f, 0.085f, 0.92f);
    launcher_draw_centered_text_rgb(gfx, title, LAUNCHER_BOT_W * 0.5f, 76.f, 1.35f, th->text_title, 1.f);
    if (line1) launcher_draw_centered_text_rgb(gfx, line1, LAUNCHER_BOT_W * 0.5f, 104.f, 1.0f, th->text_main, 0.95f);
    if (line2) launcher_draw_centered_text_rgb(gfx, line2, LAUNCHER_BOT_W * 0.5f, 122.f, 0.92f, th->text_subtle, 0.9f);
    if (footer) launcher_draw_centered_text_rgb(gfx, footer, LAUNCHER_BOT_W * 0.5f, 220.f, 0.95f, th->text_subtle, 0.95f);
}

static bool launcher_confirm_clear_cache(LauncherGfx *gfx, const LauncherGameEntry *game) {
    char line1[96];
    snprintf(line1, sizeof(line1), "CLEAR CACHE FOR %.28s?", game ? game->name : "GAME");

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) return true;
        if (kDown & (KEY_B | KEY_X | KEY_START)) return false;

        float t = (float)osGetTime() * 0.001f;
        if (gfx && gfx->ready && launcher_begin_frame(gfx)) {
            launcher_draw_cache_message(gfx, "CLEAR CACHE", line1,
                                        "A CONFIRM   B CANCEL", "THIS ONLY DELETES CACHE FOLDERS", t);
            launcher_end_frame(gfx);
        }
    }
    return false;
}

bool launcher_confirm_quit_to_launcher(LauncherGfx *gfx) {
    while (aptMainLoop()) {
        hidScanInput();
        if (!(hidKeysHeld() & KEY_START)) break;
        float t = (float)osGetTime() * 0.001f;
        if (gfx && gfx->ready && launcher_begin_frame(gfx)) {
            launcher_draw_cache_message(gfx, "QUIT TO LAUNCHER",
                                        "RETURN TO BUTTERSCOTCH MENU?",
                                        "A CONFIRM   B CANCEL",
                                        "GAME STATE IS NOT SAVED", t);
            launcher_end_frame(gfx);
        }
        gspWaitForVBlank();
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_A) return true;
        if (kDown & (KEY_B | KEY_START)) return false;

        float t = (float)osGetTime() * 0.001f;
        if (gfx && gfx->ready && launcher_begin_frame(gfx)) {
            launcher_draw_cache_message(gfx, "QUIT TO LAUNCHER",
                                        "RETURN TO BUTTERSCOTCH MENU?",
                                        "A CONFIRM   B CANCEL",
                                        "GAME STATE IS NOT SAVED", t);
            launcher_end_frame(gfx);
        }
        gspWaitForVBlank();
    }
    return false;
}

static void launcher_show_cache_result(LauncherGfx *gfx, int cleared, int errors) {
    char line1[80];
    char line2[80];
    snprintf(line1, sizeof(line1), "CACHE FOLDERS CLEARED: %d", cleared);
    snprintf(line2, sizeof(line2), errors ? "ERRORS: %d" : "NO ERRORS", errors);

    uint64_t start = osGetTime();
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if ((kDown & (KEY_A | KEY_B | KEY_START)) || osGetTime() - start > 1800) break;

        float t = (float)osGetTime() * 0.001f;
        if (gfx && gfx->ready && launcher_begin_frame(gfx)) {
            launcher_draw_cache_message(gfx, errors ? "CACHE DONE" : "CACHE CLEARED",
                                        line1, line2, "A/B TO RETURN", t);
            launcher_end_frame(gfx);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings screen (top + bottom)
// ---------------------------------------------------------------------------

static void launcher_vk_label(int vk, char *out, size_t size) {
    if (vk == VK_NOKEY) { snprintf(out, size, "DISABLED"); return; }
    if (vk == VK_ANYKEY) { snprintf(out, size, "ANYKEY"); return; }
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, size, "%c", (char)vk); return; }
    if (vk >= '0' && vk <= '9') { snprintf(out, size, "%c", (char)vk); return; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(out, size, "F%d", vk - VK_F1 + 1); return; }

    switch (vk) {
        case VK_BACKSPACE: snprintf(out, size, "BACKSPACE"); break;
        case VK_TAB:       snprintf(out, size, "TAB"); break;
        case VK_ENTER:     snprintf(out, size, "ENTER"); break;
        case VK_SHIFT:     snprintf(out, size, "SHIFT"); break;
        case VK_CONTROL:   snprintf(out, size, "CTRL"); break;
        case VK_ALT:       snprintf(out, size, "ALT"); break;
        case VK_ESCAPE:    snprintf(out, size, "ESC"); break;
        case VK_SPACE:     snprintf(out, size, "SPACE"); break;
        case VK_PAGEUP:    snprintf(out, size, "PAGEUP"); break;
        case VK_PAGEDOWN:  snprintf(out, size, "PAGEDOWN"); break;
        case VK_END:       snprintf(out, size, "END"); break;
        case VK_HOME:      snprintf(out, size, "HOME"); break;
        case VK_LEFT:      snprintf(out, size, "LEFT"); break;
        case VK_UP:        snprintf(out, size, "UP"); break;
        case VK_RIGHT:     snprintf(out, size, "RIGHT"); break;
        case VK_DOWN:      snprintf(out, size, "DOWN"); break;
        case VK_INSERT:    snprintf(out, size, "INSERT"); break;
        case VK_DELETE:    snprintf(out, size, "DELETE"); break;
        default:           snprintf(out, size, "VK%03d", vk & 255); break;
    }
}

static void launcher_step_control_vk(LauncherControlMap *map, int idx, int delta) {
    if (!map || idx < 0 || idx >= LAUNCHER_CONTROL_COUNT) return;
    int vk = (int)map->vk[idx] + delta;
    while (vk < 0) vk += GML_KEY_COUNT;
    while (vk >= GML_KEY_COUNT) vk -= GML_KEY_COUNT;
    map->vk[idx] = (uint8_t)vk;
}

static void controls_draw_top(LauncherGfx *gfx, const LauncherControlMap *map,
                              int selected, const char *title, float t) {
    const LauncherTheme *th = launcher_current_theme();
    const float W = (float)LAUNCHER_TOP_W;
    const int visible = 8;
    int first = selected - visible / 2;
    if (first < 0) first = 0;
    if (first > LAUNCHER_CONTROL_COUNT - visible) first = LAUNCHER_CONTROL_COUNT - visible;
    if (first < 0) first = 0;

    launcher_bind_screen(gfx, &gfx->topScreen, true, 0x070914FF);
    launcher_draw_themed_background(gfx, t, W, LAUNCHER_TOP_H, th);
    launcher_draw_top_chrome(gfx, W, th, t, title ? title : "CONTROLS");

    float listX = 34.f;
    float listY = 42.f;
    float rowH = 21.f;
    char value[32];
    char code[16];
    for (int row = 0; row < visible; row++) {
        int idx = first + row;
        if (idx >= LAUNCHER_CONTROL_COUNT) break;
        bool selRow = (idx == selected);
        float y = listY + (float)row * rowH;
        if (selRow) {
            float breath = 0.5f + 0.5f * sinf(t * 3.0f);
            launcher_rect(gfx, listX - 8, y - 3, W - listX * 2 + 16, rowH - 3,
                          th->accent[0], th->accent[1], th->accent[2],
                          0.18f + 0.10f * breath);
            launcher_rect(gfx, listX - 8, y - 3, 4, rowH - 3,
                          th->accent[0], th->accent[1], th->accent[2], 1.f);
        }

        launcher_vk_label(map->vk[idx], value, sizeof(value));
        snprintf(code, sizeof(code), "%03u", (unsigned)map->vk[idx]);
        launcher_draw_text_rgb(gfx, g_control_defs[idx].label, listX, y, 1.12f,
                               selRow ? th->text_main : th->text_subtle, 1.f);
        float vw = launcher_text_width(value, 1.12f);
        launcher_draw_text_rgb(gfx, value, W - 78.f - vw, y, 1.12f,
                               selRow ? th->text_title : th->text_main, 1.f);
        launcher_draw_text_rgb(gfx, code, W - 54.f, y, 1.0f, th->text_subtle, 0.75f);
    }

    launcher_draw_centered_text_rgb(gfx, "A/START SAVE   B BACK   SELECT DEFAULTS",
                                    W * 0.5f, 224.f, 0.95f, th->text_subtle, 0.95f);
}

static void controls_draw_bottom(LauncherGfx *gfx, const LauncherControlMap *map,
                                 int selected, float t) {
    if (!gfx->bottomScreen.ready) return;
    const LauncherTheme *th = launcher_current_theme();
    const float W = (float)LAUNCHER_BOT_W;
    const float H = (float)LAUNCHER_BOT_H;
    char vkName[32];
    launcher_vk_label(map->vk[selected], vkName, sizeof(vkName));

    launcher_bind_screen(gfx, &gfx->bottomScreen, true, 0x050711FF);
    launcher_draw_themed_background(gfx, t * 0.7f, W, H, th);

    launcher_rect(gfx, 0, 0, W, 22, 0.04f, 0.035f, 0.085f, 0.92f);
    launcher_rect(gfx, 0, 22, W, 1,
                  th->accent[0], th->accent[1], th->accent[2], 0.85f);
    launcher_draw_text_rgb(gfx, "VK REMAP", 10, 7, 1.20f, th->text_title, 1.f);

    launcher_draw_centered_text_rgb(gfx, g_control_defs[selected].label,
                                    W * 0.5f, 62.f, 1.65f, th->text_main, 1.f);
    launcher_draw_centered_text_rgb(gfx, vkName, W * 0.5f, 96.f,
                                    1.95f, th->text_title, 1.f);

    char code[32];
    snprintf(code, sizeof(code), "VK CODE %03u", (unsigned)map->vk[selected]);
    launcher_draw_centered_text_rgb(gfx, code, W * 0.5f, 126.f,
                                    1.10f, th->text_subtle, 0.92f);

    launcher_rect(gfx, 0, H - 30, W, 30, 0.030f, 0.028f, 0.065f, 0.93f);
    launcher_draw_text_rgb(gfx, "DPAD: MOVE   L/R: +/-1", 8, H - 23, 0.95f, th->text_main, 0.95f);
    launcher_draw_text_rgb(gfx, "X/Y: +/-10   000 DISABLES", 8, H - 12, 0.95f, th->text_subtle, 0.85f);
}

static bool launcher_run_control_mapper(LauncherGfx *gfx, LauncherControlMap *target,
                                        const char *title) {
    if (!target) return false;
    LauncherControlMap draft = *target;
    launcher_normalize_control_map(&draft);
    int selected = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_B) return false;
        if (kDown & (KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN)) {
            selected = (selected + 1) % LAUNCHER_CONTROL_COUNT;
        }
        if (kDown & (KEY_UP | KEY_DUP | KEY_CPAD_UP)) {
            selected = (selected - 1 + LAUNCHER_CONTROL_COUNT) % LAUNCHER_CONTROL_COUNT;
        }
        if (kDown & (KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R)) {
            launcher_step_control_vk(&draft, selected, +1);
        }
        if (kDown & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_L)) {
            launcher_step_control_vk(&draft, selected, -1);
        }
        if (kDown & KEY_X) launcher_step_control_vk(&draft, selected, +10);
        if (kDown & KEY_Y) launcher_step_control_vk(&draft, selected, -10);
        if (kDown & KEY_SELECT) launcher_reset_control_map(&draft);
        if (kDown & (KEY_A | KEY_START)) {
            *target = draft;
            launcher_normalize_control_map(target);
            return true;
        }

        if (gfx && gfx->ready && launcher_begin_frame(gfx)) {
            float now = launcher_anim_seconds();
            controls_draw_top(gfx, &draft, selected, title, now);
            controls_draw_bottom(gfx, &draft, selected, now);
            launcher_end_frame(gfx);
        }
        gspWaitForVBlank();
    }
    return false;
}

typedef struct {
    int          theme_index;
    LauncherGameScreen game_screen;
    int          show_side_particles;
    int          show_side_blur;
    LauncherBackdropMode backdrop_mode;
    LauncherDisplayMode display_mode;
    LauncherAppFilterMode app_filter;
    int          os_type;
    LauncherInputMode input_mode;
} LauncherSettingsDraft;

enum {
    SETTINGS_OPT_THEME = 0,
    SETTINGS_OPT_GAME_SCREEN,
    SETTINGS_OPT_DISPLAY_MODE,
    SETTINGS_OPT_APP_FILTER,
    SETTINGS_OPT_BACKDROP,
    SETTINGS_OPT_PARTICLES,
    SETTINGS_OPT_BLUR,
    SETTINGS_OPT_OS_TYPE,
    SETTINGS_OPT_INPUT_MODE,
    SETTINGS_OPT_CONTROLS,
    SETTINGS_OPTION_COUNT
};

static const char *settings_option_label(int idx) {
    switch (idx) {
        case SETTINGS_OPT_THEME:        return "THEME";
        case SETTINGS_OPT_GAME_SCREEN:  return "GAME SCREEN";
        case SETTINGS_OPT_DISPLAY_MODE: return "DISPLAY MODE";
        case SETTINGS_OPT_APP_FILTER:   return "APP FILTER";
        case SETTINGS_OPT_BACKDROP:     return "EMPTY SPACE";
        case SETTINGS_OPT_PARTICLES:    return "SIDE PARTICLES";
        case SETTINGS_OPT_BLUR:         return "SIDE BLUR";
        case SETTINGS_OPT_OS_TYPE:      return "OS TYPE";
        case SETTINGS_OPT_INPUT_MODE:   return "INPUT MODE";
        case SETTINGS_OPT_CONTROLS:     return "GLOBAL CONTROLS";
        default: return "?";
    }
}

static void settings_option_value(const LauncherSettingsDraft *d, int idx, char *out, size_t size) {
    switch (idx) {
        case SETTINGS_OPT_THEME:
            snprintf(out, size, "< %s >", launcher_theme_at(d->theme_index)->display);
            break;
        case SETTINGS_OPT_GAME_SCREEN:
            snprintf(out, size, "< %s >", d->game_screen == LAUNCHER_GAME_SCREEN_TOP ? "TOP" : "BOTTOM");
            break;
        case SETTINGS_OPT_DISPLAY_MODE:
            snprintf(out, size, "< %s >", launcher_display_mode_label(d->display_mode));
            break;
        case SETTINGS_OPT_APP_FILTER:
            snprintf(out, size, "< %s >", launcher_app_filter_label(d->app_filter));
            break;
        case SETTINGS_OPT_BACKDROP:
            snprintf(out, size, "< %s >", launcher_backdrop_label(d->backdrop_mode));
            break;
        case SETTINGS_OPT_PARTICLES:
            snprintf(out, size, "< %s >", d->show_side_particles ? "ON" : "OFF");
            break;
        case SETTINGS_OPT_BLUR:
            snprintf(out, size, "< %s >", d->show_side_blur ? "ON" : "OFF");
            break;
        case SETTINGS_OPT_OS_TYPE:
            snprintf(out, size, "< %s >", launcher_os_type_label(d->os_type));
            break;
        case SETTINGS_OPT_INPUT_MODE:
            snprintf(out, size, "< %s >", launcher_input_mode_label(d->input_mode));
            break;
        case SETTINGS_OPT_CONTROLS:
            snprintf(out, size, "PRESS A");
            break;
        default:
            snprintf(out, size, "?");
            break;
    }
}

static void settings_option_step(LauncherSettingsDraft *d, int idx, int dir) {
    switch (idx) {
        case SETTINGS_OPT_THEME: {
            int next = d->theme_index + dir;
            int n = launcher_theme_count();
            if (next < 0) next = n - 1;
            if (next >= n) next = 0;
            d->theme_index = next;
            break;
        }
        case SETTINGS_OPT_GAME_SCREEN:
            d->game_screen = (d->game_screen == LAUNCHER_GAME_SCREEN_TOP)
                                 ? LAUNCHER_GAME_SCREEN_BOTTOM : LAUNCHER_GAME_SCREEN_TOP;
            break;
        case SETTINGS_OPT_DISPLAY_MODE:
            d->display_mode = launcher_next_display_mode(d->display_mode, dir);
            break;
        case SETTINGS_OPT_APP_FILTER:
            d->app_filter = launcher_next_app_filter(d->app_filter, dir);
            break;
        case SETTINGS_OPT_BACKDROP:
            d->backdrop_mode = launcher_next_backdrop(d->backdrop_mode, dir);
            break;
        case SETTINGS_OPT_PARTICLES:
            d->show_side_particles = !d->show_side_particles;
            break;
        case SETTINGS_OPT_BLUR:
            d->show_side_blur = !d->show_side_blur;
            break;
        case SETTINGS_OPT_OS_TYPE: {
            int idx_now = launcher_os_type_index_of(d->os_type);
            if (idx_now < 0) idx_now = 0;
            int n = launcher_os_type_count();
            int next = (idx_now + dir + n) % n;
            d->os_type = launcher_os_type_at(next);
            break;
        }
        case SETTINGS_OPT_INPUT_MODE: {
            int next = ((int)d->input_mode + dir + LAUNCHER_INPUT_MODE_COUNT)
                       % LAUNCHER_INPUT_MODE_COUNT;
            d->input_mode = (LauncherInputMode)next;
            break;
        }
        case SETTINGS_OPT_CONTROLS:
            break;
        default: break;
    }
}

static void settings_draw_top(LauncherGfx *gfx, const LauncherSettingsDraft *d, int sel, float t) {
    const LauncherTheme *th = launcher_theme_at(d->theme_index);
    const float W = (float)LAUNCHER_TOP_W;
    const float H = (float)LAUNCHER_TOP_H;

    launcher_bind_screen(gfx, &gfx->topScreen, true, 0x070914FF);
    launcher_draw_themed_background(gfx, t, W, H, th);
    launcher_draw_top_chrome(gfx, W, th, t, "SETTINGS");

    float listX = 36.f;
    float listY = 44.f;
    float rowH  = 19.f;

    char value[48];
    for (int i = 0; i < SETTINGS_OPTION_COUNT; i++) {
        bool selRow = (i == sel);
        float ry = listY + (float)i * rowH;
        if (selRow) {
            float breath = 0.5f + 0.5f * sinf(t * 3.0f);
            launcher_rect(gfx, listX - 8, ry - 4, W - listX * 2 + 16, rowH - 4,
                          th->accent[0], th->accent[1], th->accent[2], 0.18f + 0.10f * breath);
            launcher_rect(gfx, listX - 8, ry - 4, 4, rowH - 4,
                          th->accent[0], th->accent[1], th->accent[2], 1.f);
        }
        launcher_draw_text_rgb(gfx, settings_option_label(i), listX, ry, 1.30f,
                               selRow ? th->text_main : th->text_subtle, 1.f);
        settings_option_value(d, i, value, sizeof(value));
        float vw = launcher_text_width(value, 1.30f);
        launcher_draw_text_rgb(gfx, value, W - listX - vw, ry, 1.30f,
                               selRow ? th->text_title : th->text_main, 1.f);
    }

    launcher_draw_centered_text_rgb(gfx, "A EDIT/SAVE   B BACK   START APPLY",
                                    W * 0.5f, 224.f, 1.05f, th->text_subtle, 0.95f);
}

static void settings_draw_bottom(LauncherGfx *gfx, const LauncherSettingsDraft *d, float t) {
    const LauncherTheme *th = launcher_theme_at(d->theme_index);
    const float W = (float)LAUNCHER_BOT_W;
    const float H = (float)LAUNCHER_BOT_H;

    if (!gfx->bottomScreen.ready) return;

    launcher_bind_screen(gfx, &gfx->bottomScreen, true, 0x050711FF);
    launcher_draw_themed_background(gfx, t * 0.7f, W, H, th);

    launcher_rect(gfx, 0, 0, W, 22, 0.04f, 0.035f, 0.085f, 0.92f);
    launcher_rect(gfx, 0, 22, W, 1,
                  th->accent[0], th->accent[1], th->accent[2], 0.85f);
    launcher_draw_text_rgb(gfx, "PREVIEW", 10, 7, 1.20f, th->text_title, 1.f);

    // Theme preview swatches
    float sx = 16.f, sy = 36.f, sw = 22.f, gap = 6.f;
    const float *swatches[5] = {
        th->accent, th->particle_a, th->particle_b, th->particle_c, th->text_main
    };
    for (int i = 0; i < 5; i++) {
        const float *c = swatches[i];
        launcher_rect(gfx, sx + i * (sw + gap), sy, sw, sw, c[0], c[1], c[2], 1.f);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%s", th->display);
    launcher_draw_text_rgb(gfx, buf, 16.f, sy + sw + 14.f, 1.40f, th->text_main, 1.f);

    snprintf(buf, sizeof(buf), "PLAY ON: %s",
             d->game_screen == LAUNCHER_GAME_SCREEN_TOP ? "TOP SCREEN" : "BOTTOM SCREEN");
    launcher_draw_text_rgb(gfx, buf, 16.f, sy + sw + 38.f, 1.10f, th->text_subtle, 0.95f);

    snprintf(buf, sizeof(buf), "DISPLAY: %s   FILTER: %s",
             launcher_display_mode_label(d->display_mode),
             launcher_app_filter_label(d->app_filter));
    launcher_draw_text_rgb(gfx, buf, 16.f, sy + sw + 54.f, 1.05f, th->text_subtle, 0.90f);

    snprintf(buf, sizeof(buf), "SPACE: %s", launcher_backdrop_label(d->backdrop_mode));
    launcher_draw_text_rgb(gfx, buf, 16.f, sy + sw + 70.f, 1.00f, th->text_subtle, 0.82f);

    snprintf(buf, sizeof(buf), "PARTICLES: %s   BLUR: %s",
             d->show_side_particles ? "ON" : "OFF",
             d->show_side_blur ? "ON" : "OFF");
    launcher_draw_text_rgb(gfx, buf, 16.f, sy + sw + 86.f, 1.00f, th->text_subtle, 0.80f);

    snprintf(buf, sizeof(buf), "OS: %s   INPUT: %s",
             launcher_os_type_label(d->os_type),
             launcher_input_mode_label(d->input_mode));
    launcher_draw_text_rgb(gfx, buf, 16.f, sy + sw + 102.f, 1.00f, th->text_subtle, 0.80f);

    launcher_rect(gfx, 0, H - 22, W, 22, 0.030f, 0.028f, 0.065f, 0.93f);
    launcher_draw_text_rgb(gfx, "DPAD: NAVIGATE  L/R: CHANGE  A: EDIT/SAVE", 6, H - 17, 0.95f, th->text_main, 1.f);
    launcher_draw_text_rgb(gfx, "B: CANCEL  Y: RESET DEFAULTS",         6, H - 9,  0.95f, th->text_subtle, 0.85f);
}

static bool launcher_run_settings(LauncherGfx *gfx) {
    LauncherSettingsDraft draft = {
        .theme_index         = g_settings.theme_index,
        .game_screen         = g_settings.game_screen,
        .show_side_particles = g_settings.show_side_particles,
        .show_side_blur      = g_settings.show_side_blur,
        .backdrop_mode       = g_settings.backdrop_mode,
        .display_mode        = g_settings.display_mode,
        .app_filter          = g_settings.app_filter,
        .os_type             = g_settings.os_type,
        .input_mode          = g_settings.input_mode,
    };
    int sel = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_B) return false; // cancelled
        if (kDown & (KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN)) {
            sel = (sel + 1) % SETTINGS_OPTION_COUNT;
        }
        if (kDown & (KEY_UP | KEY_DUP | KEY_CPAD_UP)) {
            sel = (sel - 1 + SETTINGS_OPTION_COUNT) % SETTINGS_OPTION_COUNT;
        }
        if (kDown & (KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R)) {
            settings_option_step(&draft, sel, +1);
        }
        if (kDown & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_L)) {
            settings_option_step(&draft, sel, -1);
        }
        if (kDown & KEY_Y) {
            draft.theme_index = 0;
            draft.game_screen = LAUNCHER_GAME_SCREEN_TOP;
            draft.show_side_particles = 1;
            draft.show_side_blur = 1;
            draft.backdrop_mode = LAUNCHER_BACKDROP_GRADIENT;
            draft.display_mode = LAUNCHER_DISPLAY_ORIGINAL;
            draft.app_filter = LAUNCHER_APP_FILTER_LINEAR;
            draft.os_type = OS_WINDOWS;
            draft.input_mode = LAUNCHER_INPUT_KEYBOARD;
        }
        if ((kDown & KEY_A) && sel == SETTINGS_OPT_CONTROLS) {
            if (launcher_run_control_mapper(gfx, &g_settings.global_controls, "GLOBAL CONTROLS")) {
                launcher_save_settings();
            }
            continue;
        }
        if ((kDown & KEY_START) || ((kDown & KEY_A) && sel != SETTINGS_OPT_CONTROLS)) {
            g_settings.theme_index = draft.theme_index;
            g_settings.game_screen = draft.game_screen;
            g_settings.show_side_particles = draft.show_side_particles;
            g_settings.show_side_blur = draft.show_side_blur;
            g_settings.backdrop_mode = draft.backdrop_mode;
            g_settings.display_mode = draft.display_mode;
            g_settings.app_filter = draft.app_filter;
            g_settings.os_type = draft.os_type;
            g_settings.input_mode = draft.input_mode;
            launcher_push_theme_to_renderer();
            launcher_save_settings();
            return true;
        }

        if (gfx && gfx->ready && launcher_begin_frame(gfx)) {
            float t = launcher_anim_seconds();
            settings_draw_top(gfx, &draft, sel, t);
            settings_draw_bottom(gfx, &draft, t);
            launcher_end_frame(gfx);
        }
        gspWaitForVBlank();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main launcher menu
// ---------------------------------------------------------------------------

int launcher_run_menu(LauncherGfx *gfx) {
    launcher_scan_games();
    bool gfx_ready = (gfx && gfx->ready);

    if (g_game_count == 0) {
        while (aptMainLoop()) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_START) return -1;
            if (kDown & KEY_SELECT) {
                if (launcher_run_settings(gfx)) {
                    // settings might allow re-scan on close - nothing else to do
                }
            }
            if (gfx_ready && launcher_begin_frame(gfx)) {
                float t = launcher_anim_seconds();
                const LauncherTheme *th = launcher_current_theme();
                launcher_bind_screen(gfx, &gfx->topScreen, true, 0x070914FF);
                launcher_draw_themed_background(gfx, t, LAUNCHER_TOP_W, LAUNCHER_TOP_H, th);
                launcher_draw_top_chrome(gfx, LAUNCHER_TOP_W, th, t, LAUNCHER_APP_TITLE);
                launcher_draw_centered_text_rgb(gfx, "NO GAMES FOUND", 200.f, 98.f, 2.25f, th->text_title, 1.f);
                launcher_draw_centered_text_rgb(gfx, "SDMC:/3DS/BUTTERSCOTCH", 200.f, 132.f, 1.45f, th->text_subtle, 0.9f);
                launcher_draw_bottom_panel(gfx, -1, t,
                                           "SELECT = SETTINGS    START = QUIT",
                                           "PLACE GAMES IN SDMC:/3DS/BUTTERSCOTCH");
                launcher_end_frame(gfx);
            }
            gspWaitForVBlank();
        }
        return -1;
    }

    int selected = 0;
    float cam_y = 0.f;
    float select_anim = 1.f;

    const float tile_gap_y = 92.f;
    const float grid_y = 36.f;
    const float visible_h = (float)LAUNCHER_TOP_H - grid_y - 28.f;
    const int cols = 4;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) return -1;
        if (kDown & (KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT)) { selected++; select_anim = 0.f; }
        if (kDown & (KEY_LEFT  | KEY_DLEFT  | KEY_CPAD_LEFT))  { selected--; select_anim = 0.f; }
        if (kDown & (KEY_DOWN  | KEY_DDOWN  | KEY_CPAD_DOWN))  { selected += cols; select_anim = 0.f; }
        if (kDown & (KEY_UP    | KEY_DUP    | KEY_CPAD_UP))    { selected -= cols; select_anim = 0.f; }
        if (kDown & KEY_R)                                     { selected += cols * 2; select_anim = 0.f; }
        if (kDown & KEY_L)                                     { selected -= cols * 2; select_anim = 0.f; }
        if (selected < 0)              selected = 0;
        if (selected >= g_game_count)  selected = g_game_count - 1;
        if (kDown & KEY_A) return selected;
        if (kDown & KEY_X) {
            if (launcher_confirm_clear_cache(gfx, &g_games[selected])) {
                int cleared = 0;
                int errors = 0;
                launcher_clear_cache_for_game(&g_games[selected], &cleared, &errors);
                launcher_show_cache_result(gfx, cleared, errors);
            }
        }
        if (kDown & KEY_SELECT) {
            launcher_run_settings(gfx);
        }

        int sel_row = selected / cols;
        float target_y = (float)sel_row * tile_gap_y - visible_h * 0.40f;
        int max_rows = (g_game_count + cols - 1) / cols;
        float max_y = (float)max_rows * tile_gap_y - visible_h;
        if (max_y < 0.f) max_y = 0.f;
        if (target_y < 0.f) target_y = 0.f;
        if (target_y > max_y) target_y = max_y;
        cam_y += (target_y - cam_y) * 0.28f;
        select_anim += (1.f - select_anim) * 0.22f;

        if (gfx_ready && launcher_begin_frame(gfx)) {
            float t = launcher_anim_seconds();
            launcher_render_grid(gfx, selected, t, cam_y, select_anim);
            launcher_draw_bottom_panel(gfx, selected, t,
                                       "A = LAUNCH   X = CLEAR CACHE   SELECT = SETTINGS",
                                       "START = QUIT   DPAD/CPAD = MOVE   L/R = PAGE");
            launcher_end_frame(gfx);
        }
        gspWaitForVBlank();
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Loading screen
// ---------------------------------------------------------------------------

static void loading_draw_top(LauncherGfx *gfx, const char *gameName, const char *stage,
                             int page, int total, float percent, float t) {
    const LauncherTheme *th = launcher_current_theme();
    const float W = (float)LAUNCHER_TOP_W;
    const float H = (float)LAUNCHER_TOP_H;

    launcher_bind_screen(gfx, &gfx->topScreen, true, 0x070914FF);
    launcher_draw_themed_background(gfx, t, W, H, th);

    launcher_draw_centered_text_rgb(gfx, LAUNCHER_APP_TITLE, W * 0.5f, 48.f, 2.0f, th->text_title, 1.f);

    char title[48];
    snprintf(title, sizeof(title), "%.36s", gameName ? gameName : "GAME");
    float titleScale = 1.8f;
    float titleW = launcher_text_width(title, titleScale);
    if (titleW > 350.f) titleScale *= 350.f / titleW;
    launcher_draw_centered_text_rgb(gfx, title, W * 0.5f, 82.f, titleScale, th->text_subtle, 0.95f);

    char stageText[64];
    int dots = ((int)(t * 2.5f)) % 4;
    snprintf(stageText, sizeof(stageText), "%s%s",
             stage ? stage : "LOADING",
             (dots == 0) ? "" : (dots == 1 ? "." : (dots == 2 ? ".." : "...")));
    launcher_draw_centered_text(gfx, stageText, W * 0.5f, 112.f, 1.55f, 1.f, 1.f, 1.f, 0.95f);

    float barX = 42.f, barY = 148.f, barW = 316.f, barH = 14.f;
    launcher_rect(gfx, barX - 2, barY - 2, barW + 4, barH + 4, 0.06f, 0.055f, 0.10f, 0.95f);
    launcher_rect(gfx, barX, barY, barW, barH, 0.12f, 0.11f, 0.18f, 1.f);

    float fillW = barW * (percent / 100.f);
    if (fillW > 0.f) {
        launcher_rect(gfx, barX, barY, fillW, barH,
                      th->accent[0], th->accent[1], th->accent[2], 1.f);
        float shimmerX = barX + fmodf(t * 80.f, fillW + 60.f) - 30.f;
        for (int i = 0; i < 6; i++) {
            float sx = shimmerX + (float)i * 4.f;
            if (sx < barX || sx + 3.f > barX + fillW) continue;
            float a = 0.30f - (float)i * 0.04f;
            launcher_rect(gfx, sx, barY, 3.f, barH,
                          th->text_title[0], th->text_title[1] * 0.96f, th->text_title[2] * 0.74f, a);
        }
        launcher_rect(gfx, barX, barY, fillW, 1.f,
                      th->text_title[0], th->text_title[1] * 0.96f, th->text_title[2] * 0.66f, 0.85f);
    }

    char status[48];
    snprintf(status, sizeof(status), "%d%%", (int)(percent + 0.5f));
    launcher_draw_centered_text_rgb(gfx, status, W * 0.5f, 174.f, 1.7f, th->text_main, 1.f);

    if (total > 0) {
        char pageText[48];
        snprintf(pageText, sizeof(pageText), "PAGE %d / %d", page, total);
        launcher_draw_centered_text_rgb(gfx, pageText, W * 0.5f, 198.f, 1.25f, th->text_subtle, 0.88f);
    }
}

static void loading_draw_bottom(LauncherGfx *gfx, const char *gameName, const char *stage, float t) {
    const LauncherTheme *th = launcher_current_theme();
    const float W = (float)LAUNCHER_BOT_W;
    const float H = (float)LAUNCHER_BOT_H;

    if (!gfx->bottomScreen.ready) return;

    launcher_bind_screen(gfx, &gfx->bottomScreen, true, 0x050711FF);
    launcher_draw_themed_background(gfx, t * 0.7f, W, H, th);

    launcher_draw_centered_text_rgb(gfx, "PREPARING", W * 0.5f, 64.f, 1.45f, th->text_title, 1.f);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.30s", gameName ? gameName : "GAME");
    launcher_draw_centered_text_rgb(gfx, buf, W * 0.5f, 96.f, 1.30f, th->text_main, 1.f);

    snprintf(buf, sizeof(buf), "%.30s", stage ? stage : "LOADING");
    launcher_draw_centered_text_rgb(gfx, buf, W * 0.5f, 122.f, 1.10f, th->text_subtle, 0.95f);

    launcher_draw_centered_text_rgb(gfx, "PRESS NOTHING. JUST CHILL.", W * 0.5f, 178.f,
                                    1.05f, th->text_subtle, 0.75f);
}

void launcher_render_loading(LauncherGfx *gfx, const char *gameName, const char *stage,
                             int page, int total, float percent) {
    if (!gfx || !gfx->ready || !launcher_begin_frame(gfx)) return;
    if (percent < 0.f)   percent = 0.f;
    if (percent > 100.f) percent = 100.f;

    float t = launcher_anim_seconds();
    loading_draw_top(gfx, gameName, stage, page, total, percent, t);
    loading_draw_bottom(gfx, gameName, stage, t);
    launcher_end_frame(gfx);
}

// ---------------------------------------------------------------------------
// In-game pause overlay
// ---------------------------------------------------------------------------

#define PAUSE_OPTION_COUNT 10

static int pause_option_count(bool allowDebugMode) {
    return allowDebugMode ? PAUSE_OPTION_COUNT : PAUSE_OPTION_COUNT - 1;
}

static int pause_option_index(int visibleIndex, bool allowDebugMode) {
    return (!allowDebugMode && visibleIndex >= 7) ? visibleIndex + 1 : visibleIndex;
}

static const char *pause_option_label(int idx) {
    switch (idx) {
        case 0: return "RESUME";
        case 1: return "THEME";
        case 2: return "GAME SCREEN";
        case 3: return "DISPLAY MODE";
        case 4: return "EMPTY SPACE";
        case 5: return "SIDE PARTICLES";
        case 6: return "SIDE BLUR";
        case 7: return "DEBUG MODE";
        case 8: return "GAME CONTROLS";
        case 9: return "QUIT TO LAUNCHER";
        default: return "?";
    }
}

static void pause_option_value(int idx, char *out, size_t size) {
    switch (idx) {
        case 0:
            snprintf(out, size, "PRESS A");
            break;
        case 1:
            snprintf(out, size, "< %s >", launcher_current_theme()->display);
            break;
        case 2:
            snprintf(out, size, "< %s >",
                     g_settings.game_screen == LAUNCHER_GAME_SCREEN_TOP ? "TOP" : "BOTTOM");
            break;
        case 3:
            snprintf(out, size, "< %s >", launcher_display_mode_label(g_settings.display_mode));
            break;
        case 4:
            snprintf(out, size, "< %s >", launcher_backdrop_label(g_settings.backdrop_mode));
            break;
        case 5:
            snprintf(out, size, "< %s >", g_settings.show_side_particles ? "ON" : "OFF");
            break;
        case 6:
            snprintf(out, size, "< %s >", g_settings.show_side_blur ? "ON" : "OFF");
            break;
        case 7:
            snprintf(out, size, "< %s >", g_settings.debug_mode ? "ON" : "OFF");
            break;
        case 8:
            snprintf(out, size, "PRESS A");
            break;
        case 9:
            snprintf(out, size, "PRESS A");
            break;
        default:
            snprintf(out, size, "?");
            break;
    }
}

static bool pause_option_step(int idx, int dir) {
    switch (idx) {
        case 1: {
            int next = g_settings.theme_index + dir;
            int n = launcher_theme_count();
            if (next < 0) next = n - 1;
            if (next >= n) next = 0;
            launcher_apply_theme_index(next);
            return true;
        }
        case 2:
            g_settings.game_screen = (g_settings.game_screen == LAUNCHER_GAME_SCREEN_TOP)
                                         ? LAUNCHER_GAME_SCREEN_BOTTOM
                                         : LAUNCHER_GAME_SCREEN_TOP;
            launcher_push_theme_to_renderer();
            return true;
        case 3:
            g_settings.display_mode = launcher_next_display_mode(g_settings.display_mode, dir);
            launcher_push_theme_to_renderer();
            return true;
        case 4:
            g_settings.backdrop_mode = launcher_next_backdrop(g_settings.backdrop_mode, dir);
            launcher_push_theme_to_renderer();
            return true;
        case 5:
            g_settings.show_side_particles = !g_settings.show_side_particles;
            launcher_push_theme_to_renderer();
            return true;
        case 6:
            g_settings.show_side_blur = !g_settings.show_side_blur;
            launcher_push_theme_to_renderer();
            return true;
        case 7:
            g_settings.debug_mode = !g_settings.debug_mode;
            return true;
        case 8:
            return false;
        default:
            return false;
    }
}

static void pause_draw_overlay(LauncherGfx *gfx, LauncherScreen *scr, int sel, float t,
                               const char *headline, bool allowDebugMode) {
    if (!scr || !scr->ready) return;
    const LauncherTheme *themePtr = launcher_current_theme();
    LauncherTheme theme = themePtr ? *themePtr : g_themes[0];
    const LauncherTheme *th = &theme;

    launcher_bind_screen(gfx, scr, false, 0);

    float W = (float)scr->logicalW;
    float H = (float)scr->logicalH;

    // Dim everything underneath
    launcher_rect(gfx, 0, 0, W, H, 0.0f, 0.0f, 0.0f, 0.55f);

    // Card
    float cardW = W * 0.78f;
    float cardH = H * 0.78f;
    float cardX = (W - cardW) * 0.5f;
    float cardY = (H - cardH) * 0.5f;
    launcher_rect(gfx, cardX - 4, cardY - 4, cardW + 8, cardH + 8,
                  th->accent[0], th->accent[1], th->accent[2], 0.85f);
    launcher_rect(gfx, cardX, cardY, cardW, cardH, 0.04f, 0.035f, 0.085f, 0.95f);

    // Header strip
    launcher_rect(gfx, cardX, cardY, cardW, 22.f,
                  th->accent_dim[0] * 0.6f, th->accent_dim[1] * 0.6f, th->accent_dim[2] * 0.6f, 0.95f);
    launcher_draw_text_rgb(gfx, headline, cardX + 10, cardY + 7, 1.20f, th->text_title, 1.f);

    float listX = cardX + 14.f;
    float listY = cardY + 32.f;
    float rowH  = 16.f;
    char value[48];

    int optionCount = pause_option_count(allowDebugMode);
    for (int i = 0; i < optionCount; i++) {
        int option = pause_option_index(i, allowDebugMode);
        float ry = listY + (float)i * rowH;
        bool selRow = (i == sel);
        if (selRow) {
            float breath = 0.5f + 0.5f * sinf(t * 3.0f);
            launcher_rect(gfx, listX - 6, ry - 3, cardW - 16.f, rowH - 2,
                          th->accent[0], th->accent[1], th->accent[2], 0.18f + 0.10f * breath);
            launcher_rect(gfx, listX - 6, ry - 3, 3, rowH - 2,
                          th->accent[0], th->accent[1], th->accent[2], 1.f);
        }
        launcher_draw_text_rgb(gfx, pause_option_label(option), listX, ry, 1.0f,
                               selRow ? th->text_main : th->text_subtle, 1.f);
        pause_option_value(option, value, sizeof(value));
        float vw = launcher_text_width(value, 1.0f);
        launcher_draw_text_rgb(gfx, value, cardX + cardW - 14.f - vw, ry, 1.0f,
                               selRow ? th->text_title : th->text_main, 1.f);
    }

    launcher_draw_centered_text_rgb(gfx, "L/R CHANGE   A SELECT   B RESUME",
                                    cardX + cardW * 0.5f, cardY + cardH - 18.f,
                                    0.95f, th->text_subtle, 0.95f);
}

LauncherPauseAction launcher_run_pause(LauncherGfx *gfx, bool allowDebugMode) {
    if (!gfx || !gfx->ready) return LAUNCHER_PAUSE_RESUME;

    int sel = 0;
    int optionCount = pause_option_count(allowDebugMode);

    // Wait for the chord-trigger keys to be released so we don't immediately bounce.
    while (aptMainLoop()) {
        hidScanInput();
        u32 held = hidKeysHeld();
        if (!(held & (KEY_A | KEY_START | KEY_SELECT | KEY_L | KEY_R))) break;
        gspWaitForVBlank();
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_B) return LAUNCHER_PAUSE_RESUME;
        if (kDown & (KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN)) {
            sel = (sel + 1) % optionCount;
        }
        if (kDown & (KEY_UP | KEY_DUP | KEY_CPAD_UP)) {
            sel = (sel - 1 + optionCount) % optionCount;
        }
        if (kDown & (KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R)) {
            pause_option_step(pause_option_index(sel, allowDebugMode), +1);
        }
        if (kDown & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_L)) {
            pause_option_step(pause_option_index(sel, allowDebugMode), -1);
        }
        if (kDown & KEY_A) {
            int option = pause_option_index(sel, allowDebugMode);
            if (option == 0) {
                launcher_save_settings();
                return LAUNCHER_PAUSE_RESUME;
            } else if (option == 8) {
                if (launcher_run_control_mapper(gfx, &g_active_controls, "GAME CONTROLS")) {
                    launcher_save_active_controls();
                }
                continue;
            } else if (option == 9) {
                launcher_save_settings();
                return LAUNCHER_PAUSE_QUIT_TO_LAUNCHER;
            } else {
                pause_option_step(option, +1);
            }
        }
        if (kDown & KEY_START) {
            launcher_save_settings();
            return LAUNCHER_PAUSE_RESUME;
        }

        if (launcher_begin_frame(gfx)) {
            float t = launcher_anim_seconds();
            // The companion screen shows a themed banner so the device looks
            // intentional while the player is fiddling with options.
            LauncherScreen *overlay = (g_settings.game_screen == LAUNCHER_GAME_SCREEN_TOP)
                                          ? &gfx->topScreen
                                          : &gfx->bottomScreen;
            LauncherScreen *companion = (g_settings.game_screen == LAUNCHER_GAME_SCREEN_TOP)
                                            ? &gfx->bottomScreen
                                            : &gfx->topScreen;

            if (companion && companion->ready) {
                const LauncherTheme *th = launcher_current_theme();
                launcher_bind_screen(gfx, companion, true, 0x050711FF);
                launcher_draw_themed_background(gfx, t * 0.7f,
                                                (float)companion->logicalW,
                                                (float)companion->logicalH, th);
                launcher_draw_centered_text_rgb(gfx, "PAUSED",
                                                companion->logicalW * 0.5f,
                                                companion->logicalH * 0.5f - 16.f,
                                                2.4f, th->text_title, 1.f);
                launcher_draw_centered_text_rgb(gfx, "TWEAK ON THE OTHER SCREEN",
                                                companion->logicalW * 0.5f,
                                                companion->logicalH * 0.5f + 14.f,
                                                1.10f, th->text_subtle, 0.95f);
            }

            pause_draw_overlay(gfx, overlay, sel, t, "PAUSED", allowDebugMode);
            launcher_end_frame(gfx);
        }
        gspWaitForVBlank();
    }
    return LAUNCHER_PAUSE_RESUME;
}
