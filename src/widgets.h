/*
 * widgets.h - SDL glue between the host-independent control core and the
 * existing renderer/window. Owns: font chain loading + text measurement,
 * background luminance sampling (adaptive darkening), artifact drawing,
 * pointer/keyboard input into the version-gated capture layer and the exit
 * coordinator.
 */
#ifndef WIDGETS_H
#define WIDGETS_H

#include <stdbool.h>

#include "core/vw.h"

#ifdef VWIN_HAVE_SDL_TTF
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

typedef struct {
    TTF_Font *fonts[VW_MAX_FONT_CHAIN];
    char families[VW_MAX_FONT_CHAIN][32];
    int n;
    int px;
} WidgetFonts;

/* Command hook bound by main: applies policy reconciliation and performs the
 * versioned network call (or offline journaling). Return text is surfaced. */
typedef void (*VwCommandHook)(const VwCommand *cmd, VwNetState net,
                              char *status_out, int status_n, void *hook_ctx);

typedef struct {
    VwLayoutInput input;       /* last compiled inputs */
    VwLayoutArtifact artifact; /* current on-screen layout */
    WidgetFonts fonts;
    VwCapture capture;
    VwButtonFsm start_fsm;
    VwExitCoord exit_coord;
    VwNetState net;
    bool dirty;               /* a new artifact must be drawn this frame */
    char status_line[128];
    VwCommandHook command_hook;
    void *hook_ctx;
} WidgetLayer;

/* Load the font chain for a pixel size. Missing families are flagged in the
 * input so the core's fallback policy applies. */
bool widgets_load_fonts(WidgetLayer *w, const char *const *families, int n,
                        int px);

/* Measure mean luminance of an SDL_Surface (0 black .. 255 bright). */
int widgets_measure_luminance(SDL_Surface *surface);

/* (Re)compile the artifact from input + measured luminance and swap it in. */
bool widgets_relayout(WidgetLayer *w);

void widgets_draw(WidgetLayer *w, SDL_Renderer *renderer);
void widgets_handle_press(WidgetLayer *w, int x, int y);
void widgets_handle_release(WidgetLayer *w, int x, int y);
void widgets_focus_next(WidgetLayer *w);
void widgets_destroy(WidgetLayer *w);

/* FontEnv callback the core uses for real text metrics. */
extern VwFontEnv g_widget_font_env;
#endif /* VWIN_HAVE_SDL_TTF */
#endif /* WIDGETS_H */
