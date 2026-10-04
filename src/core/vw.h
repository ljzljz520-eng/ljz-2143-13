/*
 * vw.h - Visual Window control framework: host-independent core.
 *
 * This header plus vw_*.c contain every behaviour that can be unit-tested
 * without SDL/a display: layout compilation, versioned command dispatch,
 * button state machine + single-flight idempotency, server/local availability
 * policy, exit coordination and durable recovery journal.
 */
#ifndef VW_H
#define VW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VW_MAX_TITLE_BYTES 128
#define VW_MAX_BUTTONS 4
#define VW_MAX_FONT_CHAIN 4
#define VW_MAX_GRABS 8
#define VW_IDEMPOTENCY_BYTES 32
#define VW_CMD_BYTES 512
#define VW_ERR_LEN 128
#define VW_MIN_TOUCH_TARGET 44
#define VW_EXIT_KEEP_OUT_MARGIN 8
#define VW_EXIT_HIT_GROW 12
#define VW_MAX_FONTSIZE 120
#define VW_MIN_FONTSIZE 10

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    VW_ARR_INLINE = 0,   /* title bar: 开始, 设置 ... 退出 (right, reserved) */
    VW_ARR_SPLIT = 1,    /* start/settings on a bottom action bar, exit top-right */
} VwArrangement;

typedef enum {
    VW_WIDGET_TITLE = 0,
    VW_WIDGET_START = 1,
    VW_WIDGET_SETTINGS = 2,
    VW_WIDGET_EXIT = 3,
} VwWidgetId;

typedef enum {
    VW_FONT_OK = 0,
    VW_FONT_FALLBACK = 1,
    VW_FONT_MISSING = 2,
} VwFontStatus;

typedef struct {
    int x, y, w, h;
} VwRect;

typedef struct {
    VwWidgetId id;
    VwRect visual;        /* drawn rect */
    VwRect hit;           /* pointer/touch hit rect, >= touch target, no overlap */
    int tab_index;        /* keyboard focus order, 1-based */
    bool enabled;
} VwWidget;

typedef struct {
    char family[32];
    bool available;       /* filled by font environment probe */
} VwFont;

/* Font measurement environment provided by the embedding platform. */
typedef struct {
    void *userdata;
    /* return advance width of utf8 text at px size in the given family */
    int (*measure)(void *userdata, const char *family, int px, const char *utf8);
    /* mark family available/unavailable; core picks the first available. */
    void (*set_available)(void *userdata, const char *family, bool available);
} VwFontEnv;

typedef struct {
    int screen_w;
    int screen_h;
    int dpi;
    char title[VW_MAX_TITLE_BYTES];
    int requested_font_px;
    VwArrangement arrangement;
    VwFont font_chain[VW_MAX_FONT_CHAIN];
    int font_chain_n;
    int min_touch;          /* device minimum touch target in px */
    int exit_hit_grow;      /* extra px added around exit visual */
    int bg_luminance_0_255; /* measured background luminance, 0 dark .. 255 bright */
    bool start_enabled;
    bool settings_enabled;
    bool exit_enabled;
} VwLayoutInput;

typedef struct {
    uint64_t version;
    char title[VW_MAX_TITLE_BYTES];
    bool title_truncated;
    VwRect title_clip;
    int effective_font_px;
    char effective_family[32];
    VwFontStatus font_status;
    int missing_glyph_count;
    VwWidget widgets[VW_MAX_BUTTONS];
    int widget_count;
    VwRect titlebar;
    VwRect actionbar;
    bool has_actionbar;
    int title_scrim_alpha;  /* adaptive darkening under title (0..255) */
    int action_scrim_alpha; /* adaptive darkening under action bar */
    uint32_t spec_hash;     /* checksum of the inputs that produced this layout */
    char error[VW_ERR_LEN];
} VwLayoutArtifact;

/* Compile inputs into a deterministic artifact. false => error[]. */
bool vw_layout_compile(const VwLayoutInput *in, VwFontEnv *env,
                       VwLayoutArtifact *out);

/* Hit test honouring non-overlapping hit rects; priority exit > start > settings.
 * Returns false for the title (non-interactive) and empty space. */
bool vw_layout_hit(const VwLayoutArtifact *a, int x, int y, VwWidgetId *id);

