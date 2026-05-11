// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "icon_parse.h"
#include "stb_image.h"

#define ICON_LOG(...) do { \
    fprintf(stderr, "[ICON] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while(0)

#define PE_SIG                   0x00004550u
#define MZ_SIG                   0x5A4Du
#define PE32_MAGIC               0x10Bu
#define PE32PLUS_MAGIC           0x20Bu
#define RT_ICON                  3u
#define RT_GROUP_ICON            14u
#define IMAGE_DIRECTORY_RESOURCE 2u

uint16_t rd16(const uint8_t *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

uint32_t rd32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int decode_icon_dib_to_rgba(const uint8_t *dib, size_t dib_size, uint8_t **out_rgba, int *out_w, int *out_h) {
    if (!dib || dib_size < 40 || !out_rgba || !out_w || !out_h) return 0;

    uint32_t hdr_size = rd32(dib + 0);
    int32_t w = (int32_t) rd32(dib + 4);
    int32_t h2 = (int32_t) rd32(dib + 8);
    uint16_t planes = rd16(dib + 12);
    uint16_t bpp = rd16(dib + 14);
    uint32_t compression = rd32(dib + 16);
    uint32_t clr_used = rd32(dib + 32);

    if (hdr_size < 40 || hdr_size > dib_size || w <= 0 || h2 == 0 || planes != 1) {
        ICON_LOG("Invalid DIB (hdr=%u, w=%d, h=%d, bpp=%u, comp=%u)", hdr_size, w, h2, bpp, compression);
        return 0;
    }
    if (compression != 0 && compression != 3) {
        ICON_LOG("Unsupported DIB compression %u (bpp=%u)", compression, bpp);
        return 0;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        ICON_LOG("Unsupported DIB bpp=%u", bpp);
        return 0;
    }

    int is_top_down = (h2 < 0);
    int h = abs(h2) / 2;

    size_t palette_size = 0;
    int colors = 0;

    if (compression == 3 && hdr_size <= 40) {
        palette_size = 12;
    } else if (bpp <= 8) {
        colors = (clr_used > 0) ? (int) clr_used : (1 << bpp);
        if (colors > 256) colors = 256;
        palette_size = (size_t) colors * 4;
    }

    const uint8_t *palette = dib + hdr_size;
    const uint8_t *xor_mask = palette + palette_size;

    size_t xor_pitch = (((size_t) w * bpp + 31) / 32) * 4;
    size_t and_pitch = (((size_t) w * 1 + 31) / 32) * 4;

    size_t xor_size = xor_pitch * (size_t) h;
    size_t and_size = and_pitch * (size_t) h;

    if ((size_t) (xor_mask - dib) + xor_size > dib_size) {
        ICON_LOG("DIB XOR data truncated (need %zu, have %zu)", xor_size, dib_size - (xor_mask - dib));
        return 0;
    }

    const uint8_t *and_mask = xor_mask + xor_size;
    if ((size_t) (and_mask - dib) + and_size > dib_size) {
        and_mask = NULL;
    }

    int has_valid_alpha = 0;
    if (bpp == 32) {
        for (int y = 0; y < h && !has_valid_alpha; y++) {
            const uint8_t *row = xor_mask + (size_t) y * xor_pitch;
            for (int x = 0; x < w; x++) {
                if (row[x * 4 + 3] != 0) {
                    has_valid_alpha = 1;
                    break;
                }
            }
        }
    }

    uint8_t *rgba = (uint8_t *) malloc((size_t) w * (size_t) h * 4);
    if (!rgba) return 0;

    for (int y = 0; y < h; y++) {
        int src_y = is_top_down ? y : (h - 1 - y);
        const uint8_t *src_row = xor_mask + (size_t) src_y * xor_pitch;
        const uint8_t *and_row = and_mask ? (and_mask + (size_t) src_y * and_pitch) : NULL;
        uint8_t *dst_row = rgba + (size_t) y * (size_t) w * 4;

        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;

            if (bpp == 32) {
                b = src_row[x * 4 + 0];
                g = src_row[x * 4 + 1];
                r = src_row[x * 4 + 2];
                a = has_valid_alpha ? src_row[x * 4 + 3] : 255;
            } else if (bpp == 24) {
                b = src_row[x * 3 + 0];
                g = src_row[x * 3 + 1];
                r = src_row[x * 3 + 2];
            } else if (bpp == 16) {
                uint16_t pix = rd16(src_row + x * 2);
                r = (uint8_t) (((pix >> 10) & 0x1F) * 255 / 31);
                g = (uint8_t) (((pix >> 5) & 0x1F) * 255 / 31);
                b = (uint8_t) (((pix >> 0) & 0x1F) * 255 / 31);
            } else if (bpp == 8) {
                uint8_t idx = src_row[x];
                if (idx < colors) {
                    b = palette[idx * 4];
                    g = palette[idx * 4 + 1];
                    r = palette[idx * 4 + 2];
                }
            } else if (bpp == 4) {
                uint8_t byte = src_row[x / 2];
                uint8_t idx = (x % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
                if (idx < colors) {
                    b = palette[idx * 4];
                    g = palette[idx * 4 + 1];
                    r = palette[idx * 4 + 2];
                }
            } else if (bpp == 1) {
                uint8_t byte = src_row[x / 8];
                uint8_t idx = (byte >> (7 - (x % 8))) & 1;
                if (idx < colors) {
                    b = palette[idx * 4];
                    g = palette[idx * 4 + 1];
                    r = palette[idx * 4 + 2];
                }
            }
            if (and_row && !(bpp == 32 && has_valid_alpha)) {
                uint8_t mask_bit = (and_row[x / 8] >> (7 - (x % 8))) & 1;
                if (mask_bit) a = 0;
            }

            dst_row[x * 4 + 0] = r;
            dst_row[x * 4 + 1] = g;
            dst_row[x * 4 + 2] = b;
            dst_row[x * 4 + 3] = a;
        }
    }

    *out_rgba = rgba;
    *out_w = w;
    *out_h = h;
    return 1;
}

void IconImage_free(IconImage *image) {
    if (!image) return;
    stbi_image_free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
}

static bool decode_icon_blob_to_image(const uint8_t *img, size_t img_size, IconImage *out) {
    if (!img || img_size < 8 || !out) return false;
    memset(out, 0, sizeof(*out));

    if (img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G' &&
        img[4] == 0x0D && img[5] == 0x0A && img[6] == 0x1A && img[7] == 0x0A) {
        int w = 0, h = 0, comp = 0;
        uint8_t *pixels = stbi_load_from_memory(img, (int) img_size, &w, &h, &comp, 4);
        if (!pixels) return false;
        out->pixels = pixels;
        out->width = w;
        out->height = h;
        return true;
    }

    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (decode_icon_dib_to_rgba(img, img_size, &rgba, &w, &h)) {
        out->pixels = rgba;
        out->width = w;
        out->height = h;
        return true;
    }
    return false;
}

typedef struct {
    uint8_t *data;
    size_t size;
} MemFile;

static void free_file(MemFile *mf) {
    if (mf->data) free(mf->data);
    mf->data = NULL;
    mf->size = 0;
}

static int read_file(const char *path, MemFile *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        return 0;
    }

    out->data = (uint8_t *) malloc(sz);
    if (!out->data) {
        fclose(f);
        return 0;
    }

    if (fread(out->data, 1, sz, f) != (size_t) sz) {
        free_file(out);
        fclose(f);
        return 0;
    }
    out->size = sz;
    fclose(f);
    return 1;
}

static int pe_rva_to_off(uint32_t rva, const uint8_t *data, size_t size, uint32_t sec_tbl_off, uint16_t sec_count,
                         uint32_t *out_off) {
    for (uint16_t i = 0; i < sec_count; i++) {
        const uint8_t *s = data + sec_tbl_off + i * 40;
        uint32_t v_size = rd32(s + 8);
        uint32_t v_addr = rd32(s + 12);
        uint32_t r_size = rd32(s + 16);
        uint32_t r_ptr = rd32(s + 20);

        uint32_t span = (v_size > r_size && v_size != 0) ? v_size : r_size;
        if (rva >= v_addr && rva < v_addr + span) {
            *out_off = r_ptr + (rva - v_addr);
            return (*out_off < size);
        }
    }
    return 0;
}

static int res_find_id_entry(const uint8_t *rsrc, size_t rsrc_size, uint32_t dir_off, uint32_t wanted_id,
                             uint32_t *out_child_off, int *out_is_dir) {
    if (dir_off + 16 > rsrc_size) return 0;
    uint16_t named = rd16(rsrc + dir_off + 12);
    uint16_t ids = rd16(rsrc + dir_off + 14);

    const uint8_t *entries = rsrc + dir_off + 16;
    for (uint16_t i = 0; i < named + ids; i++) {
        if (dir_off + 16 + i * 8 + 8 > rsrc_size) return 0;
        uint32_t name = rd32(entries + i * 8 + 0);
        uint32_t off = rd32(entries + i * 8 + 4);

        if (name & 0x80000000u) continue;
        if ((name & 0x7FFFFFFFu) == wanted_id) {
            *out_child_off = off & 0x7FFFFFFFu;
            *out_is_dir = (off & 0x80000000u) ? 1 : 0;
            return 1;
        }
    }

    if (wanted_id == 0xFFFFFFFFu && ids > 0) {
        uint32_t off = rd32(entries + named * 8 + 4);
        *out_child_off = off & 0x7FFFFFFFu;
        *out_is_dir = (off & 0x80000000u) ? 1 : 0;
        return 1;
    }
    return 0;
}

bool extract_icon_from_exe_pe(const char *path, IconImage *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    MemFile mf;
    if (!read_file(path, &mf)) return false;

    if (mf.size < 0x40 || rd16(mf.data) != MZ_SIG) {
        free_file(&mf);
        return false;
    }
    uint32_t pe_off = rd32(mf.data + 0x3C);
    if (pe_off + 24 > mf.size || rd32(mf.data + pe_off) != PE_SIG) {
        free_file(&mf);
        return false;
    }

    uint16_t sec_count = rd16(mf.data + pe_off + 6);
    uint16_t opt_size = rd16(mf.data + pe_off + 20);
    uint16_t magic = rd16(mf.data + pe_off + 24);
    int is64 = (magic == PE32PLUS_MAGIC);

    uint32_t opt_off = pe_off + 24;
    uint32_t dd_off = opt_off + (is64 ? 0x70 : 0x60);

    if (dd_off + 8 * (IMAGE_DIRECTORY_RESOURCE + 1) > mf.size) {
        free_file(&mf);
        return false;
    }

    uint32_t rsrc_rva = rd32(mf.data + dd_off + IMAGE_DIRECTORY_RESOURCE * 8);
    uint32_t sec_tbl_off = opt_off + opt_size;
    uint32_t rsrc_off = 0;

    if (!rsrc_rva || !pe_rva_to_off(rsrc_rva, mf.data, mf.size, sec_tbl_off, sec_count, &rsrc_off)) {
        free_file(&mf);
        return false;
    }

    const uint8_t *rsrc = mf.data + rsrc_off;
    size_t rsrc_size = mf.size - rsrc_off;

    uint32_t group_type_dir;
    int is_dir;
    if (!res_find_id_entry(rsrc, rsrc_size, 0, RT_GROUP_ICON, &group_type_dir, &is_dir) || !is_dir) {
        free_file(&mf);
        return false;
    }

    uint16_t named_grps = rd16(rsrc + group_type_dir + 12);
    uint16_t id_grps = rd16(rsrc + group_type_dir + 14);
    int total_groups = named_grps + id_grps;

    int best_icon_id = -1;
    int best_score = -1;
    const int IDEAL_DIM = 76;
    for (int g = 0; g < total_groups; g++) {
        uint32_t group_name_dir = rd32(rsrc + group_type_dir + 16 + g * 8 + 4) & 0x7FFFFFFF;
        uint32_t group_lang_dir;

        if (!res_find_id_entry(rsrc, rsrc_size, group_name_dir, 0xFFFFFFFFu, &group_lang_dir, &is_dir) || is_dir)
            continue;

        uint32_t grp_rva = rd32(rsrc + group_lang_dir);
        uint32_t grp_sz = rd32(rsrc + group_lang_dir + 4);
        uint32_t grp_off;

        if (!pe_rva_to_off(grp_rva, mf.data, mf.size, sec_tbl_off, sec_count, &grp_off) || grp_off + grp_sz > mf.size)
            continue;

        const uint8_t *grp = mf.data + grp_off;
        if (grp_sz >= 6) {
            uint16_t count = rd16(grp + 4);
            if (grp_sz >= 6u + count * 14u) {
                for (uint16_t i = 0; i < count; i++) {
                    const uint8_t *e = grp + 6 + i * 14;
                    int w = e[0] == 0 ? 256 : e[0];
                    int h = e[1] == 0 ? 256 : e[1];
                    int bpp = rd16(e + 6);
                    uint16_t id = rd16(e + 12);
                    int depth_bonus = 0;
                    if (bpp >= 32) depth_bonus = 200000;
                    else if (bpp >= 24) depth_bonus = 150000;
                    else if (bpp >= 8) depth_bonus = 80000;

                    int dim_score;
                    if (w >= IDEAL_DIM) {
                        dim_score = 100000 - (w - IDEAL_DIM) * 50;
                    } else {
                        dim_score = w * 100;
                    }

                    int score = depth_bonus + dim_score + bpp;
                    if (score > best_score) {
                        best_score = score;
                        best_icon_id = id;
                    }
                }
            }
        }
    }

    if (best_icon_id < 0) {
        free_file(&mf);
        return false;
    }

    uint32_t icon_type_dir;
    if (!res_find_id_entry(rsrc, rsrc_size, 0, RT_ICON, &icon_type_dir, &is_dir) || !is_dir) {
        free_file(&mf);
        return false;
    }
    uint32_t icon_name_dir;
    if (!res_find_id_entry(rsrc, rsrc_size, icon_type_dir, best_icon_id, &icon_name_dir, &is_dir) || !is_dir) {
        free_file(&mf);
        return false;
    }
    uint32_t icon_lang_dir;
    if (!res_find_id_entry(rsrc, rsrc_size, icon_name_dir, 0xFFFFFFFFu, &icon_lang_dir, &is_dir) || is_dir) {
        free_file(&mf);
        return false;
    }

    uint32_t icon_blob_rva = rd32(rsrc + icon_lang_dir);
    uint32_t icon_blob_sz = rd32(rsrc + icon_lang_dir + 4);
    uint32_t icon_off;
    if (!pe_rva_to_off(icon_blob_rva, mf.data, mf.size, sec_tbl_off, sec_count, &icon_off) || icon_off + icon_blob_sz >
        mf.size) {
        free_file(&mf);
        return false;
    }

    bool ok = decode_icon_blob_to_image(mf.data + icon_off, icon_blob_sz, out);
    free_file(&mf);
    return ok;
}

bool load_image_from_file(const char *path, IconImage *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    int w, h, comp;
    uint8_t *pixels = stbi_load(path, &w, &h, &comp, 4);
    if (!pixels) return false;
    out->pixels = pixels;
    out->width = w;
    out->height = h;
    return true;
}
