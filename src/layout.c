#include "layout.h"

#include <string.h>

void layout_set_defaults(ControlLayout *layout) {
    if (layout == NULL) return;
    memset(layout, 0, sizeof(*layout));
    layout->version = 1;
    layout->revision = 1;
    snprintf(layout->title, sizeof(layout->title), "%s", "视觉窗口控制台");
    snprintf(layout->font_family, sizeof(layout->font_family), "%s",
             "Noto Sans CJK SC, Noto Sans, DejaVu Sans, sans-serif");
    layout->font_size = 28;
    layout->background_mode = BG_ADAPTIVE;
    layout->darken = 0.42;
    layout->safe_area = (Rect){16, 16, 1248, 688};
    layout->title_rect = (Rect){24, 20, 900, 48};
    layout->buttons[ACTION_START].visual = (Rect){432, 600, 168, 52};
    layout->buttons[ACTION_SETTINGS].visual = (Rect){680, 600, 168, 52};
    layout->buttons[ACTION_EXIT].visual = (Rect){1160, 16, 104, 48};
    layout->focus_order[0] = ACTION_START;
    layout->focus_order[1] = ACTION_SETTINGS;
    layout->focus_order[2] = ACTION_EXIT;
    for (int i = 0; i < ACTION_COUNT; ++i) {
        layout->buttons[i].enabled = true;
        layout->buttons[i].state = BUTTON_IDLE;
    }
    layout_rebuild_hits(layout);
    layout->effective_darkness = layout->darken;
    snprintf(layout->actual_fonts, sizeof(layout->actual_fonts), "%s",
             "Noto Sans CJK SC -> Noto Sans -> DejaVu Sans -> embedded");
}

Rect layout_hit_from_visual(Rect visual) {
    Rect hit;
    hit.w = visual.w >= MIN_TOUCH_SIZE ? visual.w : MIN_TOUCH_SIZE;
    hit.h = visual.h >= MIN_TOUCH_SIZE ? visual.h : MIN_TOUCH_SIZE;
    hit.x = visual.w >= MIN_TOUCH_SIZE ? visual.x
                                      : visual.x + visual.w / 2 - hit.w / 2;
    hit.y = visual.h >= MIN_TOUCH_SIZE ? visual.y
                                      : visual.y + visual.h / 2 - hit.h / 2;
    return hit;
}

void layout_rebuild_hits(ControlLayout *layout) {
    for (int i = 0; i < ACTION_COUNT; ++i) {
        layout->buttons[i].hit = layout_hit_from_visual(layout->buttons[i].visual);
    }
}

void layout_update_effective_darkness(ControlLayout *layout, double image_luminance) {
    layout->measured_luminance = image_luminance;
    layout->has_adaptive_darkness = true;
    switch (layout->background_mode) {
        case BG_LIGHT:
            layout->effective_darkness = 0.02;
            break;
        case BG_DARK:
            layout->effective_darkness = layout->darken;
            break;
        case BG_ADAPTIVE:
            /* Bright/light photographs need a stronger scrim for white title
               text. Dark photographs need almost no additional darkening. */
            if (image_luminance > 0.68) {
                layout->effective_darkness = layout->darken + 0.10;
            } else if (image_luminance > 0.42) {
                layout->effective_darkness = layout->darken;
            } else if (image_luminance > 0.22) {
                layout->effective_darkness = layout->darken * 0.72;
            } else {
                layout->effective_darkness = layout->darken * 0.35;
            }
            if (layout->effective_darkness > 0.85) layout->effective_darkness = 0.85;
            if (layout->effective_darkness < 0.05) layout->effective_darkness = 0.05;
            break;
    }
}

const char *layout_background_mode_name(BackgroundMode mode) {
    switch (mode) {
        case BG_LIGHT: return "light";
        case BG_DARK: return "dark";
        case BG_ADAPTIVE: return "adaptive";
        default: return "unknown";
    }
}

const char *button_state_name(ButtonVisualState state) {
    switch (state) {
        case BUTTON_IDLE: return "idle";
        case BUTTON_BUSY: return "busy";
        case BUTTON_SUCCESS: return "success";
        case BUTTON_FAILURE: return "failure";
        case BUTTON_DISABLED: return "disabled";
        default: return "unknown";
    }
}

Rect rect_clamp_to_size(Rect r, int max_w, int max_h) {
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.w > max_w) r.w = max_w;
    if (r.h > max_h) r.h = max_h;
    if (r.x + r.w > max_w) r.x = max_w - r.w;
    if (r.y + r.h > max_h) r.y = max_h - r.h;
    return r;
}

bool rect_contains(Rect outer, Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

bool rect_intersects(Rect a, Rect b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

bool point_in_rect(int x, int y, Rect r) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
