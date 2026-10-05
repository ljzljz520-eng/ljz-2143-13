#ifndef RENDERER_H
#define RENDERER_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "font_manager.h"
#include "layout.h"

#define TEXT_CACHE_SIZE 96

typedef struct {
    char key[768];
    TextSegment segments[TEXT_MAX];
    int count;
    int w;
    int h;
    int font_size;
} CachedText;

typedef struct {
    SDL_Texture *background;
    int background_w;
    int background_h;
    double background_luminance;
    FontManager fonts;
    CachedText text_cache[TEXT_CACHE_SIZE];
    int cache_rotation;
} SceneRenderer;

bool renderer_load_background(SceneRenderer *scene, SDL_Renderer *renderer, const char *image_path);
void renderer_init_scene(SceneRenderer *scene);
void renderer_draw_scene(SceneRenderer *scene,
                        SDL_Renderer *renderer,
                        const ControlLayout *layout,
                        int window_width,
                        int window_height,
                        const char *status_line,
                        bool show_settings,
                        bool exiting,
                        const char *modal_message);
void renderer_destroy(SceneRenderer *scene);

#endif
