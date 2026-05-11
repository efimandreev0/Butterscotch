// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {

#endif
bool Etc1_encodeImageTiled(const uint8_t *rgbaLinear, int w, int h,
                           bool withAlpha, uint8_t *out);

void Etc1_encodeBlockRgb(const uint8_t rgb16[48], uint8_t out[8]);

#ifdef __cplusplus
}
#endif
