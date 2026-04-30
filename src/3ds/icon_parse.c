//
// Created by efimandreev0 on 30.04.2026.
//

#include "icon_parse.h"

#define ICON_LOG(...) do { \
fprintf(stderr, "[ICON] " __VA_ARGS__); \
fprintf(stderr, "\n"); \
} while(0)


static int decode_icon_dib_to_rgba(const uint8_t *dib, size_t dib_size,
                                  uint8_t **out_rgba, int *out_w, int *out_h)
{
    if (dib_size < 40) return 0;

    uint32_t hdr_size = rd32(dib + 0);
    if (hdr_size < 40 || hdr_size > dib_size) return 0;

    int32_t w = (int32_t)rd32(dib + 4);
    int32_t h2 = (int32_t)rd32(dib + 8);
    uint16_t planes = rd16(dib + 12);
    uint16_t bpp = rd16(dib + 14);
    uint32_t compression = rd32(dib + 16);
    uint32_t clr_used = 0;
    if (hdr_size >= 40) clr_used = rd32(dib + 32);

    if (planes != 1 || w <= 0 || h2 <= 0) return 0;
    if (compression != 0) return 0;

    int h = h2 / 2;
    if (h <= 0) return 0;

    int palette_entries = 0;
    if (bpp <= 8) {
        palette_entries = clr_used ? (int)clr_used : (1 << bpp);
    }

    const uint8_t *palette = dib + hdr_size;
    size_t palette_size = (size_t)palette_entries * 4u;
    if (hdr_size + palette_size > dib_size) return 0;

    const uint8_t *xor_bits = palette + palette_size;
    size_t xor_row_bytes = ((size_t)w * (size_t)bpp + 31u) / 32u * 4u;
    size_t xor_size = xor_row_bytes * (size_t)h;

    const uint8_t *and_bits = xor_bits + xor_size;
    size_t and_row_bytes = ((size_t)w + 31u) / 32u * 4u;
    size_t and_size = and_row_bytes * (size_t)h;

    if ((size_t)(and_bits - dib) + and_size > dib_size) return 0;

    uint8_t *rgba = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) return 0;

    for (int y = 0; y < h; y++) {
        int src_y = h - 1 - y; // bottom-up
        const uint8_t *xrow = xor_bits + (size_t)src_y * xor_row_bytes;
        const uint8_t *arow = and_bits + (size_t)src_y * and_row_bytes;

        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;

            switch (bpp) {
                case 32: {
                    const uint8_t *p = xrow + (size_t)x * 4u;
                    b = p[0];
                    g = p[1];
                    r = p[2];
                    a = p[3];
                    //if (a == 0) a = 255; // some icons store garbage alpha; still displayable
                } break;

                case 24: {
                    const uint8_t *p = xrow + (size_t)x * 3u;
                    b = p[0];
                    g = p[1];
                    r = p[2];
                } break;

                case 8: {
                    uint8_t idx = xrow[x];
                    if (idx < palette_entries) {
                        const uint8_t *c = palette + (size_t)idx * 4u;
                        b = c[0]; g = c[1]; r = c[2];
                    }
                } break;

                case 4: {
                    uint8_t byte = xrow[x >> 1];
                    uint8_t idx = (x & 1) ? (byte & 0x0F) : (byte >> 4);
                    if (idx < palette_entries) {
                        const uint8_t *c = palette + (size_t)idx * 4u;
                        b = c[0]; g = c[1]; r = c[2];
                    }
                } break;

                case 1: {
                    uint8_t byte = xrow[x >> 3];
                    uint8_t bit = (byte >> (7 - (x & 7))) & 1u;
                    uint8_t idx = bit;
                    if (idx < palette_entries) {
                        const uint8_t *c = palette + (size_t)idx * 4u;
                        b = c[0]; g = c[1]; r = c[2];
                    }
                } break;

                default:
                    free(rgba);
                    return 0;
            }

            // AND mask: 1 = transparent
            {
                uint8_t mask_byte = arow[x >> 3];
                uint8_t mask_bit = (mask_byte >> (7 - (x & 7))) & 1u;
                if (mask_bit) a = 0;
            }

            size_t dst = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            rgba[dst + 0] = r;
            rgba[dst + 1] = g;
            rgba[dst + 2] = b;
            rgba[dst + 3] = a;
        }
    }

    *out_rgba = rgba;
    *out_w = w;
    *out_h = h;
    return 1;
}

