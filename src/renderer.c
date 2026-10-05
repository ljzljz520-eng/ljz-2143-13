#include "renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

typedef struct {
    int width;
    int ascent;
    int segment_count;
    bool embedded;
} TextMetrics;

static SDL_Color make_color(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return (SDL_Color){r, g, b, a};
}

void renderer_init_scene(SceneRenderer *scene) {
    memset(scene, 0, sizeof(*scene));
    font_manager_init(&scene->fonts);
}

static double sample_surface_luminance(SDL_Surface *surface) {
    SDL_Surface *converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    if (converted == NULL) return 0.35;
    if (SDL_LockSurface(converted) != 0) {
        SDL_FreeSurface(converted);
        return 0.35;
    }
    unsigned char *pixels = converted->pixels;
    int stride = converted->pitch;
    int step_x = converted->w > 160 ? converted->w / 120 : 4;
    int step_y = converted->h > 120 ? converted->h / 90 : 4;
    if (step_x <= 0) step_x = 1;
    if (step_y <= 0) step_y = 1;
    double total = 0;
    int samples = 0;
    for (int y = 0; y < converted->h; y += step_y) {
        for (int x = 0; x < converted->w; x += step_x) {
            unsigned char *p = pixels + y * stride + x * 4;
            double lum = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            total += lum / 255.0;
            ++samples;
        }
    }
    SDL_UnlockSurface(converted);
    SDL_FreeSurface(converted);
    return samples > 0 ? total / samples : 0.35;
}

bool renderer_load_background(SceneRenderer *scene, SDL_Renderer *renderer, const char *image_path) {
    if (scene == NULL || renderer == NULL || image_path == NULL) return false;
    SDL_Surface *surface = IMG_Load(image_path);
    if (surface == NULL) {
        fprintf(stderr, "IMG_Load failed for %s: %s\n", image_path, IMG_GetError());
        SDL_Surface *fallback = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_WIDTH, SCREEN_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
        if (fallback == NULL) return false;
        SDL_FillRect(fallback, NULL, SDL_MapRGB(fallback->format, 42, 58, 82));
        surface = fallback;
    }
    scene->background_luminance = sample_surface_luminance(surface);
    scene->background_w = surface->w;
    scene->background_h = surface->h;
    scene->background = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return scene->background != NULL;
}

static void destroy_cached_text(CachedText *cached) {
    for (int i = 0; i < cached->count; ++i) {
        if (cached->segments[i].texture != NULL) {
            SDL_DestroyTexture(cached->segments[i].texture);
            cached->segments[i].texture = NULL;
        }
    }
    memset(cached, 0, sizeof(*cached));
}

static CachedText *find_cached_text(SceneRenderer *scene, const char *key) {
    for (int i = 0; i < TEXT_CACHE_SIZE; ++i) {
        if (scene->text_cache[i].count > 0 && strcmp(scene->text_cache[i].key, key) == 0) {
            return &scene->text_cache[i];
        }
    }
    return NULL;
}

static CachedText *available_cache_slot(SceneRenderer *scene) {
    int start = scene->cache_rotation;
    for (int i = 0; i < TEXT_CACHE_SIZE; ++i) {
        int index = (start + i) % TEXT_CACHE_SIZE;
        if (scene->text_cache[index].count == 0) {
            scene->cache_rotation = (index + 1) % TEXT_CACHE_SIZE;
            return &scene->text_cache[index];
        }
    }
    CachedText *victim = &scene->text_cache[start];
    destroy_cached_text(victim);
    scene->cache_rotation = (start + 1) % TEXT_CACHE_SIZE;
    return victim;
}

static int segment_width(FontManager *fonts, Uint32 cp, TTF_Font *font, int size) {
    if (font) {
        int w = 0;
        int h = 0;
        char utf8[5] = {0};
        if (cp < 0x80) {
            utf8[0] = (char)cp;
        } else if (cp < 0x800) {
            utf8[0] = (char)(0xC0 | (cp >> 6));
            utf8[1] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf8[0] = (char)(0xE0 | (cp >> 12));
            utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[2] = (char)(0x80 | (cp & 0x3F));
        } else {
            utf8[0] = (char)(0xF0 | (cp >> 18));
            utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[3] = (char)(0x80 | (cp & 0x3F));
        }
        if (TTF_SizeUTF8(font, utf8, &w, &h) == 0 && w > 0) return w;
    }
    (void)fonts;
    return cp == ' ' ? size / 2 : (size >= 24 ? 8 : 6);
}

