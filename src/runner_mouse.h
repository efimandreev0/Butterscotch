#pragma once

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

// GML mouse button constants (mb_*) — values match GameMaker.
#define MB_NONE    0
#define MB_LEFT    1
#define MB_RIGHT   2
#define MB_MIDDLE  3
#define MB_ANY    -1

// Indices 1..3 are used (mb_left/right/middle); index 0 stays unused so the
// constants can be passed directly into buttonDown[].
#define MOUSE_BUTTON_COUNT 4

typedef struct RunnerMouseState {
    int32_t x, y;
    bool buttonDown[MOUSE_BUTTON_COUNT];
    bool buttonDownPrev[MOUSE_BUTTON_COUNT];
    bool buttonPressed[MOUSE_BUTTON_COUNT];
    bool buttonReleased[MOUSE_BUTTON_COUNT];
} RunnerMouseState;

RunnerMouseState* RunnerMouse_create(void);
void RunnerMouse_free(RunnerMouseState* m);

// Snapshot current button state into Prev and clear pressed/released.
void RunnerMouse_beginFrame(RunnerMouseState* m);

// Platform setters.
void RunnerMouse_setPosition(RunnerMouseState* m, int32_t x, int32_t y);
void RunnerMouse_setButton(RunnerMouseState* m, int32_t mb, bool down);

// Recompute pressed/released from prev/current. Call once after all platform
// setters have run for the frame.
void RunnerMouse_endFrame(RunnerMouseState* m);

// GML queries — accept MB_LEFT/RIGHT/MIDDLE/ANY/NONE.
bool RunnerMouse_check(RunnerMouseState* m, int32_t mb);
bool RunnerMouse_checkPressed(RunnerMouseState* m, int32_t mb);
bool RunnerMouse_checkReleased(RunnerMouseState* m, int32_t mb);
