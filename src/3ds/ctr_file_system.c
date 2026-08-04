#include "ctr_file_system.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *normalizePath(const char *path) {
    if (path == NULL) return NULL;
    char *normalized = safeStrdup(path);
    for (size_t i = 0; normalized[i] != '\0'; i++) {
        if (normalized[i] == '\\') normalized[i] = '/';
    }
    return normalized;
}

static bool pathIsAbsolute(const char *path) {
    if (path == NULL || path[0] == '\0') return false;
    return path[0] == '/' || strstr(path, ":/") != NULL;
}

static bool pathExists(const char *fullPath) {
    struct stat st;
    return fullPath != NULL && stat(fullPath, &st) == 0;
}

static bool pathEqualsIgnoreCase(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *withTrailingSlash(const char *path) {
    if (path == NULL || path[0] == '\0') return safeStrdup("sdmc:/");
    size_t len = strlen(path);
    if (path[len - 1] == '/') return safeStrdup(path);
    char *out = safeMalloc(len + 2);
    memcpy(out, path, len);
    out[len] = '/';
    out[len + 1] = '\0';
    return out;
}

static char *dirnameWithSlash(const char *path) {
    const char *lastSlash = path ? strrchr(path, '/') : NULL;
    if (lastSlash == NULL) return safeStrdup("sdmc:/");
    size_t dirLen = (size_t)(lastSlash - path + 1);
    char *dir = safeMalloc(dirLen + 1);
    memcpy(dir, path, dirLen);
    dir[dirLen] = '\0';
    return dir;
}

static char *joinPath(const char *basePath, const char *normalizedPath) {
    if (normalizedPath == NULL) return NULL;
    if (pathIsAbsolute(normalizedPath)) return safeStrdup(normalizedPath);

    size_t baseLen = strlen(basePath);
    size_t relLen = strlen(normalizedPath);
    char *fullPath = safeMalloc(baseLen + relLen + 1);
    memcpy(fullPath, basePath, baseLen);
    memcpy(fullPath + baseLen, normalizedPath, relLen);
    fullPath[baseLen + relLen] = '\0';
    return fullPath;
}

static void ensureParentDir(const char *fullPath) {
    if (fullPath == NULL) return;

    char buf[1024];
    size_t len = strlen(fullPath);
    if (len >= sizeof(buf)) return;
    memcpy(buf, fullPath, len + 1);

    char *lastSlash = strrchr(buf, '/');
    if (lastSlash == NULL || lastSlash == buf) return;
    *lastSlash = '\0';

    size_t start = 1;
    char *scheme = strstr(buf, ":/");
    if (scheme != NULL) {
        start = (size_t)(scheme - buf) + 2;
    }

    for (size_t i = start; buf[i] != '\0'; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0777);
            buf[i] = '/';
        }
    }
    mkdir(buf, 0777);
}

static char *resolveForWrite(N3dsFileSystem *fs, const char *relativePath) {
    char *normalized = normalizePath(relativePath);
    if (normalized == NULL) return NULL;
    if (pathIsAbsolute(normalized)) return normalized;
    char *fullPath = joinPath(fs->savePath, normalized);
    free(normalized);
    return fullPath;
}

