// --- START OF FILE memlz.h ---

// SPDX-License-Identifier: MIT
// memlz 0.2 beta - extremely fast header-only compression library
// ARM11 (3DS) SAFE VERSION - All unaligned accesses patched via memcpy & macro fixes

#ifndef memlz__h
#define memlz__h

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

typedef struct memlz_state memlz_state;

static size_t memlz_compress(void* destination, const void* source, size_t len);
static size_t memlz_decompress(void* destination, const void* source);
static size_t memlz_stream_compress(void* destination, const void* source, size_t len, memlz_state* state);
static size_t memlz_stream_decompress(void* destination, const void* source, memlz_state* state);
static size_t memlz_compressed_len(const void* src);
static size_t memlz_decompressed_len(const void* src);
static size_t memlz_max_compressed_len(size_t input);
static size_t memlz_header_len();
static void memlz_reset(memlz_state* c);

#define MEMLZ__DO_RLE
#define MEMLZ__DO_INCOMPRESSIBLE
#define MEMLZ__INCOMPRESSIBLE_TRIGGER (4)
#define MEMLZ__INCOMPRESSIBLE_ADVANCE (16 * MEMLZ__INCOMPRESSIBLE_TRIGGER)
#define MEMLZ__PROBELEN (2 * 1024)
#define MEMLZ__BLOCKLEN (128 * 1024)
#define MEMLZ__RLE 'D'
#define MEMLZ__MIN_RLE (4 * sizeof(uint64_t))

#define MEMLZ__RESTRICT __restrict

#define MEMLZ__UNROLL4(op) op; op; op; op
#define MEMLZ__UNROLL16(op) op; op; op; op; op; op; op; op; op; op; op; op; op; op; op; op;
#define MEMLZ__NORMAL32 'A'
#define MEMLZ__NORMAL64 'B'
#define MEMLZ__UNCOMPRESSED 'C'

#define MEMLZ__MIN(X, Y) ((X) < (Y) ? (X) : (Y))

#ifdef _WIN32
#define MEMLZ__UNUSED
#else
#define MEMLZ__UNUSED __attribute__((unused))
#endif

static const size_t memlz__fields = 2;
static const size_t memlz__words_per_round = 16;

static uint16_t memlz__hash32(uint32_t v) { return (uint16_t)(((v * 2654435761ull) >> 16)); }
static uint16_t memlz__hash64(uint64_t v) { return (uint16_t)(((v * 11400714819323198485ull) >> 48)); }

// === ARM-SAFE MEMORY ACCESS ===
static uint64_t memlz__read(const void* src) {
    uint8_t* s = (uint8_t*)src;
    size_t bytes = ((size_t)*s) >> 6;
    if (bytes == 0) return *s & 0b00111111;
    uint64_t val = 0;
    if (bytes == 1) { uint16_t v; memcpy(&v, s + 1, 2); val = v; }
    else if (bytes == 2) { uint32_t v; memcpy(&v, s + 1, 4); val = v; }
    else { memcpy(&val, s + 1, 8); }
    return val;
}

static size_t memlz__bytes(const void* src) {
    uint8_t* s = (uint8_t*)src;
    size_t bytes = ((size_t)*s) >> 6;
    return bytes == 0ULL ? 1ULL : bytes == 1ULL ? 3ULL : bytes == 2ULL ? 5ULL : 9ULL;
}

static void memlz__write(void* dst, uint64_t value, size_t bytes) {
    uint8_t* d = (uint8_t*)dst;
    if (bytes == 1) { *d = (uint8_t)value; }
    else if (bytes == 3) { *d = 0b01000000; uint16_t v = (uint16_t)value; memcpy(d + 1, &v, 2); }
    else if (bytes == 5) { *d = 0b10000000; uint32_t v = (uint32_t)value; memcpy(d + 1, &v, 4); }
    else if (bytes == 9) { *d = 0b11000000; memcpy(d + 1, &value, 8); }
}

static uint64_t memlz__fit(uint64_t value) {
    return value < 64ULL ? 1ULL : value <= 0xffffULL ? 3ULL : value <= 0xffffffffULL ? 5ULL : 9ULL;
}

