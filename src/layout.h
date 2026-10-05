#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MIN_TOUCH_SIZE 48
#define ACTION_COUNT 3
#define TITLE_TEXT_MAX 80
#define FONT_FAMILY_MAX 256

typedef enum {
    ACTION_START = 0,
    ACTION_SETTINGS = 1,
    ACTION_EXIT = 2,
    ACTION_NONE = 3
} ControlAction;

typedef enum {
    BG_LIGHT = 0,
    BG_DARK = 1,
    BG_ADAPTIVE = 2
} BackgroundMode;

typedef enum {
    BUTTON_IDLE = 0,
    BUTTON_BUSY = 1,
    BUTTON_SUCCESS = 2,
    BUTTON_FAILURE = 3,
    BUTTON_DISABLED = 4
} ButtonVisualState;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

typedef struct {
    Rect visual;
    Rect hit;
    ButtonVisualState state;
    bool enabled;
    bool pressed;
    bool focus;
} ButtonControl;

typedef struct {
    int version;
    int revision;
    char title[TITLE_TEXT_MAX + 1];
    char font_family[FONT_FAMILY_MAX + 1];
    int font_size;
    BackgroundMode background_mode;
    double darken;
    Rect safe_area;
    Rect title_rect;
    ButtonControl buttons[ACTION_COUNT];
    ControlAction focus_order[ACTION_COUNT];
    bool has_adaptive_darkness;
    double measured_luminance;
    double effective_darkness;
    char actual_fonts[512];
} ControlLayout;

static inline const char *control_action_name(ControlAction action) {
    switch (action) {
        case ACTION_START: return "start";
        case ACTION_SETTINGS: return "settings";
        case ACTION_EXIT: return "exit";
        default: return "none";
    }
}

static inline const char *control_action_label(ControlAction action) {
    switch (action) {
        case ACTION_START: return "开始";
        case ACTION_SETTINGS: return "设置";
        case ACTION_EXIT: return "退出";
        default: return "";
    }
}

void layout_set_defaults(ControlLayout *layout);
Rect layout_hit_from_visual(Rect visual);
void layout_rebuild_hits(ControlLayout *layout);
void layout_update_effective_darkness(ControlLayout *layout, double image_luminance);
const char *layout_background_mode_name(BackgroundMode mode);
const char *button_state_name(ButtonVisualState state);
Rect rect_clamp_to_size(Rect r, int max_w, int max_h);
bool rect_contains(Rect outer, Rect inner);
bool rect_intersects(Rect a, Rect b);
bool point_in_rect(int x, int y, Rect r);

#endif
