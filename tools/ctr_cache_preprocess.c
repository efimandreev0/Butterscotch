// Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "data_win.h"
#include "ctr_texture_cache.h"
#include "ctr_audio_preprocess.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0777)
#endif

char g_current_cache_dir[256];

static const char *g_currentLabel = "?";

static bool path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int ascii_tolower_local(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

static bool ascii_iends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    if (tl > sl) return false;
    s += sl - tl;
    while (*s && *suffix) {
        if (ascii_tolower_local((unsigned char)*s) !=
            ascii_tolower_local((unsigned char)*suffix)) return false;
        s++;
        suffix++;
    }
    return true;
}

static void dirname_of(const char *path, char *out, size_t outSize) {
    if (!path || !out || outSize == 0) return;
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    size_t len = slash ? (size_t)(slash - path) : 0;
    if (len == 0) {
        snprintf(out, outSize, ".");
        return;
    }
    if (len >= outSize) len = outSize - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static const char *basename_of(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    return slash ? slash + 1 : path;
}

static void join_path(char *out, size_t outSize, const char *a, const char *b) {
    if (!out || outSize == 0) return;
    size_t len = a ? strlen(a) : 0;
    const char *sep = (len > 0 && a[len - 1] != '/' && a[len - 1] != '\\') ? "/" : "";
    snprintf(out, outSize, "%s%s%s", a ? a : "", sep, b ? b : "");
}

static bool ensure_dir(const char *path) {
    if (MKDIR(path) == 0) return true;
    return errno == EEXIST;
}

static bool is_external_image_file(const char *name) {
    return ascii_iends_with(name, ".png") ||
           ascii_iends_with(name, ".qoi") ||
           ascii_iends_with(name, ".jpg") ||
           ascii_iends_with(name, ".jpeg");
}

static void write_external_image_index_recursive(FILE *out, const char *root,
                                                 const char *relRoot,
                                                 int *count) {
    DIR *d = opendir(root);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, "cache") == 0) continue;

        char child[640];
        join_path(child, sizeof(child), root, name);
        char relChild[640];
        if (relRoot && *relRoot) join_path(relChild, sizeof(relChild), relRoot, name);
        else snprintf(relChild, sizeof(relChild), "%s", name);

        if (path_is_dir(child)) {
            write_external_image_index_recursive(out, child, relChild, count);
        } else if (file_exists(child) && is_external_image_file(name)) {
            fprintf(out, "%s\t%s\n", name, relChild);
            if (count) (*count)++;
        }
    }

    closedir(d);
}

static void preprocess_external_image_index(const char *gameDir, const char *cacheDir, const char *label) {
    char indexTmp[640];
    char indexPath[640];
    join_path(indexPath, sizeof(indexPath), cacheDir, "external_images.txt");
    snprintf(indexTmp, sizeof(indexTmp), "%s.tmp", indexPath);

    FILE *out = fopen(indexTmp, "wb");
    if (!out) {
        fprintf(stderr, "[%s] external images: cannot write %s (%s)\n",
                label, indexTmp, strerror(errno));
        return;
    }

    int count = 0;
    write_external_image_index_recursive(out, gameDir, "", &count);
    fclose(out);

    if (rename(indexTmp, indexPath) != 0) {
        remove(indexPath);
        if (rename(indexTmp, indexPath) != 0) {
            fprintf(stderr, "[%s] external images: cannot install %s (%s)\n",
                    label, indexPath, strerror(errno));
            remove(indexTmp);
            return;
        }
    }

    fprintf(stderr, "[%s] external images indexed: %d -> %s\n", label, count, indexPath);
}

static void cache_progress(uint32_t pageIndex, uint32_t pageCount, const char *path, void *user) {
    (void)path;
    (void)user;
    if (pageCount == 0) return;
    uint32_t pct = (pageIndex * 100u) / pageCount;
    static const char *lastLabel = NULL;
    static uint32_t lastPct = 101u;
    static uint32_t lastPage = 0;
    bool done = pageIndex >= pageCount;
    if (g_currentLabel != lastLabel) {
        lastLabel = g_currentLabel;
        lastPct = 101u;
        lastPage = 0;
    }
    if (!done && pct == lastPct && pageIndex < lastPage + 32u) return;
    lastPct = pct;
    lastPage = pageIndex;
    fprintf(stderr, "\r[%s] cache: %3u%% (%lu/%lu)",
            g_currentLabel,
            pct,
            (unsigned long)pageIndex,
            (unsigned long)pageCount);
    if (done) fputc('\n', stderr);
    fflush(stderr);
}