typedef struct memlz_state {
    uint64_t hash64[1 << 16];
    uint32_t hash32[1 << 16];
    uint64_t total_input;
    uint64_t total_output;
    size_t mod;
    size_t wordlen;
    size_t cs4;
    size_t cs8;
    size_t incompressible;
    char reset;
} memlz_state;

static size_t memlz_max_compressed_len(size_t input) { return 68 * input / 64 + 100; }
static size_t memlz_header_len() { return 18; }

static void memlz_reset(memlz_state* c) {
    memset(c->hash32, 0, sizeof(c->hash32));
    memset(c->hash64, 0, sizeof(c->hash64));
    c->total_input = 0; c->total_output = 0; c->mod = 0;
    c->wordlen = 8; c->cs4 = 0; c->cs8 = 0; c->incompressible = 0; c->reset = 'Y';
}

static size_t memlz_stream_compress(void* MEMLZ__RESTRICT destination, const void* MEMLZ__RESTRICT source, size_t len, memlz_state* state) {
    if (state->reset != 'Y') return 0;

    const size_t max = memlz_max_compressed_len(len) > len ? memlz_max_compressed_len(len) : len;
    const size_t header_len = memlz__fields * memlz__fit(max);
    size_t missing = len;
    const uint8_t* src = (const uint8_t*)source;
    uint8_t* dst = (uint8_t*)destination;
    uint16_t flags = 0;
    dst += header_len;

    for(;;) {
        state->mod++;
        if (state->mod == MEMLZ__PROBELEN / 128) {
            state->cs8 = (state->total_output + (dst - (uint8_t*)destination)) - state->cs8;
            state->cs4 = (state->total_output + (dst - (uint8_t*)destination));
            state->wordlen = 4;
        } else if (state->mod == 3 * MEMLZ__PROBELEN / 128) {
            state->cs4 = state->total_output + (dst - (uint8_t*)destination) - state->cs4;
            if (state->cs8 < state->cs4) state->wordlen = 8;
        } else if (state->mod == (MEMLZ__BLOCKLEN + MEMLZ__PROBELEN) / 128) {
            state->wordlen = 8; state->mod = 0;
            state->cs8 = state->total_output + (dst - (uint8_t*)destination); state->cs4 = 0;
        }

#ifdef MEMLZ__DO_RLE
        {
            size_t e = 1;
            uint64_t first_val; memcpy(&first_val, src, 8);
            while (e < missing / 8) {
                uint64_t cur_val; memcpy(&cur_val, src + e * 8, 8);
                if (cur_val == first_val) e++; else break;
            }
            e *= 8;
            if (e >= MEMLZ__MIN_RLE) {
                *dst++ = MEMLZ__RLE;
                size_t length = memlz__fit(e);
                memlz__write(dst, e, length);
                memcpy(dst + length, &first_val, 8);
                dst += 8 + length;
                missing -= e;
                src += e;
                continue;
            }
        }
#endif
        {
            *dst++ = state->wordlen == 8 ? MEMLZ__NORMAL64 : MEMLZ__NORMAL32;
            if (missing < 16 * state->wordlen) break;

            uint8_t* flags_ptr = dst;
            dst += 2;

            #define MEMLZ__ENCODE_WORD(tbl, typ, h, val, next) do { \
            flags <<= 1; \
            if ((tbl)[(h)] == (val)) { \
            flags |= 1; \
            uint16_t _memlz_hv = (uint16_t)(h); \
            memcpy(dst, &_memlz_hv, 2); \
            dst += 2; \
            next; \
            } else { \
            (tbl)[(h)] = (val); \
            memcpy(dst, &(val), sizeof(typ)); \
            dst += sizeof(typ); \
            next; \
            } \
            } while(0)

            #define MEMLZ__ENCODE4(tbl, typ, ha, va, hb, vb, hc, vc, hd, vd) MEMLZ__ENCODE_WORD \
                    (tbl, typ, ha, va, MEMLZ__ENCODE_WORD \
                    (tbl, typ, hb, vb, MEMLZ__ENCODE_WORD \
                    (tbl, typ, hc, vc, MEMLZ__ENCODE_WORD \
                    (tbl, typ, hd, vd, ))))

            if (state->wordlen == 8) {
                uint64_t v[4];
                uint64_t a, b, c, d; // Вынесли из макроса
                MEMLZ__UNROLL4(
                    memcpy(v, src, 32);
                    a = memlz__hash64(v[0]); b = memlz__hash64(v[1]);
                    c = memlz__hash64(v[2]); d = memlz__hash64(v[3]);
                    MEMLZ__ENCODE4(state->hash64, uint64_t, a, v[0], b, v[1], c, v[2], d, v[3]);
                    src += 32;
                )
            } else {
                uint32_t v[4];
                uint32_t a, b, c, d; // Вынесли из макроса
                MEMLZ__UNROLL4(
                    memcpy(v, src, 16);
                    a = memlz__hash32(v[0]); b = memlz__hash32(v[1]);
                    c = memlz__hash32(v[2]); d = memlz__hash32(v[3]);
                    MEMLZ__ENCODE4(state->hash32, uint32_t, a, v[0], b, v[1], c, v[2], d, v[3]);
                    src += 16;
                )
            }
            memcpy(flags_ptr, &flags, 2);
            missing -= 16 * state->wordlen;
        }

#ifdef MEMLZ__DO_INCOMPRESSIBLE
        {
            state->incompressible = flags ? 0 : state->incompressible + 1;
            if (state->incompressible > 0 && missing >= MEMLZ__INCOMPRESSIBLE_ADVANCE && state->incompressible % MEMLZ__INCOMPRESSIBLE_TRIGGER == 0) {
                size_t u = MEMLZ__INCOMPRESSIBLE_ADVANCE * state->incompressible;
                u = u > missing ? missing : u; u = u > 1024 ? 1024 : u;
                u = u & ~(sizeof(uint64_t) - 1);
                *dst++ = MEMLZ__UNCOMPRESSED;
                uint16_t u16_u = (uint16_t)u; memcpy(dst, &u16_u, 2);
                memlz__write(dst, u, memlz__fit(u));
                dst += memlz__fit(u);
                memcpy(dst, src, u);
                dst += u; src += u; missing -= u;
            }
        }
#endif
    }

    if (missing >= state->wordlen) {
        uint8_t* flag_ptr = dst;
        dst += 2; flags = 0; int flags_left = memlz__words_per_round;

        while (missing >= state->wordlen) {
            if (state->wordlen == 8) {
                uint64_t val; memcpy(&val, src, 8);
                uint64_t a = memlz__hash64(val);
                MEMLZ__ENCODE_WORD(state->hash64, uint64_t, a, val, )
            } else {
                uint32_t val; memcpy(&val, src, 4);
                uint32_t a = memlz__hash32(val);
                MEMLZ__ENCODE_WORD(state->hash32, uint32_t, a, val, )
            }
            src += state->wordlen; flags_left--; missing -= state->wordlen;
        }
        flags <<= flags_left;
        memcpy(flag_ptr, &flags, 2);
    }

    size_t tail_count = missing;
    memcpy(dst, src, tail_count);
    dst += tail_count;

    size_t compressed_len = (size_t)(dst - (uint8_t*)destination);
    if (compressed_len < memlz_header_len()) {
        memset(dst, 'M', memlz_header_len() - compressed_len);
        compressed_len = memlz_header_len();
    }

    memlz__write(destination, len, header_len / memlz__fields);
    memlz__write((uint8_t*)destination + header_len / memlz__fields, compressed_len, header_len / memlz__fields);

    state->total_input += len; state->total_output += compressed_len;
    return compressed_len;
}

