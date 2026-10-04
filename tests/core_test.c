/*
 * core_test.c - host tests for the visual-window control core.
 * Build: see Makefile target "core-test" (no SDL required).
 */
#include "../src/core/vw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                   failures++; } } while (0)

static VwLayoutInput base_input(void) {
    VwLayoutInput in;
    memset(&in, 0, sizeof(in));
    in.screen_w = 1280;
    in.screen_h = 720;
    in.dpi = 96;
    snprintf(in.title, sizeof(in.title), "Visual Window App");
    in.requested_font_px = 24;
    in.arrangement = VW_ARR_SPLIT;
    snprintf(in.font_chain[0].family, 32, "DejaVu Sans");
    in.font_chain[0].available = true;
    snprintf(in.font_chain[1].family, 32, "Noto Sans CJK SC");
    in.font_chain[1].available = false;
    in.font_chain_n = 2;
    in.min_touch = 44;
    in.exit_hit_grow = VW_EXIT_HIT_GROW;
    in.bg_luminance_0_255 = 200; /* bright background */
    in.start_enabled = true;
    in.settings_enabled = true;
    in.exit_enabled = true;
    return in;
}

static const VwWidget *w_by_id(const VwLayoutArtifact *a, VwWidgetId id) {
    for (int i = 0; i < a->widget_count; i++)
        if (a->widgets[i].id == id) return &a->widgets[i];
    return NULL;
}

static void test_layout_basics(void) {
    VwLayoutInput in = base_input();
    VwLayoutArtifact a;
    CHECK(vw_layout_compile(&in, NULL, &a));
    CHECK(a.widget_count == 3);
    const VwWidget *ex = w_by_id(&a, VW_WIDGET_EXIT);
    CHECK(ex);
    /* exit reserved top-right */
    CHECK(ex->visual.x + ex->visual.w <= in.screen_w - 8);
    CHECK(ex->visual.y == 8);
    /* touch minimum enforced */
    CHECK(ex->hit.w >= in.min_touch && ex->hit.h >= in.min_touch);
    /* hit rects never overlap */
    for (int i = 0; i < a.widget_count; i++)
        for (int j = i + 1; j < a.widget_count; j++) {
            VwRect r = {0};
            VwRect p = a.widgets[i].hit, q = a.widgets[j].hit;
            int ov = p.x < q.x + q.w && q.x < p.x + p.w &&
                     p.y < q.y + q.h && q.y < p.y + p.h;
            (void)r;
            CHECK(!ov);
        }
    /* focus order: start, settings, exit */
    CHECK(w_by_id(&a, VW_WIDGET_START)->tab_index == 1);
    CHECK(w_by_id(&a, VW_WIDGET_SETTINGS)->tab_index == 2);
    CHECK(ex->tab_index == 3);
    /* hit priority: exit wins in overlap region of grown hit zone */
    int cx = ex->hit.x + 2, cy = ex->hit.y + ex->hit.h - 2;
    VwWidgetId id = 99;
    CHECK(vw_layout_hit(&a, cx, cy, &id));
    CHECK(id == VW_WIDGET_EXIT);
    /* title is not interactive */
    CHECK(!vw_layout_hit(&a, a.title_clip.x + 4,
                         a.title_clip.y + a.title_clip.h / 2, &id));
    /* bright background gets stronger scrim than dark */
    CHECK(a.title_scrim_alpha > 100);
}

static void test_long_title_never_covers_exit(void) {
    VwLayoutInput in = base_input();
    in.screen_w = 280; /* narrow screen so the reserved title band is small */
    /* 30 CJK glyphs = 90 UTF-8 bytes (<=127 server limit) yet far wider than
     * the reserved title band at 24px, so it must be ellipsized. */
    snprintf(in.title, sizeof(in.title),
             "很长很长的标题标题标题标题标题标题标题标题标题标题标题标题标题标题标题");
    VwLayoutArtifact a;
    CHECK(vw_layout_compile(&in, NULL, &a));
    CHECK(a.title_truncated);
    const VwWidget *ex = w_by_id(&a, VW_WIDGET_EXIT);
    /* title clip ends strictly before the exit keep-out band */
    CHECK(a.title_clip.x + a.title_clip.w <=
          ex->visual.x - VW_EXIT_KEEP_OUT_MARGIN);
    /* the exit center is still pressable */
    VwWidgetId id;
    CHECK(vw_layout_hit(&a, ex->visual.x + ex->visual.w / 2,
                        ex->visual.y + ex->visual.h / 2, &id));
    CHECK(id == VW_WIDGET_EXIT);
}

