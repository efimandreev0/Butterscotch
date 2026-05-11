// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char *basePath;
    char *fallbackBasePath;
} N3dsFileSystem;

N3dsFileSystem *N3dsFileSystem_create(const char *dataWinPath);

void N3dsFileSystem_destroy(N3dsFileSystem *fs);
