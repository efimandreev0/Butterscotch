// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include "../runner_gamepad.h"

// Loads SDL gamecontroller mappings into GLFW (call after glfwInit).
void GlfwGamepad_loadMappings(const char* mappings);
// Reads the physical joystick state from GLFW and updates RunnerGamepadState.
void GlfwGamepad_poll(RunnerGamepadState* gp);
