// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include <common.h>
#include <stdint.h>
#include <stddef.h>

uint8_t *ImageDecoder_decodeToRgba(const uint8_t *blob, size_t blobSize, bool gm2022_5, int *outW, int *outH);

void ImageDecoder_freeRgba(uint8_t *pixels);
