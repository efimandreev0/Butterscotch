#include "runner.h"
#include "data_win.h"
#include "instance.h"
#include "native_scripts.h"
#include "renderer.h"
#include "vm.h"
#include "utils.h"
#include "json_writer.h"
#include "collision.h"
#include <time.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef PSP
#include <psprtc.h>
static inline uint64_t _profTickUs(void) { u64 t; sceRtcGetCurrentTick(&t); return (uint64_t)t; }
#else
static inline uint64_t _profTickUs(void) { return (uint64_t)clock() * 1000000 / CLOCKS_PER_SEC; }
#endif

#include "stb_ds.h"






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
                    
                    if (evt->actionCount > 0 && evt->actions[0].codeId >= 0) {
                        if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = currentObj;
                        return evt->actions[0].codeId;
                    }
                    if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
                    return -1;
                }
            }
        }

        
        currentObj = obj->parentId;
        depth++;
    }

    if (outOwnerObjectIndex != nullptr) *outOwnerObjectIndex = -1;
    return -1;
}



static void setVMInstanceContext(VMContext* vm, Instance* instance) {
    vm->currentInstance = instance;
}

static void restoreVMInstanceContext(VMContext* vm, Instance* savedInstance) {
    vm->currentInstance = savedInstance;
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

#ifndef DISABLE_VM_TRACING
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


typedef struct {
    Instance* inst;
    int32_t originalIndex;
} IndexedInstance;


static int compareInstanceByObjectIndex(const void* a, const void* b) {
    const IndexedInstance* ia = (const IndexedInstance*) a;
    const IndexedInstance* ib = (const IndexedInstance*) b;
    
    if (ia->inst->objectIndex < ib->inst->objectIndex) return -1;
    if (ib->inst->objectIndex < ia->inst->objectIndex) return 1;
    
    if (ia->originalIndex < ib->originalIndex) return -1;
    if (ib->originalIndex < ia->originalIndex) return 1;
    return 0;
}


typedef struct {
    int32_t codeId;
    int32_t ownerObjectIndex;
    NativeCodeFunc nativeFunc;  
} ObjEventCache;








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
                uint64_t _stepT0 = recordStep ? _profTickUs() : 0;
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
                if (recordStep) stepCacheRecord(runner, c->codeId, (uint32_t)(_profTickUs() - _stepT0));
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
                uint64_t _stepT0 = recordStep ? _profTickUs() : 0;
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
                if (recordStep) stepCacheRecord(runner, cid, (uint32_t)(_profTickUs() - _stepT0));
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
            
            TexturePageItem* tpag = &dataWin->tpag.items[tpagIndex];
            float xscale = roomW / (float) tpag->boundingWidth;
            float yscale = roomH / (float) tpag->boundingHeight;
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, 0.0f, 0.0f, 0.0f, 0.0f, xscale, yscale, 0.0f, 0xFFFFFF, bg->alpha);
        } else if (bg->tileX || bg->tileY) {
            Renderer_drawBackgroundTiled(runner->renderer, tpagIndex, bg->x, bg->y, bg->tileX, bg->tileY, roomW, roomH, bg->alpha);
        } else {
            
            runner->renderer->vtable->drawSprite(runner->renderer, tpagIndex, bg->x, bg->y, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, bg->alpha);
        }
    }
}



typedef enum { DRAWABLE_TILE, DRAWABLE_INSTANCE, DRAWABLE_LAYER } DrawableType;

typedef struct {
    DrawableType type;
    int32_t depth;
    union {
        Instance* instance;
        int32_t tileIndex; 
        RoomLayer* layer;
    };
} Drawable;

static int compareDrawableDepth(const void* a, const void* b) {
    const Drawable* da = (const Drawable*) a;
    const Drawable* db = (const Drawable*) b;
    
    if (da->depth > db->depth) return -1;
    if (db->depth > da->depth) return 1;
    
    if (da->type < db->type) return -1;
    if (db->type < da->type) return 1;
    
    
    
    
    
    
    if (da->type == DRAWABLE_TILE) {
        if (db->tileIndex > da->tileIndex) return -1;
        if (da->tileIndex > db->tileIndex) return 1;
    } else {
        
        uint32_t idA = da->instance ? da->instance->instanceId : 0;
        uint32_t idB = db->instance ? db->instance->instanceId : 0;
        if (idA < idB) return -1;
        if (idA > idB) return 1;
    }
    return 0;
}

static int compareInstanceDepth(const void* a, const void* b) {
    Instance* instA = *(Instance**) a;
    Instance* instB = *(Instance**) b;
    
    if (instA->depth > instB->depth) return -1;
    if (instB->depth > instA->depth) return 1;
    
    if (instA->instanceId < instB->instanceId) return -1;
    if (instA->instanceId > instB->instanceId) return 1;
    return 0;
}


