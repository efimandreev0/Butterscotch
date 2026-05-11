// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include "common.h"
#include "renderer.h"
#include <gsKit.h>

// ===[ GsRendererFlat Struct ]===
// Simple PS2 renderer using gsKit ONE SHOT mode.
// Renders all sprites/text as colored rectangles (no textures).
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GSGLOBAL* gsGlobal;

    // View transform state (set each view in beginView)
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
    int32_t viewX;
    int32_t viewY;
} GsRendererFlat;

Renderer* GsRendererFlat_create(GSGLOBAL* gsGlobal);