/* FNV-1a checksum of canonical layout input bytes (kept in sync with server). */
uint32_t vw_layout_spec_hash(const VwLayoutInput *in);

/* Decode one UTF-8 codepoint; returns bytes consumed, 0 on invalid sequence. */
int vw_utf8_next(const unsigned char *s, int n, uint32_t *cp);

/* ------------------------------------------------------------------ */
/* Versioned commands + press/release capture                          */
/* ------------------------------------------------------------------ */

typedef struct {
    VwWidgetId id;
    uint64_t version;
    uint32_t spec_hash;
    int x, y;
} VwCommand;

/* Build the canonical envelope the C client sends; bytes_written excludes NUL. */
int vw_command_serialize(const VwCommand *cmd, char *buf, int n);

typedef struct {
    VwWidgetId id;
    uint64_t version;
    int x, y;
    int pointer_id;
    bool active;
} VwGrab;

typedef struct {
    VwGrab grabs[VW_MAX_GRABS];
} VwCapture;

void vw_capture_init(VwCapture *c);
/* A press is captured only when the widget is enabled at that moment. */
bool vw_capture_press(VwCapture *c, const VwLayoutArtifact *a,
                      int pointer_id, int x, int y);
/* Release: emits cmd only when the layout version is unchanged since press.
 * On version swap the press is cancelled (returns false, *cancelled=true). */
bool vw_capture_release(VwCapture *c, const VwLayoutArtifact *current,
                        int pointer_id, int x, int y, VwCommand *cmd,
                        bool *cancelled);

/* ------------------------------------------------------------------ */
/* Button state machine + single-flight idempotency                    */
/* ------------------------------------------------------------------ */

typedef enum {
    VW_BTN_IDLE = 0,
    VW_BTN_EXECUTING = 1,
    VW_BTN_CONFIRMING = 2,
    VW_BTN_SUCCESS = 3,
    VW_BTN_FAILED = 4,
} VwBtnState;

typedef enum {
    VW_EV_PRESS = 0,
    VW_EV_ACK = 1,        /* server accepted, task running */
    VW_EV_TIMEOUT = 2,    /* transport timeout; safe to retry */
    VW_EV_SUCCESS = 3,
    VW_EV_FAILED = 4,
    VW_EV_RESET = 5,      /* success/failure auto-clear timer */
} VwEvent;

typedef struct {
    VwBtnState state;
    char idem[VW_IDEMPOTENCY_BYTES + 1];
    uint64_t task_id;
} VwButtonFsm;

void vw_fsm_init(VwButtonFsm *fsm);
/* Returns true if the event causes a state change. A PRESS while EXECUTING
 * is a no-op (no second task). A PRESS while CONFIRMING is a safe retry that
 * keeps the same idempotency key. */
bool vw_fsm_event(VwButtonFsm *fsm, VwEvent ev, uint64_t task_id);
const char *vw_fsm_state_name(VwBtnState s);

typedef struct {
    char key[VW_IDEMPOTENCY_BYTES + 1];
    uint64_t task_id;
    bool in_flight;
    bool finished;
    int code;
} VwFlight;

typedef struct {
    VwFlight flights[16];
} VwFlights;

void vw_flights_init(VwFlights *f);
/* Register a flight. If the key is already in flight the existing task_id is
 * returned and *duplicate=true: the second press never starts a second task. */
uint64_t vw_flight_begin(VwFlights *f, const char *key, uint64_t next_task_id,
                         bool *duplicate);
void vw_flight_finish(VwFlights *f, const char *key, int code);
const VwFlight *vw_flight_find(const VwFlights *f, const char *key);

void vw_idem_key(char *out, int n, uint64_t device_id, uint64_t session,
                 uint64_t press_seq);

/* Abstract transport so the orchestration is testable on host. */
typedef struct VwTransport VwTransport;
struct VwTransport {
    void *userdata;
    int (*start)(VwTransport *t, const char *idem, uint64_t *task_id_out);
    int (*status)(VwTransport *t, uint64_t task_id, int *code_out);
    bool online;
};

typedef struct {
    VwButtonFsm fsm;
    VwFlights flights;
    VwTransport *transport;
    uint64_t device_id;
    uint64_t session;
    uint64_t press_seq;
    uint64_t active_task;
    bool acked; /* server acknowledged the active task vs. timeout-uncertain */
} VwStart;

