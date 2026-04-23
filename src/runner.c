#include "runner.h"
#include "data_win.h"
#include "instance.h"
#include "renderer.h"
#include "native_scripts.h"
#include "vm.h"
#include "utils.h"
#include "json_writer.h"
#include "collision.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <3ds/svc.h>
static inline uint64_t _profTickUs(void) {
    const uint64_t TICKS_PER_SEC = 268123480ULL;
    uint64_t ticks = svcGetSystemTick();
    return (ticks * 1000000ULL) / TICKS_PER_SEC;
}
#include <sys/time.h>

#include "stb_ds.h"

// ===[ Runtime Layer Teardown Helpers ]===
void Runner_freeRuntimeLayer(RuntimeLayer* runtimeLayer) {
    if (runtimeLayer->dynamicName != nullptr) {
        free(runtimeLayer->dynamicName);
        runtimeLayer->dynamicName = nullptr;
    }
    size_t elementCount = arrlenu(runtimeLayer->elements);
    repeat(elementCount, i) {
        RuntimeLayerElement* el = &runtimeLayer->elements[i];
        if (el->backgroundElement != nullptr) {
            free(el->backgroundElement);
            el->backgroundElement = nullptr;
        }
        if (el->spriteElement != nullptr) {
            free(el->spriteElement);
            el->spriteElement = nullptr;
        }
    }
    arrfree(runtimeLayer->elements);
    runtimeLayer->elements = nullptr;
}

static void freeRuntimeLayersArray(RuntimeLayer** runtimeLayerArray) {
    size_t count = arrlenu(*runtimeLayerArray);
    repeat(count, i) {
        Runner_freeRuntimeLayer(&(*runtimeLayerArray)[i]);
    }
    arrfree(*runtimeLayerArray);
    *runtimeLayerArray = nullptr;
}

// ===[ Helper: Find event action in object hierarchy ]===
// Walks the parent chain starting from objectIndex to find an event handler.
// Returns the EventAction's codeId, or -1 if not found.
// If outOwnerObjectIndex is non-null, it is set to the objectIndex that owns the found event (or -1 if not found).
static int32_t findEventCodeIdAndOwner(DataWin* dataWin, int32_t objectIndex, int32_t eventType, int32_t eventSubtype, int32_t* outOwnerObjectIndex) {
    int32_t currentObj = objectIndex;
    int depth = 0;

    while (currentObj >= 0 && (uint32_t) currentObj < dataWin->objt.count && 32 > depth) {
        GameObject* obj = &dataWin->objt.objects[currentObj];

        if (OBJT_EVENT_TYPE_COUNT > eventType) {
            ObjectEventList* eventList = &obj->eventLists[eventType];
            repeat(eventList->eventCount, i) {
                ObjectEvent* evt = &eventList->events[i];
                if ((int32_t) evt->eventSubtype == eventSubtype) {
                    // Found it - return the first action's codeId
                    if (evt->actionCount > 0 && evt->actions[0].codeId >= 0) {
                        if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = currentObj;
                        return evt->actions[0].codeId;
                    }
                    if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
                    return -1;
                }
            }
        }

        // Walk to parent
        currentObj = obj->parentId;
        depth++;
    }

    if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
    return -1;
}

// ===[ Event Execution ]===

static void setVMInstanceContext(VMContext* vm, Instance* instance) {
    vm->currentInstance = instance;
}

static void restoreVMInstanceContext(VMContext* vm, Instance* savedInstance) {
    vm->currentInstance = savedInstance;
}
static inline void stepCacheRecord(Runner* runner, int32_t codeId, uint32_t us) {
    if (codeId < 0 || us == 0) return;
    for (int32_t i = 0; i < runner->stepCacheCount; i++) {
        if (runner->stepCache[i].codeId == codeId) {
            runner->stepCache[i].timeUs += us;
            runner->stepCache[i].calls++;
            return;
        }
    }
    if (runner->stepCacheCount < 256) {
        runner->stepCache[runner->stepCacheCount].codeId = codeId;
        runner->stepCache[runner->stepCacheCount].timeUs = us;
        runner->stepCache[runner->stepCacheCount].calls = 1;
        runner->stepCacheCount++;
    }


}

static void executeCodeFast(Runner* runner, Instance* instance, int32_t codeId,
                            int32_t ownerObjectIndex, int32_t eventType, int32_t eventSubtype,
                            NativeCodeFunc nativeFunc) {
    if (0 > codeId) return;

    VMContext* vm = runner->vmContext;
    Instance* savedInstance = (Instance*) vm->currentInstance;


    uint8_t* savedBytecodeBase = vm->bytecodeBase;
    uint32_t savedIP = vm->ip;
    uint32_t savedCodeEnd = vm->codeEnd;
    const char* savedCodeName = vm->currentCodeName;
    RValue* savedLocalVars = vm->localVars;
    uint32_t savedLocalVarCount = vm->localVarCount;
    ArrayMapEntry* savedLocalArrayMap = vm->localArrayMap;

    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;

    vm->currentEventType = eventType;
    vm->currentEventSubtype = eventSubtype;
    vm->currentEventObjectIndex = ownerObjectIndex;

    setVMInstanceContext(vm, instance);


    const char* codeName = NULL;
    clock_t _profStart = 0;
    if (runner->profileFile != NULL) {
        codeName = runner->dataWin->code.entries[codeId].name;
        ptrdiff_t pi = shgeti(runner->profileCalls, codeName);
        if (pi >= 0) runner->profileCalls[pi].value++;
        else shput(runner->profileCalls, (char*)codeName, 1);
        _profStart = clock();
    }

    uint64_t _stepT0 = _profTickUs();

    if (nativeFunc != nullptr) {
        nativeFunc(vm, runner, instance);
    } else {
        RValue result = VM_executeCode(vm, codeId);
        RValue_free(&result);
    }

    if (eventType != EVENT_DRAW) {
        uint32_t _stepDt = (uint32_t)(_profTickUs() - _stepT0);
        stepCacheRecord(runner, codeId, _stepDt);
    }

    if (runner->profileFile != NULL && _profStart != 0) {
        clock_t _profEnd = clock();
        int32_t usec = (int32_t)((_profEnd - _profStart) * 1000000 / CLOCKS_PER_SEC);
        if (codeName == NULL) codeName = runner->dataWin->code.entries[codeId].name;
        ptrdiff_t ti = shgeti(runner->profileTimes, codeName);
        if (ti >= 0) runner->profileTimes[ti].value += usec;
        else shput(runner->profileTimes, (char*)codeName, usec);
    }

    restoreVMInstanceContext(vm, savedInstance);
    vm->bytecodeBase = savedBytecodeBase;
    vm->ip = savedIP;
    vm->codeEnd = savedCodeEnd;
    vm->currentCodeName = savedCodeName;
    vm->localVars = savedLocalVars;
    vm->localVarCount = savedLocalVarCount;
    vm->localArrayMap = savedLocalArrayMap;
    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
}

static void executeCode(Runner* runner, Instance* instance, int32_t codeId) {

    if (0 > codeId) return;

    VMContext* vm = runner->vmContext;


    Instance* savedInstance = (Instance*) vm->currentInstance;




    uint8_t* savedBytecodeBase = vm->bytecodeBase;
    uint32_t savedIP = vm->ip;
    uint32_t savedCodeEnd = vm->codeEnd;
    const char* savedCodeName = vm->currentCodeName;
    RValue* savedLocalVars = vm->localVars;
    uint32_t savedLocalVarCount = vm->localVarCount;
    ArrayMapEntry* savedLocalArrayMap = vm->localArrayMap;
    int32_t savedStackTop = vm->stack.top;



    RValue* savedStackValues = nullptr;
    if (savedStackTop > 0) {
        savedStackValues = safeMalloc((uint32_t) savedStackTop * sizeof(RValue));
        memcpy(savedStackValues, vm->stack.slots, (uint32_t) savedStackTop * sizeof(RValue));
    }


    setVMInstanceContext(vm, instance);


    const char* codeName = runner->dataWin->code.entries[codeId].name;


    if (runner->profileFile != NULL) {
        ptrdiff_t pi = shgeti(runner->profileCalls, codeName);
        if (pi >= 0) runner->profileCalls[pi].value++;
        else shput(runner->profileCalls, (char*)codeName, 1);
    }

    clock_t _profStart = 0;
    if (runner->profileFile != NULL) _profStart = clock();


    uint64_t _stepT0 = _profTickUs();

    NativeCodeFunc nativeFunc = NativeScripts_find(codeName);
    if (nativeFunc != nullptr) {
        nativeFunc(vm, runner, instance);
    } else {

        RValue result = VM_executeCode(vm, codeId);
        RValue_free(&result);
    }


    if (vm->currentEventType != EVENT_DRAW) {
        uint32_t _stepDt = (uint32_t)(_profTickUs() - _stepT0);
        stepCacheRecord(runner, codeId, _stepDt);
    }

    if (runner->profileFile != NULL && _profStart != 0) {
        clock_t _profEnd = clock();
        int32_t usec = (int32_t)((_profEnd - _profStart) * 1000000 / CLOCKS_PER_SEC);
        ptrdiff_t ti = shgeti(runner->profileTimes, codeName);
        if (ti >= 0) runner->profileTimes[ti].value += usec;
        else shput(runner->profileTimes, (char*)codeName, usec);
    }


    restoreVMInstanceContext(vm, savedInstance);


    vm->bytecodeBase = savedBytecodeBase;
    vm->ip = savedIP;
    vm->codeEnd = savedCodeEnd;
    vm->currentCodeName = savedCodeName;
    vm->localVars = savedLocalVars;
    vm->localVarCount = savedLocalVarCount;
    vm->localArrayMap = savedLocalArrayMap;
    vm->stack.top = savedStackTop;


    if (savedStackTop > 0) {
        memcpy(vm->stack.slots, savedStackValues, (uint32_t) savedStackTop * sizeof(RValue));
        free(savedStackValues);
    }
}

const char* Runner_getEventName(int32_t eventType, int32_t eventSubtype) {
    switch (eventType) {
        case EVENT_CREATE:  return "Create";
        case EVENT_DESTROY: return "Destroy";
        case EVENT_ALARM:   return "Alarm";
        case EVENT_COLLISION: return "Collision";
        case EVENT_STEP:
            switch (eventSubtype) {
                case STEP_BEGIN:  return "BeginStep";
                case STEP_NORMAL: return "NormalStep";
                case STEP_END:    return "EndStep";
                default:          return "Step";
            }
        case EVENT_DRAW:
            switch (eventSubtype) {
                case DRAW_NORMAL:    return "Draw";
                case DRAW_GUI:       return "DrawGUI";
                case DRAW_BEGIN:     return "DrawBegin";
                case DRAW_END:       return "DrawEnd";
                case DRAW_GUI_BEGIN: return "DrawGUIBegin";
                case DRAW_GUI_END:   return "DrawGUIEnd";
                case DRAW_PRE:       return "DrawPre";
                case DRAW_POST:      return "DrawPost";
                default:             return "Draw";
            }
        case EVENT_KEYBOARD:   return "Keyboard";
        case EVENT_OTHER:
            switch (eventSubtype) {
                case OTHER_OUTSIDE_ROOM:    return "OutsideRoom";
                case OTHER_GAME_START:      return "GameStart";
                case OTHER_ROOM_START:      return "RoomStart";
                case OTHER_ROOM_END:        return "RoomEnd";
                case OTHER_END_OF_PATH:     return "EndOfPath";
                case OTHER_USER0 +  0:      return "UserEvent0";
                case OTHER_USER0 +  1:      return "UserEvent1";
                case OTHER_USER0 +  2:      return "UserEvent2";
                case OTHER_USER0 +  3:      return "UserEvent3";
                case OTHER_USER0 +  4:      return "UserEvent4";
                case OTHER_USER0 +  5:      return "UserEvent5";
                case OTHER_USER0 +  6:      return "UserEvent6";
                case OTHER_USER0 +  7:      return "UserEvent7";
                case OTHER_USER0 +  8:      return "UserEvent8";
                case OTHER_USER0 +  9:      return "UserEvent9";
                case OTHER_USER0 + 10:      return "UserEvent10";
                case OTHER_USER0 + 11:      return "UserEvent11";
                case OTHER_USER0 + 12:      return "UserEvent12";
                case OTHER_USER0 + 13:      return "UserEvent13";
                case OTHER_USER0 + 14:      return "UserEvent14";
                case OTHER_USER0 + 15:      return "UserEvent15";
                default:                    return "Other";
            }
        case EVENT_KEYPRESS:   return "KeyPress";
        case EVENT_KEYRELEASE: return "KeyRelease";
        case EVENT_PRECREATE:  return "PreCreate";
        default: return "Unknown";
    }
}

void Runner_executeEventFromObject(Runner* runner, Instance* instance, int32_t startObjectIndex, int32_t eventType, int32_t eventSubtype) {
    int32_t ownerObjectIndex = -1;
    int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, startObjectIndex, eventType, eventSubtype, &ownerObjectIndex);

    VMContext* vm = runner->vmContext;
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;

    vm->currentEventType = eventType;
    vm->currentEventSubtype = eventSubtype;
    vm->currentEventObjectIndex = ownerObjectIndex;

