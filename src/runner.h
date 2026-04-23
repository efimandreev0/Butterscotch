#pragma once

#include "common.h"
#include "audio_system.h"
#include "collision.h"
#include "data_win.h"
#include "file_system.h"
#include "instance.h"
#include "renderer.h"
#include "runner_keyboard.h"
#include "vm.h"

// ===[ Event Type Constants ]===
#define EVENT_CREATE     0
#define EVENT_DESTROY    1
#define EVENT_ALARM      2
#define EVENT_STEP       3
#define EVENT_COLLISION  4
#define EVENT_KEYBOARD   5
#define EVENT_OTHER      7
#define EVENT_DRAW       8
#define EVENT_KEYPRESS   9
#define EVENT_KEYRELEASE 10

// ===[ Step Sub-event Constants ]===
#define STEP_NORMAL 0
#define STEP_BEGIN  1
#define STEP_END    2

// ===[ Draw Sub-event Constants ]===
#define DRAW_NORMAL    0
#define DRAW_GUI       64
#define DRAW_BEGIN     72
#define DRAW_END       73
#define DRAW_GUI_BEGIN 74
#define DRAW_GUI_END   75
#define DRAW_PRE       76
#define DRAW_POST      77

// ===[ Other Sub-event Constants ]===
#define OTHER_OUTSIDE_ROOM  0
#define OTHER_GAME_START    2
#define OTHER_ROOM_START    4
#define OTHER_ROOM_END      5
#define OTHER_ANIMATION_END 7
#define OTHER_END_OF_PATH   8
#define OTHER_USER0         10

typedef struct {
    bool visible;
    bool foreground;
    int32_t backgroundIndex;  // BGND resource index (mutable at runtime)
    float x, y;               // float for sub-pixel scrolling accumulation
    bool tileX, tileY;
    float speedX, speedY;
    bool stretch;
    float alpha;
} RuntimeBackground;

typedef struct {
    bool visible;
    float offsetX;
    float offsetY;
} TileLayerState;

// stb_ds hashmap entry: depth -> tile layer state
typedef struct {
    int32_t key;
    TileLayerState value;
} TileLayerMapEntry;

// Saved state for persistent rooms. When leaving a persistent room, instance state
// and visual properties are saved here. When returning, they are restored instead
// of re-creating from the room definition.
typedef struct {
    bool initialized;
    Instance** instances; // stb_ds array of saved Instance*
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;
    bool drawBackgroundColor;
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
} SavedRoomState;

