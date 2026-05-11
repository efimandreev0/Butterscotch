// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "etc1_encoder.h"

#include <stdlib.h>
#include <string.h>
static const int kModifierTable[8][4] = {
    {2, 8, -2, -8},
    {5, 17, -5, -17},
    {9, 29, -9, -29},
    {13, 42, -13, -42},
    {18, 60, -18, -60},
    {24, 80, -24, -80},
    {33, 106, -33, -106},
    {47, 183, -47, -183},
};

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline int quant_nibble(int v) {
    int q = (v + 8) >> 4;
    if (q > 15) q = 15;
    if (q < 0) q = 0;
    return q;
}

static int abs_i(int v) { return v < 0 ? -v : v; }

static uint64_t encode_half(const uint8_t halfRgb[24],
                            int baseR, int baseG, int baseB,
                            int *outTable, int outIdx[8]) {
    int maxDelta = 0;
    for (int p = 0; p < 8; p++) {
        int dr = abs_i((int) halfRgb[p * 3 + 0] - baseR);
        int dg = abs_i((int) halfRgb[p * 3 + 1] - baseG);
        int db = abs_i((int) halfRgb[p * 3 + 2] - baseB);
        int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
        if (d > maxDelta) maxDelta = d;
    }

    int bestT = 0;
    for (int t = 0; t < 8; t++) {
        if (kModifierTable[t][1] >= maxDelta) {
            bestT = t;
            break;
        }
    }

    uint64_t err = 0;
    int bestIdxBuf[8] = {0};
    for (int p = 0; p < 8; p++) {
        int pr = halfRgb[p * 3 + 0];
        int pg = halfRgb[p * 3 + 1];
        int pb = halfRgb[p * 3 + 2];
        uint64_t bestPx = (uint64_t) -1;
        int bestK = 0;
        for (int k = 0; k < 4; k++) {
            int m = kModifierTable[bestT][k];
            int dr = clamp255(baseR + m) - pr;
            int dg = clamp255(baseG + m) - pg;
            int db = clamp255(baseB + m) - pb;
            uint64_t e = (uint64_t) (dr * dr + dg * dg + db * db);
            if (e < bestPx) {
                bestPx = e;
                bestK = k;
            }
        }
        err += bestPx;
        bestIdxBuf[p] = bestK;
    }

    *outTable = bestT;
    memcpy(outIdx, bestIdxBuf, sizeof(bestIdxBuf));
    return err;
}

void Etc1_encodeBlockRgb(const uint8_t rgb16[48], uint8_t out[8]) {
    uint64_t bestTotal = (uint64_t) -1;
    int bestFlip = 0;
    int bestTA = 0, bestTB = 0;
    int bestR1 = 0, bestG1 = 0, bestB1 = 0;
    int bestR2 = 0, bestG2 = 0, bestB2 = 0;
    int bestIdx[16] = {0};

    for (int flip = 0; flip <= 1; flip++) {
        uint8_t halfA[24], halfB[24];
        int posA[8], posB[8];
        int ai = 0, bi = 0;

        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int isA = flip ? (y < 2) : (x < 2);
                int p = x * 4 + y;
                int rgbOff = (y * 4 + x) * 3;
                if (isA) {
                    halfA[ai * 3 + 0] = rgb16[rgbOff + 0];
                    halfA[ai * 3 + 1] = rgb16[rgbOff + 1];
                    halfA[ai * 3 + 2] = rgb16[rgbOff + 2];
                    posA[ai++] = p;
                } else {
                    halfB[bi * 3 + 0] = rgb16[rgbOff + 0];
                    halfB[bi * 3 + 1] = rgb16[rgbOff + 1];
                    halfB[bi * 3 + 2] = rgb16[rgbOff + 2];
                    posB[bi++] = p;
                }
            }
        }

        int sumA[3] = {0, 0, 0};
        int sumB[3] = {0, 0, 0};
        for (int i = 0; i < 8; i++) {
            sumA[0] += halfA[i * 3 + 0];
            sumA[1] += halfA[i * 3 + 1];
            sumA[2] += halfA[i * 3 + 2];
            sumB[0] += halfB[i * 3 + 0];
            sumB[1] += halfB[i * 3 + 1];
            sumB[2] += halfB[i * 3 + 2];
        }

        int qAr = quant_nibble(sumA[0] / 8);
        int qAg = quant_nibble(sumA[1] / 8);
        int qAb = quant_nibble(sumA[2] / 8);
        int qBr = quant_nibble(sumB[0] / 8);
        int qBg = quant_nibble(sumB[1] / 8);
        int qBb = quant_nibble(sumB[2] / 8);
        int baseAr = (qAr << 4) | qAr;
        int baseAg = (qAg << 4) | qAg;
        int baseAb = (qAb << 4) | qAb;
        int baseBr = (qBr << 4) | qBr;
        int baseBg = (qBg << 4) | qBg;
        int baseBb = (qBb << 4) | qBb;

        int tA, tB;
        int idxA[8], idxB[8];
        uint64_t errA = encode_half(halfA, baseAr, baseAg, baseAb, &tA, idxA);
        uint64_t errB = encode_half(halfB, baseBr, baseBg, baseBb, &tB, idxB);
        uint64_t total = errA + errB;

        if (total < bestTotal) {
            bestTotal = total;
            bestFlip = flip;
            bestTA = tA;
            bestTB = tB;
            bestR1 = qAr;
            bestG1 = qAg;
            bestB1 = qAb;
            bestR2 = qBr;
            bestG2 = qBg;
            bestB2 = qBb;
            for (int i = 0; i < 8; i++) bestIdx[posA[i]] = idxA[i];
            for (int i = 0; i < 8; i++) bestIdx[posB[i]] = idxB[i];
        }
    }
    uint64_t bits = 0;
    bits |= ((uint64_t) (bestR1 & 0xF)) << 60;
    bits |= ((uint64_t) (bestR2 & 0xF)) << 56;
    bits |= ((uint64_t) (bestG1 & 0xF)) << 52;
    bits |= ((uint64_t) (bestG2 & 0xF)) << 48;
    bits |= ((uint64_t) (bestB1 & 0xF)) << 44;
    bits |= ((uint64_t) (bestB2 & 0xF)) << 40;
    bits |= ((uint64_t) (bestTA & 0x7)) << 37;
    bits |= ((uint64_t) (bestTB & 0x7)) << 34;
    bits |= ((uint64_t) (bestFlip & 0x1)) << 32;

    for (int p = 0; p < 16; p++) {
        int idx = bestIdx[p] & 0x3;
        int msb = (idx >> 1) & 1;
        int lsb = idx & 1;
        bits |= ((uint64_t) msb) << (16 + p);
        bits |= ((uint64_t) lsb) << p;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t) ((bits >> (i * 8)) & 0xFF);
    }
}