#ifdef ENABLE_VM_TRACING
    if (codeId >= 0 && shlen(vm->eventsToBeTraced) != -1) {
        const char* eventName = Runner_getEventName(eventType, eventSubtype);
        const char* objectName = runner->dataWin->objt.objects[instance->objectIndex].name;

        bool shouldTrace = shgeti(vm->eventsToBeTraced, "*") != -1 || shgeti(vm->eventsToBeTraced, eventName) != -1 || shgeti(vm->eventsToBeTraced, objectName) != -1;

        if (shouldTrace) {
            if (eventType == EVENT_ALARM) {
                fprintf(stderr, "Runner: [%s] %s %d (instanceId=%d)\n", objectName, eventName, eventSubtype, instance->instanceId);
            } else {
                fprintf(stderr, "Runner: [%s] %s (instanceId=%d)\n", objectName, eventName, instance->instanceId);
            }
        }
    }
#endif

    executeCode(runner, instance, codeId);

    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
}

void Runner_executeEvent(Runner* runner, Instance* instance, int32_t eventType, int32_t eventSubtype) {
    Runner_executeEventFromObject(runner, instance, instance->objectIndex, eventType, eventSubtype);
}

// Events that GMS 2.3+ routes through per-object Handle* dispatchers rather than Perform_Event_All.
static bool eventUsesBC17PerObjectDispatch(int32_t eventType) {
    return eventType == EVENT_STEP || eventType == EVENT_ALARM || eventType == EVENT_KEYBOARD || eventType == EVENT_KEYPRESS || eventType == EVENT_KEYRELEASE;
}

// Per-object event dispatch matching the native GMS 2.x eventUsesPerObjectDispatch family.
// Groups instances by objectIndex, then iterates each bucket in insertion order. Object buckets are visited in ascending objectIndex order, mirroring how the native runner's obj_has_event table enumerates objects that declare this event.
static void executeEventPerObject(Runner* runner, int32_t eventType, int32_t eventSubtype) {
    // Bucket instances by objectIndex. Each bucket preserves insertion order, so within a single object type instances still fire oldest-first.
    // The bucket snapshot taken here also doubles as a way to NOT fire events for newly created instances.
    int32_t objectCount = (int32_t) runner->dataWin->objt.count;
    Instance*** bucketsByObject = calloc((size_t) objectCount, sizeof(Instance**));
    int32_t totalInstances = (int32_t) arrlen(runner->instances);
    repeat(totalInstances, i) {
        Instance* inst = runner->instances[i];
        if (inst->objectIndex >= 0 && inst->objectIndex < objectCount) {
            arrput(bucketsByObject[inst->objectIndex], inst);
        }
    }

    // Visit object buckets in ascending objectIndex order.
    repeat(objectCount, objIdx) {
        Instance** bucket = bucketsByObject[objIdx];
        int32_t bucketCount = (int32_t) arrlen(bucket);
        if (bucketCount == 0) continue;
        // Only touch buckets whose object actually handles this event (including inherited from parent chain).
        int32_t ownerObj = -1;
        if (findEventCodeIdAndOwner(runner->dataWin, objIdx, eventType, eventSubtype, &ownerObj) < 0) {
            arrfree(bucket);
            continue;
        }
        repeat(bucketCount, i) {
            Instance* inst = bucket[i];
            if (!inst->active) continue;
            Runner_executeEvent(runner, inst, eventType, eventSubtype);
        }
        arrfree(bucket);
    }

    free(bucketsByObject);
}

static void addInstanceToCache(Runner* runner, Instance* inst) {
    if (runner->instancesByObjInclParent == NULL) return;
    if (runner->cachedInstCount < 0) return;
    int32_t oi = inst->objectIndex;
    if (oi < 0 || oi >= runner->instancesByObjMax) return;


    arrput(runner->instancesByObjDirect[oi], inst);

    int32_t byte = oi >> 3, mask = 1 << (oi & 7);
    if (!(runner->oiInListBitmap[byte] & mask)) {
        runner->oiInListBitmap[byte] |= mask;
        arrput(runner->activeOIsList, oi);
    }


    int32_t cur = oi;
    int32_t depth = 0;
    DataWin* dw = runner->dataWin;
    while (cur >= 0 && cur < runner->instancesByObjMax && depth < 32) {
        arrput(runner->instancesByObjInclParent[cur], inst);
        cur = dw->objt.objects[cur].parentId;
        depth++;
    }
    runner->cachedInstCount++;
}


static void removeInstanceFromCache(Runner* runner, Instance* inst) {
    if (runner->instancesByObjInclParent == NULL) return;
    if (runner->cachedInstCount < 0) return;
    int32_t oi = inst->objectIndex;
    if (oi < 0 || oi >= runner->instancesByObjMax) return;


    {
        Instance** list = runner->instancesByObjDirect[oi];
        int32_t n = (int32_t)arrlen(list);
        for (int32_t k = 0; k < n; k++) {
            if (list[k] == inst) {
                list[k] = list[n - 1];
                arrsetlen(runner->instancesByObjDirect[oi], n - 1);
                break;
            }
        }

    }


    int32_t cur = oi;
    int32_t depth = 0;
    DataWin* dw = runner->dataWin;
    while (cur >= 0 && cur < runner->instancesByObjMax && depth < 32) {
        Instance** plist = runner->instancesByObjInclParent[cur];
        int32_t pn = (int32_t)arrlen(plist);
        for (int32_t k = 0; k < pn; k++) {
            if (plist[k] == inst) {
                plist[k] = plist[pn - 1];
                arrsetlen(runner->instancesByObjInclParent[cur], pn - 1);
                break;
            }
        }
        cur = dw->objt.objects[cur].parentId;
        depth++;
    }
    runner->cachedInstCount--;
}


static void ensureInstancesByObj(Runner* runner) {
    int32_t current = (int32_t)arrlen(runner->instances);
    if (current == runner->cachedInstCount && runner->instancesByObjInclParent != NULL) return;

    int32_t maxOI = (int32_t)runner->dataWin->objt.count;
    DataWin* dw = runner->dataWin;

    if (runner->instancesByObjInclParent == NULL) {
        runner->instancesByObjInclParent = (Instance***)safeCalloc((size_t)maxOI, sizeof(Instance**));
        runner->instancesByObjDirect = (Instance***)safeCalloc((size_t)maxOI, sizeof(Instance**));
        runner->oiInListBitmap = (uint8_t*)safeCalloc((size_t)((maxOI + 7) / 8), 1);
        runner->instancesByObjMax = maxOI;
    }


    for (int32_t i = 0; i < runner->instancesByObjMax; i++) {
        if (runner->instancesByObjInclParent[i]) arrsetlen(runner->instancesByObjInclParent[i], 0);
        if (runner->instancesByObjDirect[i]) arrsetlen(runner->instancesByObjDirect[i], 0);
    }
    arrsetlen(runner->activeOIsList, 0);
    memset(runner->oiInListBitmap, 0, (runner->instancesByObjMax + 7) / 8);

    for (int32_t i = 0; i < current; i++) {
        Instance* inst = runner->instances[i];
        int32_t oi = inst->objectIndex;
        if (oi < 0 || oi >= runner->instancesByObjMax) continue;


        arrput(runner->instancesByObjDirect[oi], inst);
        int32_t byte = oi >> 3, mask = 1 << (oi & 7);
        if (!(runner->oiInListBitmap[byte] & mask)) {
            runner->oiInListBitmap[byte] |= mask;
            arrput(runner->activeOIsList, oi);
        }


        int32_t cur = oi;
        int32_t depth = 0;
        while (cur >= 0 && cur < runner->instancesByObjMax && depth < 32) {
            arrput(runner->instancesByObjInclParent[cur], inst);
            cur = dw->objt.objects[cur].parentId;
            depth++;
        }
    }

    runner->cachedInstCount = current;
}

typedef struct {
    int32_t codeId;
    int32_t ownerObjectIndex;
    NativeCodeFunc nativeFunc;
} ObjEventCache;

void Runner_executeEventForAll(Runner* runner, int32_t eventType, int32_t eventSubtype) {









    DataWin* dw = runner->dataWin;
    int32_t maxObjIdx = (int32_t)dw->objt.count;


    ensureInstancesByObj(runner);





    struct DispatchEntry { int32_t oi; int32_t codeId; int32_t ownerObj; NativeCodeFunc nativeFunc; };
    struct DispatchEntry entries[128];
    int32_t entryCount = 0;

    int32_t nActiveOIs = (int32_t)arrlen(runner->activeOIsList);
    for (int32_t u = 0; u < nActiveOIs && entryCount < 128; u++) {
        int32_t oi = runner->activeOIsList[u];
        if (oi < 0 || oi >= maxObjIdx) continue;

        if (arrlen(runner->instancesByObjDirect[oi]) == 0) continue;

        int32_t ownerObj = -1;
        int32_t cid = findEventCodeIdAndOwner(dw, oi, eventType, eventSubtype, &ownerObj);
        if (cid < 0) continue;

        entries[entryCount].oi = oi;
        entries[entryCount].codeId = cid;
        entries[entryCount].ownerObj = ownerObj;
        entries[entryCount].nativeFunc = NativeScripts_find(dw->code.entries[cid].name);
        entryCount++;
    }

    if (entryCount == 0) return;


    for (int32_t a = 1; a < entryCount; a++) {
        struct DispatchEntry key = entries[a];
        int32_t b = a - 1;
        while (b >= 0 && entries[b].oi > key.oi) {
            entries[b + 1] = entries[b];
            b--;
        }
        entries[b + 1] = key;
    }





    int32_t activeCount = entryCount;
    ObjEventCache objCacheLocal[128];
    int32_t oiCounts[128];
    int32_t oiOffsets[128];
    int32_t totalMatching = 0;
    for (int32_t e = 0; e < entryCount; e++) {
        totalMatching += (int32_t)arrlen(runner->instancesByObjDirect[entries[e].oi]);
    }
    Instance** sorted = (Instance**)alloca(totalMatching * sizeof(Instance*));
    int32_t offset = 0;
    for (int32_t e = 0; e < entryCount; e++) {
        struct DispatchEntry* de = &entries[e];
        objCacheLocal[e].codeId = de->codeId;
        objCacheLocal[e].ownerObjectIndex = de->ownerObj;
        objCacheLocal[e].nativeFunc = de->nativeFunc;
        oiOffsets[e] = offset;
        Instance** list = runner->instancesByObjDirect[de->oi];
        int32_t listLen = (int32_t)arrlen(list);
        for (int32_t k = 0; k < listLen; k++) {
            sorted[offset++] = list[k];
        }
        oiCounts[e] = listLen;
    }


    VMContext* vm = runner->vmContext;
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;

    for (int32_t u = 0; u < activeCount; u++) {
        ObjEventCache* c = &objCacheLocal[u];
        int32_t start = oiOffsets[u];
        int32_t end = start + oiCounts[u];

        vm->currentEventType = eventType;
        vm->currentEventSubtype = eventSubtype;
        vm->currentEventObjectIndex = c->ownerObjectIndex;

        const char* cn = NULL;
        if (runner->profileFile != NULL)
            cn = dw->code.entries[c->codeId].name;




        bool recordStep = (eventType != EVENT_DRAW);

        if (c->nativeFunc != nullptr) {
            NativeCodeFunc fn = c->nativeFunc;
            for (int32_t j = start; j < end; j++) {
                Instance* inst = sorted[j];
                if (!inst->active) continue;
                Instance* savedInst = (Instance*)vm->currentInstance;
                vm->currentInstance = inst;
                if (cn != NULL) {
                    ptrdiff_t pi = shgeti(runner->profileCalls, cn);
                    if (pi >= 0) runner->profileCalls[pi].value++;
                    else shput(runner->profileCalls, (char*)cn, 1);
                    clock_t ps = clock();
                    fn(vm, runner, inst);
                    clock_t pe = clock();
                    int32_t us = (int32_t)((pe - ps) * 1000000 / CLOCKS_PER_SEC);
                    ptrdiff_t ti = shgeti(runner->profileTimes, cn);
                    if (ti >= 0) runner->profileTimes[ti].value += us;
                    else shput(runner->profileTimes, (char*)cn, us);
                } else {
                    fn(vm, runner, inst);
                }
                vm->currentInstance = savedInst;
            }
        } else {
            int32_t cid = c->codeId;
            for (int32_t j = start; j < end; j++) {
                Instance* inst = sorted[j];
                if (!inst->active) continue;
                Instance* savedInst = (Instance*)vm->currentInstance;
                uint8_t* sBB = vm->bytecodeBase; uint32_t sIP = vm->ip;
                uint32_t sCE = vm->codeEnd; const char* sCN = vm->currentCodeName;
                RValue* sLV = vm->localVars; uint32_t sLC = vm->localVarCount;
                ArrayMapEntry* sLA = vm->localArrayMap;
                vm->currentInstance = inst;
                if (cn != NULL) {
                    ptrdiff_t pi = shgeti(runner->profileCalls, cn);
                    if (pi >= 0) runner->profileCalls[pi].value++;
                    else shput(runner->profileCalls, (char*)cn, 1);
                    clock_t ps = clock();
                    RValue result = VM_executeCode(vm, cid);
                    RValue_free(&result);
                    clock_t pe = clock();
                    int32_t us = (int32_t)((pe - ps) * 1000000 / CLOCKS_PER_SEC);
                    ptrdiff_t ti = shgeti(runner->profileTimes, cn);
                    if (ti >= 0) runner->profileTimes[ti].value += us;
                    else shput(runner->profileTimes, (char*)cn, us);
                } else {
                    RValue result = VM_executeCode(vm, cid);
                    RValue_free(&result);
                }
                vm->currentInstance = savedInst;
                vm->bytecodeBase = sBB; vm->ip = sIP; vm->codeEnd = sCE;
                vm->currentCodeName = sCN; vm->localVars = sLV;
                vm->localVarCount = sLC; vm->localArrayMap = sLA;
            }
        }
    }

    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;

}

