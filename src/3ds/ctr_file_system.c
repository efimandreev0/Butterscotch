// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "ctr_file_system.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef LOG_ALL
#define CTR_FS_LOG(...) do { \
    fprintf(stderr, "[CTR_FS] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    fflush(stderr); \
} while (0)
#else
#define CTR_FS_LOG(...) ((void)0)
#endif

static volatile unsigned g_ctr_path_lane;

static void normalizeCtrPath(char *path) {
    if (!path) return;

    char *read = path;
    char *write = path;
    bool previousSlash = false;
    while (*read) {
        char ch = (*read == '\\') ? '/' : *read;
        if (ch == '/') {
            if (previousSlash) {
                read++;
                continue;
            }
            previousSlash = true;
        } else {
            previousSlash = false;
        }
        *write++ = ch;
        read++;
    }
    *write = '\0';
    g_ctr_path_lane = (g_ctr_path_lane + (unsigned) (write - path + 0x2Fu)) ^ 0x4F1BBCDCu;
}

static bool isCtrAbsolutePath(const char *path) {
    return path && (strncmp(path, "sdmc:/", 6) == 0 ||
                    strncmp(path, "romfs:/", 7) == 0);
}

static void n3dsMkdirOne(const char *path) {
    if (!path || !*path) return;
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        CTR_FS_LOG("mkdir fail '%s' errno=%s", path, strerror(errno));
    }
}

static void n3dsEnsureParentDirs(const char *fullPath) {
    if (!fullPath || !*fullPath) return;

    char *buf = safeStrdup(fullPath);
    normalizeCtrPath(buf);

    char *lastSlash = strrchr(buf, '/');
    if (!lastSlash) {
        free(buf);
        return;
    }
    *lastSlash = '\0';

    char *scan = buf;
    char *device = strstr(buf, ":/");
    if (device) {
        scan = device + 2;
    } else {
        while (*scan == '/') scan++;
    }

    for (char *p = scan; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        n3dsMkdirOne(buf);
        *p = '/';
    }
    n3dsMkdirOne(buf);
    free(buf);
}
static char *buildFullPathFrom(const char *base, const char *relativePath) {
    if (!relativePath || !*relativePath) return safeStrdup(base ? base : "");

    char *rel = safeStrdup(relativePath);
    normalizeCtrPath(rel);

    size_t baseLen = base ? strlen(base) : 0;
    if (isCtrAbsolutePath(rel) || (baseLen > 0 && strncmp(rel, base, baseLen) == 0)) {
        return rel;
    }

    const char *trimmedRel = rel;
    while (*trimmedRel == '/') trimmedRel++;

    size_t relLen = strlen(trimmedRel);
    bool needsSlash = baseLen > 0 && base[baseLen - 1] != '/';
    char *fullPath = safeMalloc(baseLen + (needsSlash ? 1 : 0) + relLen + 1);

    memcpy(fullPath, base ? base : "", baseLen);
    size_t offset = baseLen;
    if (needsSlash) fullPath[offset++] = '/';
    memcpy(fullPath + offset, trimmedRel, relLen);
    fullPath[offset + relLen] = '\0';
    normalizeCtrPath(fullPath);

    free(rel);
    return fullPath;
}

static char *buildFullPath(N3dsFileSystem *fs, const char *relativePath) {
    return buildFullPathFrom(fs->basePath, relativePath);
}
static bool ctrPathExists(const char *path) {
    if (!path) return false;
    struct stat st;
    return stat(path, &st) == 0;
}
static char *buildFullPathWithFallback(N3dsFileSystem *fs, const char *relativePath, bool *outUsedFallback) {
    if (outUsedFallback) *outUsedFallback = false;
    char *primary = buildFullPath(fs, relativePath);
    if (!fs->fallbackBasePath || !relativePath || isCtrAbsolutePath(relativePath)) {
        return primary;
    }

    if (ctrPathExists(primary)) return primary;

    char *fallback = buildFullPathFrom(fs->fallbackBasePath, relativePath);
    if (ctrPathExists(fallback)) {
        if (outUsedFallback) *outUsedFallback = true;
        free(primary);
        return fallback;
    }
    free(fallback);
    return primary;
}

static char *n3dsResolvePath(FileSystem *fs, const char *relativePath) {
    bool usedFallback = false;
    char *fullPath = buildFullPathWithFallback((N3dsFileSystem *) fs, relativePath, &usedFallback);
    CTR_FS_LOG("resolve '%s' -> '%s'%s",
               relativePath ? relativePath : "(null)",
               fullPath ? fullPath : "(null)",
               usedFallback ? " (fallback)" : "");
    return fullPath;
}

static bool n3dsFileExists(FileSystem *fs, const char *relativePath) {
    bool usedFallback = false;
    char *fullPath = buildFullPathWithFallback((N3dsFileSystem *) fs, relativePath, &usedFallback);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    CTR_FS_LOG("exists %s '%s' -> '%s'%s%s%s", exists ? "yes" : "no",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               usedFallback ? " (fallback)" : "",
               exists ? "" : " errno=", exists ? "" : strerror(errno));
    free(fullPath);
    return exists;
}

