#pragma once

#include <common.h>
#include <stdint.h>
#include <stddef.h>

// Decodes a texture blob (PNG, GameMaker custom QOI, or BZip2+QOI) into RGBA8 pixel data.
// "gm2022_5" should be set for GameMaker 2022.5+ QOI blobs, which include the uncompressed length in the header.
// Returns a malloc'd buffer of size (*outW) * (*outH) * 4 bytes, or nullptr on failure.
// Caller is responsible for passing the returned buffer to ImageDecoder_freeRgba().
uint8_t* ImageDecoder_decodeToRgba(const uint8_t* blob, size_t blobSize, bool gm2022_5, int* outW, int* outH);

void ImageDecoder_freeRgba(uint8_t* pixels);