// ===[ Background Scrolling & Drawing ]===

void Runner_scrollBackgrounds(Runner* runner) {
    repeat(8, i) {
        RuntimeBackground* bg = &runner->backgrounds[i];
        if (!bg->visible) continue;
        bg->x += bg->speedX;
        bg->y += bg->speedY;
    }
}

void Runner_drawBackgrounds(Runner* runner, bool foreground) {
    if (runner->renderer == nullptr) return;
    DataWin* dataWin = runner->dataWin;
    float roomW = (float) runner->currentRoom->width;
    float roomH = (float) runner->currentRoom->height;

    repeat(8, i) {
        RuntimeBackground* bg = &runner->backgrounds[i];
        if (!bg->visible || bg->foreground != foreground) continue;
        if (0 > bg->backgroundIndex) continue;

        int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(dataWin, bg->backgroundIndex);
        if (0 > tpagIndex) continue;

        if (bg->stretch) {
            // Stretch to fill room dimensions
            TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
            float xscale = roomW / (float) tpag->boundingWidth;
            float yscale = roomH / (float) tpag->boundingHeight;
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, bg->alpha);
        } else if (bg->tileX || bg->tileY) {
            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, bg->x, bg->y, bg->tileX, bg->tileY, roomW, roomH, bg->alpha);
        } else {
            // Single placement
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, bg->x, bg->y, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, bg->alpha);
        }
    }
}

// ===[ Draw ]===

typedef enum { DRAWABLE_TILE, DRAWABLE_INSTANCE, DRAWABLE_LAYER } DrawableType;

typedef struct {
    DrawableType type;
    int32_t depth;
    union {
        Instance* instance;
        int32_t tileIndex; // index into currentRoom->tiles
        RuntimeLayer* runtimeLayer;
    };
} Drawable;

static int compareDrawableDepth(const void* a, const void* b) {
    const Drawable* da = (const Drawable*) a;
    const Drawable* db = (const Drawable*) b;
    // Higher depth draws first (behind), lower depth draws last (in front)
    if (da->depth > db->depth) return -1;
    if (db->depth > da->depth) return 1;
    // At same depth, tiles before instances (tiles are background)
    if (da->type < db->type) return -1;
    if (db->type < da->type) return 1;
    // At same depth and type, preserve original room order (higher index draws later = in front)
    if (da->type == DRAWABLE_TILE) {
        if (db->tileIndex > da->tileIndex) return -1;
        if (da->tileIndex > db->tileIndex) return 1;
    }
    return 0;
}

static int compareInstanceDepth(const void* a, const void* b) {
    Instance* instA = *(Instance**) a;
    Instance* instB = *(Instance**) b;
    // Higher depth draws first (behind), lower depth draws last (in front)
    if (instA->depth > instB->depth) return -1;
    if (instB->depth > instA->depth) return 1;
    return 0;
}


static void fireDrawSubtype(Runner* runner, Instance** drawList, int32_t drawCount, int32_t subtype) {


    DataWin* dw = runner->dataWin;
    int32_t maxObj = (int32_t)dw->objt.count;
    ObjEventCache* cache = (ObjEventCache*)alloca(maxObj * sizeof(ObjEventCache));
    int32_t vBytes = (maxObj + 7) / 8;
    uint8_t* visited = (uint8_t*)alloca(vBytes);
    memset(visited, 0, vBytes);

    VMContext* vm = runner->vmContext;
    int32_t savedET = vm->currentEventType;
    int32_t savedES = vm->currentEventSubtype;
    int32_t savedEO = vm->currentEventObjectIndex;

    vm->currentEventType = EVENT_DRAW;
    vm->currentEventSubtype = subtype;

    repeat(drawCount, i) {
        Instance* inst = drawList[i];
        if (!inst->active) continue;
        int32_t oi = inst->objectIndex;
        if (oi >= 0 && oi < maxObj) {
            bool isVisited = (visited[oi / 8] & (1 << (oi % 8))) != 0;
            if (isVisited && cache[oi].codeId < 0) continue;
            if (!isVisited) {
                visited[oi / 8] |= (1 << (oi % 8));
                int32_t ownerObj = -1;
                int32_t cid = findEventCodeIdAndOwner(dw, oi, EVENT_DRAW, subtype, &ownerObj);
                if (cid < 0) { cache[oi].codeId = -1; continue; }
                cache[oi].codeId = cid;
                cache[oi].ownerObjectIndex = ownerObj;
                const char* cn = dw->code.entries[cid].name;
                cache[oi].nativeFunc = NativeScripts_find(cn);
            }
        } else {
            Runner_executeEvent(runner, inst, EVENT_DRAW, subtype);
            continue;
        }

        vm->currentEventObjectIndex = cache[oi].ownerObjectIndex;
        Instance* savedInst = (Instance*)vm->currentInstance;
        vm->currentInstance = inst;

        if (cache[oi].nativeFunc != nullptr) {
            cache[oi].nativeFunc(vm, runner, inst);
        } else {
            uint8_t* sBB = vm->bytecodeBase; uint32_t sIP = vm->ip;
            uint32_t sCE = vm->codeEnd; const char* sCN = vm->currentCodeName;
            RValue* sLV = vm->localVars; uint32_t sLC = vm->localVarCount;
            ArrayMapEntry* sLA = vm->localArrayMap;
            RValue result = VM_executeCode(vm, cache[oi].codeId);
            RValue_free(&result);
            vm->bytecodeBase = sBB; vm->ip = sIP; vm->codeEnd = sCE;
            vm->currentCodeName = sCN; vm->localVars = sLV;
            vm->localVarCount = sLC; vm->localArrayMap = sLA;
        }
        vm->currentInstance = savedInst;
    }

    vm->currentEventType = savedET;
    vm->currentEventSubtype = savedES;
    vm->currentEventObjectIndex = savedEO;
}

// GMS2 tilemap cell bit layout (matches HTML5 Function_Layers.js TileIndex/Mirror/Flip/Rotate masks)
#define GMS2_TILE_INDEX_MASK  0x0007FFFF // bits 0..18
#define GMS2_TILE_MIRROR_MASK 0x10000000 // bit 28 (horizontal flip)
#define GMS2_TILE_FLIP_MASK   0x20000000 // bit 29 (vertical flip)
#define GMS2_TILE_ROTATE_MASK 0x40000000 // bit 30 (90 CW)

static void Runner_drawTileLayer(Runner* runner, RoomLayerTilesData* data, float layerOffsetX, float layerOffsetY) {
    if (data == nullptr || data->tileData == nullptr) return;
    if (0 > data->backgroundIndex) return;

    DataWin* dw = runner->dataWin;
    if ((uint32_t) data->backgroundIndex >= dw->bgnd.count) return;

    Background* tileset = &dw->bgnd.backgrounds[data->backgroundIndex];
    if (tileset->gms2TileWidth == 0 || tileset->gms2TileHeight == 0 || tileset->gms2TileColumns == 0) return;

    int32_t tpagIndex = DataWin_resolveTPAG(dw, tileset->textureOffset);
    if (0 > tpagIndex) return;

    uint32_t tileW = tileset->gms2TileWidth;
    uint32_t tileH = tileset->gms2TileHeight;
    uint32_t borderX = tileset->gms2OutputBorderX;
    uint32_t borderY = tileset->gms2OutputBorderY;
    uint32_t columns = tileset->gms2TileColumns;

    static bool rotateWarned = false;

    repeat(data->tilesY, ty) {
        repeat(data->tilesX, tx) {
            uint32_t cell = data->tileData[ty * data->tilesX + tx];
            uint32_t tileIndex = cell & GMS2_TILE_INDEX_MASK;
            if (tileIndex == 0) continue; // 0 = empty

            uint32_t col = tileIndex % columns;
            uint32_t row = tileIndex / columns;
            int32_t srcX = (int32_t) (col * (tileW + 2 * borderX) + borderX);
            int32_t srcY = (int32_t) (row * (tileH + 2 * borderY) + borderY);

            bool mirror = (cell & GMS2_TILE_MIRROR_MASK) != 0;
            bool flip = (cell & GMS2_TILE_FLIP_MASK) != 0;
            bool rotate = (cell & GMS2_TILE_ROTATE_MASK) != 0;

            if (rotate && !rotateWarned) {
                fprintf(stderr, "Runner: WARNING: GMS2 tile layer has rotated tiles; rotation not yet implemented, drawing unrotated\n");
                rotateWarned = true;
            }

            float xscale = mirror ? -1.0f : 1.0f;
            float yscale = flip ? -1.0f : 1.0f;

            // With negative scale the quad grows in the opposite direction, so shift the
            // destination by one tile to keep the origin at the top-left of the cell.
            float dstX = (float) (tx * tileW) + layerOffsetX + (mirror ? (float) tileW : 0.0f);
            float dstY = (float) (ty * tileH) + layerOffsetY + (flip ? (float) tileH : 0.0f);

            runner->renderer->vtable->drawSpritePart(runner->renderer, tpagIndex, srcX, srcY, (int32_t) tileW, (int32_t) tileH, dstX, dstY, xscale, yscale, 0xFFFFFF, 1.0f);
        }
    }
}

