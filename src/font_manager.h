#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>

#define FONT_MANAGER_CACHE_SIZE 10
#define TEXT_MAX 256

typedef struct {
    char family[96];
    char path[512];
    TTF_Font *font;
    int size;
} CachedFont;

typedef struct {
    CachedFont cache[FONT_MANAGER_CACHE_SIZE];
    char candidates[96][512];
    int candidate_count;
    bool ttf_available;
    char resolved_chain[768];
} FontManager;

typedef struct {
    int x;
    int y;
    SDL_Texture *texture;
    int w;
    int h;
    int baseline;
    bool embedded;
    Uint32 codepoint;
} TextSegment;

void font_manager_init(FontManager *manager);
void font_manager_destroy(FontManager *manager);
TTF_Font *font_manager_get(FontManager *manager, const char *family, int size,
                           Uint32 codepoint, const char **resolved_family);
int font_manager_render_fit(FontManager *manager, SDL_Renderer *renderer,
                            const char *utf8_text, const char *preferred_family,
                            int size, SDL_Color color, int max_width,
                            TextSegment *segments, int max_segments);
void font_manager_measure(const char *utf8_text, TTF_Font *font, int *w, int *h);
void font_manager_draw_embedded(SDL_Renderer *renderer, Uint32 codepoint,
                                int x, int baseline, int size, SDL_Color color);
int font_text_ascent(TTF_Font *font, int size);
int utf8_decode_step(const unsigned char *s, size_t len, Uint32 *codepoint);

#endif