static bool stringStartsWith(const char *text, const char *prefix) {
    return text != NULL && prefix != NULL && strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool isDeltaruneSharedSaveFile(N3dsFileSystem *fs, const char *normalized) {
    if (!fs->isDeltarune || normalized == NULL) return false;
    if (strchr(normalized, '/') != NULL) return false;
    return strcmp(normalized, "dr.ini") == 0 ||
           strcmp(normalized, "true_config.ini") == 0 ||
           strcmp(normalized, "options.ini") == 0 ||
           stringStartsWith(normalized, "filech") ||
           stringStartsWith(normalized, "keyconfig_") ||
           stringStartsWith(normalized, "config_");
}

static char *resolveForRead(N3dsFileSystem *fs, const char *relativePath) {
    char *normalized = normalizePath(relativePath);
    if (normalized == NULL) return NULL;
    if (pathIsAbsolute(normalized)) return normalized;

    bool sharedSave = isDeltaruneSharedSaveFile(fs, normalized);

    if (sharedSave) {
        char *saveFull = joinPath(fs->savePath, normalized);
        if (pathExists(saveFull)) {
            free(normalized);
            return saveFull;
        }
        free(saveFull);
    }

    if (!pathEqualsIgnoreCase(fs->bundlePath, fs->savePath)) {
        char *bundleFull = joinPath(fs->bundlePath, normalized);
        if (pathExists(bundleFull)) {
            free(normalized);
            return bundleFull;
        }
        free(bundleFull);
    }

    if (!sharedSave) {
        char *saveFull = joinPath(fs->savePath, normalized);
        if (pathExists(saveFull)) {
            free(normalized);
            return saveFull;
        }
        free(saveFull);
    }

    for (int i = 0; i < fs->legacyPathCount; i++) {
        char *legacyFull = joinPath(fs->legacyPaths[i], normalized);
        if (pathExists(legacyFull)) {
            free(normalized);
            return legacyFull;
        }
        free(legacyFull);
    }

    char *fallback = joinPath(fs->savePath, normalized);
    free(normalized);
    return fallback;
}

static char *n3dsResolvePath(FileSystem *fs, const char *relativePath) {
    N3dsFileSystem *nfs = (N3dsFileSystem *)fs;
    char *normalized = normalizePath(relativePath);
    if (normalized == NULL) return NULL;
    if (pathIsAbsolute(normalized)) return normalized;
    char *fullPath = joinPath(nfs->bundlePath, normalized);
    free(normalized);
    return fullPath;
}

static bool n3dsFileExists(FileSystem *fs, const char *relativePath) {
    char *fullPath = resolveForRead((N3dsFileSystem *)fs, relativePath);
    bool exists = pathExists(fullPath);
    free(fullPath);
    return exists;
}

static char *readFileTextAtPath(const char *fullPath) {
    FILE *f = fopen(fullPath, "rb");
    if (f == NULL) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char *content = safeMalloc((size_t)size + 1);
    size_t bytesRead = fread(content, 1, (size_t)size, f);
    content[bytesRead] = '\0';
    fclose(f);
    return content;
}

static char *n3dsReadFileText(FileSystem *fs, const char *relativePath) {
    char *fullPath = resolveForRead((N3dsFileSystem *)fs, relativePath);
    char *content = fullPath ? readFileTextAtPath(fullPath) : NULL;
    free(fullPath);
    return content;
}

static bool writeFileTextAtPath(const char *fullPath, const char *contents) {
    ensureParentDir(fullPath);
    FILE *f = fopen(fullPath, "wb");
    if (f == NULL) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool n3dsWriteFileText(FileSystem *fs, const char *relativePath, const char *contents) {
    char *fullPath = resolveForWrite((N3dsFileSystem *)fs, relativePath);
    bool ok = fullPath != NULL && writeFileTextAtPath(fullPath, contents);
    free(fullPath);
    return ok;
}

static bool n3dsDeleteFile(FileSystem *fs, const char *relativePath) {
    char *fullPath = resolveForWrite((N3dsFileSystem *)fs, relativePath);
    int result = fullPath ? remove(fullPath) : -1;
    free(fullPath);
    return result == 0;
}

static bool readFileBinaryAtPath(const char *fullPath, uint8_t **outData, int32_t *outSize) {
    FILE *f = fopen(fullPath, "rb");
    if (f == NULL) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return false;
    }

    uint8_t *data = safeMalloc((size_t)(size > 0 ? size : 1));
    size_t bytesRead = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (bytesRead != (size_t)size) {
        free(data);
        return false;
    }

    *outData = data;
    *outSize = (int32_t)size;
    return true;
}

static bool n3dsReadFileBinary(FileSystem *fs, const char *relativePath, uint8_t **outData, int32_t *outSize) {
    *outData = NULL;
    *outSize = 0;

    char *fullPath = resolveForRead((N3dsFileSystem *)fs, relativePath);
    bool ok = fullPath != NULL && readFileBinaryAtPath(fullPath, outData, outSize);
    free(fullPath);
    return ok;
}

static bool writeFileBinaryAtPath(const char *fullPath, const uint8_t *data, int32_t size) {
    ensureParentDir(fullPath);
    FILE *f = fopen(fullPath, "wb");
    if (f == NULL) return false;

    size_t written = fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return written == (size_t)size;
}

static bool n3dsWriteFileBinary(FileSystem *fs, const char *relativePath, const uint8_t *data, int32_t size) {
    char *fullPath = resolveForWrite((N3dsFileSystem *)fs, relativePath);
    bool ok = fullPath != NULL && writeFileBinaryAtPath(fullPath, data, size);
    free(fullPath);
    return ok;
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

static char *lowerCopy(const char *text) {
    char *out = safeStrdup(text ? text : "");
    for (size_t i = 0; out[i] != '\0'; i++) {
        out[i] = (char)tolower((unsigned char)out[i]);
    }
    return out;
}

static char *deltaruneRootFromBundle(const char *bundlePath) {
    char *lower = lowerCopy(bundlePath);
    char *match = strstr(lower, "/deltarune/");
    size_t rootLen = 0;
    if (match != NULL) {
        rootLen = (size_t)(match - lower) + strlen("/deltarune/");
    } else if (strncmp(lower, "deltarune/", 10) == 0) {
        rootLen = strlen("deltarune/");
    }
    if (match == NULL) {
        if (rootLen == 0) {
            free(lower);
            return NULL;
        }
    }

    char *root = safeMalloc(rootLen + 1);
    memcpy(root, bundlePath, rootLen);
    root[rootLen] = '\0';
    free(lower);
    return root;
}

static void addLegacyPath(N3dsFileSystem *fs, const char *path) {
    if (fs->legacyPathCount >= (int)(sizeof(fs->legacyPaths) / sizeof(fs->legacyPaths[0]))) return;
    char *legacy = withTrailingSlash(path);
    if (pathEqualsIgnoreCase(legacy, fs->savePath) ||
        pathEqualsIgnoreCase(legacy, fs->bundlePath)) {
        free(legacy);
        return;
    }
    for (int i = 0; i < fs->legacyPathCount; i++) {
        if (pathEqualsIgnoreCase(legacy, fs->legacyPaths[i])) {
            free(legacy);
            return;
        }
    }
    fs->legacyPaths[fs->legacyPathCount++] = legacy;
}

static void appendText(char **text, size_t *len, const char *data, size_t dataLen) {
    *text = safeRealloc(*text, *len + dataLen + 1);
    memcpy(*text + *len, data, dataLen);
    *len += dataLen;
    (*text)[*len] = '\0';
}

static bool lineStartsIniSection(const char *lineStart, const char *lineEnd,
                                 const char **nameStart, size_t *nameLen) {
    while (lineStart < lineEnd && (*lineStart == ' ' || *lineStart == '\t')) lineStart++;
    if (lineStart >= lineEnd || *lineStart != '[') return false;
    const char *name = lineStart + 1;
    const char *close = name;
    while (close < lineEnd && *close != ']') close++;
    if (close >= lineEnd || close == name) return false;
    *nameStart = name;
    *nameLen = (size_t)(close - name);
    return true;
}

static bool textHasIniSection(const char *text, const char *sectionName, size_t sectionNameLen) {
    const char *cursor = text ? text : "";
    while (*cursor != '\0') {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        while (*lineEnd != '\0' && *lineEnd != '\n') lineEnd++;

        const char *name = NULL;
        size_t nameLen = 0;
        if (lineStartsIniSection(lineStart, lineEnd, &name, &nameLen) &&
            nameLen == sectionNameLen && memcmp(name, sectionName, nameLen) == 0) {
            return true;
        }

        cursor = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;
    }
    return false;
}

static void mergeIniFileSections(const char *dstPath, const char *srcPath) {
    char *src = readFileTextAtPath(srcPath);
    if (src == NULL) return;

    char *dst = readFileTextAtPath(dstPath);
    if (dst == NULL) dst = safeStrdup("");

    size_t dstLen = strlen(dst);
    bool changed = false;
    const char *cursor = src;

    while (*cursor != '\0') {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        while (*lineEnd != '\0' && *lineEnd != '\n') lineEnd++;
        const char *afterLine = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;

        const char *name = NULL;
        size_t nameLen = 0;
        if (!lineStartsIniSection(lineStart, lineEnd, &name, &nameLen)) {
            cursor = afterLine;
            continue;
        }

        const char *sectionStart = lineStart;
        const char *sectionEnd = afterLine;
        while (*sectionEnd != '\0') {
            const char *nextLineStart = sectionEnd;
            const char *nextLineEnd = sectionEnd;
            while (*nextLineEnd != '\0' && *nextLineEnd != '\n') nextLineEnd++;
            const char *nextName = NULL;
            size_t nextNameLen = 0;
            if (lineStartsIniSection(nextLineStart, nextLineEnd, &nextName, &nextNameLen)) {
                break;
            }
            sectionEnd = (*nextLineEnd == '\n') ? nextLineEnd + 1 : nextLineEnd;
        }

        if (!textHasIniSection(dst, name, nameLen)) {
            if (dstLen > 0 && dst[dstLen - 1] != '\n') {
                appendText(&dst, &dstLen, "\n", 1);
            }
            appendText(&dst, &dstLen, sectionStart, (size_t)(sectionEnd - sectionStart));
            if (dstLen > 0 && dst[dstLen - 1] != '\n') {
                appendText(&dst, &dstLen, "\n", 1);
            }
            changed = true;
        }

        cursor = sectionEnd;
    }

    if (changed) writeFileTextAtPath(dstPath, dst);
    free(dst);
    free(src);
}

static void copyFileIfMissing(const char *dstPath, const char *srcPath) {
    if (pathExists(dstPath) || !pathExists(srcPath)) return;

    uint8_t *data = NULL;
    int32_t size = 0;
    if (!readFileBinaryAtPath(srcPath, &data, &size)) return;
    writeFileBinaryAtPath(dstPath, data, size);
    free(data);
}

static void migrateDeltaruneSaves(N3dsFileSystem *fs) {
    if (!fs->isDeltarune) return;

    char dst[512];
    char src[512];

    for (int i = 0; i < fs->legacyPathCount; i++) {
        snprintf(dst, sizeof(dst), "%sdr.ini", fs->savePath);
        snprintf(src, sizeof(src), "%sdr.ini", fs->legacyPaths[i]);
        mergeIniFileSections(dst, src);

        snprintf(dst, sizeof(dst), "%strue_config.ini", fs->savePath);
        snprintf(src, sizeof(src), "%strue_config.ini", fs->legacyPaths[i]);
        copyFileIfMissing(dst, src);

        snprintf(dst, sizeof(dst), "%soptions.ini", fs->savePath);
        snprintf(src, sizeof(src), "%soptions.ini", fs->legacyPaths[i]);
        copyFileIfMissing(dst, src);

        for (int chapter = 1; chapter <= 4; chapter++) {
            for (int slot = 0; slot <= 9; slot++) {
                snprintf(dst, sizeof(dst), "%sfilech%d_%d", fs->savePath, chapter, slot);
                snprintf(src, sizeof(src), "%sfilech%d_%d", fs->legacyPaths[i], chapter, slot);
                copyFileIfMissing(dst, src);
            }
        }

        for (int slot = 0; slot <= 9; slot++) {
            snprintf(dst, sizeof(dst), "%skeyconfig_%d.ini", fs->savePath, slot);
            snprintf(src, sizeof(src), "%skeyconfig_%d.ini", fs->legacyPaths[i], slot);
            copyFileIfMissing(dst, src);

            snprintf(dst, sizeof(dst), "%sconfig_%d.ini", fs->savePath, slot);
            snprintf(src, sizeof(src), "%sconfig_%d.ini", fs->legacyPaths[i], slot);
            copyFileIfMissing(dst, src);
        }
    }
}

N3dsFileSystem *N3dsFileSystem_create(const char *dataWinPath) {
    N3dsFileSystem *fs = safeCalloc(1, sizeof(N3dsFileSystem));
    fs->base.vtable = &n3dsFileSystemVtable;

    char *bundle = dirnameWithSlash(dataWinPath);
    fs->bundlePath = withTrailingSlash(bundle);
    fs->savePath = safeStrdup(fs->bundlePath);
    free(bundle);

    char *deltaruneRoot = deltaruneRootFromBundle(fs->bundlePath);
    if (deltaruneRoot != NULL) {
        free(fs->savePath);
        fs->savePath = withTrailingSlash(deltaruneRoot);
        fs->isDeltarune = true;

        const char *chapterDirs[] = {
            "chapter1_windows/",
            "chapter2_windows/",
            "chapter3_windows/",
            "chapter4_windows/",
        };
        for (size_t i = 0; i < sizeof(chapterDirs) / sizeof(chapterDirs[0]); i++) {
            size_t rootLen = strlen(fs->savePath);
            size_t dirLen = strlen(chapterDirs[i]);
            char *legacy = safeMalloc(rootLen + dirLen + 1);
            memcpy(legacy, fs->savePath, rootLen);
            memcpy(legacy + rootLen, chapterDirs[i], dirLen);
            legacy[rootLen + dirLen] = '\0';
            addLegacyPath(fs, legacy);
            free(legacy);
        }
        free(deltaruneRoot);
        migrateDeltaruneSaves(fs);
    }

    return fs;
}

void N3dsFileSystem_destroy(N3dsFileSystem *fs) {
    if (fs == NULL) return;
    free(fs->bundlePath);
    free(fs->savePath);
    for (int i = 0; i < fs->legacyPathCount; i++) {
        free(fs->legacyPaths[i]);
    }
    free(fs);
}
