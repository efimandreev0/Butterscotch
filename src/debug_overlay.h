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
#include "runner.h"

// Draws collision overlays for every active + visible instance in the current room.
// For instances backed by a sprite that has a precise mask (sepMasks == 1), every set
// mask pixel is filled with a translucent tint and the AABB outline is green.
// Otherwise, only the AABB outline is drawn in red.
//
// The drawing happens through the Renderer vtable, so this works on any platform that
// implements drawRectangle. Must be called inside a beginView/endView pair so the
// world-space coordinates project correctly.
void DebugOverlay_drawCollisionMasks(Runner* runner);
