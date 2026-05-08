#include "ctr_adpcm_encode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int32_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}
static int32_t clamp_nibble(int32_t v) {
    if (v >  7) return  7;
    if (v < -8) return -8;
    return v;
}

size_t CtrAdpcm_byteCount(uint32_t frames) {
    return (size_t)((frames + 13u) / 14u) * 8u;
}

static void lpc_coefs(const int16_t *samples, uint32_t frames,
                      int16_t *outP1, int16_t *outP2) {

    if (frames < 3) { *outP1 = 0; *outP2 = 0; return; }

    double R0 = 0.0, R1 = 0.0, R2 = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double s = (double) samples[i];
        R0 += s * s;
    }
    for (uint32_t i = 1; i < frames; i++) R1 += (double) samples[i] * samples[i - 1];
    for (uint32_t i = 2; i < frames; i++) R2 += (double) samples[i] * samples[i - 2];

    double det = R0 * R0 - R1 * R1;
    double p1 = 0.0, p2 = 0.0;
    if (fabs(det) > 1.0) {
        p1 = (R0 * R1 - R1 * R2) / det;
        p2 = (R0 * R2 - R1 * R1) / det;
    }

    if (p1 + p2 >=  1.0 - 1e-3 ||
        p2 - p1 >=  1.0 - 1e-3 ||
        p2      <= -1.0 + 1e-3) {
        p1 = 0.5;
        p2 = 0.0;
    }

    int32_t qp1 = (int32_t) lrint(p1 * 2048.0);
    int32_t qp2 = (int32_t) lrint(p2 * 2048.0);
    if (qp1 >  32767) qp1 =  32767;
    if (qp1 < -32768) qp1 = -32768;
    if (qp2 >  32767) qp2 =  32767;
    if (qp2 < -32768) qp2 = -32768;
    *outP1 = (int16_t) qp1;
    *outP2 = (int16_t) qp2;
}

static uint64_t encode_frame(const int16_t *samples, uint32_t count,
                             int16_t p1, int16_t p2,
                             int32_t hist1, int32_t hist2,
                             int outNibbles[14], int *outScale,
                             int32_t *outHist1, int32_t *outHist2) {
    uint64_t bestErr = (uint64_t) -1;

    for (int scale = 0; scale <= 12; scale++) {
        int32_t shift = 1 << scale;
        int32_t halfShift = shift >> 1;
        int32_t h1 = hist1, h2 = hist2;
        int      tmpNibbles[14];
        uint64_t err = 0;

        for (uint32_t i = 0; i < count; i++) {
            int32_t pred = ((int32_t) p1 * h1 + (int32_t) p2 * h2) >> 11;
            int32_t res  = (int32_t) samples[i] - pred;

            int32_t nib;
            if (shift == 1) nib = res;
            else if (res >= 0) nib = (res + halfShift) / shift;
            else               nib = (res - halfShift) / shift;
            nib = clamp_nibble(nib);

            int32_t recon = clamp16(pred + nib * shift);
            int32_t e = (int32_t) samples[i] - recon;
            err += (uint64_t)((int64_t) e * e);

            tmpNibbles[i] = nib;
            h2 = h1;
            h1 = recon;
        }

        if (err < bestErr) {
            bestErr = err;
            *outScale = scale;
            for (uint32_t i = 0; i < count; i++) outNibbles[i] = tmpNibbles[i];
            for (uint32_t i = count; i < 14; i++) outNibbles[i] = 0;
            *outHist1 = h1;
            *outHist2 = h2;
        }
    }

    return bestErr;
}

void CtrAdpcm_encode(const int16_t *samples, uint32_t frames,
                     uint8_t *outAdpcm, int16_t outCoefs[16]) {
    int16_t p1 = 0, p2 = 0;
    lpc_coefs(samples, frames, &p1, &p2);

    for (int i = 0; i < 8; i++) {
        outCoefs[i * 2 + 0] = p1;
        outCoefs[i * 2 + 1] = p2;
    }

    int32_t hist1 = 0, hist2 = 0;
    uint8_t *out = outAdpcm;
    uint32_t pos = 0;

    while (pos < frames) {
        uint32_t count = frames - pos;
        if (count > 14) count = 14;

        int     nibbles[14];
        int     scale = 0;
        int32_t newHist1 = 0, newHist2 = 0;
        encode_frame(samples + pos, count, p1, p2,
                     hist1, hist2,
                     nibbles, &scale, &newHist1, &newHist2);

        *out++ = (uint8_t)((0 << 4) | (scale & 0xF));

        for (int i = 0; i < 14; i += 2) {
            uint8_t hi = (uint8_t)(nibbles[i]     & 0xF);
            uint8_t lo = (uint8_t)(nibbles[i + 1] & 0xF);
            *out++ = (uint8_t)((hi << 4) | lo);
        }

        hist1 = newHist1;
        hist2 = newHist2;
        pos  += count;
    }
}