static int compareLayerDepth(const void* a, const void* b) {
    RoomLayer* instA = *(RoomLayer**) a;
    RoomLayer* instB = *(RoomLayer**) b;
    
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

    
    if(!runner->isGMS2)
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

    if (room->tileCount == 0 && !runner->isGMS2) {
        
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

        if (!runner->isGMS2) {
            
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

    
    inst->spriteIndex = objDef->spriteId;
    inst->visible = objDef->visible;
    inst->solid = objDef->solid;
    inst->persistent = objDef->persistent;
    inst->depth = objDef->depth;
    inst->maskIndex = objDef->textureMaskId;

    hmput(runner->instancesToId, instanceId, inst);
    arrput(runner->instances, inst);
    addInstanceToCache(runner, inst);  

#ifndef DISABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, objDef->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) created at (%f, %f)\n", objDef->name, instanceId, x, y);
    }
#endif

    return inst;
}



static void initRoom(Runner* runner, int32_t roomIndex) {
    // ВЫГРУЖАЕМ СТАРУЮ КОМНАТУ ИЗ ОЗУ
    if (runner->currentRoomIndex >= 0 && runner->currentRoomIndex != roomIndex) {
        DataWin_unloadRoom(runner->dataWin, runner->currentRoomIndex);
    }

    // ЗАГРУЖАЕМ НОВУЮ КОМНАТУ С ФЛЕШКИ
    DataWin_loadRoom(runner->dataWin, roomIndex);

    runner->cachedInstCount = -1;
    DataWin* dataWin = runner->dataWin;
    require(roomIndex >= 0 && dataWin->room.count > (uint32_t) roomIndex);

    Room* room = &dataWin->room.rooms[roomIndex];
    SavedRoomState* savedState = &runner->savedRoomStates[roomIndex];

    runner->currentRoom = room;
    runner->currentRoomIndex = roomIndex;

    if (runner->renderer && runner->renderer->vtable->onRoomChanged) {
        runner->renderer->vtable->onRoomChanged(runner->renderer, roomIndex);
    }
    
    runner->currentRoomOrderPosition = -1;
    repeat(dataWin->gen8.roomOrderCount, i) {
        if (dataWin->gen8.roomOrder[i] == roomIndex) {
            runner->currentRoomOrderPosition = (int32_t) i;
            break;
        }
    }

    if (room->persistent && savedState->initialized) {
        memcpy(runner->backgrounds, savedState->backgrounds, sizeof(runner->backgrounds));
        runner->backgroundColor = savedState->backgroundColor;
        runner->drawBackgroundColor = savedState->drawBackgroundColor;

        hmfree(runner->tileLayerMap);
        runner->tileLayerMap = savedState->tileLayerMap;
        savedState->tileLayerMap = nullptr;

        Instance** keptInstances = nullptr;
        int32_t oldCount = (int32_t) arrlen(runner->instances);
        repeat(oldCount, i) {
            Instance* inst = runner->instances[i];
            if (inst->persistent) {
                arrput(keptInstances, inst);
            } else {
                hmdel(runner->instancesToId, inst->instanceId);
                Instance_free(inst);
            }
        }
        arrfree(runner->instances);
        runner->instances = keptInstances;

        int32_t savedCount = (int32_t) arrlen(savedState->instances);
        repeat(savedCount, i) {
            arrput(runner->instances, savedState->instances[i]);
        }
        arrfree(savedState->instances);
        savedState->instances = nullptr;

        fprintf(stderr, "Runner: Room restored (persistent): %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
        return;
    }

    hmfree(runner->tileLayerMap);
    runner->tileLayerMap = nullptr;

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

    Instance** keptInstances = nullptr;
    int32_t oldCount = (int32_t) arrlen(runner->instances);
    repeat(oldCount, i) {
        Instance* inst = runner->instances[i];
        if (inst->persistent) {
            arrput(keptInstances, inst);
        } else {
            hmdel(runner->instancesToId, inst->instanceId);
            Instance_free(inst);
        }
    }
    arrfree(runner->instances);
    runner->instances = keptInstances;

    // ===[ ФИКС СКОРОСТИ 3: Убираем O(N^2) циклы из создания инстансов ]===
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Мгновенная проверка O(1) вместо долгого цикла
        if (hmgeti(runner->instancesToId, roomObj->instanceID) >= 0) continue;
        if (isObjectDisabled(runner, roomObj->objectDefinition)) continue;

        Instance* inst = createAndInitInstance(runner, roomObj->instanceID, roomObj->objectDefinition, (GMLReal) roomObj->x, (GMLReal) roomObj->y);
        inst->imageXscale = (float) roomObj->scaleX;
        inst->imageYscale = (float) roomObj->scaleY;
        inst->imageAngle = (float) roomObj->rotation;
    }

    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];

        // Мгновенный поиск O(1) вместо долгого цикла
        ptrdiff_t idx = hmgeti(runner->instancesToId, roomObj->instanceID);
        if (idx < 0) continue;
        Instance* inst = runner->instancesToId[idx].value;

        if (inst->createEventFired) continue;
        inst->createEventFired = true;

        executeCode(runner, inst, roomObj->preCreateCode);
        Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
        executeCode(runner, inst, roomObj->creationCode);
    }
    // ======================================================================

    if (room->creationCodeId >= 0 && dataWin->code.count > (uint32_t) room->creationCodeId) {
        RValue result = VM_executeCode(runner->vmContext, room->creationCodeId);
        RValue_free(&result);
    }

    savedState->initialized = true;
    fprintf(stderr, "Runner: Room loaded: %s (room %d) with %d instances\n", room->name, roomIndex, (int) arrlen(runner->instances));
}



