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
#include "file_system.h"

// OverlayFileSystem implements GameMaker's two-area sandboxed file system on top of plain stdio.
// It holds two base paths:
// * bundlePath: read-only "File Bundle" area, where Included Files and the data.win live.
// * savePath: read/write "Save Area", the only place writes are allowed.
//
// Read operations check savePath first and fall back to bundlePath.
// Writes always target savePath. delete only acts on savePath (it will not touch a same-named file in the bundle).
//
// https://manual.gamemaker.io/lts/en/Additional_Information/The_File_System.htm
typedef struct {
    FileSystem base;
    char* bundlePath; // includes trailing '/'
    char* savePath; // includes trailing '/'
} OverlayFileSystem;

OverlayFileSystem* OverlayFileSystem_create(const char* bundlePath, const char* savePath);
void OverlayFileSystem_destroy(OverlayFileSystem* fs);
