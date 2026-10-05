#include "font_manager.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>

typedef struct {
    const char *needle;
    const char *path;
    const char *family;
} KnownFont;

static const KnownFont known_fonts[] = {
    {"NotoSansCJK", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "Noto Sans CJK SC"},
    {"NotoSansCJKsc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "Noto Sans CJK SC"},
    {"NotoSans", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "Noto Sans"},
    {"DejaVuSans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "DejaVu Sans"},
    {"LiberationSans", "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf", "Liberation Sans"},
    {"FreeSans", "/usr/share/fonts/truetype/freefont/FreeSans.ttf", "FreeSans"},
};

static char *trim_copy(const char *value, char *out, size_t out_size) {
    size_t i = 0;
    while (value != NULL && isspace((unsigned char)*value)) value++;
    if (value == NULL) { out[0] = '\0'; return out; }
    while (*value && !isspace((unsigned char)*value) && i + 1 < out_size) out[i++] = *value++;
    out[i] = '\0';
    return out;
}

static void add_candidate(FontManager *manager, const char *path) {
    struct stat st;
    if (manager->candidate_count >= 96) return;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    for (int i = 0; i < manager->candidate_count; ++i) {
        if (strcmp(manager->candidates[i], path) == 0) return;
    }
    snprintf(manager->candidates[manager->candidate_count++],
             sizeof(manager->candidates[0]), "%s", path);
}

static void scan_font_dir(FontManager *manager, const char *directory, int depth) {
    DIR *dir = opendir(directory);
    if (dir == NULL || depth < 0) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_font_dir(manager, path, depth - 1);
        } else {
            size_t len = strlen(path);
            if ((len > 4 && strcmp(path + len - 4, ".ttf") == 0) ||
                (len > 4 && strcmp(path + len - 4, ".otf") == 0) ||
                (len > 4 && strcmp(path + len - 4, ".ttc") == 0)) {
                add_candidate(manager, path);
            }
        }
    }
    closedir(dir);
}

void font_manager_init(FontManager *manager) {
    memset(manager, 0, sizeof(*manager));
    manager->ttf_available = TTF_WasInit() || TTF_Init() == 0;
    for (size_t i = 0; i < sizeof(known_fonts) / sizeof(known_fonts[0]); ++i) {
        add_candidate(manager, known_fonts[i].path);
    }
    scan_font_dir(manager, "/usr/share/fonts", 4);
    scan_font_dir(manager, "/usr/local/share/fonts", 4);
    snprintf(manager->resolved_chain, sizeof(manager->resolved_chain),
             "preferred, Noto Sans CJK SC, Noto Sans, DejaVu Sans, embedded");
}

void font_manager_destroy(FontManager *manager) {
    if (manager == NULL) return;
    for (int i = 0; i < FONT_MANAGER_CACHE_SIZE; ++i) {
        if (manager->cache[i].font != NULL) {
            TTF_CloseFont(manager->cache[i].font);
            manager->cache[i].font = NULL;
        }
    }
}

