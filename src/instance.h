#pragma once

#include <stdint.h>
#include "rvalue.h"

#define GML_ALARM_COUNT 12

typedef struct Instance {
    uint32_t instanceId;
    int32_t objectIndex;
    double x, y;
    double xprevious, yprevious;
    bool persistent, solid, active, visible, createEventFired;
    int32_t maskIndex; // collision mask sprite override (-1 = use spriteIndex)

    // Per-instance self variable storage
    RValue* selfVars;
    uint32_t selfVarCount;
    ArrayMapEntry* selfArrayMap;
    struct { int32_t key; int32_t value; }* selfArrayVarTracker; // tracks which varIDs have array data

    // Built-in instance properties
    int32_t spriteIndex;
    double imageSpeed, imageIndex;
    double imageXscale, imageYscale, imageAngle, imageAlpha;
    uint32_t imageBlend;
    int32_t depth;
    int32_t alarm[GML_ALARM_COUNT];
} Instance;

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, double x, double y, uint32_t selfVarCount);
void Instance_free(Instance* instance);