void Runner_draw(Runner* runner) {
    Room* room = runner->currentRoom;


    uint64_t _dt0 = _profTickUs(), _dt1;
    #define DRAW_MARK(field) do { _dt1 = _profTickUs(); \
        runner->field += (uint32_t)(_dt1 - _dt0); \
        _dt0 = _dt1; } while(0)

    DataWin* dw = runner->dataWin;


    int32_t count = (int32_t) arrlen(runner->instances);
    Instance** drawListBuf = (Instance**)alloca(count * sizeof(Instance*));
    int32_t drawCount = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && inst->visible) {
            drawListBuf[drawCount++] = inst;
        }
    }
    Instance** drawList = drawListBuf;


    if (drawCount > 1) {
        qsort(drawList, drawCount, sizeof(Instance*), compareInstanceDepth);
    }

    DRAW_MARK(drawPhaseSortList);


    if(true)
        Runner_drawBackgrounds(runner, false);








    DRAW_MARK(drawPhaseBuildList);


    typedef struct { int32_t oi; int32_t codeId; int32_t ownerObj; NativeCodeFunc nativeFunc;
                     uint32_t vmTimeUs; int32_t vmCalls; } DrawCacheEntry;
    DrawCacheEntry drawCache[96];
    int32_t drawCacheCount = 0;



    #define DRAW_INSTANCE(inst) do { \
        int32_t _oi = (inst)->objectIndex; \
        DrawCacheEntry* _found = NULL; \
        for (int32_t _ci = 0; _ci < drawCacheCount; _ci++) \
            if (drawCache[_ci].oi == _oi) { _found = &drawCache[_ci]; break; } \
        if (!_found && drawCacheCount < 96) { \
            int32_t _ownerObj = -1; \
            int32_t _cid = findEventCodeIdAndOwner(dw, _oi, EVENT_DRAW, DRAW_NORMAL, &_ownerObj); \
            _found = &drawCache[drawCacheCount++]; \
            _found->oi = _oi; _found->codeId = _cid; _found->ownerObj = _ownerObj; \
            _found->nativeFunc = (_cid >= 0) ? NativeScripts_find(dw->code.entries[_cid].name) : NULL; \
            _found->vmTimeUs = 0; _found->vmCalls = 0; \
        } \
        if (_found && _found->codeId >= 0) { \
            VMContext* _vm = runner->vmContext; \
            int32_t _sET = _vm->currentEventType, _sES = _vm->currentEventSubtype, _sEO = _vm->currentEventObjectIndex; \
            _vm->currentEventType = EVENT_DRAW; _vm->currentEventSubtype = DRAW_NORMAL; \
            _vm->currentEventObjectIndex = _found->ownerObj; \
            Instance* _si = (Instance*)_vm->currentInstance; _vm->currentInstance = (inst); \
            if (_found->nativeFunc) { \
                uint64_t _nt0 = _profTickUs(); \
                _found->nativeFunc(_vm, runner, (inst)); \
                runner->drawNrmInstNative += (uint32_t)(_profTickUs() - _nt0); \
                runner->drawNrmCountInstNative++; \
            } else { \
                uint8_t* _sBB=_vm->bytecodeBase; uint32_t _sIP=_vm->ip, _sCE=_vm->codeEnd; \
                const char* _sCN=_vm->currentCodeName; RValue* _sLV=_vm->localVars; \
                uint32_t _sLC=_vm->localVarCount; ArrayMapEntry* _sLA=_vm->localArrayMap; \
                uint64_t _vt0 = _profTickUs(); \
                RValue _r = VM_executeCode(_vm, _found->codeId); RValue_free(&_r); \
                uint32_t _vdt = (uint32_t)(_profTickUs() - _vt0); \
                runner->drawNrmInstVM += _vdt; \
                runner->drawNrmCountInstVM++; \
                _found->vmTimeUs += _vdt; _found->vmCalls++; \
                _vm->bytecodeBase=_sBB; _vm->ip=_sIP; _vm->codeEnd=_sCE; \
                _vm->currentCodeName=_sCN; _vm->localVars=_sLV; \
                _vm->localVarCount=_sLC; _vm->localArrayMap=_sLA; \
            } \
            _vm->currentInstance = _si; _vm->currentEventType = _sET; \
            _vm->currentEventSubtype = _sES; _vm->currentEventObjectIndex = _sEO; \
        } else if (runner->renderer != nullptr) { \
            uint64_t _dt = _profTickUs(); \
            Renderer_drawSelf(runner->renderer, (inst)); \
            runner->drawNrmInstDrawSelf += (uint32_t)(_profTickUs() - _dt); \
            runner->drawNrmCountInstDrawSelf++; \
        } \
    } while(0)

    if (room->tileCount == 0) {

        repeat(drawCount, i) {
            Instance* inst = drawList[i];
            if (inst->active) DRAW_INSTANCE(inst);
        }
    } else {

        uint64_t _nrmBldT0 = _profTickUs();
        int32_t maxDrawables = drawCount + (int32_t)room->tileCount + 16;
        Drawable* drawables = (Drawable*)alloca(maxDrawables * sizeof(Drawable));
        int32_t drawableCount = 0;

        repeat(drawCount, i) {
            drawables[drawableCount++] = (Drawable){ .type = DRAWABLE_INSTANCE, .depth = drawList[i]->depth, .instance = drawList[i] };
        }

        if (true) {

            int32_t viewL = 0, viewT = 0, viewR = room->width, viewB = room->height;
            bool viewsEnabled = (room->flags & 1) != 0;
            if (viewsEnabled) {
                int vi = runner->viewCurrent;
                if (vi >= 0 && vi < 8 && room->views[vi].enabled) {
                    viewL = room->views[vi].viewX;
                    viewT = room->views[vi].viewY;
                    viewR = viewL + room->views[vi].viewWidth;
                    viewB = viewT + room->views[vi].viewHeight;
                }
            }

            int32_t cachedDepth = INT32_MIN; bool cachedVisible = true;
            float cachedOffX = 0, cachedOffY = 0;
            repeat(room->tileCount, i) {
                RoomTile* tile = &room->tiles[i];
                if (tile->tileDepth != cachedDepth) {
                    cachedDepth = tile->tileDepth; cachedVisible = true; cachedOffX = 0; cachedOffY = 0;
                    ptrdiff_t li = hmgeti(runner->tileLayerMap, tile->tileDepth);
                    if (li >= 0) { cachedVisible = runner->tileLayerMap[li].value.visible;
                        cachedOffX = runner->tileLayerMap[li].value.offsetX;
                        cachedOffY = runner->tileLayerMap[li].value.offsetY; }
                }
                if (!cachedVisible) continue;

                float tileL = (float)tile->x + cachedOffX;
                float tileT = (float)tile->y + cachedOffY;
                float tileR = tileL + (float)tile->width;
                float tileB = tileT + (float)tile->height;
                if (tileR < (float)viewL || tileL > (float)viewR ||
                    tileB < (float)viewT || tileT > (float)viewB) continue;
                drawables[drawableCount++] = (Drawable){ .type = DRAWABLE_TILE, .depth = tile->tileDepth, .tileIndex = (int32_t)i };
            }
        }

        runner->drawNrmBuildDrawables += (uint32_t)(_profTickUs() - _nrmBldT0);

        if (drawableCount > 1) {
            uint64_t _qsT0 = _profTickUs();
            qsort(drawables, drawableCount, sizeof(Drawable), compareDrawableDepth);
            runner->drawNrmQsort += (uint32_t)(_profTickUs() - _qsT0);
        }

        int32_t dCachedDepth = INT32_MIN; float dCachedOffX = 0, dCachedOffY = 0;
        repeat(drawableCount, i) {
            Drawable* d = &drawables[i];
            if (d->type == DRAWABLE_TILE) {
                if (runner->renderer != nullptr) {
                    RoomTile* tile = &room->tiles[d->tileIndex];
                    if (tile->tileDepth != dCachedDepth) {
                        dCachedDepth = tile->tileDepth; dCachedOffX = 0; dCachedOffY = 0;
                        ptrdiff_t li = hmgeti(runner->tileLayerMap, tile->tileDepth);
                        if (li >= 0) { dCachedOffX = runner->tileLayerMap[li].value.offsetX;
                            dCachedOffY = runner->tileLayerMap[li].value.offsetY; }
                    }
                    uint64_t _tt0 = _profTickUs();
                    Renderer_drawTile(runner->renderer, tile, dCachedOffX, dCachedOffY);
                    runner->drawNrmTiles += (uint32_t)(_profTickUs() - _tt0);
                    runner->drawNrmCountTiles++;
                }
            } else if (d->type == DRAWABLE_INSTANCE) {
                Instance* inst = d->instance;
                if (inst->active) DRAW_INSTANCE(inst);
            }
        }
    }

    #undef DRAW_INSTANCE


    runner->topVmDrawName[0] = runner->topVmDrawName[1] = runner->topVmDrawName[2] = NULL;
    runner->topVmDrawTimeUs[0] = runner->topVmDrawTimeUs[1] = runner->topVmDrawTimeUs[2] = 0;
    runner->topVmDrawCalls[0] = runner->topVmDrawCalls[1] = runner->topVmDrawCalls[2] = 0;
    for (int32_t _ci = 0; _ci < drawCacheCount; _ci++) {
        if (drawCache[_ci].vmCalls == 0) continue;
        uint32_t t = drawCache[_ci].vmTimeUs;
        int rank = -1;
        if (t > runner->topVmDrawTimeUs[0]) rank = 0;
        else if (t > runner->topVmDrawTimeUs[1]) rank = 1;
        else if (t > runner->topVmDrawTimeUs[2]) rank = 2;
        if (rank >= 0) {
            for (int k = 2; k > rank; k--) {
                runner->topVmDrawName[k] = runner->topVmDrawName[k-1];
                runner->topVmDrawTimeUs[k] = runner->topVmDrawTimeUs[k-1];
                runner->topVmDrawCalls[k] = runner->topVmDrawCalls[k-1];
            }
            runner->topVmDrawName[rank] = dw->code.entries[drawCache[_ci].codeId].name;
            runner->topVmDrawTimeUs[rank] = t;
            runner->topVmDrawCalls[rank] = drawCache[_ci].vmCalls;
        }
    }
    DRAW_MARK(drawPhaseNormal);





    Runner_drawBackgrounds(runner, true);

    fireDrawSubtype(runner, drawList, drawCount, DRAW_POST);


    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI);



    DRAW_MARK(drawPhaseGUI);

}

void Runner_drawGUI(Runner* runner) {
    Instance** drawList = nullptr;
    int32_t count = (int32_t) arrlen(runner->instances);
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->active && inst->visible) {
            arrput(drawList, inst);
        }
    }

    int32_t drawCount = (int32_t) arrlen(drawList);
    if (drawCount > 1) {
        qsort(drawList, drawCount, sizeof(Instance*), compareInstanceDepth);
    }

    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI_BEGIN);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI);
    fireDrawSubtype(runner, drawList, drawCount, DRAW_GUI_END);

    arrfree(drawList);
}

// ===[ Instance Creation Helper ]===

static bool isObjectDisabled(Runner* runner, int32_t objectIndex) {
    if (runner->disabledObjects == nullptr) return false;
    const char* name = runner->dataWin->objt.objects[objectIndex].name;
    return shgeti(runner->disabledObjects, name) != -1;
}

static Instance* createAndInitInstance(Runner* runner, int32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) {
    DataWin* dataWin = runner->dataWin;
    require(objectIndex >= 0 && dataWin->objt.count > (uint32_t) objectIndex);

    GameObject* objDef = &dataWin->objt.objects[objectIndex];

    Instance* inst = Instance_create(instanceId, objectIndex, x, y);

    // Copy properties from object definition
    inst->spriteIndex = objDef->spriteId;
    inst->visible = objDef->visible;
    inst->solid = objDef->solid;
    inst->persistent = objDef->persistent;
    inst->depth = objDef->depth;
    inst->maskIndex = objDef->textureMaskId;

    hmput(runner->instancesToId, instanceId, inst);
    arrput(runner->instances, inst);

#ifdef ENABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) created at (%f, %f)\n", objDef->name, instanceId, x, y);
    }
#endif

    return inst;
}

// ===[ Room Management ]===

// Collect persistent instances from the previous room (they travel with the player), and free the rest.
// You should re-append them at the tail AFTER creating the new room's own instances, so the iteration order matches the native runner: room-local instances first, persistent arrivals last.
static Instance** takePersistentInstances(Runner* runner) {
    Instance** carriedPersistent = nullptr;
    int32_t oldCount = (int32_t) arrlen(runner->instances);
    repeat(oldCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->persistent) {
            arrput(carriedPersistent, inst);
        } else {
            hmdel(runner->instancesToId, inst->instanceId);
            Instance_free(inst);
        }
    }

    arrfree(runner->instances);
    runner->instances = nullptr;

    return carriedPersistent;
}

// Append the carried-over persistent instances at the tail of runner->instances and free the temporary array. Pairs with takePersistentInstances.
static void returnPersistentInstances(Runner* runner, Instance** carriedPersistent) {
    repeat(arrlen(carriedPersistent), i) {
        arrput(runner->instances, carriedPersistent[i]);
    }
    arrfree(carriedPersistent);
}

static void copyRoomViewToRuntimeView(RoomView* roomView, RuntimeView* runtimeView) {
    runtimeView->enabled = roomView->enabled;
    runtimeView->viewX = roomView->viewX;
    runtimeView->viewY = roomView->viewY;
    runtimeView->viewWidth = roomView->viewWidth;
    runtimeView->viewHeight = roomView->viewHeight;
    runtimeView->portX = roomView->portX;
    runtimeView->portY = roomView->portY;
    runtimeView->portWidth = roomView->portWidth;
    runtimeView->portHeight = roomView->portHeight;
    runtimeView->borderX = roomView->borderX;
    runtimeView->borderY = roomView->borderY;
    runtimeView->speedX = roomView->speedX;
    runtimeView->speedY = roomView->speedY;
    runtimeView->objectId = roomView->objectId;
    runtimeView->viewAngle = 0;
}

