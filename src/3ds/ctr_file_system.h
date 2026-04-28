#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char *basePath;
} N3dsFileSystem;

N3dsFileSystem *N3dsFileSystem_create(const char *dataWinPath);

void N3dsFileSystem_destroy(N3dsFileSystem *fs);
