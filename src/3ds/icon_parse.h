// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#ifndef BUTTERSCOTCH_ICON_PARSE_H
#define BUTTERSCOTCH_ICON_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
} IconImage;

bool extract_icon_from_exe_pe(const char *path, IconImage *out);
bool load_image_from_file(const char *path, IconImage *out);
void IconImage_free(IconImage *image);

uint16_t rd16(const uint8_t *p);
uint32_t rd32(const uint8_t *p);

#endif //BUTTERSCOTCH_ICON_PARSE_H//
