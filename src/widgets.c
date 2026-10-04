/*
 * widgets.c - SDL/SDL_ttf rendering + input glue for the control core.
 *
 * Compiled only when SDL2_ttf is available (see Makefile / Dockerfile).
 * All decisions (geometry, hit zones, focus order, states, version gating,
 * exit sequencing) live in the core sources; this file only renders
 * platform events.
 */
#ifdef VWIN_HAVE_SDL_TTF

#include "widgets.h"

#include <stdio.h>
#include <string.h>

/* ---- font environment bridging TTF metrics into the core compiler ---- */

static int env_measure(void *ud, const char *family, int px,
                       const char *utf8) {
    (void)px;
    WidgetLayer *w = ud;
    TTF_Font *font = NULL;
    for (int i = 0; i < w->fonts.n; i++)
        if (strcmp(w->fonts.families[i], family) == 0) font = w->fonts.fonts[i];
    if (!font) {
        /* synthetic CJK/latin advance when no face is available */
        int width = 0;
        const unsigned char *s = (const unsigned char *)utf8;
        int n = (int)strlen(utf8);
        while (n > 0) {
            uint32_t cp;
            int k = vw_utf8_next(s, n, &cp);
            if (k == 0) { s++; n--; continue; }
            width += (cp >= 0x1100) ? w->fonts.px
                                   : (w->fonts.px * 55 + 50) / 100;
            s += k; n -= k;
        }
        return width;
    }
    int ww = 0, hh = 0;
    if (TTF_SizeUTF8(font, utf8, &ww, &hh) != 0) return 0;
    return ww;
}

static void env_set_available(void *ud, const char *family, bool available) {
    (void)ud; (void)family; (void)available;
    /* availability is determined at load time into VwLayoutInput */
}

VwFontEnv g_widget_font_env = {NULL, env_measure, env_set_available};

bool widgets_load_fonts(WidgetLayer *w, const char *const *families, int n,
                        int px) {
    memset(&w->fonts, 0, sizeof(w->fonts));
    w->fonts.px = px;
    for (int i = 0; i < n && i < VW_MAX_FONT_CHAIN; i++) {
        snprintf(w->fonts.families[i], sizeof(w->fonts.families[i]), "%s",
                 families[i]);
        TTF_Font *f = TTF_OpenFont(families[i], px);
        w->fonts.fonts[i] = f; /* NULL means unavailable -> core falls back */
        w->fonts.n++;
        snprintf(w->input.font_chain[i].family,
                 sizeof(w->input.font_chain[i].family), "%s", families[i]);
        w->input.font_chain[i].available = (f != NULL);
    }
    w->input.font_chain_n = w->fonts.n;
    return w->fonts.fonts[0] != NULL ||
           (w->fonts.n > 1 && w->fonts.fonts[1] != NULL);
}

int widgets_measure_luminance(SDL_Surface *surface) {
    if (!surface) return 128;
    SDL_Surface *rgb = SDL_ConvertSurfaceFormat(
        surface, SDL_PIXELFORMAT_RGB24, 0);
    if (!rgb) return 128;
    SDL_LockSurface(rgb);
    unsigned long sum = 0;
    int samples = 0;
    int step = (rgb->w * rgb->h) / 4096;
    if (step < 1) step = 1;
    for (int i = 0; i < rgb->w * rgb->h; i += step) {
        unsigned char *p = (unsigned char *)rgb->pixels +
                           (size_t)i * 3;
        /* Rec. 601 luma */
        sum += (299 * p[0] + 587 * p[1] + 114 * p[2]) / 1000;
        samples++;
    }
    SDL_UnlockSurface(rgb);
    SDL_FreeSurface(rgb);
    return samples ? (int)(sum / (unsigned long)samples) : 128;
}

bool widgets_relayout(WidgetLayer *w) {
    g_widget_font_env.userdata = w;
    uint64_t keep_version = w->artifact.version;
    VwLayoutArtifact next;
    if (!vw_layout_compile(&w->input, &g_widget_font_env, &next)) {
        snprintf(w->status_line, sizeof(w->status_line),
                 "layout rejected: %s", next.error);
        return false;
    }
    next.version = keep_version; /* version is assigned by the server */
    w->artifact = next;
    w->dirty = true;
    return true;
}

/* ---------------- drawing ---------------- */

static void set_draw(SDL_Renderer *r, int cr, int cg, int cb, int ca) {
    SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)ca);
}

static TTF_Font *active_font(WidgetLayer *w) {
    for (int i = 0; i < w->fonts.n; i++)
        if (w->fonts.fonts[i] &&
            w->artifact.effective_family[0] != '<' &&
            strcmp(w->fonts.families[i], w->artifact.effective_family) == 0)
            return w->fonts.fonts[i];
    for (int i = 0; i < w->fonts.n; i++)
        if (w->fonts.fonts[i]) return w->fonts.fonts[i];
    return w->fonts.fonts[0]; /* may be NULL (MISSING): rectangles only */
}