static void initRoom(Runner* runner, int32_t roomIndex) {
    DataWin* dataWin = runner->dataWin;
    require(roomIndex >= 0 && dataWin->room.count > (uint32_t) roomIndex);

    Room* room = &dataWin->room.rooms[roomIndex];

    // Lazy-room load: if the payload wasn't loaded, read it from the data.win file now before anything touches the room's game objects/tiles/layers.
    if (!room->payloadLoaded) {
        DataWin_loadRoomPayload(dataWin, roomIndex);
    }

    SavedRoomState* savedState = &runner->savedRoomStates[roomIndex];

    runner->currentRoom = room;
    runner->currentRoomIndex = roomIndex;

    // Find position in room order
    runner->currentRoomOrderPosition = -1;
    repeat(dataWin->gen8.roomOrderCount, i) {
        if (dataWin->gen8.roomOrder[i] == roomIndex) {
            runner->currentRoomOrderPosition = (int32_t) i;
            break;
        }
    }

    // If this is a persistent room that was previously visited, restore saved state
    if (room->persistent && savedState->initialized) {
        memcpy(runner->views, savedState->views, sizeof(runner->views));

        // Restore backgrounds from saved state
        memcpy(runner->backgrounds, savedState->backgrounds, sizeof(runner->backgrounds));
        runner->backgroundColor = savedState->backgroundColor;
        runner->drawBackgroundColor = savedState->drawBackgroundColor;

        // Restore tile layer map
        hmfree(runner->tileLayerMap);
        runner->tileLayerMap = savedState->tileLayerMap;
        savedState->tileLayerMap = nullptr;

        // Restore runtime layers
        freeRuntimeLayersArray(&runner->runtimeLayers);
        runner->runtimeLayers = savedState->runtimeLayers;
        savedState->runtimeLayers = nullptr;

        Instance** carriedPersistent = takePersistentInstances(runner);

        // The native runner restores the room's own linked list first, then appends persistent arrivals at the tail.
        // Event iteration is forward (oldest first), so a persistent instance runs after the room's own instances.
        int32_t savedCount = (int32_t) arrlen(savedState->instances);
        repeat(savedCount, i) {
            arrput(runner->instances, savedState->instances[i]);
        }
        arrfree(savedState->instances);
        savedState->instances = nullptr;

        returnPersistentInstances(runner, carriedPersistent);

        // No Create events, no preCreateCode, no creationCode, no room creation code
        fprintf(stderr, "Runner: Room restored (persistent): %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
        return;
    }

    // === Normal room initialization (first visit, or non-persistent room) ===

    // Initialize the views from scratch
    repeat(MAX_VIEWS, vi) {
        copyRoomViewToRuntimeView(&room->views[vi], &runner->views[vi]);
    }

    // Reset tile layer state for the new room
    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;

    // Populate runtime layers from parsed room layers (GMS2+ only; empty for GMS1.x).
    // Dynamic layers created via layer_create are appended to this array later.
    freeRuntimeLayersArray(&runner->runtimeLayers);
    uint32_t maxLayerId = 0;
    repeat(room->layerCount, i) {
        RoomLayer* layerSource = &room->layers[i];
        RuntimeLayer runtimeLayer = {
            .id = layerSource->id,
            .depth = layerSource->depth,
            .visible = layerSource->visible,
            .xOffset = layerSource->xOffset,
            .yOffset = layerSource->yOffset,
            .hSpeed = layerSource->hSpeed,
            .vSpeed = layerSource->vSpeed,
            .dynamic = false,
            .dynamicName = nullptr,
            .elements = nullptr,
        };
        arrput(runner->runtimeLayers, runtimeLayer);
        if (layerSource->id > maxLayerId) maxLayerId = layerSource->id;
    }
    // Watermark: ensure runtime-allocated IDs (layers + elements) stay above parsed IDs.
    if (maxLayerId >= runner->nextLayerId) runner->nextLayerId = maxLayerId + 1;

    // Populate runtime sprite elements for Assets layers, so they can be queried and destroyed via layer_sprite_get_sprite/layer_sprite_destroy
    repeat(room->layerCount, i) {
        RoomLayer* layerSource = &room->layers[i];
        if (layerSource->type != RoomLayerType_Assets || layerSource->assetsData == nullptr) continue;
        RoomLayerAssetsData* assets = layerSource->assetsData;
        RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
        repeat(assets->spriteCount, j) {
            SpriteInstance* src = &assets->sprites[j];
            RuntimeSpriteElement* spriteElement = safeMalloc(sizeof(RuntimeSpriteElement));
            spriteElement->spriteIndex = src->spriteIndex;
            spriteElement->x = src->x;
            spriteElement->y = src->y;
            spriteElement->scaleX = src->scaleX;
            spriteElement->scaleY = src->scaleY;
            spriteElement->color = src->color;
            spriteElement->animationSpeed = src->animationSpeed;
            spriteElement->animationSpeedType = src->animationSpeedType;
            spriteElement->frameIndex = src->frameIndex;
            spriteElement->rotation = src->rotation;
            RuntimeLayerElement el = {
                .id = Runner_getNextLayerId(runner),
                .type = RuntimeLayerElementType_Sprite,
                .backgroundElement = nullptr,
                .spriteElement = spriteElement,
            };
            arrput(runtimeLayer->elements, el);
        }
    }

    // Copy room background definitions into mutable runtime state
    runner->backgroundColor = room->backgroundColor;
    runner->drawBackgroundColor = room->drawBackgroundColor;
    repeat(8, i) {
        RoomBackground* src = &room->backgrounds[i];
        RuntimeBackground* dst = &runner->backgrounds[i];
        dst->visible = src->enabled;
        dst->foreground = src->foreground;
        dst->backgroundIndex = src->backgroundDefinition;
        dst->x = (float) src->x;
        dst->y = (float) src->y;
        dst->tileX = (bool) src->tileX;
        dst->tileY = (bool) src->tileY;
        dst->speedX = (float) src->speedX;
        dst->speedY = (float) src->speedY;
        dst->stretch = src->stretch;
        dst->alpha = 1.0f;
    }

    Instance** carriedPersistent = takePersistentInstances(runner);

    // Two-pass instance creation (matches HTML5 runner behavior):
    // Pass 1: Create all instance objects so they exist for cross-references
    // Pass 2: Fire preCreateCode, CREATE events, and creationCode
    // This ensures that when an instance's Create event reads another instance
    // (e.g. obj_mainchara reading obj_markerA.x), the target already exists.

    // Pass 1: Create all instances without firing events
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Skip if a persistent instance carried over from the previous room already owns this ID (re-entering the persistent instance's home room, don't create a duplicate!).
        if (hmget(runner->instancesToId, roomObj->instanceID) != nullptr) continue;
        if (isObjectDisabled(runner, roomObj->objectDefinition)) continue;

        Instance* inst = createAndInitInstance(runner, roomObj->instanceID, roomObj->objectDefinition, (GMLReal) roomObj->x, (GMLReal) roomObj->y);
        inst->imageXscale = (float) roomObj->scaleX;
        inst->imageYscale = (float) roomObj->scaleY;
        inst->imageAngle = (float) roomObj->rotation;
        inst->imageSpeed = roomObj->imageSpeed;
        inst->imageIndex = (float) roomObj->imageIndex;
    }

    // In GMS2, instances get their depth from their room layer, not the object definition.
    // This must happen before firing Create events so scripts like scr_depth() read the layer depth.
    if (DataWin_isVersionAtLeast(runner->dataWin, 2, 0, 0, 0)) {
        repeat(room->layerCount, li) {
            RoomLayer* layer = &room->layers[li];
            if (layer->type != RoomLayerType_Instances || layer->instancesData == nullptr) continue;
            RoomLayerInstancesData* layerData = layer->instancesData;
            repeat(layerData->instanceCount, ii) {
                Instance* inst = hmget(runner->instancesToId, layerData->instanceIds[ii]);
                if (inst != nullptr) {
                    inst->depth = layer->depth;
                }
            }
        }
    }

    // Pass 2: Fire events for newly created instances (in room definition order)
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        Instance* inst = hmget(runner->instancesToId, roomObj->instanceID);
        if (inst == nullptr) continue;

        // Skip instances that already had their Create event fired (persistent carry-overs
        // that hmget also matches, since instancesToId still holds them).
        if (inst->createEventFired) continue;
        inst->createEventFired = true;

        Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
        executeCode(runner, inst, roomObj->preCreateCode);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
        executeCode(runner, inst, roomObj->creationCode);
    }

    // Run room creation code
    if (room->creationCodeId >= 0 && dataWin->code.count > (uint32_t) room->creationCodeId) {
        // Room creation code runs in global context (no specific instance)
        RValue result = VM_executeCode(runner->vmContext, room->creationCodeId);
        RValue_free(&result);
    }

    // Append persistent instances carried over from the previous room at the tail, so forward event iteration processes the new room's own instances first and the travelers last.
    returnPersistentInstances(runner, carriedPersistent);

    // Mark this room as initialized for persistent room support
    savedState->initialized = true;

    fprintf(stderr, "Runner: Room loaded: %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
}

// Cleans up the runner state, used when freeing the Runner or when restarting the Runner
static void cleanupState(Runner* runner) {
    // Free all instances
    repeat(arrlen(runner->instances), i) {
        hmdel(runner->instancesToId, runner->instances[i]->instanceId);
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);
    runner->instances = nullptr;

    // Free saved room states
    if (runner->savedRoomStates != nullptr) {
        repeat(runner->dataWin->room.count, i) {
            SavedRoomState* state = &runner->savedRoomStates[i];
            int32_t savedCount = (int32_t) arrlen(state->instances);
            repeat(savedCount, j) {
                hmdel(runner->instancesToId, state->instances[j]->instanceId);
                Instance_free(state->instances[j]);
            }
            arrfree(state->instances);
            hmfree(state->tileLayerMap);
            freeRuntimeLayersArray(&state->runtimeLayers);
        }
        free(runner->savedRoomStates);
    }
    runner->savedRoomStates = nullptr;

    // Free struct instances (created via @@NewGMLObject@@)
    repeat(arrlen(runner->structInstances), i) {
        hmdel(runner->instancesToId, runner->structInstances[i]->instanceId);
        Instance_free(runner->structInstances[i]);
    }
    arrfree(runner->structInstances);
    runner->structInstances = nullptr;

    hmfree(runner->instancesToId);
    runner->instancesToId = nullptr;
    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;
    freeRuntimeLayersArray(&runner->runtimeLayers);
    shfree(runner->disabledObjects);
    runner->disabledObjects = nullptr;

    // Free ds_map pool
    repeat((int32_t) arrlen(runner->dsMapPool), i) {
        DsMapEntry* map = runner->dsMapPool[i];
        if (map != nullptr) {
            repeat(shlen(map), j) {
                free(map[j].key);
                RValue_free(&map[j].value);
            }
            shfree(map);
        }
    }
    arrfree(runner->dsMapPool);
    runner->dsMapPool = nullptr;

    // Free ds_list pool
    repeat((int32_t) arrlen(runner->dsListPool), i) {
        DsList* list = &runner->dsListPool[i];
        repeat(arrlen(list->items), j) {
            RValue_free(&list->items[j]);
        }
        arrfree(list->items);
    }
    arrfree(runner->dsListPool);
    runner->dsListPool = nullptr;

    // Free mp_grid pool
    repeat((int32_t) arrlen(runner->mpGridPool), i) {
        free(runner->mpGridPool[i].cells);
    }
    arrfree(runner->mpGridPool);
    runner->mpGridPool = nullptr;

    // Free INI state
    if (runner->currentIni != nullptr) {
        Ini_free(runner->currentIni);
        runner->currentIni = nullptr;
    }
    free(runner->currentIniPath);
    runner->currentIniPath = nullptr;
    if (runner->cachedIni != nullptr) {
        Ini_free(runner->cachedIni);
        runner->cachedIni = nullptr;
    }
    free(runner->cachedIniPath);
    runner->cachedIniPath = nullptr;

    // Free open text files
    repeat(MAX_OPEN_TEXT_FILES, i) {
        OpenTextFile* file = &runner->openTextFiles[i];
        if (file->isOpen) {
            free(file->content);
            free(file->writeBuffer);
            free(file->filePath);
            *file = (OpenTextFile) {0};
        }
    }
}

// ===[ Public API ]===

void Runner_reset(Runner* runner) {
    // This actually sets the default runner values, used for initialization and restarting
    cleanupState(runner);

    // Reset VM state
    VM_reset(runner->vmContext);

    runner->pendingRoom = -1;
    runner->gameStartFired = false;
    runner->currentRoomIndex = -1;
    runner->currentRoomOrderPosition = -1;
    runner->nextInstanceId = runner->dataWin->gen8.lastObj + 1;
    runner->savedRoomStates = safeCalloc(runner->dataWin->room.count, sizeof(SavedRoomState));
    runner->nextLayerId = 1;
    runner->audioSystem->vtable->stopAll(runner->audioSystem);

    // Create the instance used for "self" in GLOB scripts
    Instance_free(runner->globalScopeInstance);
    runner->globalScopeInstance = Instance_create(0, -1, 0, 0);

    // Reset builtin function state
    runner->mpPotMaxrot = 30.0;
    runner->mpPotStep = 10.0;
    runner->mpPotAhead = 3.0;
    runner->mpPotOnSpot = true;
    runner->lastMusicInstance = -1;
}

Runner* Runner_create(DataWin* dataWin, VMContext* vm, Renderer* renderer, FileSystem* fileSystem, AudioSystem* audioSystem) {
    requireNotNull(dataWin);
    requireNotNull(vm);
    requireNotNull(renderer);
    requireNotNull(fileSystem);
    requireNotNull(audioSystem);

    Runner* runner = safeCalloc(1, sizeof(Runner));
    runner->dataWin = dataWin;
    runner->vmContext = vm;
    runner->renderer = renderer;
    runner->fileSystem = fileSystem;
    runner->audioSystem = audioSystem;
    runner->frameCount = 0;
    runner->osType = OS_WINDOWS;
    runner->keyboard = RunnerKeyboard_create();

    Runner_reset(runner);

    // Link runner to VM context
    vm->runner = (struct Runner*) runner;

    renderer->vtable->init(renderer, dataWin);
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);

    NativeScripts_init(vm, runner);

    return runner;
}

static inline void dispatchInstanceCreationEvents(Runner* runner, Instance* inst) {
    inst->createEventFired = true;
    Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
    Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
}

Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    dispatchInstanceCreationEvents(runner, inst);
    return inst;
}

// Same as Runner_createInstance, but sets depth BEFORE firing Create events so scripts like scr_depth can override.
Instance* Runner_createInstanceWithDepth(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex, int32_t depth) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    inst->depth = depth;
    dispatchInstanceCreationEvents(runner, inst);
    return inst;
}

Instance* Runner_copyInstance(Runner* runner, Instance* source, bool performEvent) {
    requireNotNull(source);
    if (isObjectDisabled(runner, source->objectIndex)) return nullptr;

    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, source->objectIndex, source->x, source->y);
    Instance_copyFields(inst, source);
    inst->createEventFired = true;
    if (performEvent) {
        Runner_executeEvent(runner, inst, EVENT_PRECREATE, 0);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    }
    return inst;
}

