//
// Created by Notebook on 03.05.2026.
//

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
    char    name[64];
    char    path[256];
    char    exe_path[256];
    bool    icon_ready;
    int     icon_w, icon_h;
    int     icon_pot_w, icon_pot_h;
    C3D_Tex icon_tex;
} LauncherGameEntry;

typedef enum {
    LAUNCHER_GAME_SCREEN_TOP    = 0,
    LAUNCHER_GAME_SCREEN_BOTTOM = 1
} LauncherGameScreen;

typedef enum {
    LAUNCHER_BACKDROP_GRADIENT = 0,
    LAUNCHER_BACKDROP_BLUR,
    LAUNCHER_BACKDROP_BLACK,
    LAUNCHER_BACKDROP_STRETCH,
} LauncherBackdropMode;

typedef enum {
    LAUNCHER_DISPLAY_ORIGINAL = 0,
    LAUNCHER_DISPLAY_STRETCH,
    LAUNCHER_DISPLAY_3DS_WIDE,
    LAUNCHER_DISPLAY_MODE_COUNT
} LauncherDisplayMode;

typedef enum {
    LAUNCHER_APP_FILTER_LINEAR = 0,
    LAUNCHER_APP_FILTER_NEAREST,
    LAUNCHER_APP_FILTER_COUNT
} LauncherAppFilterMode;

typedef enum {
    LAUNCHER_FRAME_PACING_30 = 0,
    LAUNCHER_FRAME_PACING_60,
    LAUNCHER_FRAME_PACING_COUNT
} LauncherFramePacing;

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

#define LAUNCHER_CONTROL_MAP_MAGIC   0x4D545243u // 'CRTM'
#define LAUNCHER_CONTROL_MAP_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  vk[LAUNCHER_CONTROL_COUNT];
    uint8_t  _reserved[42];
} LauncherControlMap;

typedef struct {
    char  id[12];
    char  display[20];

    // Background gradient (used by launcher and renderer letterbox)
    float bg_top[3];
    float bg_mid[3];
    float bg_bot[3];

    // Accent colour (title bar, scrollbar thumb, selected halo)
    float accent[3];
    float accent_dim[3];

    // Three particle hues
    float particle_a[3];
    float particle_b[3];
    float particle_c[3];

    // Text colours
    float text_main[3];
    float text_title[3];
    float text_subtle[3];

    // Side decoration intensity (letterbox blur multiplier for renderer)
    float side_blur_alpha;
    float side_particle_alpha;
} LauncherTheme;

typedef enum {
    LAUNCHER_INPUT_KEYBOARD = 0,
    LAUNCHER_INPUT_GAMEPAD,
    LAUNCHER_INPUT_TOUCH,
    LAUNCHER_INPUT_MODE_COUNT
} LauncherInputMode;

#define LAUNCHER_SETTINGS_VERSION 7u

typedef struct {
    uint32_t           magic;
    uint32_t           version;
    int                theme_index;
    LauncherGameScreen game_screen;
    int                show_side_particles;
    int                show_side_blur;
    LauncherBackdropMode backdrop_mode;
    int                os_type;          // YoYoOperatingSystem value reported via os_type to GML
    LauncherInputMode  input_mode;
    LauncherDisplayMode  display_mode;
    LauncherAppFilterMode app_filter;
    LauncherFramePacing frame_pacing;
    int                _reserved[2];
    LauncherControlMap global_controls;
} LauncherSettings;

typedef struct {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} LauncherVertex;

typedef struct {
    C3D_RenderTarget *target;
    int               logicalW;
    int               logicalH;
    bool              owns;
    bool              ready;
} LauncherScreen;

typedef struct {
    int              uLoc_projection;
    C3D_AttrInfo     attrInfo;
    C3D_Tex          whiteTex;
    LauncherVertex  *vbuf;
    uint32_t         vbufCap;
    uint32_t         vbufHead;
    uint32_t         batchStart;
    uint32_t         batchVerts;
    C3D_Tex         *batchTex;

    LauncherScreen   topScreen;
    LauncherScreen   bottomScreen;

    LauncherScreen  *currentScreen;

    bool             ready;
    bool             inFrame;
} LauncherGfx;

typedef enum {
    LAUNCHER_PAUSE_RESUME = 0,
    LAUNCHER_PAUSE_QUIT_TO_LAUNCHER,
    LAUNCHER_PAUSE_QUIT_APP
} LauncherPauseAction;

// ---- Theme/settings public API ----------------------------------------------

const LauncherTheme *launcher_theme_at(int index);
int                  launcher_theme_count(void);
const LauncherTheme *launcher_current_theme(void);
const LauncherSettings *launcher_get_settings(void);
const LauncherControlMap *launcher_get_global_controls(void);
const LauncherControlMap *launcher_get_active_controls(void);
void                 launcher_apply_theme_index(int index);
void                 launcher_apply_settings(const LauncherSettings *s);
void                 launcher_save_settings(void);
void                 launcher_load_settings(void);
void                 launcher_load_active_controls(const char *data_win_path);
void                 launcher_save_active_controls(void);
void                 launcher_apply_3ds_input(RunnerKeyboardState *kb, u32 down, u32 up, u32 held);

// Populates gamepad slot 0 of `gp` from current 3DS hardware state. Treats the
// circle pad / c-stick as left/right analog axes and any held button as a
// digital button-down. Always marks the slot connected so games see one device.
void                 launcher_apply_3ds_gamepad(RunnerGamepadState *gp,
                                                u32 down, u32 up, u32 held,
                                                int circleX, int circleY,
                                                int cstickX, int cstickY);

// Feeds the bottom-screen touchscreen into the runner's mouse state. `gameW`/
// `gameH` are the logical game dimensions used to scale the 320x240 touch
// coords into game space. `touched` is non-zero if the user is currently
// touching the screen.
void                 launcher_apply_3ds_touch(RunnerMouseState *mouse,
                                              bool touched, int touchX, int touchY,
                                              int gameW, int gameH);

// Returns short labels for settings UI / debug.
const char          *launcher_os_type_label(int osType);
int                  launcher_os_type_count(void);
int                  launcher_os_type_at(int index);
int                  launcher_os_type_index_of(int osType);
const char          *launcher_input_mode_label(LauncherInputMode mode);

// ---- Library bootstrap ------------------------------------------------------

bool launcher_gfx_init(LauncherGfx *gfx);
bool launcher_gfx_init_borrowed(LauncherGfx *gfx,
                                C3D_RenderTarget *topTarget, int topW, int topH,
                                C3D_RenderTarget *bottomTarget, int bottomW, int bottomH);
void launcher_gfx_destroy(LauncherGfx *gfx);

// ---- Game-list helpers ------------------------------------------------------

void launcher_scan_games(void);
int  launcher_game_count(void);
const LauncherGameEntry *launcher_game(int index);
void launcher_free_game_icons(void);

// ---- Top-level screens ------------------------------------------------------

// Returns selected game index, or -1 to quit.
int  launcher_run_menu(LauncherGfx *gfx);

// Loading screen rendered on top + bottom.
void launcher_render_loading(LauncherGfx *gfx, const char *gameName, const char *stage,
                             int page, int total, float percent);

// In-game pause menu. Borrowed gfx that draws on whatever target is active.
LauncherPauseAction launcher_run_pause(LauncherGfx *gfx);

// ---- Helpers used by main and ctr_renderer ----------------------------------

float launcher_anim_seconds(void);

// Path resolution copied from main; placed here because launcher owns the menu state.
void launcher_resolve_new_game_path(const char *request, char *out_path, size_t out_size);

#endif //BUTTERSCOTCH_LAUNCHER_H