static void test_font_fallback_and_missing(void) {
    VwLayoutInput in = base_input();
    in.font_chain[0].available = false;
    in.font_chain[1].available = true;
    VwLayoutArtifact a;
    CHECK(vw_layout_compile(&in, NULL, &a));
    CHECK(a.font_status == VW_FONT_FALLBACK);
    CHECK(strstr(a.effective_family, "Noto Sans CJK SC") != NULL);

    in.font_chain[0].available = false;
    in.font_chain[1].available = false;
    CHECK(vw_layout_compile(&in, NULL, &a));
    CHECK(a.font_status == VW_FONT_MISSING); /* still lays out synthetically */
    CHECK(a.widget_count == 3);
}

static void test_adaptive_scrim(void) {
    VwLayoutInput dark = base_input();
    dark.bg_luminance_0_255 = 10;
    VwLayoutArtifact ad;
    CHECK(vw_layout_compile(&dark, NULL, &ad));
    VwLayoutInput bright = base_input();
    VwLayoutArtifact ab;
    CHECK(vw_layout_compile(&bright, NULL, &ab));
    CHECK(ab.title_scrim_alpha > ad.title_scrim_alpha);
}

static void test_inline_degrades_without_cover(void) {
    VwLayoutInput in = base_input();
    in.arrangement = VW_ARR_INLINE;
    snprintf(in.title, sizeof(in.title), "X");
    VwLayoutArtifact a;
    CHECK(vw_layout_compile(&in, NULL, &a));
    const VwWidget *ex = w_by_id(&a, VW_WIDGET_EXIT);
    const VwWidget *st = w_by_id(&a, VW_WIDGET_START);
    /* start is left of exit and never overlaps its hit zone */
    CHECK(st->hit.x + st->hit.w <= ex->hit.x);
}

/* ---- capture: version swap between press and release cancels ---- */
static void test_capture_version_swap(void) {
    VwLayoutInput in = base_input();
    VwLayoutArtifact v1, v2;
    CHECK(vw_layout_compile(&in, NULL, &v1));
    v1.version = 100;
    in.requested_font_px = 30; /* republish -> different artifact */
    CHECK(vw_layout_compile(&in, NULL, &v2));
    v2.version = 101;

    const VwWidget *st = w_by_id(&v1, VW_WIDGET_START);
    int x = st->hit.x + st->hit.w / 2;
    int y = st->hit.y + st->hit.h / 2;

    VwCapture cap;
    vw_capture_init(&cap);
    CHECK(vw_capture_press(&cap, &v1, 1, x, y));
    VwCommand cmd;
    bool cancelled = false;
    /* release against the NEW layout must be cancelled, no command emitted */
    bool emitted = vw_capture_release(&cap, &v2, 1, x, y, &cmd, &cancelled);
    CHECK(!emitted);
    CHECK(cancelled);

    /* same press/release within the same version emits the versioned command */
    vw_capture_init(&cap);
    CHECK(vw_capture_press(&cap, &v1, 1, x, y));
    char buf[VW_CMD_BYTES];
    emitted = vw_capture_release(&cap, &v1, 1, x, y, &cmd, &cancelled);
    CHECK(emitted && !cancelled);
    CHECK(cmd.version == 100);
    int n = vw_command_serialize(&cmd, buf, sizeof(buf));
    CHECK(n > 0 && n < (int)sizeof(buf));
    CHECK(strstr(buf, "\"widget\":\"start\""));
    CHECK(strstr(buf, "\"layout_version\":100"));
}

/* ---- fake transport for the single-flight/idempotency tests ---- */
typedef struct {
    VwTransport base;
    int start_calls;
    int next_code;       /* status code returned (-1 = still running) */
    int start_behavior;  /* 0 ok-ack, 1 timeout, 2 server rejects */
    uint64_t idem_task[8];
    char idem_keys[8][VW_IDEMPOTENCY_BYTES + 1];
    int idem_n;
} FakeTransport;

static int fake_start(VwTransport *t, const char *idem, uint64_t *task_id_out) {
    FakeTransport *f = (FakeTransport *)t;
    f->start_calls++;
    for (int i = 0; i < f->idem_n; i++) {
        if (strcmp(f->idem_keys[i], idem) == 0) {
            *task_id_out = f->idem_task[i]; /* idempotent replay */
            return VW_OK_E;
        }
    }
    if (f->start_behavior == 1) return VW_E_TRANSPORT;
    if (f->start_behavior == 2) return VW_E_DENIED;
    uint64_t tid = 5000 + (uint64_t)f->idem_n;
    snprintf(f->idem_keys[f->idem_n], sizeof(f->idem_keys[0]), "%s", idem);
    f->idem_task[f->idem_n] = tid;
    f->idem_n++;
    *task_id_out = tid;
    return VW_OK_E;
}