Runner* Runner_create(DataWin* dataWin, VMContext* vm, FileSystem* fileSystem) {
    Runner* runner = safeCalloc(1, sizeof(Runner));
    runner->dataWin = dataWin;
    runner->vmContext = vm;
    runner->fileSystem = fileSystem;
    runner->frameCount = 0;
    runner->bboxCache = NULL;
    runner->bboxCacheCount = 0;
    runner->bboxCacheFrame = -1;
    runner->instances = nullptr;
    runner->pendingRoom = -1;
    runner->gameStartFired = false;
    runner->currentRoomIndex = -1;
    runner->currentRoomOrderPosition = -1;
    runner->nextInstanceId = dataWin->gen8.lastObj + 1;
    runner->keyboard = RunnerKeyboard_create();
    runner->savedRoomStates = safeCalloc(dataWin->room.count, sizeof(SavedRoomState));
    runner->isGMS2 = (dataWin->gen8.major >= 2);

    
    vm->runner = (struct Runner*) runner;

    
    NativeScripts_init(vm, runner);

    return runner;
}

Instance* Runner_createInstance(Runner* runner, GMLReal x, GMLReal y, int32_t objectIndex) {
    if (isObjectDisabled(runner, objectIndex)) return nullptr;
    Instance* inst = createAndInitInstance(runner, runner->nextInstanceId++, objectIndex, x, y);
    inst->createEventFired = true;
    Runner_executeEvent(runner, inst, EVENT_CREATE, 0);
    return inst;
}

void Runner_destroyInstance(MAYBE_UNUSED Runner* runner, Instance* inst) {
    GameObject* gameObject = &runner->dataWin->objt.objects[inst->objectIndex];
    Runner_executeEvent(runner, inst, EVENT_DESTROY, 0);
    
    
    inst->active = false;
    inst->destroyed = true;

#ifndef DISABLE_VM_TRACING
    if (shgeti(runner->vmContext->instanceLifecyclesToBeTraced, "*") != -1 || shgeti(runner->vmContext->instanceLifecyclesToBeTraced, gameObject->name) != -1) {
        fprintf(stderr, "VM: Instance %s (%d) destroyed\n", gameObject->name, inst->instanceId);
    }
#endif
}

void Runner_cleanupDestroyedInstances(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->instances);
    int32_t writeIdx = 0;
    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->destroyed) {
            runner->instances[writeIdx++] = inst;
        } else {
            removeInstanceFromCache(runner, inst);  
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

    
    repeat(dataWin->glob.count, i) {
        int32_t codeId = dataWin->glob.codeIds[i];
        if (codeId >= 0 && dataWin->code.count > (uint32_t) codeId) {
            fprintf(stderr, "Runner: Executing global init script: %s\n", dataWin->code.entries[codeId].name);
            RValue result = VM_executeCode(runner->vmContext, codeId);
            RValue_free(&result);
        }
    }

    
    initRoom(runner, firstRoomIndex);

    
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_GAME_START);
    runner->gameStartFired = true;

    
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
}



static void executeCollisionEvent(Runner* runner, Instance* self, Instance* other, int32_t targetObjectIndex) {
    VMContext* vm = runner->vmContext;

    
    int32_t savedEventType = vm->currentEventType;
    int32_t savedEventSubtype = vm->currentEventSubtype;
    int32_t savedEventObjectIndex = vm->currentEventObjectIndex;
    struct Instance* savedOtherInstance = vm->otherInstance;

    
    vm->currentEventType = EVENT_COLLISION;
    vm->currentEventSubtype = targetObjectIndex;
    vm->otherInstance = other;

    int32_t ownerObjectIndex = -1;
    int32_t codeId = findEventCodeIdAndOwner(runner->dataWin, self->objectIndex, EVENT_COLLISION, targetObjectIndex, &ownerObjectIndex);

    vm->currentEventObjectIndex = ownerObjectIndex;

#ifndef DISABLE_VM_TRACING
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

    
    vm->currentEventType = savedEventType;
    vm->currentEventSubtype = savedEventSubtype;
    vm->currentEventObjectIndex = savedEventObjectIndex;
    vm->otherInstance = savedOtherInstance;
}


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


typedef struct { int32_t key; int32_t* value; } ObjInstIndex; 

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
                            if (sprSelf) DataWin_ensureSpriteMasks(dataWin, sprSelf);
                            if (sprOther) DataWin_ensureSpriteMasks(dataWin, sprOther);

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



