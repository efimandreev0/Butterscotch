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
