#include "data_win.h"
#include "ctr_texture_cache.h"

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

static void cache_progress(uint32_t pageIndex, uint32_t pageCount, const char *path, void *user) {
    (void)path;
    (void)user;
    if (pageCount == 0) return;
    uint32_t pct = (pageIndex * 100u) / pageCount;
    fprintf(stderr, "\r[%s] cache: %3u%% (%lu/%lu)",
            g_currentLabel,
            pct,
            (unsigned long)pageIndex,
            (unsigned long)pageCount);
    if (pageIndex >= pageCount) fputc('\n', stderr);
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
    bool ok = CtrTextureCache_indexIsCurrentPath(atlasPath);
    DataWin_free(dw);

    if (!ok) {
        fprintf(stderr, "[%s] cache build failed or produced stale atlas: %s\n",
                label, atlasPath);
        return 1;
    }

    fprintf(stderr, "[%s] ready: %s\n", label, atlasPath);
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
            "Usage: %s <data.win|game-dir> [cache-dir]\n"
            "\n"
            "Modes:\n"
            "  %s undertale/data.win              process a single data.win, cache -> <dir>/cache\n"
            "  %s undertale/data.win out/cache    process a single data.win, cache -> out/cache\n"
            "  %s deltarune                       walk recursively, cache every data.win into its own sibling cache/\n"
            "  %s deltarune deltarune/cache       legacy: only deltarune/data.win, cache -> deltarune/cache\n",
            argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 2;
    }

    const char *target = argv[1];
    const char *cacheOverride = (argc == 3) ? argv[2] : NULL;

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
