#include "instance.h"

#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"
#include "utils.h"

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, double x, double y, uint32_t selfVarCount) {
    Instance* inst = calloc(1, sizeof(Instance));
    inst->instanceId = instanceId;
    inst->objectIndex = objectIndex;
    inst->x = x;
    inst->y = y;
    inst->persistent = false;
    inst->solid = false;
    inst->active = true;
    inst->visible = true;
    inst->spriteIndex = -1;
    inst->imageSpeed = 1.0;
    inst->imageIndex = 0.0;
    inst->imageXscale = 1.0;
    inst->imageYscale = 1.0;
    inst->imageAngle = 0.0;
    inst->imageAlpha = 1.0;
    inst->imageBlend = 0xFFFFFF;
    inst->depth = 0;
    inst->selfArrayMap = nullptr;

    // Initialize alarms to -1 (inactive)
    repeat(GML_ALARM_COUNT, i) {
        inst->alarm[i] = -1;
    }

    // Allocate self vars
    inst->selfVarCount = selfVarCount;
    if (selfVarCount > 0) {
        inst->selfVars = calloc(selfVarCount, sizeof(RValue));
        for (uint32_t i = 0; selfVarCount > i; i++) {
            inst->selfVars[i].type = RVALUE_UNDEFINED;
        }
    } else {
        inst->selfVars = nullptr;
    }

    return inst;
}

void Instance_free(Instance* instance) {
    if (instance == nullptr) return;

    // Free owned strings in selfVars
    if (instance->selfVars != nullptr) {
        for (uint32_t i = 0; instance->selfVarCount > i; i++) {
            RValue_free(&instance->selfVars[i]);
        }
        free(instance->selfVars);
    }

    // Free selfArrayMap
    for (ptrdiff_t i = 0; hmlen(instance->selfArrayMap) > i; i++) {
        RValue_free(&instance->selfArrayMap[i].value);
    }
    hmfree(instance->selfArrayMap);

    free(instance);
}