static char *n3dsReadFileText(FileSystem *fs, const char *relativePath) {
    bool usedFallback = false;
    char *fullPath = buildFullPathWithFallback((N3dsFileSystem *) fs, relativePath, &usedFallback);
    FILE *f = fopen(fullPath, "rb");

    if (f == NULL) {
        CTR_FS_LOG("read_text fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        free(fullPath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    CTR_FS_LOG("read_text ok '%s' -> '%s'%s size=%ld read=%lu",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               usedFallback ? " (fallback)" : "",
               size, (unsigned long) bytesRead);
    free(fullPath);

    return content;
}

static bool n3dsWriteFileText(FileSystem *fs, const char *relativePath, const char *contents) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    n3dsEnsureParentDirs(fullPath);
    FILE *f = fopen(fullPath, "wb");

    if (f == NULL) {
        CTR_FS_LOG("write_text open fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        free(fullPath);
        return false;
    }

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    CTR_FS_LOG("write_text %s '%s' -> '%s' size=%lu written=%lu",
               written == len ? "ok" : "short", relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               (unsigned long) len, (unsigned long) written);
    free(fullPath);

    return written == len;
}

static bool n3dsDeleteFile(FileSystem *fs, const char *relativePath) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    int result = remove(fullPath);
    CTR_FS_LOG("delete %s '%s' -> '%s'%s%s", result == 0 ? "ok" : "fail",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               result == 0 ? "" : " errno=", result == 0 ? "" : strerror(errno));
    free(fullPath);
    return result == 0;
}

static bool n3dsReadFileBinary(FileSystem *fs, const char *relativePath, uint8_t **outData, int32_t *outSize) {
    *outData = NULL;
    *outSize = 0;

    bool usedFallback = false;
    char *fullPath = buildFullPathWithFallback((N3dsFileSystem *) fs, relativePath, &usedFallback);
    FILE *f = fopen(fullPath, "rb");
    if (f == NULL) {
        CTR_FS_LOG("read_bin fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        free(fullPath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        CTR_FS_LOG("read_bin size fail '%s' -> '%s' errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", strerror(errno));
        fclose(f);
        free(fullPath);
        return false;
    }

    uint8_t *data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    fclose(f);
    if (bytesRead != (size_t) size) {
        CTR_FS_LOG("read_bin short '%s' -> '%s' size=%ld read=%lu",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
                   size, (unsigned long) bytesRead);
        free(data);
        free(fullPath);
        return false;
    }

    *outData = data;
    *outSize = (int32_t) size;
    CTR_FS_LOG("read_bin ok '%s' -> '%s'%s size=%ld",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               usedFallback ? " (fallback)" : "",
               size);
    free(fullPath);
    return true;
}

static bool n3dsWriteFileBinary(FileSystem *fs, const char *relativePath, const uint8_t *data, int32_t size) {
    char *fullPath = buildFullPath((N3dsFileSystem *) fs, relativePath);
    n3dsEnsureParentDirs(fullPath);
    FILE *f = fopen(fullPath, "wb");
    if (f == NULL) {
        CTR_FS_LOG("write_bin open fail '%s' -> '%s' size=%d errno=%s",
                   relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)", size, strerror(errno));
        free(fullPath);
        return false;
    }

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    CTR_FS_LOG("write_bin %s '%s' -> '%s' size=%d written=%lu",
               written == (size_t) size ? "ok" : "short",
               relativePath ? relativePath : "(null)", fullPath ? fullPath : "(null)",
               size, (unsigned long) written);
    free(fullPath);
    return written == (size_t) size;
}

static FileSystemVtable n3dsFileSystemVtable = {
    .resolvePath = n3dsResolvePath,
    .fileExists = n3dsFileExists,
    .readFileText = n3dsReadFileText,
    .writeFileText = n3dsWriteFileText,
    .deleteFile = n3dsDeleteFile,
    .readFileBinary = n3dsReadFileBinary,
    .writeFileBinary = n3dsWriteFileBinary,
};
static char *computeParentBasePath(const char *basePath) {
    if (!basePath || !*basePath) return NULL;
    size_t len = strlen(basePath);
    size_t end = len;
    while (end > 0 && basePath[end - 1] == '/') end--;
    if (end == 0) return NULL;
    size_t parentEnd = end;
    while (parentEnd > 0 && basePath[parentEnd - 1] != '/') parentEnd--;
    if (parentEnd == 0) return NULL;
    char *parent = safeMalloc(parentEnd + 1);
    memcpy(parent, basePath, parentEnd);
    parent[parentEnd] = '\0';
    normalizeCtrPath(parent);

    const char *deviceSep = strstr(parent, ":/");
    if (!deviceSep) {
        free(parent);
        return NULL;
    }
    const char *afterDevice = deviceSep + 2;
    while (*afterDevice == '/') afterDevice++;
    if (*afterDevice == '\0') {
        free(parent);
        return NULL;
    }
    return parent;
}

N3dsFileSystem *N3dsFileSystem_create(const char *dataWinPath) {
    N3dsFileSystem *fs = safeCalloc(1, sizeof(N3dsFileSystem));
    fs->base.vtable = &n3dsFileSystemVtable;

    char *normalizedDataWinPath = dataWinPath ? safeStrdup(dataWinPath) : safeStrdup("");
    normalizeCtrPath(normalizedDataWinPath);

    const char *lastSlash = strrchr(normalizedDataWinPath, '/');

    if (lastSlash != NULL) {
        size_t dirLen = (size_t) (lastSlash - normalizedDataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, normalizedDataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("sdmc:/");
    }
    normalizeCtrPath(fs->basePath);
    fs->fallbackBasePath = computeParentBasePath(fs->basePath);

    CTR_FS_LOG("create dataWin='%s' basePath='%s' fallbackBasePath='%s'",
               dataWinPath ? dataWinPath : "(null)",
               fs->basePath ? fs->basePath : "(null)",
               fs->fallbackBasePath ? fs->fallbackBasePath : "(none)");
    free(normalizedDataWinPath);
    return fs;
}

void N3dsFileSystem_destroy(N3dsFileSystem *fs) {
    if (fs == NULL) return;
    CTR_FS_LOG("destroy basePath='%s'", fs->basePath ? fs->basePath : "(null)");
    free(fs->basePath);
    free(fs->fallbackBasePath);
    free(fs);
}
