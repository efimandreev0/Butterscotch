#pragma once

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include "real_type.h"

#define forEach(type, item, array, count) \
    for (typeof(count) item##_i_ = 0; item##_i_ < (count); item##_i_++) \
    for (type* item = &(array)[item##_i_]; item; item = NULL)

#define forEachIndexed(type, item, index, array, count) \
    for (typeof(count) index = 0; index < (count); index++) \
    for (type* item = &(array)[index]; item; item = NULL)

// The "typeof((typeof(n))0" is used to remove the "const" from the typeof

#define repeat(n, it) for (typeof((typeof(n))0) it = 0; it < (n); it++)

#define require(condition) \
    do { \
        if (!(condition)) { \
        fprintf(stderr, "Requirement failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define requireMessage(condition, message) \
do { \
if (!(condition)) { \
fprintf(stderr, "Requirement failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
abort(); \
} \
} while (0)

#define requireMessageFormatted(condition, fmt, ...) \
do { \
if (!(condition)) { \
fprintf(stderr, "Requirement failed at %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
abort(); \
} \
} while (0)

#define requireNotNull(ptr) ({ \
typeof(ptr) _val = (ptr); \
if (_val == NULL) { \
fprintf(stderr, "%s:%d: requireNotNull failed: '%s'\n", __FILE__, __LINE__, #ptr); \
abort(); \
} \
_val; \
})

#define requireNotNullMessage(ptr, msg) ({ \
typeof(ptr) _val = (ptr); \
if (_val == NULL) { \
fprintf(stderr, "%s:%d: requireNotNull failed: %s\n", __FILE__, __LINE__, (msg)); \
abort(); \
} \
_val; \
})

// Safe allocation macros - check for nullptr and abort with file/line info
#define safeMalloc(size) ({ \
    void* _ptr = malloc(size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: malloc(%zu) failed at %s:%d\n", (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeCalloc(count, size) ({ \
    void* _ptr = calloc(count, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: calloc(%zu, %zu) failed at %s:%d\n", (size_t)(count), (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeRealloc(ptr, size) ({ \
    void* _ptr = realloc(ptr, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: realloc(%zu) failed at %s:%d\n", (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeMemalign(alignment, size) ({ \
    void* _ptr = memalign(alignment, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: memalign(%zu, %zu) failed at %s:%d\n", (size_t)(alignment), (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeStrdup(str) ({ \
    char* _ptr = strdup(str); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: strdup() failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

// Big-array allocator. For multi-megabyte buffers (asset metadata arrays,
// code/script tables, parsed pools, anything sized by `count * sizeof(T)`
// where count comes from a multi-thousand asset list) we'd rather burn linear
// RAM than crowd the 3DS heap. Behaviour falls back to malloc/calloc/free on
// other platforms so semantics stay identical.
//
// IMPORTANT: only call bigFree on pointers obtained from bigMalloc/bigCalloc.
// Mixing with regular free() corrupts the linear arena.
#ifdef __3DS__
#include <3ds.h>
#define bigMalloc(size) ({ \
    void* _bp = linearAlloc((size_t)(size)); \
    if (_bp == nullptr) { \
        fprintf(stderr, "FATAL: linearAlloc(%zu) for big-buf failed at %s:%d\n", \
                (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _bp; \
})
#define bigCalloc(count, size) ({ \
    size_t _bb = (size_t)(count) * (size_t)(size); \
    void* _bp = linearAlloc(_bb); \
    if (_bp == nullptr) { \
        fprintf(stderr, "FATAL: linearAlloc(%zu) for big-buf failed at %s:%d\n", \
                _bb, __FILE__, __LINE__); \
        abort(); \
    } \
    memset(_bp, 0, _bb); \
    _bp; \
})
#define bigFree(ptr) do { if ((ptr) != nullptr) linearFree(ptr); } while (0)
// linearAlloc has no realloc — manually grow: alloc new, memcpy, free old.
// `oldBytes` MUST be the live data size of the existing allocation; passing
// the wrong value silently corrupts whatever follows the old block.
// `newBytes` is the size of the returned buffer.
#define bigRealloc(ptr, oldBytes, newBytes) ({ \
    size_t _on = (size_t)(oldBytes); \
    size_t _nn = (size_t)(newBytes); \
    void* _np = linearAlloc(_nn); \
    if (_np == nullptr) { \
        fprintf(stderr, "FATAL: linearAlloc(%zu) for big-realloc failed at %s:%d\n", \
                _nn, __FILE__, __LINE__); \
        abort(); \
    } \
    if ((ptr) != nullptr && _on > 0) memcpy(_np, (ptr), _on < _nn ? _on : _nn); \
    if ((ptr) != nullptr) linearFree(ptr); \
    _np; \
})
#else
#define bigMalloc(size)                       safeMalloc(size)
#define bigCalloc(count, size)                safeCalloc((count), (size))
#define bigFree(ptr)                          free(ptr)
#define bigRealloc(ptr, oldBytes, newBytes)   safeRealloc((ptr), (newBytes))
#endif

// Truncates to 6 decimal places, matching the HTML5 runner's ClampFloat
static inline GMLReal clampFloat(GMLReal f) {
    return ((GMLReal) ((int64_t) (f * 1000000.0))) / 1000000.0;
}

#define BGR_B(c) (((c) >> 16) & 0xFF)
#define BGR_G(c) (((c) >>  8) & 0xFF)
#define BGR_R(c) (((c) >>  0) & 0xFF)

#define shcopyFromTo(src, dst)                        \
do {                                        \
(dst) = NULL;                           \
for (int i = 0; i < shlen(src); i++)    \
shput((dst), (src)[i].key, (src)[i].value); \
} while (0)

typedef struct {
    char* key;
    bool value;
} StringBooleanEntry;