int utf8_decode_step(const unsigned char *s, size_t len, Uint32 *codepoint) {
    if (len == 0) return 0;
    unsigned char first = s[0];
    int extra;
    Uint32 cp;
    if (first < 0x80) { *codepoint = first; return 1; }
    if ((first & 0xE0) == 0xC0) { extra = 1; cp = first & 0x1F; }
    else if ((first & 0xF0) == 0xE0) { extra = 2; cp = first & 0x0F; }
    else if ((first & 0xF8) == 0xF0) { extra = 3; cp = first & 0x07; }
    else { *codepoint = 0xFFFD; return 1; }
    if ((size_t)extra + 1 > len) { *codepoint = 0xFFFD; return (int)len; }
    for (int i = 1; i <= extra; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *codepoint = 0xFFFD; return 1; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *codepoint = cp;
    return extra + 1;
}

static bool family_matches_path(const char *family, const char *path, const char *known_family) {
    if (strcasecmp(family, "sans-serif") == 0 || strcasecmp(family, "sans") == 0 ||
        strcasecmp(family, "default") == 0) {
        return true;
    }
    if (known_family != NULL) {
        if (family_canonical(family, known_family, NULL)) return true;
        if (strcmp(known_family, "Noto Sans CJK SC") == 0 &&
            family_canonical(family, "Noto Sans CJK SC", "Noto Sans SC",
                             "Noto Sans CJK", "Microsoft YaHei", "PingFang SC",
                             "WenQuanYi Micro Hei", NULL)) {
            return true;
        }
        if (strcmp(known_family, "Noto Sans") == 0 &&
            family_canonical(family, "Noto Sans", "Noto", NULL)) return true;
        if (strcmp(known_family, "DejaVu Sans") == 0 &&
            family_canonical(family, "DejaVu Sans", "DejaVu", NULL)) return true;
    }
    char compact[128];
    char family_compact[128];
    int ci = 0, fi = 0;
    const char *file = strrchr(path, '/');
    file = file ? file + 1 : path;
    for (const char *p = file; *p && ci + 1 < (int)sizeof(compact); ++p) {
        if (isalnum((unsigned char)*p)) compact[ci++] = (char)tolower((unsigned char)*p);
    }
    compact[ci] = 0;
    for (const char *p = family; *p && fi + 1 < (int)sizeof(family_compact); ++p) {
        if (isalnum((unsigned char)*p)) family_compact[fi++] = (char)tolower((unsigned char)*p);
    }
    family_compact[fi] = 0;
    return family_compact[0] && strstr(compact, family_compact) != NULL;
}

static const char *known_family_for_path(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *file = slash ? slash + 1 : path;
    if (strcasestr(file, "NotoSansCJK") != NULL ||
        strcasestr(file, "NotoSansSC") != NULL ||
        strcasestr(file, "DroidSansFallback") != NULL ||
        strcasestr(file, "WenQuanYi") != NULL)
        return "Noto Sans CJK SC";
    if (strcasestr(file, "NotoSans") != NULL) return "Noto Sans";
    if (strcasestr(file, "DejaVuSans") != NULL) return "DejaVu Sans";
    if (strcasestr(file, "LiberationSans") != NULL) return "Liberation Sans";
    if (strcasestr(file, "FreeSans") != NULL) return "FreeSans";
    for (size_t i = 0; i < sizeof(known_fonts) / sizeof(known_fonts[0]); ++i) {
        if (strcmp(known_fonts[i].path, path) == 0) return known_fonts[i].family;
    }
    return "system-font";
}

static bool family_canonical(const char *name, const char *canonical, ...) {
    va_list args;
    va_start(args, canonical);
    bool match = strcasecmp(name, canonical) == 0;
    const char *alias;
    while (!match && (alias = va_arg(args, const char *)) != NULL) {
        match = strcasecmp(name, alias) == 0;
    }
    va_end(args);
    return match;
}

static bool font_supports(TTF_Font *font, Uint32 codepoint) {
    return font != NULL &&
           (codepoint == 0 || TTF_GlyphIsProvided(font, (Uint16)codepoint));
}

static TTF_Font *open_supporting_font(const char *path, int size, Uint32 codepoint) {
    TTF_Font *font = TTF_OpenFont(path, size);
    if (font_supports(font, codepoint)) return font;
    if (font != NULL) TTF_CloseFont(font);
    size_t path_len = strlen(path);
    if (path_len > 4 && strcmp(path + path_len - 4, ".ttc") == 0) {
        /* TTC collections have several CJK faces. Find the first face that
           actually contains the requested codepoint instead of assuming face
           zero is Simplified Chinese. */
        for (long face = 1; face <= 16; ++face) {
            font = TTF_OpenFontIndex(path, size, face);
            if (font_supports(font, codepoint)) return font;
            if (font != NULL) TTF_CloseFont(font);
        }
    }
    return NULL;
}

static TTF_Font *open_font_path(const char *path, int size, const char **family) {
    TTF_Font *font = open_supporting_font(path, size, 0);
    if (font != NULL && family != NULL) *family = known_family_for_path(path);
    return font;
}

TTF_Font *font_manager_get(FontManager *manager, const char *family, int size,
                           Uint32 codepoint, const char **resolved_family) {
    if (!manager->ttf_available) return NULL;
    char list[512];
    snprintf(list, sizeof(list), "%s", family ? family : "");
    char *save = NULL;
    char *list_iter = list;

    /* Cache slot is safe when the slot either covers the codepoint, or the
       caller is measuring a control codepoint and just needs a same-size font. */
    for (char *token = strtok_r(list_iter, ",", &save); token != NULL;
             token = strtok_r(NULL, ",", &save)) {
        char name[96];
        trim_copy(token, name, sizeof(name));
        if (name[0] == '\0') continue;
        for (int c = 0; c < manager->candidate_count; ++c) {
            const char *path = manager->candidates[c];
            const char *known = known_family_for_path(path);
            if (!family_matches_path(name, path, known)) continue;
            for (int i = 0; i < FONT_MANAGER_CACHE_SIZE; ++i) {
                CachedFont *cached = &manager->cache[i];
                if (cached->font && strcmp(cached->path, path) == 0 && cached->size == size &&
                    (codepoint == 0 || TTF_GlyphIsProvided(cached->font, (Uint16)codepoint))) {
                    if (resolved_family) *resolved_family = cached->family;
                    return cached->font;
                }
            }
            const char *opened_family = known;
            TTF_Font *font = open_supporting_font(path, size, codepoint);
            if (font != NULL && (codepoint == 0 || TTF_GlyphIsProvided(font, (Uint16)codepoint))) {
                int free_slot = -1;
                int replaceable = -1;
                for (int i = 0; i < FONT_MANAGER_CACHE_SIZE; ++i) {
                    if (manager->cache[i].font == NULL) { free_slot = i; break; }
                    if (strcmp(manager->cache[i].path, path) == 0 &&
                        manager->cache[i].size == size) replaceable = i;
                }
                int slot = free_slot >= 0 ? free_slot : replaceable;
                if (slot >= 0) {
                    if (manager->cache[slot].font != NULL)
                        TTF_CloseFont(manager->cache[slot].font);
                    snprintf(manager->cache[slot].path,
                             sizeof(manager->cache[slot].path), "%s", path);
                    snprintf(manager->cache[slot].family,
                             sizeof(manager->cache[slot].family), "%s", opened_family);
                    manager->cache[slot].font = font;
                    manager->cache[slot].size = size;
                }
                if (resolved_family) *resolved_family = opened_family;
                return font;
            }
            if (font != NULL) TTF_CloseFont(font);
        }
    }

    /* Generic fallback chain: CJK first, then Latin, then any available font.
       Returning NULL lets the renderer draw the embedded tofu/ASCII glyph. */
    static const char *fallback_order[] = {
        "NotoSansCJK-Regular.ttc", "NotoSansCJK", "NotoSansSC", "NotoSans",
        "DroidSansFallback", "WenQuanYi", "DejaVuSans.ttf", "LiberationSans",
        "FreeSans"
    };
    for (size_t i = 0; i < sizeof(fallback_order) / sizeof(fallback_order[0]); ++i) {
        for (int c = 0; c < manager->candidate_count; ++c) {
            const char *path = manager->candidates[c];
            if (strcasestr(path, fallback_order[i]) == NULL) continue;
            TTF_Font *font = NULL;
            for (int slot = 0; slot < FONT_MANAGER_CACHE_SIZE; ++slot) {
                if (manager->cache[slot].font && strcmp(manager->cache[slot].path, path) == 0 &&
                    manager->cache[slot].size == size) font = manager->cache[slot].font;
            }
            if (font == NULL) font = open_supporting_font(path, size, codepoint);
            if (font != NULL && (codepoint == 0 || TTF_GlyphIsProvided(font, (Uint16)codepoint))) {
                if (resolved_family) *resolved_family = known_family_for_path(path);
                return font;
            }
            if (font != NULL) TTF_CloseFont(font);
        }
    }
    return NULL;
}

void font_manager_measure(const char *utf8_text, TTF_Font *font, int *w, int *h) {
    if (font != NULL && TTF_SizeUTF8(font, utf8_text, w, h) == 0) return;
    int chars = 0;
    const unsigned char *s = (const unsigned char *)utf8_text;
    while (*s) {
        Uint32 cp;
        int step = utf8_decode_step(s, strlen((const char *)s), &cp);
        if (cp >= 32 && cp != 0xFFFD) ++chars;
        s += step;
    }
    int size = TTF_FontHeight(font ? font : NULL);
    (void)size;
    *w = chars * 9;
    *h = 16;
}

int font_text_ascent(TTF_Font *font, int size) {
    if (font) return TTF_FontAscent(font);
    return size;
}

void font_manager_draw_embedded(SDL_Renderer *renderer, Uint32 codepoint,
                                int x, int baseline, int size, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int top = baseline - size;
    int cell_w = size >= 24 ? 7 : 5;
    int cell_h = size >= 24 ? 9 : 7;

    /* The embedded fallback deliberately stays tiny. ASCII is rendered with a
       simple one-stroke-per-column font; unknown scripts get an outlined
       glyph box ("tofu") so missing fonts remain visibly diagnosable. */
    if (codepoint < 32 || codepoint > 126) {
        SDL_Rect box = {x, top + 1, cell_w * 4, cell_h * 6};
        SDL_RenderDrawRect(renderer, &box);
        for (int i = 1; i <= 3; ++i) {
            SDL_RenderDrawLine(renderer, box.x + i, box.y + box.h - 1,
                               box.x + i, box.y);
        }
        return;
    }
    unsigned char ch = (unsigned char)codepoint;
    static const unsigned char glyphs5x7[95][5] = {
        {0,0,0,0,0},{0,0,0x17,0,0},{0x03,0,0x1f,0,3},{10,31,10,31,10},
        {25,20,31,10,19},{27,16,15,1,30},{13,18,21,10,17},{0,0,7,0,0},
        {0,14,17,0,0},{0,17,14,0,0},{21,10,31,10,21},{4,4,31,4,4},
        {0,0,0,0,16},{0,0,31,0,0},{16,8,4,2,1},{15,17,19,21,14},{14,17,1,2,31},
        {15,17,5,9,30},{31,16,30,1,30},{1,15,17,31,1},{31,16,30,1,30},
        {15,16,30,17,14},{31,1,3,6,12},{14,17,14,17,14},{14,17,15,1,14},
        {0,12,0,12,0},{0,12,0,4,8},{2,4,8,4,2},{0,31,0,31,0},{8,4,2,4,8},
        {14,1,7,16,15},{31,17,17,17,31},{31,9,5,9,31},{15,17,31,16,16},
        {15,17,17,17,15},{31,9,5,1,1},{31,9,5,9,31},{31,9,5,1,1},
        {15,17,31,17,17},{31,4,31,4,31},{0,0,31,0,0},{17,17,17,15,1},
        {31,8,4,8,31},{31,8,4,2,1},{17,19,21,25,17},{1,1,31,1,1},
        {31,16,31,0,31},{31,16,14,17,14},{15,17,17,17,15},{31,9,5,0,0},
        {15,17,17,21,13},{31,9,9,18,28},{1,1,1,31,1},{31,16,8,16,31},
        {31,16,8,4,31},{15,1,15,17,15},{15,17,31,17,17},{14,17,14,2,28},
        {30,17,30,18,15},{15,16,15,1,15},{30,17,18,17,30},{15,1,14,17,14},
        {15,1,14,1,30},{31,4,4,4,31},{17,17,17,17,31},{17,17,17,10,4},
        {17,17,21,21,10},{17,10,4,10,17},{17,17,10,4,4},{31,2,4,8,31}
    };
    const unsigned char *pattern = glyphs5x7[ch - 32];
    int px = size >= 24 ? 5 : 3;
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((pattern[col] >> (6 - row)) & 1U) {
                SDL_Rect rect = {x + col * px, top + row * px, px - 1, px - 1};
                SDL_RenderFillRect(renderer, &rect);
            }
        }
    }
}

