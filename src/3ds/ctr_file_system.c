#include "ctr_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ===[ Helpers ]===

// The caller must make sure to free the returned string!
static char* buildFullPath(CtrFileSystem* fs, const char* relativePath) {
    if (strstr(relativePath, fs->basePath) != nullptr) return safeStrdup(relativePath);
    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);
    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';
    return fullPath;
}

// ===[ Vtable Implementations ]===

static char* ctrResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((CtrFileSystem*) fs, relativePath);
}

static bool ctrFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((CtrFileSystem*) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    free(fullPath);
    return exists;
}

static char* ctrReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((CtrFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr) return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    return content;
}

static bool ctrWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = buildFullPath((CtrFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool ctrDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((CtrFileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

static bool ctrReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = buildFullPath((CtrFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    fclose(f);

    *outData = data;
    *outSize = (int32_t) bytesRead;
    return true;
}

static bool ctrWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    char* fullPath = buildFullPath((CtrFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr) return false;

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    return written == (size_t) size;
}

// ===[ Vtable ]===

static FileSystemVtable ctrFileSystemVtable = {
    .resolvePath = ctrResolvePath,
    .fileExists = ctrFileExists,
    .readFileText = ctrReadFileText,
    .writeFileText = ctrWriteFileText,
    .deleteFile = ctrDeleteFile,
    .readFileBinary = ctrReadFileBinary,
    .writeFileBinary = ctrWriteFileBinary,
};

// ===[ Lifecycle ]===

CtrFileSystem* CtrFileSystem_create(const char* dataWinPath) {
    CtrFileSystem* fs = safeCalloc(1, sizeof(CtrFileSystem));
    fs->base.vtable = &ctrFileSystemVtable;

    const char* lastSlash = strrchr(dataWinPath, '/');
    const char* lastBackslash = strrchr(dataWinPath, '\\');
    if (lastBackslash != nullptr && (lastSlash == nullptr || lastBackslash > lastSlash))
        lastSlash = lastBackslash;
    if (lastSlash != nullptr) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("./");
    }

    return fs;
}

void CtrFileSystem_destroy(CtrFileSystem* fs) {
    if (fs == nullptr) return;
    free(fs->basePath);
    free(fs);
}
