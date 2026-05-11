// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include <stddef.h>
#include <stdint.h>

size_t CtrAdpcm_byteCount(uint32_t frames);

void CtrAdpcm_encode(const int16_t *samples, uint32_t frames,
                     uint8_t *outAdpcm, int16_t outCoefs[16]);