static void pack_alpha_block(const uint8_t alpha16[16], uint8_t out[8]) {
    memset(out, 0, 8);
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            int p = x * 4 + y;
            uint8_t a4 = (uint8_t) (alpha16[y * 4 + x] >> 4);
            int byteIdx = p >> 1;
            if ((p & 1) == 0) out[byteIdx] |= a4;
            else out[byteIdx] |= (uint8_t) (a4 << 4);
        }
    }
}

bool Etc1_encodeImageTiled(const uint8_t *rgbaLinear, int w, int h,
                           bool withAlpha, uint8_t *out) {
    if (!rgbaLinear || !out || w <= 0 || h <= 0 || (w & 7) || (h & 7)) return false;

    int tilesX = w >> 3;
    int tilesY = h >> 3;
    int blockBytes = withAlpha ? 16 : 8;
    uint8_t blockRgb[48];
    uint8_t blockAlpha[16];
    uint8_t encoded[8];
    uint8_t alphaBytes[8];
    uint64_t outOffset = 0;

    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            for (int mb = 0; mb < 4; mb++) {
                int bx = mb & 1;
                int by = (mb >> 1) & 1;
                int outBlockX = tx * 8 + bx * 4;
                int outBlockYTop = ty * 8 + by * 4;
                for (int py = 0; py < 4; py++) {
                    for (int px = 0; px < 4; px++) {
                        int sx = outBlockX + px;
                        int syOut = outBlockYTop + py;
                        int sy = (h - 1) - syOut;
                        int srcOff = (sy * w + sx) * 4;
                        int dstOff = (py * 4 + px) * 3;
                        blockRgb[dstOff + 0] = rgbaLinear[srcOff + 0];
                        blockRgb[dstOff + 1] = rgbaLinear[srcOff + 1];
                        blockRgb[dstOff + 2] = rgbaLinear[srcOff + 2];
                        if (withAlpha) {
                            blockAlpha[py * 4 + px] = rgbaLinear[srcOff + 3];
                        }
                    }
                }

                Etc1_encodeBlockRgb(blockRgb, encoded);
                if (withAlpha) {
                    pack_alpha_block(blockAlpha, alphaBytes);
                    memcpy(out + outOffset, alphaBytes, 8);
                    memcpy(out + outOffset + 8, encoded, 8);
                } else {
                    memcpy(out + outOffset, encoded, 8);
                }
                outOffset += (uint64_t) blockBytes;
            }
        }
    }

    return true;
}
