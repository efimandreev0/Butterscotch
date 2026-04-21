#pragma once

#include "common.h"
#include "file_system.h"

typedef struct {
    FileSystem base;
    char* basePath; // directory containing data.win, with trailing separator
} CtrFileSystem;

// Creates a CtrFileSystem from the path to the data.win file (e.g. "sdmc:/3ds/butterscotch/data.win")
// The basePath is derived by stripping the filename from dataWinPath.
CtrFileSystem* CtrFileSystem_create(const char* dataWinPath);
void CtrFileSystem_destroy(CtrFileSystem* fs);