static char *fit_text_to_width(FontManager *fonts, const char *text,
                               const char *family, int size, int max_width,
                               TextMetrics *metrics) {
    size_t total = strlen(text);
    char *result = calloc(total + 4, 1);
    if (result == NULL) return NULL;
    int width = 0;
    size_t pos = 0;
    size_t fit_bytes = 0;
    const char *resolved = NULL;
    TTF_Font *dot_font = font_manager_get(fonts, family, size, '.', NULL);
    int dot_w = segment_width(fonts, '.', dot_font, size);
    while (pos < total) {
        Uint32 cp;
        int step = utf8_decode_step((const unsigned char *)text + pos, total - pos, &cp);
        TTF_Font *font = font_manager_get(fonts, family, size, cp, &resolved);
        int cw = segment_width(fonts, cp, font, size);
        if (width + cw + dot_w * 3 > max_width) {
            break;
        }
        memcpy(result + fit_bytes, text + pos, (size_t)step);
        fit_bytes += (size_t)step;
        width += cw;
        pos += (size_t)step;
    }
    if (pos < total) {
        strcpy(result + fit_bytes, "...");
        width += dot_w * 3;
    }
    metrics->width = width;
    metrics->ascent = font_text_ascent(font_manager_get(fonts, family, size, 0, &resolved), size);
    metrics->embedded = false;
    metrics->segment_count = 0;
    for (const char *p = result; *p;) {
        Uint32 cp;
        int step = utf8_decode_step((const unsigned char *)p, strlen(p), &cp);
        if (!font_manager_get(fonts, family, size, cp, NULL)) metrics->embedded = true;
        ++metrics->segment_count;
        p += step;
    }
    return result;
}

static void draw_text_fitted(SceneRenderer *scene, SDL_Renderer *renderer,
                             const char *text, const char *family, int size,
                             SDL_Color color, Rect box, bool center_x, bool center_y,
                             const char *cache_suffix) {
    char key[900];
    TextMetrics metrics = {0};
    char *fitted = fit_text_to_width(&scene->fonts, text, family, size, box.w, &metrics);
    const char *draw_text = fitted ? fitted : text;
    snprintf(key, sizeof(key), "%s|%d|%d-%d-%d-%d|%s|%s|%d",
             draw_text, size, color.r, color.g, color.b, color.a,
             family, cache_suffix ? cache_suffix : "", box.w);
    CachedText *cached = find_cached_text(scene, key);
    if (cached == NULL) {
        cached = available_cache_slot(scene);
        cached->count = font_manager_render_fit(
            &scene->fonts, renderer, draw_text, family, size, color, box.w,
            cached->segments, TEXT_MAX);
        cached->w = 0;
        cached->h = 0;
        cached->font_size = size;
        for (int i = 0; i < cached->count; ++i) {
            cached->segments[i].x = cached->w;
            cached->w += cached->segments[i].w;
            if (cached->segments[i].h > cached->h) cached->h = cached->segments[i].h;
        }
        snprintf(cached->key, sizeof(cached->key), "%s", key);
    }
    int x = box.x + (center_x ? (box.w - cached->w) / 2 : 0);
    int y = box.y + (center_y ? (box.h - cached->h) / 2 : 0);
    for (int i = 0; i < cached->count; ++i) {
        TextSegment *segment = &cached->segments[i];
        if (segment->texture) {
            SDL_Rect dst = {x + segment->x, y, segment->w, segment->h};
            SDL_RenderCopy(renderer, segment->texture, NULL, &dst);
        } else {
            font_manager_draw_embedded(renderer, segment->codepoint,
                                       x + segment->x,
                                       y + segment->baseline,
                                       size, color);
        }
    }
    free(fitted);
}

static void draw_rect_outline(SDL_Renderer *renderer, Rect r, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect s = {r.x, r.y, r.w, r.h};
    SDL_RenderDrawRect(renderer, &s);
}

static void draw_button(SceneRenderer *scene, SDL_Renderer *renderer,
                        const ControlLayout *layout, ControlAction action) {
    const ButtonControl *button = &layout->buttons[action];
    Rect r = button->visual;
    SDL_Color fill;
    if (!button->enabled) fill = make_color(100, 116, 139, 220);
    else if (button->state == BUTTON_BUSY) fill = make_color(217, 119, 6, 235);
    else if (button->state == BUTTON_SUCCESS) fill = make_color(22, 163, 74, 235);
    else if (button->state == BUTTON_FAILURE) fill = make_color(220, 38, 38, 235);
    else fill = make_color(37, 99, 235, 225);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
    SDL_Rect rect = {r.x, r.y, r.w, r.h};
    SDL_RenderFillRect(renderer, &rect);
    SDL_Color border = button->pressed ? make_color(255, 255, 255, 255) : make_color(226, 232, 240, 190);
    draw_rect_outline(renderer, r, border);
    const char *label = control_action_label(action);
    if (action == ACTION_START && button->state == BUTTON_BUSY) label = "执行中…";
    if (action == ACTION_START && button->state == BUTTON_SUCCESS) label = "成功";
    if (action == ACTION_START && button->state == BUTTON_FAILURE) label = "失败";
    draw_text_fitted(scene, renderer, label, layout->font_family, 20,
                     make_color(255, 255, 255, 255), r, true, true, "button");
    if (button->focus) {
        Rect focus = {r.x - 4, r.y - 4, r.w + 8, r.h + 8};
        draw_rect_outline(renderer, focus, make_color(147, 197, 253, 255));
    }
    draw_rect_outline(renderer, button->hit, make_color(74, 222, 128, 180));
}

