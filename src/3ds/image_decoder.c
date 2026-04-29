#include "image_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>

#include "stb_image.h"

static uint8_t *decodeQoi(const uint8_t *data, size_t size, int *outW, int *outH) {
    if (size < 14) return NULL;

    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') {
        return NULL;
    }

    int width = (int)(((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7]);
    int height = (int)(((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11]);
    int channels = data[12];

    if (width == 0 || height == 0 || channels < 3 || channels > 4) return NULL;

    size_t pixels_len = (size_t)width * height * 4;
    uint8_t *pixels = (uint8_t *)malloc(pixels_len);
    if (!pixels) return NULL;

    uint8_t index[64 * 4] = {0};
    uint8_t r = 0, g = 0, b = 0, a = 255;
    int run = 0;
    size_t p = 14;

    for (size_t px_pos = 0; px_pos < pixels_len; px_pos += 4) {
        if (run > 0) {
            run--;
        } else if (p < size) {
            int b1 = data[p++];

            if (b1 == 0xfe) { // QOI_OP_RGB
                if (p + 2 < size) {
                    r = data[p++];
                    g = data[p++];
                    b = data[p++];
                }
            } else if (b1 == 0xff) { // QOI_OP_RGBA
                if (p + 3 < size) {
                    r = data[p++];
                    g = data[p++];
                    b = data[p++];
                    a = data[p++];
                }
            } else if ((b1 & 0xc0) == 0x00) { // QOI_OP_INDEX
                int index_pos = (b1 & 0x3f) * 4;
                r = index[index_pos];
                g = index[index_pos + 1];
                b = index[index_pos + 2];
                a = index[index_pos + 3];
            } else if ((b1 & 0xc0) == 0x40) { // QOI_OP_DIFF
                r += ((b1 >> 4) & 0x03) - 2;
                g += ((b1 >> 2) & 0x03) - 2;
                b += (b1 & 0x03) - 2;
            } else if ((b1 & 0xc0) == 0x80) { // QOI_OP_LUMA
                if (p < size) {
                    int b2 = data[p++];
                    int vg = (b1 & 0x3f) - 32;
                    r += vg - 8 + ((b2 >> 4) & 0x0f);
                    g += vg;
                    b += vg - 8 + (b2 & 0x0f);
                }
            } else if ((b1 & 0xc0) == 0xc0) { // QOI_OP_RUN
                run = (b1 & 0x3f);
            }

            int index_pos = (r * 3 + g * 5 + b * 7 + a * 11) % 64 * 4;
            index[index_pos] = r;
            index[index_pos + 1] = g;
            index[index_pos + 2] = b;
            index[index_pos + 3] = a;
        }

        pixels[px_pos] = r;
        pixels[px_pos + 1] = g;
        pixels[px_pos + 2] = b;
        pixels[px_pos + 3] = a;
    }

    *outW = width;
    *outH = height;
    return pixels;
}

static uint8_t *decodeBz2Qoi(const uint8_t *blob, size_t blobSize, int *outW, int *outH) {
    if (blobSize < 16) return NULL;

    size_t bz2Offset = 0;

    for (size_t i = 4; i <= 32 && i + 3 <= blobSize; i++) {
        if (blob[i] == 'B' && blob[i+1] == 'Z' && blob[i+2] == 'h') {
            bz2Offset = i;
            break;
        }
    }

    if (bz2Offset == 0) return NULL; // Not found

    uint32_t expectedUncompressedSize = 8 * 1024 * 1024;
    if (blob[0] == 'q' && blob[1] == 'o' && blob[2] == 'z' && blob[3] == '2') {
        uint32_t qoi_size = ((uint32_t)blob[4]) | ((uint32_t)blob[5] << 8) | ((uint32_t)blob[6] << 16) | ((uint32_t)blob[7] << 24);
        if (qoi_size > 0 && qoi_size <= 16 * 1024 * 1024) {
            expectedUncompressedSize = qoi_size;
        }
    }

    uint8_t *uncompressed = (uint8_t *) malloc(expectedUncompressedSize);
    if (!uncompressed) return NULL;

    unsigned int destLen = expectedUncompressedSize;
    int rc = BZ2_bzBuffToBuffDecompress((char *) uncompressed, &destLen,
                                        (char *) (blob + bz2Offset),
                                        (unsigned int) (blobSize - bz2Offset), 0, 0);
    if (rc != BZ_OK) {
        fprintf(stderr, "ImageDecoder: BZ2 decompress failed (rc=%d)\n", rc);
        free(uncompressed);
        return NULL;
    }

    uint8_t *result = decodeQoi(uncompressed, destLen, outW, outH);
    free(uncompressed);
    return result;
}

uint8_t *ImageDecoder_decodeToRgba(const uint8_t *blob, size_t blobSize, bool gm2022_5, int *outW, int *outH) {
    (void) gm2022_5;
    if (blobSize < 4 || !blob) return NULL;

    if (blob[0] == 'q' && blob[1] == 'o' && blob[2] == 'i' && blob[3] == 'f') {
        return decodeQoi(blob, blobSize, outW, outH);
    }

    if ((blob[0] == 'q' && blob[1] == 'o' && blob[2] == 'i' && blob[3] == 'z') ||
        (blob[0] == 'q' && blob[1] == 'o' && blob[2] == 'z' && blob[3] == '2')) {
        return decodeBz2Qoi(blob, blobSize, outW, outH);
    }

    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(blob, (int) blobSize, &w, &h, &channels, 4);
    if (!pixels) return NULL;
    *outW = w;
    *outH = h;
    return pixels;
}