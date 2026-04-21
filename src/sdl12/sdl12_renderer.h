#pragma once
#include <SDL/SDL.h>
#include "../renderer.h"

typedef struct {
    Renderer base;
    
    SDL_Surface* screenSurface;
    SDL_Surface* fboSurface;

    SDL_Surface** sdlSurfaces;
    int32_t* textureWidths;
    int32_t* textureHeights;

    uint32_t textureCount;
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    int32_t windowW, windowH;
    int32_t gameW, gameH;
    int32_t fboWidth, fboHeight;

    float currentViewX, currentViewY, currentViewW, currentViewH;
    float currentPortX, currentPortY, currentPortW, currentPortH;
    float currentViewAngle;

    float camScaleX, camScaleY;
    float camCX, camCY;
    float camCos, camSin;

    float uniScale;
    int   viewOffX;      // Letterbox offset X
    int   viewOffY;      // Letterbox offset Y
    int   gameNativeW;   // gen8->defaultWindowWidth
    int   gameNativeH;   // gen8->defaultWindowHeight

    SDL_Surface* prevFboSurface;
    int          interlaceField;

    int presentNeedsClear;
    int presentFrameParity;

    float frameTimeAvg;
    uint32_t lastTicks;
    int interlaceEnabled;

    int interlaceParity;
} SDLRenderer;

Renderer* SDLRenderer_create(SDL_Surface* screenSurface);
void preloadAllTextures(SDLRenderer* sdl);