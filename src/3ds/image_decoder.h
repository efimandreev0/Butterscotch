#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

uint8_t *ImageDecoder_decodeToRgba(const uint8_t *blob, size_t blobSize, bool gm2022_5, int *outW, int *outH);
