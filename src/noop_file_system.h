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

// Creates an in-memory FileSystem backed by a hashmap instead of real disk I/O
// Files written via writeFileText are kept in memory and can be read back
// Use this as a fallback while you don't have a proper file system implementation for your target!
FileSystem* NoopFileSystem_create(void);
void NoopFileSystem_destroy(FileSystem* fs);
