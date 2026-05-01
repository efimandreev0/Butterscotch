//
// Created by efimandreev0 on 30.04.2026.
//

#ifndef BUTTERSCOTCH_ICON_PARSE_H
#define BUTTERSCOTCH_ICON_PARSE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <NovaGL.h>
#include "stb_image.h"

GLuint extract_icon_from_exe_pe(const char *path);
GLuint load_texture_from_file(const char *path);
GLuint upload_rgba_texture(const uint8_t *pixels, int w, int h);

uint16_t rd16(const uint8_t *p);
uint32_t rd32(const uint8_t *p);

#endif //BUTTERSCOTCH_ICON_PARSE_H//