static int32_t followAxis(int32_t viewPos, int32_t viewSize, int32_t targetPos, uint32_t border, int32_t speed, int32_t roomSize) {
    int32_t pos = viewPos;

    
    if (2 * (int32_t) border >= viewSize) {
        pos = targetPos - viewSize / 2;
    } else if (targetPos - (int32_t) border < viewPos) {
        pos = targetPos - (int32_t) border;
    } else if (targetPos + (int32_t) border > viewPos + viewSize) {
        pos = targetPos + (int32_t) border - viewSize;
    }

    
    if (0 > pos) pos = 0;
    if (pos + viewSize > roomSize) pos = roomSize - viewSize;

    
    if (speed >= 0) {
        if (pos < viewPos && viewPos - pos > speed) pos = viewPos - speed;
        if (pos > viewPos && pos - viewPos > speed) pos = viewPos + speed;
    }

    return pos;
}

static void updateViews(Runner* runner) {
    Room* room = runner->currentRoom;
    if (!(room->flags & 1)) return;

    for (int32_t vi = 0; 8 > vi; vi++) {
        RoomView* view = &room->views[vi];
        if (!view->enabled || 0 > view->objectId) continue;

        
        Instance* target = nullptr;
        int32_t count = (int32_t) arrlen(runner->instances);
        for (int32_t i = 0; count > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(runner->dataWin, inst->objectIndex, view->objectId)) { target = inst; break; };
        }
        if (target == nullptr) continue;

        int32_t ix = (int32_t) GMLReal_floor(target->x);
        int32_t iy = (int32_t) GMLReal_floor(target->y);

        view->viewX = followAxis(view->viewX, view->viewWidth, ix, view->borderX, view->speedX, (int32_t) room->width);
        view->viewY = followAxis(view->viewY, view->viewHeight, iy, view->borderY, view->speedY, (int32_t) room->height);
    }
}

static void dispatchOutsideRoomEvents(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    int32_t roomWidth = (int32_t) runner->currentRoom->width;
    int32_t roomHeight = (int32_t) runner->currentRoom->height;
    int32_t count = (int32_t) arrlen(runner->instances);

    
    int8_t outsideCache[512];
    int32_t maxOutObj = (dataWin->objt.count < 512) ? (int32_t)dataWin->objt.count : 512;
    memset(outsideCache, -1, maxOutObj);

    repeat(count, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        
        int32_t oi = inst->objectIndex;
        if (oi >= 0 && oi < maxOutObj) {
            if (outsideCache[oi] == 0) continue;
            if (outsideCache[oi] == -1) {
                outsideCache[oi] = (findEventCodeIdAndOwner(dataWin, oi, EVENT_OTHER, OTHER_OUTSIDE_ROOM, nullptr) >= 0) ? 1 : 0;
                if (outsideCache[oi] == 0) continue;
            }
        }

        
        bool outside;
        InstanceBBox bbox = Collision_computeBBox(dataWin, inst);
        if (bbox.valid) {
            outside = (0 > bbox.right || bbox.left > roomWidth || 0 > bbox.bottom || bbox.top > roomHeight);
        } else {
            
            outside = (0 > inst->x || inst->x > roomWidth || 0 > inst->y || inst->y > roomHeight);
        }

        
        if (outside && !inst->outsideRoom) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_OUTSIDE_ROOM);
            if (runner->pendingRoom >= 0) break;
        }

        inst->outsideRoom = outside;
    }
}




static bool adaptPath(Runner* runner, Instance* inst) {
    if (0 > inst->pathIndex) return false;

    DataWin* dataWin = runner->dataWin;
    if ((uint32_t) inst->pathIndex >= dataWin->path.count) return false;

    GamePath* path = &dataWin->path.paths[inst->pathIndex];
    if (0.0 >= path->length) return false;

    bool atPathEnd = false;

    GMLReal orient = inst->pathOrientation * M_PI / 180.0;

    
    PathPositionResult cur = GamePath_getPosition(path, inst->pathPosition);
    GMLReal sp = cur.speed / (100.0 * inst->pathScale);

    
    inst->pathPosition = (float) (inst->pathPosition + inst->pathSpeed * sp / path->length);

    
    PathPositionResult pos0 = GamePath_getPosition(path, 0.0);
    if (inst->pathPosition >= 1.0f || 0.0f >= inst->pathPosition) {
        atPathEnd = (inst->pathSpeed == 0.0f) ? false : true;

        switch (inst->pathEndAction) {
            
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
            
            case 1: {
                if (0.0f > inst->pathPosition) {
                    inst->pathPosition += 1.0f;
                } else {
                    inst->pathPosition -= 1.0f;
                }
                break;
            }
            
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
            
            default: {
                inst->pathPosition = 1.0f;
                inst->pathIndex = -1;
                break;
            }
        }
    }

    
    PathPositionResult newPos = GamePath_getPosition(path, inst->pathPosition);
    GMLReal xx = newPos.x - pos0.x; 
    GMLReal yy = newPos.y - pos0.y;

    GMLReal newx = inst->pathXStart + inst->pathScale * (xx * GMLReal_cos(orient) + yy * GMLReal_sin(orient));
    GMLReal newy = inst->pathYStart + inst->pathScale * (yy * GMLReal_cos(orient) - xx * GMLReal_sin(orient));

    
    inst->hspeed = (float) (newx - inst->x);
    inst->vspeed = (float) (newy - inst->y);
    Instance_computeSpeedFromComponents(inst);

    
    inst->speed = 0.0f;
    inst->hspeed = 0.0f;
    inst->vspeed = 0.0f;

    
    inst->x = (float) newx;
    inst->y = (float) newy;

    return atPathEnd;
}