static SDL_Texture *render_ttf_segment(SDL_Renderer *renderer, TTF_Font *font,
                                       const char *text, int bytes, SDL_Color color,
                                       int *w, int *h) {
    char buf[128];
    if (bytes >= (int)sizeof(buf)) bytes = (int)sizeof(buf) - 1;
    memcpy(buf, text, (size_t)bytes);
    buf[bytes] = '\0';
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, buf, color);
    if (surface == NULL) return NULL;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    *w = surface->w;
    *h = surface->h;
    SDL_FreeSurface(surface);
    return texture;
}

int font_manager_render_fit(FontManager *manager, SDL_Renderer *renderer,
                            const char *utf8_text, const char *preferred_family,
                            int size, SDL_Color color, int max_width,
                            TextSegment *segments, int max_segments) {
    (void)max_width;
    const unsigned char *s = (const unsigned char *)utf8_text;
    size_t total = strlen(utf8_text);
    size_t pos = 0;
    int count = 0;
    while (pos < total && count < max_segments) {
        Uint32 cp;
        int step = utf8_decode_step(s + pos, total - pos, &cp);
        const char *resolved = "embedded";
        TTF_Font *font = manager->ttf_available
            ? font_manager_get(manager, preferred_family, size, cp, &resolved)
            : NULL;
        SDL_Texture *texture = NULL;
        int w = 9, h = size;
        if (font) texture = render_ttf_segment(renderer, font, (const char *)s + pos, step, color, &w, &h);
        segments[count].x = 0;
        segments[count].y = 0;
        segments[count].texture = texture;
        int embedded_width = cp == ' ' ? size / 2 : (size >= 24 ? 28 : 20);
        int embedded_height = size + 2;
        segments[count].w = texture ? w : embedded_width;
        segments[count].h = texture ? h : embedded_height;
        segments[count].baseline = font ? TTF_FontAscent(font) : embedded_height - 2;
        segments[count].embedded = texture == NULL;
        segments[count].codepoint = cp;
        (void)resolved;
        ++count;
        pos += (size_t)step;
    }
    return count;
}