static int process_one(const char *dataWinPath, const char *cacheDir, const char *label) {
    g_currentLabel = label;

    if (!ensure_dir(cacheDir)) {
        fprintf(stderr, "[%s] failed to create cache dir: %s (%s)\n",
                label, cacheDir, strerror(errno));
        return 1;
    }
    snprintf(g_current_cache_dir, sizeof(g_current_cache_dir), "%s", cacheDir);

    DataWinParserOptions opt = {
        .parseGen8 = true,
        .parseSprt = true,
        .parseBgnd = true,
        .parseFont = true,
        .parseTpag = true,
        .parseTxtr = true,
        .parseStrg = true,
        .skipLoadingPreciseMasksForNonPreciseSprites = true,
        .skipTextureBlobData = false,
        .skipAudioBlobData = true,
    };

    fprintf(stderr, "[%s] reading %s\n", label, dataWinPath);
    DataWin *dw = DataWin_parse(dataWinPath, opt);
    if (!dw) {
        fprintf(stderr, "[%s] failed to parse data.win\n", label);
        return 1;
    }

    CtrTextureCache_setProgressCallback(cache_progress, NULL);
    CtrTextureCache_prepare(dw);
    CtrTextureCache_setProgressCallback(NULL, NULL);

    char atlasPath[512];
    CtrTextureCache_indexPath(atlasPath, sizeof(atlasPath));
    bool textureOk = CtrTextureCache_indexIsCurrentPath(atlasPath);
    DataWin_free(dw);

    if (!textureOk) {
        fprintf(stderr, "[%s] cache build failed or produced stale atlas: %s\n",
                label, atlasPath);
        // Audio is independent: bail before the audio pass on a texture failure
        // to keep error output focused.
        return 1;
    }
    fprintf(stderr, "[%s] textures ready: %s\n", label, atlasPath);

    char gameDir[512];
    dirname_of(dataWinPath, gameDir, sizeof(gameDir));
    preprocess_external_image_index(gameDir, cacheDir, label);

    // Audio side. Re-parses data.win because its parser options are different
    // (we want SOND/AGRP/AUDO and don't need the texture metadata). Failures
    // here are non-fatal — the runtime falls back to "no audio" silently if
    // audio.bin is missing or unreadable.
    int audioRc = CtrAudioPreprocess_run(dataWinPath, cacheDir, label);
    if (audioRc != 0) {
        fprintf(stderr, "[%s] audio preprocess failed (rc=%d) — runtime will boot without audio\n",
                label, audioRc);
    }

    return 0;
}

// Recursively walks `root`, processing every data.win found. For each game directory
// (the one containing a data.win) writes its cache into a sibling `cache/` subdir.
// Returns number of failures.
static int walk_and_process(const char *root, int *processed) {
    int failures = 0;

    char dataWinPath[640];
    join_path(dataWinPath, sizeof(dataWinPath), root, "data.win");
    if (file_exists(dataWinPath)) {
        char cacheDir[640];
        join_path(cacheDir, sizeof(cacheDir), root, "cache");
        const char *label = basename_of(root);
        if (!*label) label = root;
        failures += process_one(dataWinPath, cacheDir, label);
        if (processed) (*processed)++;
    }

    DIR *d = opendir(root);
    if (!d) {
        fprintf(stderr, "cannot open dir: %s (%s)\n", root, strerror(errno));
        return failures + 1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, "cache") == 0) continue;

        char child[640];
        join_path(child, sizeof(child), root, name);
        if (path_is_dir(child)) {
            failures += walk_and_process(child, processed);
        }
    }
    closedir(d);
    return failures;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [--etc1|--no-etc1] <data.win|game-dir> [cache-dir]\n"
            "\n"
            "Texture quality:\n"
            "  --no-etc1   write color atlases as RGBA4 (bigger, much cleaner; default)\n"
            "  --etc1      allow ETC1/ETC1A4 compression (smaller, uglier on pixel art)\n"
            "\n"
            "Modes:\n"
            "  %s undertale/data.win              process a single data.win, cache -> <dir>/cache\n"
            "  %s undertale/data.win out/cache    process a single data.win, cache -> out/cache\n"
            "  %s deltarune                       walk recursively, cache every data.win into its own sibling cache/\n"
            "  %s deltarune deltarune/cache       legacy: only deltarune/data.win, cache -> deltarune/cache\n",
            argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    int argi = 1;
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--etc1") == 0) {
            CtrTextureCache_setUseEtc1(true);
        } else if (strcmp(argv[argi], "--no-etc1") == 0) {
            CtrTextureCache_setUseEtc1(false);
        } else {
            usage(argv[0]);
            return 2;
        }
        argi++;
    }

    int remaining = argc - argi;
    if (remaining < 1 || remaining > 2) {
        usage(argv[0]);
        return 2;
    }

    fprintf(stderr, "Texture cache ETC1: %s\n",
            CtrTextureCache_getUseEtc1() ? "enabled" : "disabled (RGBA4)");

    const char *target = argv[argi];
    const char *cacheOverride = (remaining == 2) ? argv[argi + 1] : NULL;

    if (file_exists(target)) {
        // Single data.win path
        char gameDir[512];
        char cacheDir[512];
        dirname_of(target, gameDir, sizeof(gameDir));
        if (cacheOverride) {
            snprintf(cacheDir, sizeof(cacheDir), "%s", cacheOverride);
        } else {
            join_path(cacheDir, sizeof(cacheDir), gameDir, "cache");
        }
        const char *label = basename_of(gameDir);
        if (!*label) label = "game";
        return process_one(target, cacheDir, label);
    }

    if (!path_is_dir(target)) {
        fprintf(stderr, "not a file or directory: %s\n", target);
        return 1;
    }

    if (cacheOverride) {
        // Legacy 2-arg dir form: <game-dir> <cache-dir>, no recursion.
        char dataWinPath[512];
        join_path(dataWinPath, sizeof(dataWinPath), target, "data.win");
        if (!file_exists(dataWinPath)) {
            fprintf(stderr, "data.win not found: %s\n", dataWinPath);
            return 1;
        }
        const char *label = basename_of(target);
        if (!*label) label = "game";
        return process_one(dataWinPath, cacheOverride, label);
    }

    // Recursive mode: walk the tree and process every data.win we find.
    int processed = 0;
    int failures = walk_and_process(target, &processed);

    if (processed == 0) {
        fprintf(stderr, "no data.win files found under: %s\n", target);
        return 1;
    }

    fprintf(stderr, "Done: %d game(s) processed, %d failure(s)\n", processed, failures);
    return failures > 0 ? 1 : 0;
}