void Runner_step(Runner* runner) {
    
    runner->stepCacheCount = 0;
    runner->topVmStepName[0] = runner->topVmStepName[1] = runner->topVmStepName[2] = NULL;
    runner->topVmStepTimeUs[0] = runner->topVmStepTimeUs[1] = runner->topVmStepTimeUs[2] = 0;
    runner->topVmStepCalls[0] = runner->topVmStepCalls[1] = runner->topVmStepCalls[2] = 0;

    
    
    uint64_t _t0 = _profTickUs(), _t1;
    #define STEP_MARK(field) do { _t1 = _profTickUs(); \
        runner->field += (uint32_t)(_t1 - _t0); \
        _t0 = _t1; } while(0)

    
    int8_t animEndCache[512];
    int32_t maxAnimObj = (runner->dataWin->objt.count < 512) ? (int32_t)runner->dataWin->objt.count : 512;
    memset(animEndCache, -1, maxAnimObj);

    int32_t instCount = (int32_t) arrlen(runner->instances);
    repeat(instCount, i) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;

        
        inst->xprevious = inst->x;
        inst->yprevious = inst->y;
        inst->pathPositionPrevious = inst->pathPosition;

        
        
        
        
        if (inst->spriteIndex >= 0 && inst->imageSpeed != 0.0f) {
            inst->imageIndex += inst->imageSpeed;
            Sprite* sprite = &runner->dataWin->sprt.sprites[inst->spriteIndex];
            float frameCount = (float) sprite->textureCount;
            bool wrapped = false;
            if (inst->imageIndex >= frameCount) { inst->imageIndex -= frameCount; wrapped = true; }
            else if (0.0f > inst->imageIndex) { inst->imageIndex += frameCount; wrapped = true; }
            if (wrapped) {
                int32_t oi = inst->objectIndex;
                if (oi >= 0 && oi < maxAnimObj) {
                    if (animEndCache[oi] == -1)
                        animEndCache[oi] = (findEventCodeIdAndOwner(runner->dataWin, oi, EVENT_OTHER, OTHER_ANIMATION_END, NULL) >= 0) ? 1 : 0;
                    if (animEndCache[oi])
                        Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
                } else {
                    Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_ANIMATION_END);
                }
            }
        }
    }

    STEP_MARK(phaseUsAnim);

    
    Runner_scrollBackgrounds(runner);

    
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_BEGIN);
    STEP_MARK(phaseUsBeginStep);

    
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

    STEP_MARK(phaseUsKeyboard);

    
    {
        int32_t alarmCount = (int32_t) arrlen(runner->instances);
        repeat(alarmCount, i) {
            Instance* inst = runner->instances[i];
            if (!inst->active) continue;

            
            bool hasAlarm = false;
            for (int a = 0; a < GML_ALARM_COUNT; a++) {
                if (inst->alarm[a] > 0) { hasAlarm = true; break; }
            }
            if (!hasAlarm) continue;

            for (int alarmIdx = 0; alarmIdx < GML_ALARM_COUNT; alarmIdx++) {
                if (inst->alarm[alarmIdx] > 0) {
                    inst->alarm[alarmIdx]--;
                    if (inst->alarm[alarmIdx] == 0) {
                        inst->alarm[alarmIdx] = -1;
                        Runner_executeEvent(runner, inst, EVENT_ALARM, alarmIdx);
                    }
                }
            }
        }
    }

    STEP_MARK(phaseUsAlarms);

    
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_NORMAL);
    STEP_MARK(phaseUsNormalStep);

    
    int32_t motionCount = (int32_t) arrlen(runner->instances);
    repeat(motionCount, mi) {
        Instance* inst = runner->instances[mi];
        if (!inst->active) continue;

        
        if (inst->hspeed == 0.0f && inst->vspeed == 0.0f &&
            inst->friction == 0.0f && inst->gravity == 0.0f &&
            inst->pathIndex < 0) continue;

        
        if (inst->friction != 0.0f) {
            float ns = (inst->speed > 0.0f) ? inst->speed - inst->friction : inst->speed + inst->friction;
            if ((inst->speed > 0.0f && ns < 0.0f) || (inst->speed < 0.0f && ns > 0.0f)) {
                inst->speed = 0.0f;
            } else if (inst->speed != 0.0f) {
                inst->speed = ns;
            }
            Instance_computeComponentsFromSpeed(inst);
        }

        
        
        
        
        if (inst->gravity != 0.0f) {
            float dir = inst->gravityDirection;
            float g = inst->gravity;
            if (dir == 90.0f) {
                
                inst->vspeed -= g;
            } else if (dir == 270.0f) {
                
                inst->vspeed += g;
            } else if (dir == 0.0f || dir == 360.0f) {
                
                inst->hspeed += g;
            } else if (dir == 180.0f) {
                
                inst->hspeed -= g;
            } else {
                GMLReal gravDirRad = dir * (M_PI / 180.0);
                inst->hspeed += (float) (g * clampFloat(GMLReal_cos(gravDirRad)));
                inst->vspeed -= (float) (g * clampFloat(GMLReal_sin(gravDirRad)));
            }
            Instance_computeSpeedFromComponents(inst);
        }

        
        if (adaptPath(runner, inst)) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_END_OF_PATH);
        }

        
        if (inst->hspeed != 0.0f || inst->vspeed != 0.0f) {
            inst->x += inst->hspeed;
            inst->y += inst->vspeed;
        }
    }

    STEP_MARK(phaseUsMotion);

    
    dispatchOutsideRoomEvents(runner);
    STEP_MARK(phaseUsOutsideRoom);

    
    dispatchCollisionEvents(runner);
    STEP_MARK(phaseUsCollision);

    
    Runner_executeEventForAll(runner, EVENT_STEP, STEP_END);
    STEP_MARK(phaseUsEndStep);

    
    updateViews(runner);

    
    if (runner->pendingRoom >= 0) {
        int32_t oldRoomIndex = runner->currentRoomIndex;
        Room* oldRoom = runner->currentRoom;
        const char* oldRoomName = oldRoom->name;

        
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_END);

        int32_t newRoomIndex = runner->pendingRoom;
        runner->pendingRoom = -1;
        require(runner->dataWin->room.count > (uint32_t) newRoomIndex);
        const char* newRoomName = runner->dataWin->room.rooms[newRoomIndex].name;

        fprintf(stderr, "Room changed: %s (room %d) -> %s (room %d)\n", oldRoomName, oldRoomIndex, newRoomName, newRoomIndex);

        
        if (oldRoom->persistent) {
            SavedRoomState* state = &runner->savedRoomStates[oldRoomIndex];

            
            int32_t prevSavedCount = (int32_t) arrlen(state->instances);
            repeat(prevSavedCount, i) {
                hmdel(runner->instancesToId, state->instances[i]->instanceId);
                Instance_free(state->instances[i]);
            }
            arrfree(state->instances);
            state->instances = nullptr;
            hmfree(state->tileLayerMap);
            state->tileLayerMap = nullptr;

            
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

            
            memcpy(state->backgrounds, runner->backgrounds, sizeof(runner->backgrounds));
            state->backgroundColor = runner->backgroundColor;
            state->drawBackgroundColor = runner->drawBackgroundColor;

            
            state->tileLayerMap = runner->tileLayerMap;
            runner->tileLayerMap = nullptr;

            state->initialized = true;
        }

        
        initRoom(runner, newRoomIndex);

        
        Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ROOM_START);
    }

    Runner_cleanupDestroyedInstances(runner);

    
    if (runner->profileFile != NULL && shlen(runner->profileCalls) > 0) {
        const char* roomName = (runner->currentRoom && runner->currentRoom->name) ? runner->currentRoom->name : "?";
        int32_t totalCalls = 0;
        for (ptrdiff_t i = 0; i < shlen(runner->profileCalls); i++)
            totalCalls += runner->profileCalls[i].value;

        int32_t activeInst = 0;
        for (int32_t ii = 0; ii < (int32_t)arrlen(runner->instances); ii++)
            if (runner->instances[ii]->active) activeInst++;
        int32_t tileCount = runner->currentRoom ? (int32_t)runner->currentRoom->tileCount : 0;
        fprintf(runner->profileFile, "Frame %d (room: %s, %d calls, %d instances, %d tiles):\n",
                runner->frameCount, roomName, totalCalls, activeInst, tileCount);
        
        fprintf(runner->profileFile,
                "  PHASES anim=%u beginStep=%u keyboard=%u alarms=%u normalStep=%u motion=%u outsideRoom=%u collision=%u endStep=%u\n",
                runner->phaseUsAnim, runner->phaseUsBeginStep, runner->phaseUsKeyboard,
                runner->phaseUsAlarms, runner->phaseUsNormalStep, runner->phaseUsMotion,
                runner->phaseUsOutsideRoom, runner->phaseUsCollision, runner->phaseUsEndStep);
        fprintf(runner->profileFile,
                "  DRAW sortList=%u buildList=%u drawNormal=%u drawGUI=%u\n",
                runner->drawPhaseSortList, runner->drawPhaseBuildList,
                runner->drawPhaseNormal, runner->drawPhaseGUI);
        runner->phaseUsAnim = 0; runner->phaseUsBeginStep = 0; runner->phaseUsKeyboard = 0;
        runner->phaseUsAlarms = 0; runner->phaseUsNormalStep = 0; runner->phaseUsMotion = 0;
        runner->phaseUsOutsideRoom = 0; runner->phaseUsCollision = 0; runner->phaseUsEndStep = 0;
        fprintf(runner->profileFile,
                "  NRM build=%u qsort=%u tiles=%u(n%u) instVM=%u(n%u) instNat=%u(n%u) drawSelf=%u(n%u)\n",
                runner->drawNrmBuildDrawables, runner->drawNrmQsort,
                runner->drawNrmTiles, runner->drawNrmCountTiles,
                runner->drawNrmInstVM, runner->drawNrmCountInstVM,
                runner->drawNrmInstNative, runner->drawNrmCountInstNative,
                runner->drawNrmInstDrawSelf, runner->drawNrmCountInstDrawSelf);
        runner->drawPhaseSortList = 0; runner->drawPhaseBuildList = 0;
        runner->drawPhaseNormal = 0; runner->drawPhaseGUI = 0;
        runner->drawNrmBuildDrawables = 0; runner->drawNrmQsort = 0;
        runner->drawNrmTiles = 0; runner->drawNrmInstVM = 0;
        runner->drawNrmInstNative = 0; runner->drawNrmInstDrawSelf = 0;
        runner->drawNrmCountTiles = 0; runner->drawNrmCountInstVM = 0;
        runner->drawNrmCountInstNative = 0; runner->drawNrmCountInstDrawSelf = 0;

        
        for (ptrdiff_t i = 0; i < shlen(runner->profileCalls); i++) {
            ptrdiff_t maxIdx = i;
            for (ptrdiff_t j = i + 1; j < shlen(runner->profileCalls); j++) {
                if (runner->profileCalls[j].value > runner->profileCalls[maxIdx].value) maxIdx = j;
            }
            if (maxIdx != i) {
                
                char* tmpKey = runner->profileCalls[i].key;
                int tmpVal = runner->profileCalls[i].value;
                runner->profileCalls[i].key = runner->profileCalls[maxIdx].key;
                runner->profileCalls[i].value = runner->profileCalls[maxIdx].value;
                runner->profileCalls[maxIdx].key = tmpKey;
                runner->profileCalls[maxIdx].value = tmpVal;
            }
            
            const char* scriptName = runner->profileCalls[i].key;
            int usec = 0;
            ptrdiff_t ti = shgeti(runner->profileTimes, scriptName);
            if (ti >= 0) usec = runner->profileTimes[ti].value;
            fprintf(runner->profileFile, "  %3d× %s [%d us]\n", runner->profileCalls[i].value, scriptName, usec);
        }
        fprintf(runner->profileFile, "\n");
        fflush(runner->profileFile);
        shfree(runner->profileCalls);
        runner->profileCalls = NULL;
        shfree(runner->profileTimes);
        runner->profileTimes = NULL;
    }

