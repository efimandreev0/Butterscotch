// Original Code by MrPowerGamerBR and the Butterscotch contributors.
// Modifications Copyright (c) 2026 Efim Andreev and Vyacheslav Ivanov.
//
// This file is part of Butterscotch (Nintendo 3DS port).
//
// Butterscotch is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3.

#pragma once

#include <stdbool.h>
#ifndef nullptr
#define nullptr NULL
#endif

#if defined(__has_c_attribute)
    #if __has_c_attribute(maybe_unused)
        #define MAYBE_UNUSED [[maybe_unused]]
    #endif
#endif

#ifndef MAYBE_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define MAYBE_UNUSED __attribute__((unused))
    #else
        #define MAYBE_UNUSED
    #endif
#endif