void Runner_destroyInstance(MAYBE_UNUSED Runner* runner, Instance* inst) {
    GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
    Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    // A destroyed instance must ALWAYS be not active
    // If a destroyed instance is active, then well, something went VERY wrong
    inst->active = false;
    inst->destroyed = true;

#ifdef ENABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) destroyed\n", gameObject->name, inst->instanceId);
    }
#endif
}

RuntimeLayer* Runner_findRuntimeLayerById(Runner* runner, int32_t id) {
    size_t count = arrlenu(runner->runtimeLayers);
    repeat(count, i) {
        if ((int32_t) runner->runtimeLayers[i].id == id)
            return &runner->runtimeLayers[i];
    }
    return nullptr;
}

RoomLayer* Runner_findRoomLayerById(Runner* runner, int32_t id) {
    if (runner->currentRoom == nullptr) return nullptr;
    repeat(runner->currentRoom->layerCount, i) {
        if ((int32_t) runner->currentRoom->layers[i].id == id) return &runner->currentRoom->layers[i];
    }
    return nullptr;
}

RuntimeLayerElement* Runner_findLayerElementById(Runner* runner, int32_t elementId, RuntimeLayer** outLayer) {
    size_t layerCount = arrlenu(runner->runtimeLayers);
    repeat(layerCount, i) {
        RuntimeLayer* runtimeLayer = &runner->runtimeLayers[i];
        size_t elementCount = arrlenu(runtimeLayer->elements);
        repeat(elementCount, j) {
            if ((int32_t) runtimeLayer->elements[j].id == elementId) {
                if (outLayer != nullptr)
                    *outLayer = runtimeLayer;

                return &runtimeLayer->elements[j];
            }
        }
    }
    if (outLayer != nullptr) *outLayer = nullptr;
    return nullptr;
}

uint32_t Runner_getNextLayerId(Runner* runner) {
    return runner->nextLayerId++;
}

void Runner_cleanupDestroyedInstances(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->instances);
    int32_t writeIdx = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->destroyed) {
            runner->instances[writeIdx++] = inst;
        } else {
            hmdel(runner->instancesToId, inst->instanceId);
            Instance_free(inst);
        }
    }
    arrsetlen(runner->instances, writeIdx);
}

void Runner_initFirstRoom(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    require(dataWin->gen8.roomOrderCount > 0);

    int32_t firstRoomIndex = dataWin->gen8.roomOrder[0];

    // Run global init scripts with the global scope instance as "self"
    // In GMS 2.3+ (BC17), GLOB scripts store function declarations on "self" via Pop.v.v
    runner->vmContext->currentInstance = runner->globalScopeInstance;
    repeat(dataWin->glob.count, i) {
        int32_t codeId = dataWin->glob.codeIds[i];
        if (codeId >= 0 && dataWin->code.count > (uint32_t) codeId) {
            fprintf(stderr, "Runner: Executing global init script: %s\n", dataWin->code.entries[codeId].name);
            RValue result = VM_executeCode(runner->vmContext, codeId);
            RValue_free(&result);
        }
    }
    runner->vmContext->currentInstance = nullptr;

    // Initialize the first room
    initRoom(runner, firstRoomIndex);

    // Fire Game Start for all instances
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_GAME_START);
    runner->gameStartFired = true;

    // Fire Room Start for all instances
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
}

// ===[ Collision Event Dispatch ]===

static void executeCollisionEvent(Runner* runner, Instance* self, Instance* other, int32_t targetObjectIndex) {
    VMContext* vm = runner->vmContext;

    // Save event context
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;
    struct Instance* savedOtherInstance = vm->otherInstance;

    // Set collision event context
    vm->currentEventType = EVENT_COLLISION;
    vm->currentEventSubtype = targetObjectIndex;
    vm->otherInstance = other;

    int32_t ownerObjectIndex = -1;
    int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, self->objectIndex, EVENT_COLLISION, targetObjectIndex, &ownerObjectIndex);

    vm->currentEventObjectIndex = ownerObjectIndex;

#ifdef ENABLE_VM_TRACING
    if (codeId >= 0 && shlen(vm->eventsToBeTraced) != -1) {
        const char* selfName = runner->dataWin->objt.objects[self->objectIndex].name;
        const char* targetName = runner->dataWin->objt.objects[targetObjectIndex].name;
        bool shouldTrace = shgeti(vm->eventsToBeTraced, "*") != -1 || shgeti(vm->eventsToBeTraced, "Collision") != -1 || shgeti(vm->eventsToBeTraced, selfName) != -1;
        if (shouldTrace) {
            fprintf(stderr, "Runner: [%s] Collision with %s (instanceId=%d, otherId=%d)\n", selfName, targetName, self->instanceId, other->instanceId);
        }
    }
#endif

    executeCode(runner, self, codeId);

    // Restore event context
    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
    vm->otherInstance = savedOtherInstance;
}

typedef struct { int32_t key; int32_t* value; } ObjInstIndex;
void Runner_ensureBBoxCache(Runner* runner) {
    if (runner->bboxCacheFrame == runner->frameCount) return;
    runner->bboxCacheFrame = runner->frameCount;
    int32_t count = (int32_t) arrlen(runner->instances);

    if (count > runner->bboxCacheCount) {
        runner->bboxCache = realloc(runner->bboxCache, count * sizeof(InstanceBBox));
        runner->bboxCacheCount = count;
    }
    DataWin* dw = runner->dataWin;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        inst->indexInRunner = i;
        if (inst->active)
            runner->bboxCache[i] = Collision_computeBBox(dw, inst);
        else
            runner->bboxCache[i].valid = false;
    }
}

static void dispatchCollisionEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    int32_t count = (int32_t) arrlen(runner->instances);


    runner->bboxCacheFrame = -1;
    Runner_ensureBBoxCache(runner);
    InstanceBBox* bboxCache = runner->bboxCache;


    ensureInstancesByObj(runner);





    repeat(count, i) {
        Instance* self = runner->instances[i];
        if (!self->active) continue;

        int32_t currentObj = self->objectIndex;
        int depth = 0;
        while (currentObj >= 0 && dataWin->objt.count > (uint32_t) currentObj && 32 > depth) {
            GameObject* obj = &dataWin->objt.objects[currentObj];

            if (OBJT_EVENT_TYPE_COUNT > EVENT_COLLISION) {
                ObjectEventList* eventList = &obj->eventLists[EVENT_COLLISION];
                repeat(eventList->eventCount, evtIdx) {
                    ObjectEvent* evt = &eventList->events[evtIdx];
                    int32_t targetObjIndex = (int32_t) evt->eventSubtype;

                    if (evt->actionCount == 0 || 0 > evt->actions[0].codeId) continue;

                    InstanceBBox bboxSelf = bboxCache[i];
                    if (!bboxSelf.valid) continue;


                    if (targetObjIndex < 0 || targetObjIndex >= runner->instancesByObjMax) continue;
                    Instance** targetList = runner->instancesByObjInclParent[targetObjIndex];
                    int32_t targetCount = (int32_t)arrlen(targetList);

                    for (int32_t tj = 0; tj < targetCount; tj++) {
                        Instance* other = targetList[tj];
                        if (other == self) continue;
                        if (!other->active) continue;


                        int32_t j = other->indexInRunner;
                        if (j < 0 || j >= count || runner->instances[j] != other) continue;
                        if (!bboxCache[j].valid) continue;

                        InstanceBBox bboxOther = bboxCache[j];

                        if (bboxSelf.left >= bboxOther.right || bboxOther.left >= bboxSelf.right ||
                            bboxSelf.top >= bboxOther.bottom || bboxOther.top >= bboxSelf.bottom) continue;

                        Sprite* sprSelf = Collision_getSprite(dataWin, self);
                        Sprite* sprOther = Collision_getSprite(dataWin, other);
                        bool needsPrecise = (sprSelf != nullptr && sprSelf->sepMasks == 1) || (sprOther != nullptr && sprOther->sepMasks == 1);

                        if (needsPrecise) {
                            if (!Collision_instancesOverlapPrecise(dataWin, self, other, bboxSelf, bboxOther)) continue;
                        }

                        if (self->solid || other->solid) {
                            self->x = self->xprevious;
                            self->y = self->yprevious;
                            other->x = other->xprevious;
                            other->y = other->yprevious;
                        }

                        executeCollisionEvent(runner, self, other, targetObjIndex);


                        bboxCache[i] = Collision_computeBBox(dataWin, self);
                        bboxCache[j] = Collision_computeBBox(dataWin, other);
                        bboxSelf = bboxCache[i];
                    }
                }
            }

            currentObj = obj->parentId;
            depth++;
        }
    }
}

// ===[ View Following + Clamping ]===
// Single-axis follow with border-based scrolling, room clamping, and speed limit.
static int32_t followAxis(int32_t viewPos, int32_t viewSize, int32_t targetPos, uint32_t border, int32_t speed, int32_t roomSize) {
    int32_t pos = viewPos;

    // Border-based scrolling
    if (2 * (int32_t) border >= viewSize) {
        pos = targetPos - viewSize / 2;
    } else if (targetPos - (int32_t) border < viewPos) {
        pos = targetPos - (int32_t) border;
    } else if (targetPos + (int32_t) border > viewPos + viewSize) {
        pos = targetPos + (int32_t) border - viewSize;
    }

    // Clamp to room bounds
    if (0 > pos) pos = 0;
    if (pos + viewSize > roomSize) pos = roomSize - viewSize;

    // Speed limit
    if (speed >= 0) {
        if (pos < viewPos && viewPos - pos > speed) pos = viewPos - speed;
        if (pos > viewPos && pos - viewPos > speed) pos = viewPos + speed;
    }

    return pos;
}

static void updateViews(Runner* runner) {
    Room* room = runner->currentRoom;
    if (!(room->flags & 1)) return;

    repeat(MAX_VIEWS, vi) {
        RuntimeView* view = &runner->views[vi];
        if (!view->enabled || 0 > view->objectId) continue;

        // Find first active instance of the target object
        Instance* target = nullptr;
        int32_t count = (int32_t) arrlen(runner->instances);
        repeat(count, i) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(runner->dataWin, inst->objectIndex, view->objectId)) { target = inst; break; };
        }

        if (target != nullptr) {
            int32_t ix = (int32_t) GMLReal_floor(target->x);
            int32_t iy = (int32_t) GMLReal_floor(target->y);
            view->viewX = followAxis(view->viewX, view->viewWidth, ix, view->borderX, view->speedX, (int32_t) room->width);
            view->viewY = followAxis(view->viewY, view->viewHeight, iy, view->borderY, view->speedY, (int32_t) room->height);
        }
    }
}

static void dispatchOutsideRoomEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    int32_t roomWidth = (int32_t) runner->currentRoom->width;
    int32_t roomHeight = (int32_t) runner->currentRoom->height;
    int32_t count = (int32_t) arrlen(runner->instances);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        // Early-out: skip instances whose object has no Outside Room event
        if (0 > findEventCodeIdAndOwner(dataWin, inst->objectIndex, EVENT_OTHER, OTHER_OUTSIDE_ROOM, nullptr)) continue;

        // Compute bounding box
        bool outside;
        InstanceBBox bbox = Collision_computeBBox(dataWin, inst);
        if (bbox.valid) {
            outside = (0 > bbox.right || bbox.left > roomWidth || 0 > bbox.bottom || bbox.top > roomHeight);
        } else {
            // No sprite/mask: use raw position as a point
            outside = (0 > inst->x || inst->x > roomWidth || 0 > inst->y || inst->y > roomHeight);
        }

        // Fire event only on inside-to-outside transition (edge-triggered)
        if (outside && !inst->outsideRoom) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_OUTSIDE_ROOM);
            if (runner->pendingRoom >= 0) break;
        }

        inst->outsideRoom = outside;
    }
}