#ifdef VM_PSP_TRACE
    
    
    
    if (runner->traceFile && runner->traceEnabled) {
        fprintf(runner->traceFile, "=== end of frame %d ===\n", runner->frameCount);
        fflush(runner->traceFile);
    }
#endif

    
    
    
    for (int32_t _si = 0; _si < runner->stepCacheCount; _si++) {
        if (runner->stepCache[_si].calls == 0) continue;
        uint32_t t = runner->stepCache[_si].timeUs;
        int rank = -1;
        if (t > runner->topVmStepTimeUs[0]) rank = 0;
        else if (t > runner->topVmStepTimeUs[1]) rank = 1;
        else if (t > runner->topVmStepTimeUs[2]) rank = 2;
        if (rank >= 0) {
            for (int k = 2; k > rank; k--) {
                runner->topVmStepName[k]   = runner->topVmStepName[k-1];
                runner->topVmStepTimeUs[k] = runner->topVmStepTimeUs[k-1];
                runner->topVmStepCalls[k]  = runner->topVmStepCalls[k-1];
            }
            int32_t cid = runner->stepCache[_si].codeId;
            runner->topVmStepName[rank]   = runner->dataWin->code.entries[cid].name;
            runner->topVmStepTimeUs[rank] = t;
            runner->topVmStepCalls[rank]  = runner->stepCache[_si].calls;
        }
    }

    runner->frameCount++;
}



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

        
        bool hasAlarm = false;
        repeat(GML_ALARM_COUNT, alarmIdx) {
            if (inst->alarm[alarmIdx] >= 0) {
                if (!hasAlarm) { printf("  Alarms:"); hasAlarm = true; }
                printf(" [%d]=%d", alarmIdx, inst->alarm[alarmIdx]);
            }
        }
        if (hasAlarm) printf("\n");

        
        bool hasSelfVars = false;
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

            if (!hasSelfVars) { printf("  Self Variables:\n"); hasSelfVars = true; }
            char* valStr = RValue_toStringFancy(val);
            printf("    %s = %s\n", varName, valStr);
            free(valStr);
        }

        
        int64_t selfArrayLen = hmlen(inst->selfArrayMap);
        if (selfArrayLen > 0) {
            printf("  Self Arrays:\n");
            repeat(selfArrayLen, arrIdx) {
                int64_t key = inst->selfArrayMap[arrIdx].key;
                RValue val = inst->selfArrayMap[arrIdx].value;
                int32_t varID = (int32_t) (key >> 32);
                int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

                
                const char* varName = "<unknown>";
                repeat(dataWin->vari.variableCount, varIdx) {
                    Variable* var = &dataWin->vari.variables[varIdx];
                    if (var->varID == varID && var->instanceType == INSTANCE_SELF) {
                        varName = var->name;
                        break;
                    }
                }

                char* valStr = RValue_toStringFancy(val);
                printf("    %s[%d] = %s\n", varName, arrayIndex, valStr);
                free(valStr);
            }
        }
    }

    
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
        case RVALUE_ARRAY_REF: {
            char buf[64];
            snprintf(buf, sizeof(buf), "<array_ref:%d>", val.int32);
            JsonWriter_string(w, buf);
            break;
        }
    }
}

