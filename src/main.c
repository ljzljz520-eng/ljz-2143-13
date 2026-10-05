#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "app.h"

static bool init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
    int initted = IMG_Init(img_flags);
    if ((initted & img_flags) != img_flags) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return false;
    }
    return true;
}

static void shutdown_sdl(void) {
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}

int main(void) {
    if (!init_sdl()) return 1;
    VisualApp app;
    int rc = app_run(&app);
    shutdown_sdl();
    return rc;
}