// ===[ Path Adaptation ]===
// Advances path position and updates instance x/y (HTML5: yyInstance.js Adapt_Path, lines 2755-2881)
// Returns true if end of path was reached (and pathSpeed != 0), to fire OTHER_END_OF_PATH event.
static bool adaptPath(Runner* runner, Instance* inst) {
    if (0 > inst->pathIndex) return false;

    DataWin* dataWin = runner->dataWin;
    if ((uint32_t) inst->pathIndex >= dataWin->path.count) return false;

    GamePath* path = &dataWin->path.paths[inst->pathIndex];
    if (0.0 >= path->length) return false;

    bool atPathEnd = false;

    GMLReal orient = inst->pathOrientation * M_PI / 180.0;

    // Get current position's speed factor
    PathPositionResult cur = GamePath_getPosition(path, inst->pathPosition);
    GMLReal sp = cur.speed / (100.0 * inst->pathScale);

    // Advance position (compute in higher precision, truncate to float on store - matches native runner)
    inst->pathPosition = (float) (inst->pathPosition + inst->pathSpeed * sp / path->length);

    // Handle end actions if position out of [0,1]
    PathPositionResult pos0 = GamePath_getPosition(path, 0.0);
    if (inst->pathPosition >= 1.0f || 0.0f >= inst->pathPosition) {
        atPathEnd = (inst->pathSpeed == 0.0f) ? false : true;

        switch (inst->pathEndAction) {
            // stop moving
            case 0: {
                if (inst->pathSpeed >= 0.0f) {
                    if (inst->pathSpeed != 0.0f) {
                        inst->pathPosition = 1.0f;
                        inst->pathIndex = -1;
                    }
                } else {
                    inst->pathPosition = 0.0f;
                    inst->pathIndex = -1;
                }
                break;
            }
            // continue from start position (restart)
            case 1: {
                if (0.0f > inst->pathPosition) {
                    inst->pathPosition += 1.0f;
                } else {
                    inst->pathPosition -= 1.0f;
                }
                break;
            }
            // continue from current position
            case 2: {
                PathPositionResult pos1 = GamePath_getPosition(path, 1.0);
                GMLReal xx = pos1.x - pos0.x;
                GMLReal yy = pos1.y - pos0.y;
                GMLReal xdif = inst->pathScale * (xx * GMLReal_cos(orient) + yy * GMLReal_sin(orient));
                GMLReal ydif = inst->pathScale * (yy * GMLReal_cos(orient) - xx * GMLReal_sin(orient));

                if (0.0f > inst->pathPosition) {
                    inst->pathXStart -= (float) xdif;
                    inst->pathYStart -= (float) ydif;
                    inst->pathPosition += 1.0f;
                } else {
                    inst->pathXStart += (float) xdif;
                    inst->pathYStart += (float) ydif;
                    inst->pathPosition -= 1.0f;
                }
                break;
            }
            // reverse
            case 3: {
                if (0.0f > inst->pathPosition) {
                    inst->pathPosition = -inst->pathPosition;
                    inst->pathSpeed = (float) GMLReal_fabs(inst->pathSpeed);
                } else {
                    inst->pathPosition = 2.0f - inst->pathPosition;
                    inst->pathSpeed = (float) -GMLReal_fabs(inst->pathSpeed);
                }
                break;
            }
            // default: stop
            default: {
                inst->pathPosition = 1.0f;
                inst->pathIndex = -1;
                break;
            }
        }
    }

    // Find the new position in the room
    PathPositionResult newPos = GamePath_getPosition(path, inst->pathPosition);
    GMLReal xx = newPos.x - pos0.x; // relative
    GMLReal yy = newPos.y - pos0.y;

    GMLReal newx = inst->pathXStart + inst->pathScale * (xx * GMLReal_cos(orient) + yy * GMLReal_sin(orient));
    GMLReal newy = inst->pathYStart + inst->pathScale * (yy * GMLReal_cos(orient) - xx * GMLReal_sin(orient));

    // Trick to set the direction: set hspeed/vspeed to delta, which updates direction
    inst->hspeed = (float) (newx - inst->x);
    inst->vspeed = (float) (newy - inst->y);
    Instance_computeSpeedFromComponents(inst);

    // Normal speed should not be used
    inst->speed = 0.0f;
    inst->hspeed = 0.0f;
    inst->vspeed = 0.0f;

    // Set the new position
    inst->x = (float) newx;
    inst->y = (float) newy;

    return atPathEnd;
}

static void persistRoomState(Runner* runner, int32_t roomIndex) {
    SavedRoomState* state = &runner->savedRoomStates[roomIndex];

    // Free any previously saved instances (from an earlier visit)
    int32_t prevSavedCount = (int32_t) arrlen(state->instances);
    repeat(prevSavedCount, i) {
        hmdel(runner->instancesToId, state->instances[i]->instanceId);
        Instance_free(state->instances[i]);
    }
    arrfree(state->instances);
    state->instances = nullptr;
    hmfree(state->tileLayerMap);
    state->tileLayerMap = nullptr;
    freeRuntimeLayersArray(&state->runtimeLayers);

    // Separate persistent instances (travel with player) from room instances (saved)
    Instance** keptInstances = nullptr;
    int32_t count = (int32_t) arrlen(runner->instances);
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (inst->persistent) {
            arrput(keptInstances, inst);
        } else if (inst->active) {
            arrput(state->instances, inst);
        } else {
            hmdel(runner->instancesToId, inst->instanceId);
            Instance_free(inst);
        }
    }
    arrfree(runner->instances);
    runner->instances = keptInstances;

    // Save room visual state
    memcpy(state->backgrounds, runner->backgrounds, sizeof(runner->backgrounds));
    memcpy(state->views, runner->views, sizeof(runner->views));
    state->backgroundColor = runner->backgroundColor;
    state->drawBackgroundColor = runner->drawBackgroundColor;

    // Transfer tile layer map ownership to saved state
    state->tileLayerMap = runner->tileLayerMap;
    runner->tileLayerMap = nullptr;

    // Transfer runtime layer ownership to saved state
    state->runtimeLayers = runner->runtimeLayers;
    runner->runtimeLayers = nullptr;

    state->initialized = true;
}

void Runner_step(Runner* runner) {
    // Save xprevious/yprevious and path_positionprevious for all active instances
    int32_t prevCount = (int32_t) arrlen(runner->instances);
    repeat(prevCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->active) {
            inst->xprevious = inst->x;
            inst->yprevious = inst->y;
            inst->pathPositionPrevious = inst->pathPosition;
        }
    }

    // Advance image_index by image_speed for all active instances
    int32_t animCount = (int32_t) arrlen(runner->instances);
    repeat(animCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (0 > inst->spriteIndex) continue;

        inst->imageIndex += inst->imageSpeed;

        // Wrap image_index (matches HTML5 runner: manual subtract/add instead of using fmod)
        Sprite* sprite = &runner->dataWin->sprt.sprites[inst->spriteIndex];
        float frameCount = (float) sprite->textureCount;
        if (inst->imageIndex >= frameCount) {
            inst->imageIndex -= frameCount;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
        } else if (0.0f > inst->imageIndex) {
            inst->imageIndex += frameCount;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
        }
    }

    // Scroll backgrounds
    Runner_scrollBackgrounds(runner);

    // Advance GMS2 layer parallax (hspeed/vspeed per frame)
    size_t layerCount = arrlenu(runner->runtimeLayers);
    repeat(layerCount, i) {
        RuntimeLayer* rl = &runner->runtimeLayers[i];
        rl->xOffset += rl->hSpeed;
        rl->yOffset += rl->vSpeed;
    }

    // Execute Begin Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_BEGIN);

    // Dispatch keyboard events
    RunnerKeyboardState* kb = runner->keyboard;
    for (int32_t key = 0; GML_KEY_COUNT > key; key++) {
        if (kb->keyPressed[key]) {
            Runner_executeEventForAll(runner, EVENT_KEYPRESS, key);
        }
    }
    for (int32_t key = 0; GML_KEY_COUNT > key; key++) {
        if (kb->keyDown[key]) {
            Runner_executeEventForAll(runner, EVENT_KEYBOARD, key);
        }
    }
    for (int32_t key = 0; GML_KEY_COUNT > key; key++) {
        if (kb->keyReleased[key]) {
            Runner_executeEventForAll(runner, EVENT_KEYRELEASE, key);
        }
    }

    // Process alarms for all instances
    int32_t alarmCount = (int32_t) arrlen(runner->instances);
    repeat(alarmCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        GameObject* object = &runner->dataWin->objt.objects[inst->objectIndex];

        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] > 0) {
#ifdef ENABLE_VM_TRACING
                if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                    fprintf(stderr, "VM: [%s] Ticking down Alarm[%d] (instanceId=%d), current tick is %d\n", object->name, alarmIdx, inst->instanceId, inst->alarm[alarmIdx]);
                }
#endif

                inst->alarm[alarmIdx]--;
                if (inst->alarm[alarmIdx] == 0) {
                    inst->alarm[alarmIdx] = -1;

#ifdef ENABLE_VM_TRACING
                    if (shgeti(runner->vmContext->alarmsToBeTraced, "*") != -1 || shgeti(runner->vmContext->alarmsToBeTraced, object->name) != -1) {
                        fprintf(stderr, "VM: [%s] Firing Alarm[%d] (instanceId=%d)\n", object->name, alarmIdx, inst->instanceId);
                    }
#endif

                    Runner_executeEvent(runner, inst, EVENT_ALARM, alarmIdx);
                }
            }
        }
    }

    // Execute Normal Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_NORMAL);

    // Apply motion: friction, gravity, then x += hspeed, y += vspeed
    int32_t motionCount = (int32_t) arrlen(runner->instances);
    repeat(motionCount, mi) {
        Instance* inst = runner->instances[mi];
        if (!inst->active) continue;

        // Friction: reduce speed toward zero (HTML5: AdaptSpeed)
        if (inst->friction != 0.0f) {
            float ns = (inst->speed > 0.0f) ? inst->speed - inst->friction : inst->speed + inst->friction;
            if ((inst->speed > 0.0f && ns < 0.0f) || (inst->speed < 0.0f && ns > 0.0f)) {
                inst->speed = 0.0f;
            } else if (inst->speed != 0.0f) {
                inst->speed = ns;
            }
            Instance_computeComponentsFromSpeed(inst);
        }

        // Gravity: add velocity in gravity_direction (HTML5: AddTo_Speed)
        if (inst->gravity != 0.0f) {
            GMLReal gravDirRad = inst->gravityDirection * (M_PI / 180.0);
            inst->hspeed += (float) (inst->gravity * clampFloat(GMLReal_cos(gravDirRad)));
            inst->vspeed -= (float) (inst->gravity * clampFloat(GMLReal_sin(gravDirRad)));
            Instance_computeSpeedFromComponents(inst);
        }

        // Path adaptation (HTML5: Adapt_Path, runs after friction/gravity, before x+=hspeed)
        if (adaptPath(runner, inst)) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_END_OF_PATH);
        }

        // Apply movement
        if (inst->hspeed != 0.0f || inst->vspeed != 0.0f) {
            inst->x += inst->hspeed;
            inst->y += inst->vspeed;
        }
    }

    // Dispatch outside room events
    dispatchOutsideRoomEvents(runner);

    // Dispatch collision events
    dispatchCollisionEvents(runner);

    // Execute End Step for all instances
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_END);

    // Update view following
    updateViews(runner);

    // Handle game restart
    if (runner->pendingRoom == ROOM_RESTARTGAME) {
        // See you soon!
        // Free the currently-loaded non-eager room before reset so lazyLoadRooms stays steady-state.
        if (runner->dataWin->lazyLoadRooms && runner->currentRoom != nullptr && !runner->currentRoom->eagerlyLoaded) {
            DataWin_freeRoomPayload(runner->currentRoom);
        }
        Runner_reset(runner);
        Runner_initFirstRoom(runner);
        runner->frameCount++;
        return;
    }

    // Handle room transition
    if (runner->pendingRoom >= 0) {
        int32_t oldRoomIndex = runner->currentRoomIndex;
        Room* oldRoom = runner->currentRoom;
        const char* oldRoomName = oldRoom->name;

        // Fire Room End for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_END);

        int32_t newRoomIndex = runner->pendingRoom;
        runner->pendingRoom = -1;
        require(runner->dataWin->room.count > (uint32_t) newRoomIndex);
        const char* newRoomName = runner->dataWin->room.rooms[newRoomIndex].name;

        fprintf(stderr, "Room changed: %s (room %d) -> %s (room %d)\n", oldRoomName, oldRoomIndex, newRoomName, newRoomIndex);

        // If the old room is persistent, save its instance and visual state
        if (oldRoom->persistent) {
            persistRoomState(runner, oldRoomIndex);
        }

        // Free the outgoing room's payload under lazyLoadRooms, unless it's eagerly pinned or we're restarting the same room (initRoom would just re-load it).
        if (runner->dataWin->lazyLoadRooms && !oldRoom->eagerlyLoaded && newRoomIndex != oldRoomIndex) {
            DataWin_freeRoomPayload(oldRoom);
        }

        // Load new room
        initRoom(runner, newRoomIndex);

        // Fire Room Start for all instances
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
    }

    Runner_cleanupDestroyedInstances(runner);

    runner->frameCount++;
}

// ===[ State Dump ]===

