#pragma once

#include "renderer.h"
#include <SDL2/SDL.h>

typedef struct {
    Renderer base;

    SDL_Renderer* sdlRenderer;
    SDL_Window* sdlWindow;

    SDL_Texture** sdlTextures;
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t textureCount;

    SDL_Texture* whiteTexture;

    SDL_Texture* fboTexture;
    int32_t fboWidth;
    int32_t fboHeight;
    int32_t windowW;
    int32_t windowH;
    int32_t gameW;
    int32_t gameH;

    // View state
    float currentViewAngle;
    float currentViewX, currentViewY;
    float currentViewW, currentViewH;
    float currentPortX, currentPortY;
    float currentPortW, currentPortH;

    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
} SDLRenderer;

Renderer* SDLRenderer_create(SDL_Window* window, SDL_Renderer* renderer);