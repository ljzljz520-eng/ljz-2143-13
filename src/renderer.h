#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <SDL2/SDL.h>

typedef struct {
    SDL_Texture *background;
} SceneRenderer;

bool renderer_load_background(
    SceneRenderer *scene,
    SDL_Renderer *renderer,
    const char *image_path
);

/* Draw the background (and clear) WITHOUT presenting, so the widget layer can
 * composite controls in the same frame before a single SDL_RenderPresent. */
void renderer_compose_background(
    const SceneRenderer *scene,
    SDL_Renderer *renderer,
    int window_width,
    int window_height
);

void renderer_draw_background(
    const SceneRenderer *scene,
    SDL_Renderer *renderer,
    int window_width,
    int window_height
);

void renderer_destroy(SceneRenderer *scene);

#endif
