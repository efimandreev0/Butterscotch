// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#include "gml_method.h"
#include "common.h"
#include "utils.h"
#include <stdlib.h>

GMLMethod* GMLMethod_create(int32_t codeIndex, int32_t boundInstanceId) {
    GMLMethod* m = safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = codeIndex;
    m->boundInstanceId = boundInstanceId;
    return m;
}

GMLMethod* GMLMethod_createBuiltin(BuiltinFunc builtin, int32_t boundInstanceId) {
    GMLMethod* m = safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = -1;
    m->boundInstanceId = boundInstanceId;
    m->builtin = builtin;
    return m;
}

GMLMethod* GMLMethod_createUnresolved(const char* name, int32_t boundInstanceId) {
    GMLMethod* m = safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = -1;
    m->boundInstanceId = boundInstanceId;
    m->unresolvedName = name;
    return m;
}

void GMLMethod_incRef(GMLMethod* m) {
    if (m == nullptr) return;
    m->refCount++;
}

void GMLMethod_decRef(GMLMethod* m) {
    if (m == nullptr) return;
    require(m->refCount > 0);
    m->refCount--;
    if (m->refCount > 0) return;
    free(m);
}