static int fake_status(VwTransport *t, uint64_t task_id, int *code_out) {
    FakeTransport *f = (FakeTransport *)t;
    (void)task_id;
    if (f->start_behavior == 1) return VW_E_TRANSPORT;
    *code_out = f->next_code;
    return VW_OK_E;
}

static void fake_init(FakeTransport *f, int behavior, int next_code) {
    memset(f, 0, sizeof(*f));
    f->base.userdata = f;
    f->base.start = fake_start;
    f->base.status = fake_status;
    f->base.online = true;
    f->start_behavior = behavior;
    f->next_code = next_code;
}

static void test_double_start_single_task(void) {
    FakeTransport ft;
    fake_init(&ft, 0, -1); /* healthy server, task stays running */
    VwStart s;
    vw_start_init(&s, &ft.base, 42, 7);
    CHECK(vw_start_press(&s) == VW_OK_E);
    CHECK(s.fsm.state == VW_BTN_CONFIRMING);
    uint64_t first_task = s.active_task;
    /* immediate re-press while running must not start a second task */
    CHECK(vw_start_press(&s) == VW_OK_E);
    CHECK(ft.start_calls == 1);
    CHECK(s.active_task == first_task);
    /* pump: task completes successfully */
    ft.next_code = 0;
    CHECK(vw_start_pump(&s));
    CHECK(s.fsm.state == VW_BTN_SUCCESS);
    /* after success another press is a no-op until reset */
    CHECK(vw_start_press(&s) == VW_OK_E);
    CHECK(ft.start_calls == 1);
}

static void test_timeout_retry_same_key(void) {
    FakeTransport ft;
    fake_init(&ft, 1, 0); /* POST times out */
    VwStart s;
    vw_start_init(&s, &ft.base, 42, 7);
    int rc = vw_start_press(&s);
    CHECK(rc == VW_E_TRANSPORT);
    CHECK(s.fsm.state == VW_BTN_CONFIRMING);
    char saved_key[VW_IDEMPOTENCY_BYTES + 1];
    snprintf(saved_key, sizeof(saved_key), "%s", s.fsm.idem);

    /* network restored; server actually created the task under this key */
    ft.start_behavior = 0;
    snprintf(ft.idem_keys[0], sizeof(ft.idem_keys[0]), "%s", saved_key);
    ft.idem_task[0] = 999;
    ft.idem_n = 1;
    CHECK(vw_start_press(&s) == VW_OK_E);   /* safe retry, same key */
    CHECK(strcmp(s.fsm.idem, saved_key) == 0);
    CHECK(ft.start_calls == 2);             /* one timed-out POST + one retry */
    CHECK(s.active_task == 999);            /* server deduped to existing task */
    CHECK(vw_start_pump(&s));
    CHECK(s.fsm.state == VW_BTN_SUCCESS);
}

static void test_fsm_states(void) {
    VwButtonFsm fsm;
    vw_fsm_init(&fsm);
    CHECK(vw_fsm_event(&fsm, VW_EV_PRESS, 0));
    CHECK(fsm.state == VW_BTN_EXECUTING);
    CHECK(!vw_fsm_event(&fsm, VW_EV_PRESS, 0)); /* executing: press ignored */
    CHECK(vw_fsm_event(&fsm, VW_EV_ACK, 77));
    CHECK(fsm.state == VW_BTN_CONFIRMING && fsm.task_id == 77);
    CHECK(vw_fsm_event(&fsm, VW_EV_FAILED, 77));
    CHECK(fsm.state == VW_BTN_FAILED);
    CHECK(vw_fsm_event(&fsm, VW_EV_RESET, 0));
    CHECK(fsm.state == VW_BTN_IDLE);
}

/* ---- policy matrix ---- */
static void test_policy(void) {
    VwServerPolicy srv = { true, false, true, 1 };
    VwLocalState loc = { true, false, false };
    VwDecision d;

    d = vw_policy_decide(VW_ACT_START, VW_NET_ONLINE, &srv, &loc);
    CHECK(d.allow && !d.decided_locally);
    d = vw_policy_decide(VW_ACT_SETTINGS, VW_NET_ONLINE, &srv, &loc);
    CHECK(!d.allow);
    /* offline: start allowed locally (journaled), settings blocked (no cache) */
    d = vw_policy_decide(VW_ACT_START, VW_NET_OFFLINE, NULL, &loc);
    CHECK(d.allow && d.decided_locally);
    d = vw_policy_decide(VW_ACT_SETTINGS, VW_NET_OFFLINE, NULL, &loc);
    CHECK(!d.allow);
    /* settings allowed offline with a valid cache */
    loc.settings_cache_valid = true;
    d = vw_policy_decide(VW_ACT_SETTINGS, VW_NET_OFFLINE, NULL, &loc);
    CHECK(d.allow);
    /* start denied offline while a task is already running */
    loc.task_running = true;
    d = vw_policy_decide(VW_ACT_START, VW_NET_OFFLINE, NULL, &loc);
    CHECK(!d.allow);
    /* exit always allowed, even with a hostile server decision */
    VwServerPolicy hostile = { false, false, false, 1 };
    d = vw_policy_decide(VW_ACT_EXIT, VW_NET_ONLINE, &hostile, &loc);
    CHECK(d.allow);
    d = vw_policy_decide(VW_ACT_EXIT, VW_NET_OFFLINE, NULL, &loc);
    CHECK(d.allow);
}

