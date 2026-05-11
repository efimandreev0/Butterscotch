// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include "common.h"
#include "string_builder.h"
#include <stdint.h>
#include <stddef.h>

// ===[ JsonWriter Type ]===

typedef struct {
    StringBuilder out;
    bool needsComma;
} JsonWriter;

// ===[ Lifecycle ]===

JsonWriter JsonWriter_create(void);
void JsonWriter_free(JsonWriter* writer);

// ===[ Structure ]===

void JsonWriter_beginObject(JsonWriter* writer);
void JsonWriter_endObject(JsonWriter* writer);
void JsonWriter_beginArray(JsonWriter* writer);
void JsonWriter_endArray(JsonWriter* writer);

// ===[ Object Keys ]===

void JsonWriter_key(JsonWriter* writer, const char* key);

// ===[ Values ]===

void JsonWriter_string(JsonWriter* writer, const char* value);
void JsonWriter_int(JsonWriter* writer, int64_t value);
void JsonWriter_double(JsonWriter* writer, double value);
void JsonWriter_bool(JsonWriter* writer, bool value);
void JsonWriter_null(JsonWriter* writer);

// ===[ Property Convenience ]===

void JsonWriter_propertyString(JsonWriter* writer, const char* key, const char* value);
void JsonWriter_propertyInt(JsonWriter* writer, const char* key, int64_t value);
void JsonWriter_propertyDouble(JsonWriter* writer, const char* key, double value);
void JsonWriter_propertyBool(JsonWriter* writer, const char* key, bool value);
void JsonWriter_propertyNull(JsonWriter* writer, const char* key);

// ===[ Output ]===

const char* JsonWriter_getOutput(const JsonWriter* writer);
char* JsonWriter_copyOutput(const JsonWriter* writer);
size_t JsonWriter_getLength(const JsonWriter* writer);