static void draw_modal(SDL_Renderer *renderer, const char *line1, const char *line2) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 15, 23, 42, 210);
    SDL_Rect backdrop = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    SDL_RenderFillRect(renderer, &backdrop);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 245);
    SDL_Rect panel = {320, 230, 640, 260};
    SDL_RenderFillRect(renderer, &panel);
    draw_rect_outline(renderer, panel, make_color(37, 99, 235, 255));
    (void)line1;
    (void)line2;
}

static void draw_text_line(SceneRenderer *scene, SDL_Renderer *renderer,
                           const char *text, int x, int y, int size,
                           SDL_Color color, int max_width) {
    Rect box = {x, y, max_width, size + 10};
    draw_text_fitted(scene, renderer, text,
                     "Noto Sans CJK SC, Noto Sans, DejaVu Sans, sans-serif",
                     size, color, box, false, false, "hud");
}

void renderer_draw_scene(SceneRenderer *scene, SDL_Renderer *renderer,
                         const ControlLayout *layout, int window_width,
                         int window_height, const char *status_line,
                         bool show_settings, bool exiting,
                         const char *modal_message) {
    SDL_SetRenderDrawColor(renderer, 12, 18, 30, 255);
    SDL_RenderClear(renderer);

    if (scene->background) {
        double scale_x = (double)window_width / scene->background_w;
        double scale_y = (double)window_height / scene->background_h;
        double scale = scale_x > scale_y ? scale_x : scale_y;
        int w = (int)lround(scene->background_w * scale);
        int h = (int)lround(scene->background_h * scale);
        SDL_Rect dst = {(window_width - w) / 2, (window_height - h) / 2, w, h};
        SDL_RenderCopy(renderer, scene->background, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, 42, 58, 82, 255);
        SDL_Rect fallback = {0, 0, window_width, window_height};
        SDL_RenderFillRect(renderer, &fallback);
    }

    Uint8 alpha = (Uint8)lround(layout->effective_darkness * 255.0);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
    SDL_RenderFillRect(renderer, &(SDL_Rect){0, 0, window_width, window_height});

    draw_rect_outline(renderer, layout->safe_area, make_color(56, 189, 248, 210));
    draw_text_fitted(scene, renderer, layout->title, layout->font_family,
                     layout->font_size, make_color(255, 255, 255, 255),
                     layout->title_rect, false, true, "title");
    for (int i = 0; i < ACTION_COUNT; ++i) {
        draw_button(scene, renderer, layout, (ControlAction)i);
    }
    draw_text_line(scene, renderer, status_line ? status_line : "", 24, 668, 18,
                   make_color(226, 232, 240, 235), 900);
    char font_hud[640];
    snprintf(font_hud, sizeof(font_hud), "v%d | %s | 亮度%.2f | 暗化%.0f%% | 字体:%s",
             layout->version, layout_background_mode_name(layout->background_mode),
             layout->measured_luminance, layout->effective_darkness * 100.0,
             layout->actual_fonts);
    draw_text_line(scene, renderer, font_hud, 24, 690, 15,
                   make_color(203, 213, 225, 220), 1180);

    if (show_settings) {
        draw_modal(renderer, NULL, NULL);
        draw_text_line(scene, renderer, "设置 / 本地策略", 360, 270, 30,
                       make_color(15, 23, 42, 255), 560);
        draw_text_line(scene, renderer,
                       "在线：服务器策略决定开始/设置；断网：仅设置与受控退出允许。",
                       360, 330, 20, make_color(30, 41, 59, 255), 560);
        draw_text_line(scene, renderer,
                       "退出始终可达；退出会保存业务、记录恢复并释放渲染资源。",
                       360, 370, 20, make_color(30, 41, 59, 255), 560);
        draw_text_line(scene, renderer, "按 Esc 或点击“设置”关闭。", 360, 430, 18,
                       make_color(71, 85, 105, 255), 560);
    }
    if (exiting) {
        draw_modal(renderer, NULL, NULL);
        draw_text_line(scene, renderer, "正在受控退出…", 390, 280, 32,
                       make_color(15, 23, 42, 255), 500);
        draw_text_line(scene, renderer, modal_message ? modal_message :
                       "等待业务确认保存；服务不可达时将按超时路径退出。",
                       360, 350, 20, make_color(30, 41, 59, 255), 560);
    }
    SDL_RenderPresent(renderer);
}

void renderer_destroy(SceneRenderer *scene) {
    if (scene == NULL) return;
    for (int i = 0; i < TEXT_CACHE_SIZE; ++i) {
        for (int j = 0; j < scene->text_cache[i].count; ++j) {
            if (scene->text_cache[i].segments[j].texture != NULL) {
                SDL_DestroyTexture(scene->text_cache[i].segments[j].texture);
                scene->text_cache[i].segments[j].texture = NULL;
            }
        }
        scene->text_cache[i].count = 0;
    }
    if (scene->background != NULL) {
        SDL_DestroyTexture(scene->background);
        scene->background = NULL;
    }
    font_manager_destroy(&scene->fonts);
}
