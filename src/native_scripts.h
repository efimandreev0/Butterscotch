// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include "vm.h"
#include "runner.h"


typedef void (*NativeCodeFunc)(VMContext *ctx, Runner *runner, Instance *instance);


void NativeScripts_init(VMContext *ctx, Runner *runner);


NativeCodeFunc NativeScripts_find(const char *codeName);


void NativeScripts_register(const char *codeName, NativeCodeFunc func);
