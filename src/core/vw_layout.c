/*
 * vw_layout.c - deterministic layout compiler.
 *
 * Rules (mirrored by the server-side validator in server/server.py):
 *  - exit owns a reserved top-right zone; long titles are clipped before it
 *  - hit rects never overlap each other; the exit hit zone may grow, others
 *    are shaved instead
 *  - touch targets are clamped up to min_touch px
 *  - font chain falls back to first available family; invalid bytes reported
 *  - bright background -> stronger scrim (adaptive darkening); dark -> veil
 */
#include "vw.h"

#include <stdio.h>
#include <string.h>

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int rect_intersects(const VwRect *a, const VwRect *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static void seterr(VwLayoutArtifact *out, const char *msg) {
    snprintf(out->error, sizeof(out->error), "%s", msg);
}

int vw_utf8_next(const unsigned char *s, int n, uint32_t *cp) {
    if (n <= 0) return 0;
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len; uint32_t v;
    if ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; }
    else return 0;
    if (n < len) return 0;
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) ||
        (len == 4 && (v < 0x10000 || v > 0x10FFFF))) return 0;
    *cp = v;
    return len;
}

uint32_t vw_layout_spec_hash(const VwLayoutInput *in) {
    char canon[512];
    snprintf(canon, sizeof(canon),
        "%d|%d|%d|%s|%d|%d|%d|%d|%d|%d|%d",
        in->screen_w, in->screen_h, in->dpi, in->title,
        in->requested_font_px, (int)in->arrangement, in->min_touch,
        in->exit_hit_grow, in->bg_luminance_0_255,
        in->start_enabled ? 1 : 0, in->settings_enabled ? 1 : 0);
    uint32_t h = 2166136261u;
    for (size_t i = 0; canon[i]; i++) {
        h ^= (unsigned char)canon[i];
        h *= 16777619u;
    }
    return h;
}

static int fallback_advance(int px, uint32_t cp) {
    if (cp < 0x80) return (px * 55 + 50) / 100;
    if (cp >= 0x1100) return px;
    return (px * 70 + 50) / 100;
}

static int count_invalid(const char *utf8) {
    int missing = 0;
    const unsigned char *s = (const unsigned char *)utf8;
    int n = (int)strlen(utf8);
    while (n > 0) {
        uint32_t cp;
        int k = vw_utf8_next(s, n, &cp);
        if (k == 0) { missing++; s++; n--; continue; }
        s += k; n -= k;
    }
    return missing;
}

static int measure_text(VwFontEnv *env, const char *family, int px,
                        const char *utf8, int *missing_out) {
    if (missing_out) *missing_out = count_invalid(utf8);
    if (env && env->measure)
        return env->measure(env->userdata, family, px, utf8);
    int width = 0;
    const unsigned char *s = (const unsigned char *)utf8;
    int n = (int)strlen(utf8);
    while (n > 0) {
        uint32_t cp;
        int k = vw_utf8_next(s, n, &cp);
        if (k == 0) { s++; n--; continue; }
        width += fallback_advance(px, cp);
        s += k; n -= k;
    }
    return width;
}

/* fit title so that width(text-with-ellipsis) <= max_w */
static void fit_title(const char *src, char *dst, int dstn, VwFontEnv *env,
                      const char *family, int px, int max_w,
                      bool *truncated, int *missing) {
    int total_missing = 0;
    if (measure_text(env, family, px, src, &total_missing) <= max_w) {
        snprintf(dst, dstn, "%s", src);
        *truncated = false;
        *missing = total_missing;
        return;
    }
    char tmp[VW_MAX_TITLE_BYTES];
    int ti = 0;
    tmp[0] = '\0';
    const unsigned char *s = (const unsigned char *)src;
    int n = (int)strlen(src);
    while (n > 0 && ti + 4 < (int)sizeof(tmp) - 4) {
        uint32_t cp;
        int k = vw_utf8_next(s, n, &cp);
        if (k == 0) { total_missing++; s++; n--; continue; }
        char probe[VW_MAX_TITLE_BYTES];
        snprintf(probe, sizeof(probe), "%.*s\xE2\x80\xA6", ti, tmp);
        if (measure_text(env, family, px, probe, NULL) > max_w) break;
        memcpy(tmp + ti, s, (size_t)k);
        ti += k;
        tmp[ti] = '\0';
        s += k; n -= k;
    }
    if (ti == 0) { /* even one glyph does not fit; emit ellipsis alone */
        snprintf(dst, dstn, "\xE2\x80\xA6");
    } else {
        snprintf(dst, dstn, "%s\xE2\x80\xA6", tmp);
    }
    *truncated = true;
    *missing = total_missing;
}

