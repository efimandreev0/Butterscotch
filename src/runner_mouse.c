// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "runner_mouse.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

RunnerMouseState* RunnerMouse_create(void) {
    return safeCalloc(1, sizeof(RunnerMouseState));
}

void RunnerMouse_free(RunnerMouseState* m) {
    free(m);
}

void RunnerMouse_beginFrame(RunnerMouseState* m) {
    if (!m) return;
    memcpy(m->buttonDownPrev, m->buttonDown, sizeof(m->buttonDown));
    memset(m->buttonPressed, 0, sizeof(m->buttonPressed));
    memset(m->buttonReleased, 0, sizeof(m->buttonReleased));
}

void RunnerMouse_setPosition(RunnerMouseState* m, int32_t x, int32_t y) {
    if (!m) return;
    m->x = x;
    m->y = y;
}

void RunnerMouse_setButton(RunnerMouseState* m, int32_t mb, bool down) {
    if (!m || mb <= 0 || mb >= MOUSE_BUTTON_COUNT) return;
    m->buttonDown[mb] = down;
}

void RunnerMouse_endFrame(RunnerMouseState* m) {
    if (!m) return;
    for (int i = 1; i < MOUSE_BUTTON_COUNT; i++) {
        bool now = m->buttonDown[i];
        bool was = m->buttonDownPrev[i];
        if (now && !was) m->buttonPressed[i] = true;
        if (!now && was) m->buttonReleased[i] = true;
    }
}

static bool any_button(const bool* arr) {
    for (int i = 1; i < MOUSE_BUTTON_COUNT; i++) if (arr[i]) return true;
    return false;
}

bool RunnerMouse_check(RunnerMouseState* m, int32_t mb) {
    if (!m) return false;
    if (mb == MB_NONE) return !any_button(m->buttonDown);
    if (mb == MB_ANY)  return any_button(m->buttonDown);
    if (mb <= 0 || mb >= MOUSE_BUTTON_COUNT) return false;
    return m->buttonDown[mb];
}

bool RunnerMouse_checkPressed(RunnerMouseState* m, int32_t mb) {
    if (!m) return false;
    if (mb == MB_NONE) return false;
    if (mb == MB_ANY)  return any_button(m->buttonPressed);
    if (mb <= 0 || mb >= MOUSE_BUTTON_COUNT) return false;
    return m->buttonPressed[mb];
}

bool RunnerMouse_checkReleased(RunnerMouseState* m, int32_t mb) {
    if (!m) return false;
    if (mb == MB_NONE) return false;
    if (mb == MB_ANY)  return any_button(m->buttonReleased);
    if (mb <= 0 || mb >= MOUSE_BUTTON_COUNT) return false;
    return m->buttonReleased[mb];
}
