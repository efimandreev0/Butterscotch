#pragma once

#include "common.h"
#include "data_win.h"
#include "instance.h"
#include "vm.h"

#include <math.h>





static inline bool Collision_matchesTarget(DataWin* dataWin, Instance* inst, int32_t target) {
    if (target >= 100000) return inst->instanceId == target;
    if (target == INSTANCE_ALL) return true;
    return VM_isObjectOrDescendant(dataWin, inst->objectIndex, target);
}

typedef struct {
    GMLReal left, right, top, bottom;
    bool valid;
} InstanceBBox;


static inline Sprite* Collision_getSprite(DataWin* dataWin, Instance* inst) {
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;
    if (0 > sprIdx || (uint32_t) sprIdx >= dataWin->sprt.count) return nullptr;
    return &dataWin->sprt.sprites[sprIdx];
}


static inline InstanceBBox Collision_computeBBox(DataWin* dataWin, Instance* inst) {
    Sprite* spr = Collision_getSprite(dataWin, inst);
    if (spr == nullptr) return (InstanceBBox){0, 0, 0, 0, false};

    GMLReal marginL = (GMLReal) spr->marginLeft;
    GMLReal marginR = (GMLReal) (spr->marginRight + 1);
    GMLReal marginT = (GMLReal) spr->marginTop;
    GMLReal marginB = (GMLReal) (spr->marginBottom + 1);
    GMLReal originX = (GMLReal) spr->originX;
    GMLReal originY = (GMLReal) spr->originY;

    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        
        GMLReal rad = inst->imageAngle * M_PI / 180.0;
        GMLReal cs = GMLReal_cos(rad);
        GMLReal sn = GMLReal_sin(rad);

        
        GMLReal lx0 = inst->imageXscale * (marginL - originX);
        GMLReal ly0 = inst->imageYscale * (marginT - originY);
        GMLReal lx1 = inst->imageXscale * (marginR - originX);
        GMLReal ly1 = inst->imageYscale * (marginB - originY);

        
        GMLReal cx[4], cy[4];
        cx[0] = cs * lx0 + sn * ly0;  cy[0] = -sn * lx0 + cs * ly0;
        cx[1] = cs * lx1 + sn * ly0;  cy[1] = -sn * lx1 + cs * ly0;
        cx[2] = cs * lx0 + sn * ly1;  cy[2] = -sn * lx0 + cs * ly1;
        cx[3] = cs * lx1 + sn * ly1;  cy[3] = -sn * lx1 + cs * ly1;

        GMLReal minX = cx[0], maxX = cx[0], minY = cy[0], maxY = cy[0];
        for (int c = 1; 4 > c; c++) {
            if (minX > cx[c]) minX = cx[c];
            if (cx[c] > maxX) maxX = cx[c];
            if (minY > cy[c]) minY = cy[c];
            if (cy[c] > maxY) maxY = cy[c];
        }

        return (InstanceBBox){
            .left   = inst->x + minX,
            .right  = inst->x + maxX,
            .top    = inst->y + minY,
            .bottom = inst->y + maxY,
            .valid  = true
        };
    }

    
    GMLReal left   = inst->x + inst->imageXscale * (marginL - originX);
    GMLReal right  = inst->x + inst->imageXscale * (marginR - originX);
    GMLReal top    = inst->y + inst->imageYscale * (marginT - originY);
    GMLReal bottom = inst->y + inst->imageYscale * (marginB - originY);

    
    if (left > right) { GMLReal tmp = left; left = right; right = tmp; }
    if (top > bottom) { GMLReal tmp = top; top = bottom; bottom = tmp; }

    return (InstanceBBox){left, right, top, bottom, true};
}
static DataWin* dwin;
static inline bool Collision_hasFrameMasks(Sprite* sprite) {
    DataWin_ensureSpriteMasks(dwin, sprite);
    return sprite != nullptr && sprite->sepMasks == 1 && sprite->masks != nullptr && sprite->maskCount > 0;
}




static inline bool Collision_pointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py) {
    if (spr == nullptr) return false;

    
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;

    
    GMLReal dx = px - inst->x;
    GMLReal dy = py - inst->y;

    
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        GMLReal rad = inst->imageAngle * M_PI / 180.0;
        GMLReal cs = GMLReal_cos(rad);
        GMLReal sn = GMLReal_sin(rad);
        GMLReal rx = cs * dx - sn * dy;
        GMLReal ry = sn * dx + cs * dy;
        dx = rx;
        dy = ry;
    }

    
    GMLReal localX = dx / inst->imageXscale + (GMLReal) spr->originX;
    GMLReal localY = dy / inst->imageYscale + (GMLReal) spr->originY;

    int32_t ix = (int32_t) localX;
    int32_t iy = (int32_t) localY;

    
    if (0 > ix || 0 > iy || ix >= (int32_t) spr->width || iy >= (int32_t) spr->height) return false;

    if (Collision_hasFrameMasks(spr)) {
        
        uint32_t frameIdx = ((uint32_t) inst->imageIndex) % spr->maskCount;
        uint8_t* mask = spr->masks[frameIdx];
        uint32_t bytesPerRow = (spr->width + 7) / 8;
        return (mask[iy * bytesPerRow + (ix >> 3)] & (1 << (7 - (ix & 7)))) != 0;
    }

    return true;
}










static inline bool Collision_instancesOverlapPrecise(DataWin* dataWin, Instance* a, Instance* b, InstanceBBox bboxA, InstanceBBox bboxB) {
    
    GMLReal iLeft   = GMLReal_fmax(bboxA.left, bboxB.left);
    GMLReal iRight  = GMLReal_fmin(bboxA.right, bboxB.right);
    GMLReal iTop    = GMLReal_fmax(bboxA.top, bboxB.top);
    GMLReal iBottom = GMLReal_fmin(bboxA.bottom, bboxB.bottom);

    if (iLeft >= iRight || iTop >= iBottom) return false;

    Sprite* sprA = Collision_getSprite(dataWin, a);
    Sprite* sprB = Collision_getSprite(dataWin, b);
    dwin = dataWin;

    if (sprA) DataWin_ensureSpriteMasks(dataWin, sprA);
    if (sprB) DataWin_ensureSpriteMasks(dataWin, sprB);

    if (sprA == nullptr || sprB == nullptr) return false;

    
    bool preciseA = Collision_hasFrameMasks(sprA);
    bool preciseB = Collision_hasFrameMasks(sprB);
    if (!preciseA && !preciseB) return true;

    int32_t startX = (int32_t) GMLReal_floor(iLeft);
    int32_t endX   = (int32_t) GMLReal_ceil(iRight);
    int32_t startY = (int32_t) GMLReal_floor(iTop);
    int32_t endY   = (int32_t) GMLReal_ceil(iBottom);

    for (int32_t py = startY; endY > py; py++) {
        for (int32_t px = startX; endX > px; px++) {
            GMLReal wpx = (GMLReal) px + 0.5;
            GMLReal wpy = (GMLReal) py + 0.5;

            if (!Collision_pointInInstance(sprA, a, wpx, wpy)) continue;
            if (!Collision_pointInInstance(sprB, b, wpx, wpy)) continue;
            return true;
        }
    }

    return false;
}
