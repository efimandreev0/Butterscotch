#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CTR_STBI_ALLOC_MAGIC 0x33494253u
#define CTR_STBI_BACKEND_HEAP 1u
#define CTR_STBI_BACKEND_LINEAR 2u

typedef struct {
    uint32_t magic;
    uint32_t backend;
    uint32_t size;
    uint32_t reserved;
} CtrStbiAllocHeader;

void *CtrStbi_malloc(size_t size) {
    if (size == 0) size = 1;
    size_t total = sizeof(CtrStbiAllocHeader) + size;
    // Большие буферы (>=256 КБ) ВСЕГДА в линейку — у нас heap забит DataWin'ом,
    // VM bytecode'ом и т.п., туда не влезет 16-21 МБ под декод RGBA8 страниц.
    // Старый «safety margin 2 МБ» отправлял такие в heap при чуть занятой линейке
    // и взрывал сборку atlas.bin.
    bool preferLinear = size >= (256u * 1024u);

    void *base = NULL;
    uint32_t backend = 0;
    if (preferLinear) {
        base = linearAlloc(total);
        backend = CTR_STBI_BACKEND_LINEAR;
        if (!base) {
            base = malloc(total);
            backend = CTR_STBI_BACKEND_HEAP;
        }
    } else {
        base = malloc(total);
        backend = CTR_STBI_BACKEND_HEAP;
        if (!base) {
            base = linearAlloc(total);
            backend = CTR_STBI_BACKEND_LINEAR;
        }
    }
    if (!base) return NULL;

    CtrStbiAllocHeader *hdr = (CtrStbiAllocHeader *)base;
    hdr->magic = CTR_STBI_ALLOC_MAGIC;
    hdr->backend = backend;
    hdr->size = (uint32_t)size;
    hdr->reserved = 0;
    return (void *)(hdr + 1);
}

void CtrStbi_free(void *ptr) {
    if (!ptr) return;
    CtrStbiAllocHeader *hdr = ((CtrStbiAllocHeader *)ptr) - 1;
    if (hdr->magic != CTR_STBI_ALLOC_MAGIC) {
        free(ptr);
        return;
    }
    hdr->magic = 0;
    if (hdr->backend == CTR_STBI_BACKEND_LINEAR) linearFree(hdr);
    else free(hdr);
}

void *CtrStbi_realloc(void *ptr, size_t size) {
    if (!ptr) return CtrStbi_malloc(size);
    if (size == 0) {
        CtrStbi_free(ptr);
        return NULL;
    }

    CtrStbiAllocHeader *oldHdr = ((CtrStbiAllocHeader *)ptr) - 1;
    if (oldHdr->magic != CTR_STBI_ALLOC_MAGIC) return realloc(ptr, size);

    void *next = CtrStbi_malloc(size);
    if (!next) return NULL;
    size_t copy = oldHdr->size < size ? oldHdr->size : size;
    memcpy(next, ptr, copy);
    CtrStbi_free(ptr);
    return next;
}

#define STBI_MALLOC(sz)       CtrStbi_malloc(sz)
#define STBI_REALLOC(p, sz)   CtrStbi_realloc(p, sz)
#define STBI_FREE(p)          CtrStbi_free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
