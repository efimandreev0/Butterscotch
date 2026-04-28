#include "ctr_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char* buildFullPath(N3dsFileSystem* fs, const char* relativePath) {
    if(strncmp(relativePath, fs->basePath, strlen(fs->basePath)) == 0) {
        return safeStrdup(relativePath);
    }

    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);

    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';

    return fullPath;
}

static char* n3dsResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((N3dsFileSystem*) fs, relativePath);
}

static bool n3dsFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((N3dsFileSystem*) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    free(fullPath);
    return exists;
}

static char* n3dsReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((N3dsFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);

    if (f == NULL) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);

    return content;
}

static bool n3dsWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = buildFullPath((N3dsFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);

    if (f == NULL) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);

    return written == len;
}

static bool n3dsDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((N3dsFileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

static FileSystemVtable n3dsFileSystemVtable = {
    .resolvePath = n3dsResolvePath,
    .fileExists = n3dsFileExists,
    .readFileText = n3dsReadFileText,
    .writeFileText = n3dsWriteFileText,
    .deleteFile = n3dsDeleteFile,
};

N3dsFileSystem* N3dsFileSystem_create(const char* dataWinPath) {
    N3dsFileSystem* fs = safeCalloc(1, sizeof(N3dsFileSystem));
    fs->base.vtable = &n3dsFileSystemVtable;

    const char* lastSlash = strrchr(dataWinPath, '/');

    if (lastSlash != NULL) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("sdmc:/");
    }

    return fs;
}

void N3dsFileSystem_destroy(N3dsFileSystem* fs) {
    if (fs == NULL) return;
    free(fs->basePath);
    free(fs);
}