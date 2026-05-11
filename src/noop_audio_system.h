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
#include "audio_system.h"

// A no-op audio system that silently ignores all audio calls.
// Useful for headless mode or platforms without audio support.

typedef struct {
    AudioSystem base;
} NoopAudioSystem;

NoopAudioSystem* NoopAudioSystem_create(void);