static VwWidget *add_widget(VwLayoutArtifact *a, VwWidgetId id,
                            const VwRect *visual, int touch_min, bool enabled) {
    VwWidget *w = &a->widgets[a->widget_count++];
    w->id = id;
    w->visual = *visual;
    w->enabled = enabled;
    VwRect h = *visual;
    int cx = h.x + h.w / 2, cy = h.y + h.h / 2;
    if (h.w < touch_min) { h.w = touch_min; h.x = cx - h.w / 2; }
    if (h.h < touch_min) { h.h = touch_min; h.y = cy - h.h / 2; }
    w->hit = h;
    w->tab_index = 0;
    return w;
}

bool vw_layout_compile(const VwLayoutInput *in, VwFontEnv *env,
                       VwLayoutArtifact *out) {
    memset(out, 0, sizeof(*out));
    if (!in) { seterr(out, "null argument"); return false; }
    if (in->screen_w < 240 || in->screen_h < 240) {
        seterr(out, "screen too small"); return false;
    }
    if (in->dpi <= 0) { seterr(out, "invalid dpi"); return false; }
    if (in->font_chain_n <= 0) { seterr(out, "empty font chain"); return false; }
    if (in->title[0] == '\0') { seterr(out, "empty title"); return false; }
    if (strlen(in->title) >= VW_MAX_TITLE_BYTES) {
        seterr(out, "title too long"); return false;
    }
    int touch_min = in->min_touch > 0 ? in->min_touch : VW_MIN_TOUCH_TARGET;

    /* font fallback: first available family in the chain */
    const char *family = NULL;
    for (int i = 0; i < in->font_chain_n; i++) {
        if (env && env->set_available)
            env->set_available(env->userdata, in->font_chain[i].family,
                               in->font_chain[i].available);
        if (in->font_chain[i].available && family == NULL)
            family = in->font_chain[i].family;
    }
    if (family) {
        out->font_status = strcmp(family, in->font_chain[0].family) == 0
                               ? VW_FONT_OK : VW_FONT_FALLBACK;
        snprintf(out->effective_family, sizeof(out->effective_family), "%s",
                 family);
    } else {
        out->font_status = VW_FONT_MISSING;
        family = in->font_chain[0].family;
        snprintf(out->effective_family, sizeof(out->effective_family),
                 "<missing:%.21s>", family);
    }

    int font_px = clampi(in->requested_font_px, VW_MIN_FONTSIZE,
                         VW_MAX_FONTSIZE);
    out->effective_font_px = font_px;

    /* reserved exit zone, top-right */
    int margin = 8;
    int exit_size = clampi(font_px + 18, 36, 72);
    int exit_x = in->screen_w - margin - exit_size;
    int exit_y = margin;
    VwRect exit_visual = { exit_x, exit_y, exit_size, exit_size };

    int titlebar_h = exit_size + 2 * margin;
    int natural = font_px * 2 + 12;
    if (natural > titlebar_h) titlebar_h = natural;
    out->titlebar = (VwRect){0, 0, in->screen_w, titlebar_h};

    /* title clipped before the exit keep-out band: it can never cover exit */
    int title_x = margin * 2;
    int title_max_w =
        exit_x - VW_EXIT_KEEP_OUT_MARGIN - title_x;
    if (title_max_w < font_px * 2) {
        seterr(out, "usable title width too small"); return false;
    }
    VwRect title_clip = {title_x, (titlebar_h - font_px - 8) / 2,
                         title_max_w, font_px + 8};
    bool trunc = false; int missing = 0;
    char fitted[VW_MAX_TITLE_BYTES];
    fit_title(in->title, fitted, sizeof(fitted), env, family, font_px,
              title_max_w, &trunc, &missing);
    snprintf(out->title, sizeof(out->title), "%s", fitted);
    out->title_truncated = trunc;
    out->missing_glyph_count = missing;
    out->title_clip = title_clip;

    int gap = 12;
    int btn_h = clampi(font_px + 20, touch_min, 88);
    int start_w = measure_text(env, family, font_px,
                               "\xE5\xBC\x80\xE5\xA7\x8B", NULL) + 2 * gap + 12;
    int settings_w = measure_text(env, family, font_px,
                                  "\xE8\xAE\xBE\xE7\xBD\xAE", NULL)
                     + 2 * gap + 12;
    if (start_w < touch_min * 2) start_w = touch_min * 2;
    if (settings_w < touch_min * 2) settings_w = touch_min * 2;

    VwRect start_visual, settings_visual;
    bool use_split = in->arrangement == VW_ARR_SPLIT;
    if (!use_split && in->arrangement == VW_ARR_INLINE) {
        int sx = exit_x - VW_EXIT_KEEP_OUT_MARGIN - settings_w;
        settings_visual = (VwRect){sx, (titlebar_h - btn_h) / 2,
                                   settings_w, btn_h};
        start_visual = (VwRect){sx - gap - start_w,
                                (titlebar_h - btn_h) / 2, start_w, btn_h};
        /* crowding the title? degrade to a bottom action bar automatically */
        if (start_visual.x < title_x + title_max_w + gap)
            use_split = true;
    }
    if (use_split) {
        out->has_actionbar = true;
        int bar_h = btn_h + 2 * gap;
        out->actionbar = (VwRect){0, in->screen_h - bar_h, in->screen_w,
                                  bar_h};
        start_visual = (VwRect){margin, in->screen_h - bar_h + gap, start_w,
                                btn_h};
        settings_visual = (VwRect){margin + start_w + gap,
                                   in->screen_h - bar_h + gap, settings_w,
                                   btn_h};
    }

    add_widget(out, VW_WIDGET_START, &start_visual, touch_min,
               in->start_enabled);
    add_widget(out, VW_WIDGET_SETTINGS, &settings_visual, touch_min,
               in->settings_enabled);
    VwWidget *exit_w = add_widget(out, VW_WIDGET_EXIT, &exit_visual, touch_min,
                                  in->exit_enabled);
    exit_w->hit.x -= in->exit_hit_grow;
    exit_w->hit.y -= in->exit_hit_grow;
    exit_w->hit.w += 2 * in->exit_hit_grow;
    exit_w->hit.h += 2 * in->exit_hit_grow;
    if (exit_w->hit.x < 0) exit_w->hit.x = 0;
    if (exit_w->hit.y < 0) exit_w->hit.y = 0;
    if (exit_w->hit.x + exit_w->hit.w > in->screen_w)
        exit_w->hit.w = in->screen_w - exit_w->hit.x;
    if (exit_w->hit.y + exit_w->hit.h > in->screen_h)
        exit_w->hit.h = in->screen_h - exit_w->hit.y;

    for (int i = 0; i < out->widget_count; i++) {
        VwWidget *w = &out->widgets[i];
        if (w->id == VW_WIDGET_EXIT) continue;
        VwRect *h = &w->hit;
        if (h->x < 0) { h->w += h->x; h->x = 0; }
        if (h->y < 0) { h->h += h->y; h->y = 0; }
        if (h->x + h->w > in->screen_w) h->w = in->screen_w - h->x;
        if (h->y + h->h > in->screen_h) h->h = in->screen_h - h->y;
        if (rect_intersects(h, &exit_w->hit)) {
            int overlap = (h->x + h->w) - exit_w->hit.x;
            if (overlap > 0) h->w -= overlap;
        }
    }

    for (int i = 0; i < out->widget_count; i++)
        for (int j = i + 1; j < out->widget_count; j++)
            if (rect_intersects(&out->widgets[i].hit,
                                &out->widgets[j].hit)) {
                seterr(out, "hit rect overlap after resolution");
                return false;
            }

    /* focus order: visual top-down then left-right; exit last in tab ring */
    VwWidget *ord[VW_MAX_BUTTONS];
    int k = 0;
    for (int i = 0; i < out->widget_count; i++)
        if (out->widgets[i].id != VW_WIDGET_EXIT) ord[k++] = &out->widgets[i];
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0; j--) {
            VwRect *a = &ord[j - 1]->visual, *b = &ord[j]->visual;
            if (a->y > b->y || (a->y == b->y && a->x > b->x)) {
                VwWidget *t = ord[j - 1]; ord[j - 1] = ord[j]; ord[j] = t;
            }
        }
    int tab = 1;
    for (int i = 0; i < k; i++) ord[i]->tab_index = tab++;
    exit_w->tab_index = tab;

    /* adaptive darkening from measured background luminance */
    int L = clampi(in->bg_luminance_0_255, 0, 255);
    if (L < 64) {
        out->title_scrim_alpha = 28;
        out->action_scrim_alpha = 20;
    } else {
        out->title_scrim_alpha = 40 + L * 3 / 5;
        out->action_scrim_alpha = 30 + L / 2;
    }

    out->spec_hash = vw_layout_spec_hash(in);
    return true;
}

bool vw_layout_hit(const VwLayoutArtifact *a, int x, int y, VwWidgetId *id) {
    if (!a) return false;
    VwRect p = {x, y, 1, 1};
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < a->widget_count; i++) {
            const VwWidget *w = &a->widgets[i];
            bool is_exit = w->id == VW_WIDGET_EXIT;
            if ((pass == 0) != is_exit) continue;
            if (w->enabled && rect_intersects(&p, &w->hit)) {
                if (id) *id = w->id;
                return true;
            }
        }
    }
    return false;
}