/* ---- exit coordinator ---- */
typedef struct {
    int save_ticks; int flush_ticks; int release_ticks;
    bool flush_ok;
} ExitCtx;

static bool ec_save(VwExitDeps *d, void *ctx) {
    ExitCtx *c = ctx;
    return (++c->save_ticks >= 3) || d->save_confirmed;
}
static bool ec_flush(VwExitDeps *d, void *ctx) {
    ExitCtx *c = ctx;
    (void)d;
    c->flush_ticks++;
    return c->flush_ok;
}
static bool ec_release(VwExitDeps *d, void *ctx) {
    ExitCtx *c = ctx;
    (void)d;
    return ++c->release_ticks >= 1;
}

static void test_exit_graceful_and_forced(void) {
    ExitCtx ctx = {0, 0, 0, false}; /* server unreachable: flush fails */
    VwExitCoord ec;
    vw_exit_init(&ec, ec_save, ec_flush, ec_release, &ctx, 8000);
    vw_exit_request(&ec);
    CHECK(ec.state == VW_EXIT_SAVING);
    bool changed = false;
    for (int i = 0; i < 3 && ec.state != VW_EXIT_DONE; i++)
        changed |= vw_exit_tick(&ec, 100);
    CHECK(changed);
    /* saving took 3 ticks, flush attempted once and failed, release ran */
    CHECK(ec.deps.save_confirmed);
    CHECK(ec.deps.flush_code != 0);
    CHECK(ec.deps.resources_released);
    CHECK(ec.state == VW_EXIT_DONE);

    /* forced path: second press during a long task skips wait */
    ExitCtx ctx2 = {0, 0, 0, false};
    VwExitCoord ec2;
    vw_exit_init(&ec2, ec_save, ec_flush, ec_release, &ctx2, 8000);
    vw_exit_request(&ec2);
    vw_exit_tick(&ec2, 10); /* business still busy */
    CHECK(ec2.state == VW_EXIT_SAVING);
    vw_exit_request(&ec2); /* user insists: controlled force */
    vw_exit_tick(&ec2, 10);
    CHECK(ec2.forced);
    CHECK(ec2.state == VW_EXIT_RELEASING || ec2.state == VW_EXIT_DONE);
    while (ec2.state != VW_EXIT_DONE) CHECK(vw_exit_tick(&ec2, 10));
    CHECK(ctx2.release_ticks >= 1);
}

/* ---- journal recovery ---- */
static void visit_count(const VwJournalEntry *e, void *ctx) {
    int *seen = ctx;
    if (strcmp(e->key, "d1-s1-p1") == 0 && e->status == VW_J_PENDING)
        (*seen)++;
}

static void test_journal(const char *path) {
    VwJournal j;
    void *h;
    CHECK(vw_journal_file_open(path, &j, &h));
    VwJournalEntry e1 = {"d1-s1-p1", "start", 100, VW_J_PENDING};
    VwJournalEntry e2 = {"d1-s1-p2", "start", 101, VW_J_PENDING};
    CHECK(j.append(&j, &e1));
    CHECK(j.append(&j, &e2));
    CHECK(j.update(&j, "d1-s1-p2", VW_J_DONE, 101));
    vw_journal_file_close(h);

    VwJournalStats stats = {0, 0, 0};
    int seen_pending_p1 = 0;
    CHECK(vw_journal_replay(path, visit_count, &seen_pending_p1, &stats));
    CHECK(stats.pending == 1); /* p1 still unfinished */
    CHECK(stats.done == 1);    /* p2 resolved to latest status */
    CHECK(seen_pending_p1 == 1);
}

int main(void) {
    test_layout_basics();
    test_long_title_never_covers_exit();
    test_font_fallback_and_missing();
    test_adaptive_scrim();
    test_inline_degrades_without_cover();
    test_capture_version_swap();
    test_double_start_single_task();
    test_timeout_retry_same_key();
    test_fsm_states();
    test_policy();
    test_exit_graceful_and_forced();

    const char *path = "build/test_journal.log";
    remove(path);
    test_journal(path);

    if (failures) {
        printf("\n%d CHECK(s) failed\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