typedef struct Runner {
    DataWin* dataWin;
    VMContext* vmContext;
    Renderer* renderer;
    FileSystem* fileSystem;
    AudioSystem* audioSystem;
    Room* currentRoom;
    int32_t currentRoomIndex;
    int32_t currentRoomOrderPosition;
    Instance** instances; // stb_ds array of Instance*
    int32_t pendingRoom;  // -1 = none
    bool gameStartFired;
    int frameCount;
    uint32_t nextInstanceId;
    RunnerKeyboardState* keyboard;
    RuntimeBackground backgrounds[8];
    uint32_t backgroundColor;      // runtime-mutable (BGR format)
    bool drawBackgroundColor;
    bool shouldExit;
    bool debugMode;
    TileLayerMapEntry* tileLayerMap; // stb_ds hashmap: depth -> tile layer state
    SavedRoomState* savedRoomStates; // array of size dataWin->room.count, for persistent room support
    float viewAngles[8]; // runtime-only view_angle per view (not stored in data.win)
    int32_t viewCurrent; // index of the view currently being drawn (for view_current)
    struct { char* key; int value; }* disabledObjects; // stb_ds string hashmap, nullptr = no filtering
    struct { int key; Instance* value; }* instancesToId;
    bool isGMS2;

    // ===[ Per-frame bbox cache — computed once, used by collision builtins + dispatchCollisionEvents ]===
    InstanceBBox* bboxCache;     // array parallel to instances[], recomputed each frame
    int32_t bboxCacheCount;      // current size (matches arrlen(instances))
    int32_t bboxCacheFrame;      // frameCount when last computed (-1 = invalid)

    // VM opcode trace (for PSP crash diagnostics — only when VM_PSP_TRACE defined).
    // traceEnabled gates the actual logging — toggle via SELECT button on PSP.
    // tracePath is kept so we can close+reopen the file for each opcode write:
    // PSP libc only syncs on fclose, so we have to close/reopen to survive crashes.
    FILE* traceFile;
    char* tracePath;
    bool traceEnabled;

    // ===[ Call profiler — enabled by --profile-calls <file> ]===
    FILE* profileFile;                              // output file (NULL = disabled)
    struct { char* key; int value; }* profileCalls; // stb_ds: codeName -> call count per frame
    struct { char* key; int value; }* profileTimes; // stb_ds: codeName -> total microseconds per frame

    // Per-frame step phase timings (us), reset after each profile flush
    uint32_t phaseUsAnim, phaseUsBeginStep, phaseUsKeyboard, phaseUsAlarms;
    uint32_t phaseUsNormalStep, phaseUsMotion, phaseUsCollision, phaseUsEndStep;
    uint32_t phaseUsOutsideRoom;

    // Per-frame draw phase timings (us)
    uint32_t drawPhaseSortList, drawPhaseBuildList, drawPhaseNormal, drawPhaseGUI;
    // Sub-breakdown of drawPhaseNormal: localize where the Nrm ms go.
    uint32_t drawNrmBuildDrawables; // building drawables[] (instance + tile loops)
    uint32_t drawNrmQsort;          // sorting drawables by depth
    uint32_t drawNrmTiles;          // calls to Renderer_drawTile (sum over drawable loop)
    uint32_t drawNrmInstVM;         // DRAW_INSTANCE via VM_executeCode
    uint32_t drawNrmInstNative;     // DRAW_INSTANCE via native func
    uint32_t drawNrmInstDrawSelf;   // DRAW_INSTANCE fallthrough to Renderer_drawSelf (no Draw_0)
    uint32_t drawNrmCountTiles, drawNrmCountInstVM, drawNrmCountInstNative, drawNrmCountInstDrawSelf;
    // Top-3 slowest Draw_0 scripts this frame (via VM). Name pointer into DataWin code entries.
    const char* topVmDrawName[3];
    uint32_t    topVmDrawTimeUs[3];
    int32_t     topVmDrawCalls[3];

    // Per-frame Step-phase (non-Draw) script accumulator + top-3.
    // stepCache is filled during Runner_step() across every VM/native event
    // dispatch EXCEPT EVENT_DRAW; extracted into topVmStep* at end of step.
    // Covers: Step_Begin/Normal/End, Keyboard, Alarm, Collision, Other_*,
    // Animation_End, End_Of_Path, Outside_Room — everything under S timer.
    struct RunnerStepCacheEntry { int32_t codeId; uint32_t timeUs; int32_t calls; } stepCache[256];
    int32_t stepCacheCount;
    const char* topVmStepName[3];
    uint32_t    topVmStepTimeUs[3];
    int32_t     topVmStepCalls[3];

    // ===[ Instance-by-object cache ]===
    // Lazily rebuilt when arrlen(instances) != cachedInstCount.
    // Allows executeEventForAll to iterate only objectIndexes that exist in the
    // room, rather than scanning every instance. For 252-instance rooms with
    // ~20 unique object types, cuts dispatch overhead by ~10×.
    Instance*** instancesByObjInclParent; // dense array: for each objectIndex, list of instances whose type is oi OR has oi as ancestor
    Instance*** instancesByObjDirect;     // dense array: direct-match only (no parent chain)
    int32_t* activeOIsList;               // stb_ds array of objectIndexes that ever had an instance (never shrinks; empty buckets harmless)
    uint8_t* oiInListBitmap;              // bitmap flag so we only add to activeOIsList once per oi
    int32_t instancesByObjMax;    // allocated size (== dataWin->objt.count at init)
    int32_t cachedInstCount;      // arrlen(instances) at last rebuild (-1 = force rebuild)
} Runner;

const char* Runner_getEventName(int32_t eventType, int32_t eventSubtype);
Runner* Runner_create(DataWin* dataWin, VMContext* vm, FileSystem* fileSystem);
void Runner_initFirstRoom(Runner* runner);
void Runner_step(Runner* runner);
void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype);
void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype);
void Runner_draw(Runner* runner);
void Runner_drawBackgrounds(Runner* runner, bool foreground);
void Runner_scrollBackgrounds(Runner* runner);
Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex);
void Runner_destroyInstance(Runner* runner, Instance* inst);
void Runner_cleanupDestroyedInstances(Runner* runner);

// Per-frame bbox cache: computed once, used by collision builtins + dispatchCollisionEvents
void Runner_ensureBBoxCache(Runner* runner);
static inline InstanceBBox Runner_getCachedBBox(Runner* runner, int32_t instanceIdx) {
    Runner_ensureBBoxCache(runner);
    if (instanceIdx >= 0 && instanceIdx < runner->bboxCacheCount)
        return runner->bboxCache[instanceIdx];
    return (InstanceBBox){0, 0, 0, 0, false};
}
void Runner_dumpState(Runner* runner);
char* Runner_dumpStateJson(Runner* runner);
void Runner_free(Runner* runner);