static GLuint decode_icon_blob_to_texture(const uint8_t *img, size_t img_size) {
    if (img_size >= 8 &&
        img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G' &&
        img[4] == 0x0D && img[5] == 0x0A && img[6] == 0x1A && img[7] == 0x0A)
    {
        int w = 0, h = 0, comp = 0;
        uint8_t *pixels = stbi_load_from_memory(img, (int)img_size, &w, &h, &comp, 4);
        if (!pixels) return 0;

        GLuint tex = upload_rgba_texture(pixels, w, h);
        stbi_image_free(pixels);
        return tex;
    }

    // DIB / BMP-style icon resource
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (decode_icon_dib_to_rgba(img, img_size, &rgba, &w, &h)) {
        GLuint tex = upload_rgba_texture(rgba, w, h);
        free(rgba);
        return tex;
    }

    return 0;
}

#define ICON_LOG(...) do { \
    fprintf(stderr, "[ICON] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)

#define PE_SIG              0x00004550u
#define MZ_SIG              0x5A4Du
#define PE32_MAGIC          0x10Bu
#define PE32PLUS_MAGIC      0x20Bu
#define RT_ICON             3u
#define RT_GROUP_ICON       14u
#define IMAGE_DIRECTORY_RESOURCE 2

uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

typedef struct {
    const uint8_t *data;
    size_t size;
} MemFile;

static int read_file_all(const char *path, MemFile *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }

    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return 0;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    uint8_t *buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return 0;
    }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (got != (size_t)sz) {
        free(buf);
        return 0;
    }

    out->data = buf;
    out->size = (size_t)sz;
    return 1;
}

static void free_file_all(MemFile *f) {
    if (f && f->data) free((void*)f->data);
    memset(f, 0, sizeof(*f));
}

static int pe_get_header_info(const uint8_t *data, size_t size,
                              uint32_t *out_pe_off,
                              uint16_t *out_num_sections,
                              uint16_t *out_opt_size,
                              int *out_is_pe32plus)
{
    if (size < 0x40) return 0;
    if (rd16(data + 0x00) != MZ_SIG) return 0;

    uint32_t pe_off = rd32(data + 0x3C);
    if (pe_off + 4 + 20 > size) return 0;

    if (rd32(data + pe_off) != PE_SIG) return 0;

    uint16_t num_sections = rd16(data + pe_off + 6);
    uint16_t opt_size     = rd16(data + pe_off + 20);

    if (pe_off + 24 + opt_size > size) return 0;
    if (opt_size < 2) return 0;

    uint16_t magic = rd16(data + pe_off + 24);
    if (magic != PE32_MAGIC && magic != PE32PLUS_MAGIC) return 0;

    *out_pe_off = pe_off;
    *out_num_sections = num_sections;
    *out_opt_size = opt_size;
    *out_is_pe32plus = (magic == PE32PLUS_MAGIC) ? 1 : 0;
    return 1;
}