static void draw_text(WidgetLayer *w, SDL_Renderer *r, const char *s,
                      int x, int y, int max_w) {
    TTF_Font *font = active_font(w);
    if (!font || !s || !*s) return;
    SDL_Color white = {245, 248, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, white);
    if (!surf) return;
    if (max_w > 0 && surf->w > max_w) {
        /* clamp via a clip rect on the texture draw */
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    if (max_w > 0 && dst.w > max_w) dst.w = max_w;
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static void draw_button(WidgetLayer *w, SDL_Renderer *r, const VwWidget *wid,
                        const char *label) {
    /* state colour: enabled neutral; start follows the FSM state */
    int rr = 70, gg = 110, bb = 200, aa = 220;
    if (wid->id == VW_WIDGET_START) {
        switch (w->start_fsm.state) {
        case VW_BTN_EXECUTING:
        case VW_BTN_CONFIRMING: rr = 232; gg = 163; bb = 61; break; /* 执行中 */
        case VW_BTN_SUCCESS: rr = 47; gg = 191; bb = 113; break;
        case VW_BTN_FAILED: rr = 239; gg = 91; bb = 91; break;
        default: break;
        }
    }
    if (!wid->enabled) { rr = gg = bb = 90; aa = 140; }
    set_draw(r, rr, gg, bb, aa);
    SDL_RenderFillRect(r, (SDL_Rect *)&wid->visual);
    set_draw(r, 255, 255, 255, 200);
    SDL_RenderDrawRect(r, (SDL_Rect *)&wid->visual);
    draw_text(w, r, label, wid->visual.x + 12,
              wid->visual.y + (wid->visual.h - w->input.requested_font_px) / 2,
              wid->visual.w - 24);
}

void widgets_draw(WidgetLayer *w, SDL_Renderer *renderer) {
    const VwLayoutArtifact *a = &w->artifact;
    /* adaptive scrims */
    set_draw(renderer, 0, 0, 0, a->title_scrim_alpha);
    SDL_RenderFillRect(renderer, (SDL_Rect *)&a->titlebar);
    if (a->has_actionbar) {
        set_draw(renderer, 0, 0, 0, a->action_scrim_alpha);
        SDL_RenderFillRect(renderer, (SDL_Rect *)&a->actionbar);
    }
    /* title, clipped before the exit keep-out */
    draw_text(w, renderer, a->title, a->title_clip.x, a->title_clip.y,
              a->title_clip.w);
    for (int i = 0; i < a->widget_count; i++) {
        const VwWidget *wid = &a->widgets[i];
        const char *label = wid->id == VW_WIDGET_START ? "\xE5\xBC\x80\xE5\xA7\x8B"
                          : wid->id == VW_WIDGET_SETTINGS ? "\xE8\xAE\xBE\xE7\xBD\xAE"
                          : "\xE9\x80\x80\xE5\x87\xBA"; /* 退出 */
        draw_button(w, renderer, wid, label);
    }
    /* debug/status line (font fallback indicator etc.) */
    char line[160];
    snprintf(line, sizeof(line),
             "v%llu font=%s%s  %s",
             (unsigned long long)a->version,
             a->effective_family,
             a->font_status == VW_FONT_MISSING ? " (MISSING,synthetic)"
             : a->font_status == VW_FONT_FALLBACK ? " (fallback)" : "",
             w->status_line);
    (void)line;
}

/* ---------------- input ---------------- */

void widgets_handle_press(WidgetLayer *w, int x, int y) {
    if (vw_capture_press(&w->capture, &w->artifact, 1, x, y)) {
        VwWidgetId id;
        if (vw_layout_hit(&w->artifact, x, y, &id) && id == VW_WIDGET_START)
            snprintf(w->status_line, sizeof(w->status_line),
                     "start pressed (%s)",
                     vw_fsm_state_name(w->start_fsm.state));
    }
}

void widgets_handle_release(WidgetLayer *w, int x, int y) {
    VwCommand cmd;
    bool cancelled = false;
    if (vw_capture_release(&w->capture, &w->artifact, 1, x, y, &cmd,
                           &cancelled)) {
        char buf[VW_CMD_BYTES];
        vw_command_serialize(&cmd, buf, sizeof(buf));
        if (cmd.id == VW_WIDGET_EXIT) {
            vw_exit_request(&w->exit_coord);
            snprintf(w->status_line, sizeof(w->status_line),
                     "exit requested (press again to force)");
        } else if (w->command_hook) {
            w->command_hook(&cmd, w->net, w->status_line,
                            sizeof(w->status_line), w->hook_ctx);
        } else {
            (void)buf;
            snprintf(w->status_line, sizeof(w->status_line),
                     "command v%llu: %s",
                     (unsigned long long)cmd.version,
                     cmd.id == VW_WIDGET_START ? "start" : "settings");
        }
    } else if (cancelled) {
        snprintf(w->status_line, sizeof(w->status_line),
                 "press cancelled: layout version changed mid-gesture");
    }
}

void widgets_focus_next(WidgetLayer *w) {
    (void)w; /* Tab order is precomputed on every artifact; the window manager
                or main loop maps SDL keyboard focus to tab_index in order. */
}

void widgets_destroy(WidgetLayer *w) {
    for (int i = 0; i < w->fonts.n; i++)
        if (w->fonts.fonts[i]) TTF_CloseFont(w->fonts.fonts[i]);
    memset(w, 0, sizeof(*w));
}
#endif /* VWIN_HAVE_SDL_TTF */
