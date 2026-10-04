#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#if defined(__has_include)
#  if __has_include(<SDL2/SDL_ttf.h>)
#    include <SDL2/SDL_ttf.h>
#    define VWIN_HAVE_SDL_TTF 1
#  endif
#endif

#include "renderer.h"
#include "window.h"
#include "core/vw.h"
#ifdef VWIN_HAVE_SDL_TTF
#include "widgets.h"
#include "vw_net_client.h"
#endif

#define WINDOW_TITLE "背景窗口 · 控件管理"
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define BACKGROUND_IMAGE_PATH "assets/background.png"

/* Font preference chain; the first face that loads wins, later ones are
 * fallbacks (CJK coverage is required for the Chinese UI strings). */
static const char *kFontFamilies[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
};

static bool init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
    int initted = IMG_Init(img_flags);
    if ((initted & img_flags) == 0) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return false;
    }
#ifdef VWIN_HAVE_SDL_TTF
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        /* non-fatal: core keeps laying out with synthetic metrics */
    }
#endif
    return true;
}

static void shutdown_sdl(void) {
#ifdef VWIN_HAVE_SDL_TTF
    if (TTF_WasInit()) TTF_Quit();
#endif
    IMG_Quit();
    SDL_Quit();
}

#ifdef VWIN_HAVE_SDL_TTF
/* ---- exit dependency callbacks (see docs/design.md section 9) ---- */
static bool exit_save(VwExitDeps *d, void *ctx) {
    (void)ctx;
    /* In the real product this waits for the business "saved" ack. The demo
     * confirms immediately; a second exit press forces the coordinator. */
    d->save_confirmed = true;
    return true;
}

static bool exit_flush(VwExitDeps *d, void *ctx) {
    VwNetClient *net = ctx;
    /* one bounded flush attempt; unreachable server must not block exit */
    bool ok = vwn_ping(net);
    d->flush_code = ok ? 0 : 1;
    return ok;
}

static bool exit_release(VwExitDeps *d, void *ctx) {
    (void)ctx;
    /* textures/fonts are torn down after the loop; mark readiness here */
    d->resources_released = true;
    return true;
}

/* ---- "开始": policy reconciliation + idempotent task over libcurl ---- */
typedef struct {
    VwTransport base;
    VwNetClient *net;
} HttpTransport;

static int http_start(VwTransport *t, const char *idem, uint64_t *tid_out) {
    HttpTransport *ht = (HttpTransport *)t;
    bool replay = false;
    int rc = vwn_start_task(ht->net, idem, tid_out, &replay);
    (void)replay;
    return rc;
}

static int http_status(VwTransport *t, uint64_t tid, int *code_out) {
    (void)t; (void)tid;
    /* The demo server leaves tasks "running"; report success on query. A real
     * deployment adds GET /tasks/<id>; the FSM/timeout behaviour is unchanged. */
    *code_out = 0;
    return VW_OK_E;
}

static void on_command(const VwCommand *cmd, VwNetState net, char *out, int n,
                       void *ctx) {
    VwStart *start = ctx;
    if (cmd->id != VW_WIDGET_START) {
        snprintf(out, n, "settings command v%llu dispatched",
                 (unsigned long long)cmd->version);
        return;
    }
    /* server-vs-local policy decides the offline action range */
    VwServerPolicy srv = {true, true, true, 0};
    VwLocalState loc = {false, true, start->active_task != 0};
    VwDecision d = vw_policy_decide(VW_ACT_START, net,
                                   net == VW_NET_ONLINE ? &srv : NULL, &loc);
    if (!d.allow) {
        snprintf(out, n, "start denied: %s", d.reason);
        return;
    }
    /* single-flight + idempotency: a re-press after timeout can never create
     * a second task (same key; server UNIQUE constraint backs it up) */
    char env[VW_CMD_BYTES];
    vw_command_serialize(cmd, env, sizeof(env));
    (void)env;
    int rc = vw_start_press(start);
    snprintf(out, n, "start: %s (%s)%s", vw_fsm_state_name(start->fsm.state),
             d.decided_locally ? "local policy" : "server policy",
             rc == VW_E_TRANSPORT ? " [timeout: safe retry same key]" :
             rc == VW_E_OFFLINE ? " [offline: journaled]" : "");
}
#endif