static int pe_get_resource_dir(const uint8_t *data, size_t size,
                               uint32_t *out_rva, uint32_t *out_size,
                               uint32_t *out_section_table_off,
                               uint16_t *out_num_sections)
{
    uint32_t pe_off = 0;
    uint16_t sec_count = 0, opt_size = 0;
    int is64 = 0;

    if (!pe_get_header_info(data, size, &pe_off, &sec_count, &opt_size, &is64))
        return 0;

    // DataDirectory starts after standard fields:
    // PE32  : 0x60
    // PE32+ : 0x70
    uint32_t opt_off = pe_off + 24;
    uint32_t dd_off = opt_off + (is64 ? 0x70u : 0x60u);

    // need at least 3rd directory (RESOURCE), each entry 8 bytes
    if (dd_off + (IMAGE_DIRECTORY_RESOURCE + 1u) * 8u > size) return 0;

    uint32_t rsrc_rva  = rd32(data + dd_off + IMAGE_DIRECTORY_RESOURCE * 8u + 0);
    uint32_t rsrc_size = rd32(data + dd_off + IMAGE_DIRECTORY_RESOURCE * 8u + 4);

    uint32_t sec_table_off = opt_off + opt_size;
    if (sec_table_off + (uint32_t)sec_count * 40u > size) return 0;

    *out_rva = rsrc_rva;
    *out_size = rsrc_size;
    *out_section_table_off = sec_table_off;
    *out_num_sections = sec_count;
    return 1;
}