void Runner_dumpState(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    printf("=== Frame %d State Dump ===\n", runner->frameCount);
    printf("Room: %s (index %d)\n", runner->currentRoom->name, runner->currentRoomIndex);
    printf("Instance count: %d\n", instanceCount);

    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        GameObject* gameObject = nullptr;
        const char* objName = "<unknown>";
        if (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) {
            gameObject = &dataWin->objt.objects[inst->objectIndex];
            objName = gameObject->name;
        }

        const char* spriteName = "<none>";
        if (inst->spriteIndex >= 0 && dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
            spriteName = dataWin->sprt.sprites[inst->spriteIndex].name;
        }

        const char* parentName = "<none>";
        if (gameObject != nullptr && gameObject->parentId >= 0 && dataWin->objt.count > (uint32_t) gameObject->parentId) {
            parentName = dataWin->objt.objects[gameObject->parentId].name;
        }

        printf("\n--- Instance #%d (%s, objectIndex=%d) ---\n", inst->instanceId, objName, inst->objectIndex);
        printf("  Position: (%g, %g)\n", inst->x, inst->y);
        printf("  Depth: %d\n", inst->depth);
        printf("  Sprite: %s (index %d), imageIndex=%g, imageSpeed=%g\n", spriteName, inst->spriteIndex, inst->imageIndex, inst->imageSpeed);
        printf("  Scale: (%g, %g), Angle: %g, Alpha: %g, Blend: 0x%06X\n", inst->imageXscale, inst->imageYscale, inst->imageAngle, inst->imageAlpha, inst->imageBlend);
        printf("  Visible: %s, Active: %s, Solid: %s, Persistent: %s\n", inst->visible ? "true" : "false", inst->active ? "true" : "false", inst->solid ? "true" : "false", inst->persistent ? "true" : "false");
        printf("  Parent: %s (parentId=%d)\n", parentName, gameObject != nullptr ? gameObject->parentId : -1);

        // Active alarms
        bool hasAlarm = false;
        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] >= 0) {
                if (!hasAlarm) { printf("  Alarms:"); hasAlarm = true; }
                printf(" [%d]=%d", alarmIdx, inst->alarm[alarmIdx]);
            }
        }
        if (hasAlarm) printf("\n");

        // Self variables
        bool hasSelfVars = false;
        bool hasSelfArrays = false;
        repeat(hmlen(inst->selfVars), svIdx) {
            int32_t varID = inst->selfVars[svIdx].key;
            RValue val = inst->selfVars[svIdx].value;
            if (val.type == RVALUE_UNDEFINED) continue;

            const char* varName = "?";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->instanceType == INSTANCE_SELF && var->varID == varID) {
                    varName = var->name;
                    break;
                }
            }

            if (val.type == RVALUE_ARRAY && val.array != nullptr) {
                if (!hasSelfArrays) { printf("  Self Arrays:\n"); hasSelfArrays = true; }
                repeat(GMLArray_length1D(val.array), ai) {
                    RValue* cell = GMLArray_slot(val.array, ai);
                    if (cell == nullptr || cell->type == RVALUE_UNDEFINED) continue;
                    char* innerStr = RValue_toStringFancy(*cell);
                    printf("    %s[%d] = %s\n", varName, (int) ai, innerStr);
                    free(innerStr);
                }
            } else {
                if (!hasSelfVars) { printf("  Self Variables:\n"); hasSelfVars = true; }
                char* valStr = RValue_toStringFancy(val);
                printf("    %s = %s\n", varName, valStr);
                free(valStr);
            }
        }
    }

    // Global variables (non-array)
    printf("\n=== Global Variables ===\n");
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* var = &dataWin->vari.variables[varIdx];
        if (var->instanceType != INSTANCE_GLOBAL || var->varID < 0) continue;
        if ((uint32_t) var->varID >= vm->globalVarCount) continue;
        RValue val = vm->globalVars[var->varID];
        if (val.type == RVALUE_UNDEFINED) continue;

        char* valStr = RValue_toStringFancy(val);
        printf("  %s = %s\n", var->name, valStr);
        free(valStr);
    }

    // Global arrays: scan globalVars slots for RVALUE_ARRAY entries
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* var = &dataWin->vari.variables[varIdx];
        if (var->instanceType != INSTANCE_GLOBAL || var->varID < 0) continue;
        if ((uint32_t) var->varID >= vm->globalVarCount) continue;
        RValue val = vm->globalVars[var->varID];
        if (val.type != RVALUE_ARRAY || val.array == nullptr) continue;
        repeat(GMLArray_length1D(val.array), ai) {
            RValue* cell = GMLArray_slot(val.array, ai);
            if (cell == nullptr || cell->type == RVALUE_UNDEFINED) continue;
            char* innerStr = RValue_toStringFancy(*cell);
            printf("  %s[%d] = %s\n", var->name, (int) ai, innerStr);
            free(innerStr);
        }
    }

    int64_t globalArrayLen = hmlen(vm->globalArrayMap);
    if (globalArrayLen > 0) {
        repeat(globalArrayLen, arrIdx) {
            int64_t key = vm->globalArrayMap[arrIdx].key;
            RValue val = vm->globalArrayMap[arrIdx].value;
            int32_t varID = (int32_t) (key >> 32);
            int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

            const char* varName = "<unknown>";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->varID == varID && var->instanceType == INSTANCE_GLOBAL) {
                    varName = var->name;
                    break;
                }
            }

            char* valStr = RValue_toStringFancy(val);
            printf("  %s[%d] = %s\n", varName, arrayIndex, valStr);
            free(valStr);
        }
    }
    printf("\n=== End Frame %d State Dump ===\n", runner->frameCount);
}

// ===[ JSON State Dump ]===

static void writeRValueJson(JsonWriter* w, RValue val) {
    switch (val.type) {
        case RVALUE_REAL:
            JsonWriter_double(w, val.real);
            break;
        case RVALUE_INT32:
            JsonWriter_int(w, val.int32);
            break;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            JsonWriter_int(w, val.int64);
            break;
#endif
        case RVALUE_STRING:
            JsonWriter_string(w, val.string);
            break;
        case RVALUE_BOOL:
            JsonWriter_bool(w, val.int32 != 0);
            break;
        case RVALUE_UNDEFINED:
            JsonWriter_null(w);
            break;
        case RVALUE_ARRAY: {
            // Render arrays as a JSON array. Skips RVALUE_UNDEFINED entries (they read as 0/null anyway).
            JsonWriter_beginArray(w);
            if (val.array != nullptr) {
                repeat(GMLArray_length1D(val.array), ai) {
                    RValue* cell = GMLArray_slot(val.array, ai);
                    writeRValueJson(w, cell != nullptr ? *cell : (RValue){ .type = RVALUE_UNDEFINED });
                }
            }
            JsonWriter_endArray(w);
            break;
        }
#if IS_BC17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<method:%d>", val.method->codeIndex);
            JsonWriter_string(w, buf);
            break;
        }
#endif
    }
}

char* Runner_dumpStateJson(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    JsonWriter w = JsonWriter_create();

    JsonWriter_beginObject(&w);

    JsonWriter_propertyInt(&w, "frame", runner->frameCount);

    // Room info
    JsonWriter_key(&w, "room");
    JsonWriter_beginObject(&w);
    JsonWriter_propertyString(&w, "name", runner->currentRoom->name);
    JsonWriter_propertyInt(&w, "index", runner->currentRoomIndex);
    JsonWriter_endObject(&w);

    // Instances
    JsonWriter_key(&w, "instances");
    JsonWriter_beginArray(&w);

    repeat(instanceCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        const char* objName = (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) ? dataWin->objt.objects[inst->objectIndex].name : nullptr;

        const char* spriteName = nullptr;
        if (inst->spriteIndex >= 0 && dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
            spriteName = dataWin->sprt.sprites[inst->spriteIndex].name;
        }

        JsonWriter_beginObject(&w);

        JsonWriter_propertyInt(&w, "instanceId", inst->instanceId);
        JsonWriter_propertyString(&w, "objectName", objName);
        JsonWriter_propertyInt(&w, "objectIndex", inst->objectIndex);

        // Parent object
        const char* parentName = nullptr;
        int32_t parentId = -1;
        if (inst->objectIndex >= 0 && dataWin->objt.count > (uint32_t) inst->objectIndex) {
            parentId = dataWin->objt.objects[inst->objectIndex].parentId;
            if (parentId >= 0 && dataWin->objt.count > (uint32_t) parentId) {
                parentName = dataWin->objt.objects[parentId].name;
            }
        }
        JsonWriter_propertyString(&w, "parentObjectName", parentName);
        JsonWriter_propertyInt(&w, "parentObjectIndex", parentId);

        JsonWriter_propertyDouble(&w, "x", inst->x);
        JsonWriter_propertyDouble(&w, "y", inst->y);
        JsonWriter_propertyInt(&w, "depth", inst->depth);

        // Sprite
        JsonWriter_key(&w, "sprite");
        JsonWriter_beginObject(&w);
        JsonWriter_propertyString(&w, "name", spriteName);
        JsonWriter_propertyInt(&w, "index", inst->spriteIndex);
        JsonWriter_propertyDouble(&w, "imageIndex", inst->imageIndex);
        JsonWriter_propertyDouble(&w, "imageSpeed", inst->imageSpeed);
        JsonWriter_endObject(&w);

        // Scale
        JsonWriter_key(&w, "scale");
        JsonWriter_beginObject(&w);
        JsonWriter_propertyDouble(&w, "x", inst->imageXscale);
        JsonWriter_propertyDouble(&w, "y", inst->imageYscale);
        JsonWriter_endObject(&w);

        JsonWriter_propertyDouble(&w, "angle", inst->imageAngle);
        JsonWriter_propertyDouble(&w, "alpha", inst->imageAlpha);
        JsonWriter_propertyInt(&w, "blend", inst->imageBlend);
        JsonWriter_propertyBool(&w, "visible", inst->visible);
        JsonWriter_propertyBool(&w, "active", inst->active);
        JsonWriter_propertyBool(&w, "solid", inst->solid);
        JsonWriter_propertyBool(&w, "persistent", inst->persistent);

        // Alarms
        JsonWriter_key(&w, "alarms");
        JsonWriter_beginObject(&w);
        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] >= 0) {
                char alarmKey[4];
                snprintf(alarmKey, sizeof(alarmKey), "%d", alarmIdx);
                JsonWriter_propertyInt(&w, alarmKey, inst->alarm[alarmIdx]);
            }
        }
        JsonWriter_endObject(&w);

        // Self variables (non-array, sparse hashmap)
        JsonWriter_key(&w, "selfVariables");
        JsonWriter_beginObject(&w);
        repeat(hmlen(inst->selfVars), svIdx) {
            int32_t varID = inst->selfVars[svIdx].key;
            RValue val = inst->selfVars[svIdx].value;
            if (val.type == RVALUE_UNDEFINED) continue;

            // Resolve variable name from VARI chunk
            const char* varName = "?";
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->instanceType == INSTANCE_SELF && var->varID == varID) {
                    varName = var->name;
                    break;
                }
            }

            JsonWriter_key(&w, varName);
            writeRValueJson(&w, val);
        }
        JsonWriter_endObject(&w);
        JsonWriter_endObject(&w);
    }

    JsonWriter_endArray(&w);

    // Tiles
    Room* dumpRoom = runner->currentRoom;
    JsonWriter_key(&w, "tiles");
    JsonWriter_beginArray(&w);
    repeat(dumpRoom->tileCount, tileIdx) {
        RoomTile* tile = &dumpRoom->tiles[tileIdx];
        const char* bgName = (tile->backgroundDefinition >= 0 && dataWin->bgnd.count > (uint32_t) tile->backgroundDefinition) ? dataWin->bgnd.backgrounds[tile->backgroundDefinition].name : nullptr;

        JsonWriter_beginObject(&w);
        JsonWriter_propertyInt(&w, "index", tileIdx);
        JsonWriter_propertyInt(&w, "x", tile->x);
        JsonWriter_propertyInt(&w, "y", tile->y);
        JsonWriter_propertyInt(&w, "backgroundIndex", tile->backgroundDefinition);
        if (bgName != nullptr) {
            JsonWriter_propertyString(&w, "backgroundName", bgName);
        } else {
            JsonWriter_propertyNull(&w, "backgroundName");
        }
        JsonWriter_propertyInt(&w, "sourceX", tile->sourceX);
        JsonWriter_propertyInt(&w, "sourceY", tile->sourceY);
        JsonWriter_propertyInt(&w, "width", tile->width);
        JsonWriter_propertyInt(&w, "height", tile->height);
        JsonWriter_propertyInt(&w, "depth", tile->tileDepth);
        JsonWriter_propertyInt(&w, "instanceID", tile->instanceID);
        JsonWriter_propertyDouble(&w, "scaleX", tile->scaleX);
        JsonWriter_propertyDouble(&w, "scaleY", tile->scaleY);
        JsonWriter_propertyInt(&w, "color", tile->color);

        ptrdiff_t layerIdx = hmgeti(runner->tileLayerMap, tile->tileDepth);
        bool visible = (layerIdx >= 0) ? runner->tileLayerMap[layerIdx].value.visible : true;
        JsonWriter_propertyBool(&w, "visible", visible);
        JsonWriter_endObject(&w);
    }
    JsonWriter_endArray(&w);

    // Global variables (non-array)
    JsonWriter_key(&w, "globalVariables");
    JsonWriter_beginObject(&w);
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* var = &dataWin->vari.variables[varIdx];
        if (var->instanceType != INSTANCE_GLOBAL || var->varID < 0) continue;
        if ((uint32_t) var->varID >= vm->globalVarCount) continue;
        RValue val = vm->globalVars[var->varID];
        if (val.type == RVALUE_UNDEFINED) continue;

        JsonWriter_key(&w, var->name);
        writeRValueJson(&w, val);
    }
    JsonWriter_endObject(&w);
    JsonWriter_endObject(&w);

    char* result = JsonWriter_copyOutput(&w);
    JsonWriter_free(&w);
    return result;
}

void Runner_free(Runner* runner) {
    if (runner == nullptr) return;

    cleanupState(runner);

    RunnerKeyboard_free(runner->keyboard);
    Instance_free(runner->globalScopeInstance);
    free(runner);
}