int main(void) {
    if (!init_sdl()) {
        return 1;
    }

    AppWindow app = {0};
    if (!window_init(&app, WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        shutdown_sdl();
        return 1;
    }

    SceneRenderer scene = {0};
    bool have_bg = renderer_load_background(&scene, app.renderer,
                                            BACKGROUND_IMAGE_PATH);
    if (!have_bg) {
        fprintf(stderr, "warning: background missing, drawing plain screen\n");
    }

#ifdef VWIN_HAVE_SDL_TTF
    WidgetLayer wl;
    memset(&wl, 0, sizeof(wl));
    wl.input.screen_w = WINDOW_WIDTH;
    wl.input.screen_h = WINDOW_HEIGHT;
    wl.input.dpi = 96;
    snprintf(wl.input.title, sizeof(wl.input.title), "Visual Window App");
    wl.input.requested_font_px = 24;
    wl.input.arrangement = VW_ARR_SPLIT;
    wl.input.min_touch = 44;
    wl.input.exit_hit_grow = VW_EXIT_HIT_GROW;
    wl.input.start_enabled = true;
    wl.input.settings_enabled = true;
    wl.input.exit_enabled = true;
    wl.input.bg_luminance_0_255 = 128;
    wl.net = VW_NET_ONLINE;
    wl.artifact.version = 1;
    vw_capture_init(&wl.capture);
    vw_fsm_init(&wl.start_fsm);

    bool fonts_ok = widgets_load_fonts(
        &wl, kFontFamilies,
        (int)(sizeof(kFontFamilies) / sizeof(kFontFamilies[0])), 24);
    if (!fonts_ok)
        fprintf(stderr, "no preferred font found; core uses fallback metrics\n");
    if (scene.background) {
        SDL_Surface *bg_surface = IMG_Load(BACKGROUND_IMAGE_PATH);
        if (bg_surface) {
            wl.input.bg_luminance_0_255 = widgets_measure_luminance(bg_surface);
            SDL_FreeSurface(bg_surface);
        }
    }
    widgets_relayout(&wl);

    VwNetClient net;
    vwn_init(&net, getenv("VW_API") ? getenv("VW_API") : "http://127.0.0.1:8080",
             "DEV-1");
    vw_exit_init(&wl.exit_coord, exit_save, exit_flush, exit_release, &net,
                 8000);

    /* idempotent start pipeline: core orchestration over the HTTP transport */
    HttpTransport http = {0};
    http.net = &net;
    http.base.userdata = &http;
    http.base.start = http_start;
    http.base.status = http_status;
    http.base.online = true; /* refreshed from net.online each frame */
    static VwStart start;
    vw_start_init(&start, &http.base, 1, 1);
    wl.command_hook = on_command;
    wl.hook_ctx = &start;
#endif

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE)) {
#ifdef VWIN_HAVE_SDL_TTF
                /* window close is routed through the same controlled exit */
                vw_exit_request(&wl.exit_coord);
#else
                running = false;
#endif
            }
#ifdef VWIN_HAVE_SDL_TTF
            else if (event.type == SDL_MOUSEBUTTONDOWN) {
                widgets_handle_press(&wl, event.button.x, event.button.y);
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                widgets_handle_release(&wl, event.button.x, event.button.y);
            } else if (event.type == SDL_KEYDOWN &&
                       event.key.keysym.sym == SDLK_TAB) {
                widgets_focus_next(&wl);
            }
#endif
        }

        /* composite background + widgets, then present once per frame */
        renderer_compose_background(&scene, app.renderer, app.width,
                                    app.height);
#ifdef VWIN_HAVE_SDL_TTF
        widgets_draw(&wl, app.renderer);
        /* cheap reachability cadence: refresh online state for the policy */
        static int probe_ticks = 0;
        if (++probe_ticks % 60 == 0) {
            wl.net = vwn_ping(&net) ? VW_NET_ONLINE : VW_NET_OFFLINE;
            http.base.online = net.online;
        }
        /* settle an in-flight start task (idempotent single-flight) */
        if (start.fsm.state == VW_BTN_CONFIRMING ||
            start.fsm.state == VW_BTN_EXECUTING)
            vw_start_pump(&start);
        /* advance the controlled-exit coordinator each frame (~16ms) */
        if (wl.exit_coord.state != VW_EXIT_IDLE &&
            wl.exit_coord.state != VW_EXIT_DONE) {
            vw_exit_tick(&wl.exit_coord, 16);
        }
        if (wl.exit_coord.state == VW_EXIT_DONE)
            running = false;
#endif
        SDL_RenderPresent(app.renderer);
        SDL_Delay(16);
    }

#ifdef VWIN_HAVE_SDL_TTF
    widgets_destroy(&wl);
#endif
    renderer_destroy(&scene);
    window_destroy(&app);
    shutdown_sdl();
    return 0;
}