static int pe_rva_to_off(uint32_t rva, const uint8_t *data, size_t size,
                         uint32_t section_table_off, uint16_t sec_count,
                         uint32_t *out_off)
{
    for (uint16_t i = 0; i < sec_count; i++) {
        const uint8_t *s = data + section_table_off + (size_t)i * 40u;

        uint32_t virtual_size    = rd32(s + 8);
        uint32_t virtual_address = rd32(s + 12);
        uint32_t raw_size        = rd32(s + 16);
        uint32_t raw_ptr         = rd32(s + 20);

        uint32_t span = (virtual_size > raw_size) ? virtual_size : raw_size;

        if (rva >= virtual_address && rva < virtual_address + span) {
            uint32_t off = raw_ptr + (rva - virtual_address);
            if (off < size) {
                *out_off = off;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

// ===== Resource tree helpers =====

static int res_dir_count(const uint8_t *rsrc, size_t rsrc_size,
                         uint32_t dir_off, uint16_t *out_total)
{
    if (dir_off + 16 > rsrc_size) return 0;
    uint16_t named = rd16(rsrc + dir_off + 12);
    uint16_t ids   = rd16(rsrc + dir_off + 14);
    uint32_t total = (uint32_t)named + (uint32_t)ids;
    if (dir_off + 16u + total * 8u > rsrc_size) return 0;
    *out_total = (uint16_t)total;
    return 1;
}

static int res_find_id_entry(const uint8_t *rsrc, size_t rsrc_size,
                             uint32_t dir_off, uint32_t wanted_id,
                             uint32_t *out_child_off, int *out_is_dir)
{
    uint16_t total = 0;
    if (!res_dir_count(rsrc, rsrc_size, dir_off, &total)) return 0;

    const uint8_t *entries = rsrc + dir_off + 16;
    for (uint16_t i = 0; i < total; i++) {
        const uint8_t *e = entries + (size_t)i * 8u;
        uint32_t name = rd32(e + 0);
        uint32_t off  = rd32(e + 4);

        if (name & 0x80000000u) continue; // string name, ignore
        if ((name & 0x7FFFFFFFu) != wanted_id) continue;

        *out_child_off = off & 0x7FFFFFFFu;
        *out_is_dir = (off & 0x80000000u) ? 1 : 0;
        return 1;
    }
    return 0;
}

static int res_get_data_entry(const uint8_t *rsrc, size_t rsrc_size,
                              uint32_t type_id, uint32_t name_id,
                              uint32_t *out_data_rva, uint32_t *out_data_size)
{
    uint32_t type_dir = 0;
    int is_dir = 0;

    if (!res_find_id_entry(rsrc, rsrc_size, 0, type_id, &type_dir, &is_dir) || !is_dir)
        return 0;

    uint32_t name_dir = 0;
    if (!res_find_id_entry(rsrc, rsrc_size, type_dir, name_id, &name_dir, &is_dir) || !is_dir)
        return 0;

    uint16_t total = 0;
    if (!res_dir_count(rsrc, rsrc_size, name_dir, &total) || total == 0)
        return 0;

    // third level: language entries. any one is enough
    const uint8_t *lang_entry = rsrc + name_dir + 16;
    uint32_t data_rel = rd32(lang_entry + 4);
    if (data_rel & 0x80000000u) return 0;
    data_rel &= 0x7FFFFFFFu;

    if (data_rel + 16 > rsrc_size) return 0;

    *out_data_rva  = rd32(rsrc + data_rel + 0);
    *out_data_size = rd32(rsrc + data_rel + 4);
    return 1;
}

static int pick_best_group_icon_index(const uint8_t *grp, size_t grp_size) {
    if (grp_size < 6) return -1;

    uint16_t count = rd16(grp + 4);
    if (count == 0) return -1;

    if (grp_size < 6u + (size_t)count * 14u) return -1;

    int best = 0;
    int best_score = -1;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = grp + 6u + (size_t)i * 14u;

        int w = e[0] ? e[0] : 256;
        int h = e[1] ? e[1] : 256;
        int bpp = (int)rd16(e + 6);

        // area dominates, bpp breaks ties
        int score = w * h * 16 + bpp;

        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }

    return best;
}

GLuint upload_rgba_texture(const uint8_t *pixels, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}


// Загрузка кастомной иконки из файла (icon.png)
GLuint load_texture_from_file(const char *path) {
    int w, h, comp;
    uint8_t *pixels = stbi_load(path, &w, &h, &comp, 4);
    if (!pixels) return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(pixels);
    return tex;
}



static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

uint32_t rva_to_offset(uint32_t rva,
                       IMAGE_SECTION_HEADER* sections,
                       int section_count)
{
    for (int i = 0; i < section_count; i++)
    {
        uint32_t va = sections[i].VirtualAddress;
        uint32_t size = sections[i].SizeOfRawData;

        if (rva >= va && rva < va + size)
        {
            return sections[i].PointerToRawData + (rva - va);
        }
    }

    printf("[ICON] rva_to_offset FAIL: %08X\n", rva);
    return 0;
}

static GLuint extract_icon_from_exe_pe(const char *path) {
    ICON_LOG("Opening: %s", path);

    MemFile mf;
    if (!read_file_all(path, &mf)) {
        ICON_LOG("FAIL: open/read");
        return 0;
    }

    ICON_LOG("File size: %zu", mf.size);

    uint32_t pe_off = 0, sec_table_off = 0;
    uint16_t sec_count = 0, opt_size = 0;
    int is64 = 0;

    if (!pe_get_header_info(mf.data, mf.size, &pe_off, &sec_count, &opt_size, &is64)) {
        ICON_LOG("FAIL: not a valid PE32/PE32+");
        free_file_all(&mf);
        return 0;
    }

    ICON_LOG("PE offset: %08X", pe_off);
    ICON_LOG("Sections: %u", sec_count);
    ICON_LOG("Format: %s", is64 ? "PE32+" : "PE32");

    uint32_t rsrc_rva = 0, rsrc_size = 0;
    if (!pe_get_resource_dir(mf.data, mf.size, &rsrc_rva, &rsrc_size, &sec_table_off, &sec_count)) {
        ICON_LOG("FAIL: cannot read resource directory");
        free_file_all(&mf);
        return 0;
    }

    ICON_LOG("Resource RVA: %08X size: %08X", rsrc_rva, rsrc_size);

    uint32_t rsrc_off = 0;
    if (!pe_rva_to_off(rsrc_rva, mf.data, mf.size, sec_table_off, sec_count, &rsrc_off)) {
        ICON_LOG("FAIL: cannot map resource RVA to file offset");
        free_file_all(&mf);
        return 0;
    }

    ICON_LOG("Resource file offset: %08X", rsrc_off);

    if (rsrc_off >= mf.size) {
        ICON_LOG("FAIL: resource offset out of range");
        free_file_all(&mf);
        return 0;
    }

    const uint8_t *rsrc = mf.data + rsrc_off;
    rsrc_size = mf.size - rsrc_off;

    // 1) Find group icon directory and raw blob
    uint32_t grp_rva = 0, grp_size = 0;
    if (!res_get_data_entry(rsrc, rsrc_size, RT_GROUP_ICON, 1, &grp_rva, &grp_size)) {
        // name id 1 is not guaranteed; just search root for type 14 first
        uint32_t type_dir = 0;
        int is_dir = 0;
        if (!res_find_id_entry(rsrc, rsrc_size, 0, RT_GROUP_ICON, &type_dir, &is_dir) || !is_dir) {
            ICON_LOG("FAIL: no RT_GROUP_ICON");
            free_file_all(&mf);
            return 0;
        }

        // use first group icon name entry
        uint16_t total = 0;
        if (!res_dir_count(rsrc, rsrc_size, type_dir, &total) || total == 0) {
            ICON_LOG("FAIL: empty RT_GROUP_ICON directory");
            free_file_all(&mf);
            return 0;
        }

        const uint8_t *first = rsrc + type_dir + 16;
        uint32_t name_id = rd32(first + 0) & 0x7FFFFFFFu;

        if (!res_get_data_entry(rsrc, rsrc_size, RT_GROUP_ICON, name_id, &grp_rva, &grp_size)) {
            ICON_LOG("FAIL: cannot read group icon blob");
            free_file_all(&mf);
            return 0;
        }
    }

    uint32_t grp_off = 0;
    if (!pe_rva_to_off(grp_rva, mf.data, mf.size, sec_table_off, sec_count, &grp_off)) {
        ICON_LOG("FAIL: cannot map group icon to file offset");
        free_file_all(&mf);
        return 0;
    }

    if (grp_off + grp_size > mf.size) {
        ICON_LOG("FAIL: group icon out of range");
        free_file_all(&mf);
        return 0;
    }

    const uint8_t *grp = mf.data + grp_off;

    if (grp_size < 6) {
        ICON_LOG("FAIL: group icon too small");
        free_file_all(&mf);
        return 0;
    }

    uint16_t count = rd16(grp + 4);
    ICON_LOG("Icon count: %u", count);

    if (count == 0 || grp_size < 6u + (size_t)count * 14u) {
        ICON_LOG("FAIL: broken group icon table");
        free_file_all(&mf);
        return 0;
    }

    int best = pick_best_group_icon_index(grp, grp_size);
    if (best < 0) {
        ICON_LOG("FAIL: cannot pick best icon");
        free_file_all(&mf);
        return 0;
    }

    const uint8_t *best_entry = grp + 6u + (size_t)best * 14u;
    uint16_t icon_id = rd16(best_entry + 12);

    ICON_LOG("Chosen icon resource id: %u", icon_id);

    // 2) Extract RT_ICON blob by id
    uint32_t icon_rva = 0, icon_size = 0;
    if (!res_get_data_entry(rsrc, rsrc_size, RT_ICON, icon_id, &icon_rva, &icon_size)) {
        ICON_LOG("FAIL: cannot find RT_ICON payload");
        free_file_all(&mf);
        return 0;
    }

    uint32_t icon_off = 0;
    if (!pe_rva_to_off(icon_rva, mf.data, mf.size, sec_table_off, sec_count, &icon_off)) {
        ICON_LOG("FAIL: cannot map RT_ICON to file offset");
        free_file_all(&mf);
        return 0;
    }

    if (icon_off + icon_size > mf.size) {
        ICON_LOG("FAIL: RT_ICON out of range");
        free_file_all(&mf);
        return 0;
    }

    const uint8_t *img = mf.data + icon_off;
    ICON_LOG("Icon blob offset: %08X size: %u", icon_off, icon_size);

    GLuint tex = decode_icon_blob_to_texture(img, icon_size);
    if (!tex) {
        ICON_LOG("FAIL: icon decode failed");
        free_file_all(&mf);
        return 0;
    }

    ICON_LOG("SUCCESS");
    free_file_all(&mf);
    return tex;
}
