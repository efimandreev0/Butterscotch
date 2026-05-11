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
#include "../file_system.h"
#include "../json_reader.h"

// Creates a PS2 file system that maps game-relative file names to PS2 device paths
// using the "fileSystem" object from a parsed CONFIG.JSN root
//
// configRoot: parsed JSON root of CONFIG.JSN (caller retains ownership, not freed here)
// gameTitle: the game's display name (used for icon.sys on memory card saves)
FileSystem* Ps2FileSystem_create(JsonValue* configRoot, const char* gameTitle);
void Ps2FileSystem_destroy(FileSystem* fs);