char* Runner_dumpStateJson(Runner* runner) {
    DataWin* dataWin = runner->dataWin;
    VMContext* vm = runner->vmContext;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    JsonWriter w = JsonWriter_create();

    JsonWriter_beginObject(&w);

    JsonWriter_propertyInt(&w, "frame", runner->frameCount);

    
    JsonWriter_key(&w, "room");
    JsonWriter_beginObject(&w);
    JsonWriter_propertyString(&w, "name", runner->currentRoom->name);
    JsonWriter_propertyInt(&w, "index", runner->currentRoomIndex);
    JsonWriter_endObject(&w);

    
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

        
        JsonWriter_key(&w, "sprite");
        JsonWriter_beginObject(&w);
        JsonWriter_propertyString(&w, "name", spriteName);
        JsonWriter_propertyInt(&w, "index", inst->spriteIndex);
        JsonWriter_propertyDouble(&w, "imageIndex", inst->imageIndex);
        JsonWriter_propertyDouble(&w, "imageSpeed", inst->imageSpeed);
        JsonWriter_endObject(&w);

        
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

        
        JsonWriter_key(&w, "selfVariables");
        JsonWriter_beginObject(&w);
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

            JsonWriter_key(&w, varName);
            writeRValueJson(&w, val);
        }
        JsonWriter_endObject(&w);

        
        JsonWriter_key(&w, "selfArrays");
        JsonWriter_beginObject(&w);
        int64_t selfArrayLen = hmlen(inst->selfArrayMap);
        if (selfArrayLen > 0) {
            repeat(selfArrayLen, arrIdx) {
                int64_t key = inst->selfArrayMap[arrIdx].key;
                RValue val = inst->selfArrayMap[arrIdx].value;
                int32_t varID = (int32_t) (key >> 32);
                int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

                
                const char* varName = nullptr;
                repeat(dataWin->vari.variableCount, varIdx) {
                    Variable* var = &dataWin->vari.variables[varIdx];
                    if (var->varID == varID && var->instanceType == INSTANCE_SELF) {
                        varName = var->name;
                        break;
                    }
                }

                if (varName == nullptr) continue;

                
                
                
                
                char compositeKey[256];
                snprintf(compositeKey, sizeof(compositeKey), "%s[%d]", varName, arrayIndex);
                JsonWriter_key(&w, compositeKey);
                writeRValueJson(&w, val);
            }
        }
        JsonWriter_endObject(&w);

        JsonWriter_endObject(&w);
    }

    JsonWriter_endArray(&w);

    
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

    
    JsonWriter_key(&w, "globalArrays");
    JsonWriter_beginObject(&w);
    int64_t globalArrayLen = hmlen(vm->globalArrayMap);
    if (globalArrayLen > 0) {
        repeat(globalArrayLen, arrIdx) {
            int64_t key = vm->globalArrayMap[arrIdx].key;
            RValue val = vm->globalArrayMap[arrIdx].value;
            int32_t varID = (int32_t) (key >> 32);
            int32_t arrayIndex = (int32_t) (key & 0xFFFFFFFF);

            const char* varName = nullptr;
            repeat(dataWin->vari.variableCount, varIdx) {
                Variable* var = &dataWin->vari.variables[varIdx];
                if (var->varID == varID && var->instanceType == INSTANCE_GLOBAL) {
                    varName = var->name;
                    break;
                }
            }

            if (varName == nullptr) continue;

            char compositeKey[256];
            snprintf(compositeKey, sizeof(compositeKey), "%s[%d]", varName, arrayIndex);
            JsonWriter_key(&w, compositeKey);
            writeRValueJson(&w, val);
        }
    }
    JsonWriter_endObject(&w);

    JsonWriter_endObject(&w);

    char* result = JsonWriter_copyOutput(&w);
    JsonWriter_free(&w);
    return result;
}

void Runner_free(Runner* runner) {
    if (runner == nullptr) return;

    
    repeat(arrlen(runner->instances), i) {
        hmdel(runner->instancesToId, runner->instances[i]->instanceId);
        Instance_free(runner->instances[i]);
    }
    arrfree(runner->instances);

    
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
        }
        free(runner->savedRoomStates);
    }

    hmfree(runner->tileLayerMap);
    shfree(runner->disabledObjects);
    RunnerKeyboard_free(runner->keyboard);
    free(runner);
}