static size_t memlz_decompressed_len(const void* src) { return memlz__read(src); }
static size_t memlz_compressed_len(const void* src) {
    size_t header_field_len = memlz__bytes(src);
    return memlz__read((uint8_t*)src + header_field_len);
}

#define MEMLZ__R(p, l) do { if ((p) < r1 || (l) > (size_t)((r2) - (p))) return 0; } while (0)
#define MEMLZ__W(p, l) do { if ((p) < w1 || (l) > (size_t)((w2) - (p))) return 0; } while (0)

static size_t memlz_stream_decompress(void* MEMLZ__RESTRICT destination, const void* MEMLZ__RESTRICT source, memlz_state* state) {
    if (state->reset != 'Y') return 0;

    const size_t decompressed_len = memlz_decompressed_len(source);
    const size_t compressed_len = memlz_compressed_len(source);
    if (compressed_len > memlz_max_compressed_len(decompressed_len)) return 0;

    const uint8_t* r1 = (uint8_t*)source; const uint8_t* r2 = (uint8_t*)source + compressed_len;
    const uint8_t* w1 = (uint8_t*)destination; const uint8_t* w2 = (uint8_t*)destination + decompressed_len;

    size_t header_length = memlz__bytes(source) * memlz__fields;
    const uint8_t* src = (const uint8_t*)source + header_length;
    uint8_t* dst = (uint8_t*)destination;

    size_t missing = decompressed_len; size_t last_missing = 0;
    uint8_t blocktype = 0; size_t memlz__wordlen = 0;

    for (;;) {
        const size_t min_advance = MEMLZ__MIN(64, MEMLZ__MIN(MEMLZ__INCOMPRESSIBLE_ADVANCE, MEMLZ__MIN_RLE));
        if (last_missing != 0 && missing > last_missing + min_advance) return 0;
        last_missing = missing;

        MEMLZ__R(src, 1);
        blocktype = *src++;

#ifdef MEMLZ__DO_INCOMPRESSIBLE
        if (blocktype == MEMLZ__UNCOMPRESSED) {
            MEMLZ__R(src, 1);
            size_t len = memlz__bytes(src); MEMLZ__R(src, len);
            size_t unc = memlz__read(src); src += len;
            MEMLZ__R(src, unc); MEMLZ__W(dst, unc);
            memcpy(dst, src, unc);
            src += unc; dst += unc; missing -= unc;
            continue;
        }
#endif

#ifdef MEMLZ__DO_RLE
        if (blocktype == MEMLZ__RLE) {
            MEMLZ__R(src, 1);
            size_t len = memlz__bytes(src); MEMLZ__R(src, len);
            uint64_t z = memlz__read(src); src += len;
            MEMLZ__R(src, 8);
            uint64_t v; memcpy(&v, src, 8); src += 8;
            MEMLZ__W(dst, z);
            for (uint64_t n = 0; n < z / 8; n++) memcpy(dst + n * 8, &v, 8);
            dst += z; missing -= z;
            continue;
        }
#endif

        if (blocktype == MEMLZ__NORMAL64) memlz__wordlen = 8;
        else if (blocktype == MEMLZ__NORMAL32) memlz__wordlen = 4;
        else return 0;

        if (missing < memlz__wordlen * 16) break;

        MEMLZ__R(src, 2);
        uint16_t flags; memcpy(&flags, src, 2); src += 2;

#define MEMLZ__DECODE_WORD(safe, h, tbl, typ, next) \
        if (flags & 0b1000000000000000) { \
            if(safe) { MEMLZ__R(src, 2); } \
            memcpy(&hash_idx, src, 2); src += 2; \
            word = tbl[hash_idx]; \
            memcpy(dst, &word, sizeof(typ)); dst += sizeof(typ); \
            flags = (uint16_t)(flags << 1); \
            next; \
        } else { \
            if(safe) { MEMLZ__R(src, sizeof(typ)); } \
            memcpy(&word, src, sizeof(typ)); src += sizeof(typ); \
            tbl[h(word)] = word; \
            memcpy(dst, &word, sizeof(typ)); dst += sizeof(typ); \
            flags = (uint16_t)(flags << 1); \
            next; \
        }

#define MEMLZ__DECODE4(safe, h, tbl, typ) MEMLZ__DECODE_WORD(safe, h, tbl, typ, MEMLZ__DECODE_WORD(safe, h, tbl, typ, MEMLZ__DECODE_WORD(safe, h, tbl, typ, MEMLZ__DECODE_WORD(safe, h, tbl, typ, ))))

        if (src + 16 * sizeof(uint64_t) < r2) {
            if (blocktype == MEMLZ__NORMAL64) {
                uint64_t word; uint16_t hash_idx; // Вынесли из макроса
                MEMLZ__W(dst, 16 * sizeof(word));
                MEMLZ__UNROLL4(MEMLZ__DECODE4(0, memlz__hash64, state->hash64, uint64_t))
                missing -= 16 * sizeof(uint64_t);
            } else {
                uint32_t word; uint16_t hash_idx; // Вынесли из макроса
                MEMLZ__W(dst, 16 * sizeof(word));
                MEMLZ__UNROLL4(MEMLZ__DECODE4(0, memlz__hash32, state->hash32, uint32_t))
                missing -= 16 * sizeof(uint32_t);
            }
        } else {
            if (blocktype == MEMLZ__NORMAL64) {
                uint64_t word; uint16_t hash_idx; // Вынесли из макроса
                MEMLZ__W(dst, 16 * sizeof(word));
                MEMLZ__UNROLL4(MEMLZ__DECODE4(1, memlz__hash64, state->hash64, uint64_t))
                missing -= 16 * sizeof(uint64_t);
            } else {
                uint32_t word; uint16_t hash_idx; // Вынесли из макроса
                MEMLZ__W(dst, 16 * sizeof(word));
                MEMLZ__UNROLL4(MEMLZ__DECODE4(1, memlz__hash32, state->hash32, uint32_t))
                missing -= 16 * sizeof(uint32_t);
            }
        }
    }
    if (missing >= memlz__wordlen) {
        MEMLZ__R(src, 2);
        uint16_t flags; memcpy(&flags, src, 2); src += 2;

        while (missing >= memlz__wordlen) {
            if (memlz__wordlen == 8) {
                uint64_t word; uint16_t hash_idx; MEMLZ__W(dst, sizeof(word));
                MEMLZ__DECODE_WORD(1, memlz__hash64, state->hash64, uint64_t,)
            } else {
                uint32_t word; uint16_t hash_idx; MEMLZ__W(dst, sizeof(word));
                MEMLZ__DECODE_WORD(1, memlz__hash32, state->hash32, uint32_t,)
            }
            missing -= memlz__wordlen;
        }
    }

    size_t tail_count = missing;
    while (tail_count) { MEMLZ__R(src, 1); MEMLZ__W(dst, 1); *dst++ = *src++; tail_count--; }

    state->total_input += compressed_len; state->total_output += decompressed_len;
    return decompressed_len;
}