void vw_start_init(VwStart *s, VwTransport *t, uint64_t device_id,
                   uint64_t session);
/* User pressed/released 开始. Returns 0 if a (possibly existing) task is bound,
 * VW_E_* otherwise. */
int vw_start_press(VwStart *s);
/* Drive network outcomes; call periodically. Returns true when state moved. */
bool vw_start_pump(VwStart *s);

/* ------------------------------------------------------------------ */
/* Availability: server decision vs terminal local policy              */
/* ------------------------------------------------------------------ */

typedef enum { VW_ACT_START = 0, VW_ACT_SETTINGS = 1, VW_ACT_EXIT = 2 } VwAction;
typedef enum { VW_NET_ONLINE = 0, VW_NET_OFFLINE = 1 } VwNetState;

typedef struct {
    bool start;
    bool settings;
    bool exit_;
    uint64_t decided_at;
} VwServerPolicy;

typedef struct {
    bool has_unsynced_work;  /* local journal contains unfinished ops */
    bool settings_cache_valid;
    bool task_running;
} VwLocalState;

typedef struct {
    bool allow;
    bool decided_locally;   /* true = decided without any server input */
    char reason[96];
} VwDecision;

VwDecision vw_policy_decide(VwAction action, VwNetState net,
                            const VwServerPolicy *srv, const VwLocalState *loc);

/* ------------------------------------------------------------------ */
/* Exit coordination                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    VW_EXIT_IDLE = 0,
    VW_EXIT_SAVING = 1,
    VW_EXIT_FLUSHING = 2,
    VW_EXIT_RELEASING = 3,
    VW_EXIT_DONE = 4,
} VwExitState;

typedef struct {
    bool save_confirmed;    /* business confirmed save */
    bool resources_released;/* renderer released textures/fonts */
    int flush_code;         /* server flush result, 0 ok */
} VwExitDeps;

typedef bool (*VwExitStep)(VwExitDeps *deps, void *ctx);

typedef struct {
    VwExitState state;
    VwExitDeps deps;
    VwExitStep save;
    VwExitStep flush;
    VwExitStep release;
    void *ctx;
    int deadline_ms;        /* total budget */
    int elapsed_ms;
    int save_budget_ms;
    int flush_budget_ms;
    bool forced;
    bool flush_attempted;
    bool release_ran;
} VwExitCoord;

void vw_exit_init(VwExitCoord *c, VwExitStep save, VwExitStep flush,
                  VwExitStep release, void *ctx, int deadline_ms);
/* First press: graceful. Second press during a long task: controlled force. */
void vw_exit_request(VwExitCoord *c);
/* Advance; now_ms delta is fed by embedder via vw_exit_tick. */
bool vw_exit_tick(VwExitCoord *c, int delta_ms);
const char *vw_exit_state_name(VwExitState s);

/* ------------------------------------------------------------------ */
/* Durable operation journal (unfinished-op recovery)                  */
/* ------------------------------------------------------------------ */

typedef enum {
    VW_J_PENDING = 0,
    VW_J_DONE = 1,
    VW_J_FAILED = 2,
} VwJournalStatus;

typedef struct {
    char key[VW_IDEMPOTENCY_BYTES + 1];
    char action[24];
    uint64_t task_id;
    VwJournalStatus status;
} VwJournalEntry;

typedef struct {
    uint64_t pending;
    uint64_t done;
    uint64_t failed;
} VwJournalStats;

typedef struct VwJournal VwJournal;
struct VwJournal {
    void *userdata;
    bool (*append)(VwJournal *j, const VwJournalEntry *e);
    bool (*update)(VwJournal *j, const char *key, VwJournalStatus status,
                   uint64_t task_id);
};

bool vw_journal_file_open(const char *path, VwJournal *j, void **handle_out);
void vw_journal_file_close(void *handle);
/* Replay path through visitor; returns false on hard parse error. */
bool vw_journal_replay(const char *path,
                       void (*visit)(const VwJournalEntry *, void *),
                       void *ctx, VwJournalStats *stats);

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

enum {
    VW_OK_E = 0,
    VW_E_DENIED = 1,
    VW_E_OFFLINE = 2,
    VW_E_TRANSPORT = 3,
    VW_E_VERSION = 4,
};

#ifdef __cplusplus
}
#endif
#endif