MEMLZ__UNUSED static size_t memlz_decompress(void* MEMLZ__RESTRICT destination, const void* MEMLZ__RESTRICT source) {
    memlz_state* s = (memlz_state*)malloc(sizeof(memlz_state));
    if (!s) return 0;
    memlz_reset(s); size_t r = memlz_stream_decompress(destination, source, s);
    free(s); return r;
}

MEMLZ__UNUSED static size_t memlz_compress(void* MEMLZ__RESTRICT destination, const void* MEMLZ__RESTRICT source, size_t len) {
    memlz_state* s = (memlz_state*)malloc(sizeof(memlz_state));
    if (!s) return 0;
    memlz_reset(s); size_t r = memlz_stream_compress(destination, source, len, s);
    free(s); return r;
}

#undef MEMLZ__UNROLL4
#undef MEMLZ__UNROLL16
#undef MEMLZ__ENCODE_WORD
#undef MEMLZ__DECODE_WORD
#undef MEMLZ__VOID
#undef MEMLZ__NORMAL32
#undef MEMLZ__NORMAL64
#undef MEMLZ__UNCOMPRESSED
#undef MEMLZ__RLE
#undef MEMLZ__WORDPROBE4096
#undef MEMLZ__BLOCKLEN
#undef MEMLZ__MIN
#undef MEMLZ__DO_RLE
#undef MEMLZ__DO_INCOMPRESSIBLE
#undef MEMLZ__INCOMPRESSIBLE
#undef MEMLZ__PROBELEN
#undef MEMLZ__MIN_RLE
#undef MEMLZ__RESTRICT
#undef MEM_UNUSED

#endif // memlz__h

// --- END OF FILE memlz.h ---