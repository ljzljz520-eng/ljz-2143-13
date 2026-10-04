/*
 * vw_fsm.c - button state machine (idle/executing/confirming/success/failed),
 * single-flight idempotency registry and the 开始 (start) orchestrator.
 */
#include "vw.h"

#include <stdio.h>
#include <string.h>

/* ---------------- FSM ---------------- */

void vw_fsm_init(VwButtonFsm *fsm) {
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = VW_BTN_IDLE;
}

const char *vw_fsm_state_name(VwBtnState s) {
    static const char *names[] = {"idle", "executing", "confirming",
                                  "success", "failed"};
    return names[(int)s];
}

bool vw_fsm_event(VwButtonFsm *fsm, VwEvent ev, uint64_t task_id) {
    switch (fsm->state) {
    case VW_BTN_IDLE:
        if (ev == VW_EV_PRESS) { fsm->state = VW_BTN_EXECUTING; return true; }
        return false;
    case VW_BTN_EXECUTING:
        switch (ev) {
        case VW_EV_ACK:
            fsm->task_id = task_id;
            fsm->state = VW_BTN_CONFIRMING;
            return true;
        case VW_EV_TIMEOUT:
            /* keep waiting but remember task may exist; move to confirming so
             * a re-press is a safe retry, not a second task */
            fsm->state = VW_BTN_CONFIRMING;
            return true;
        case VW_EV_SUCCESS:
            fsm->task_id = task_id;
            fsm->state = VW_BTN_SUCCESS;
            return true;
        case VW_EV_FAILED:
            fsm->task_id = task_id;
            fsm->state = VW_BTN_FAILED;
            return true;
        case VW_EV_PRESS:
            return false; /* ignore: request already in flight */
        default: return false;
        }
    case VW_BTN_CONFIRMING:
        switch (ev) {
        case VW_EV_PRESS:
            /* safe retry: idempotency key preserved by the orchestrator */
            fsm->state = VW_BTN_EXECUTING;
            return true;
        case VW_EV_SUCCESS:
            fsm->task_id = task_id;
            fsm->state = VW_BTN_SUCCESS;
            return true;
        case VW_EV_FAILED:
            fsm->task_id = task_id;
            fsm->state = VW_BTN_FAILED;
            return true;
        case VW_EV_ACK:
            fsm->task_id = task_id;
            return false; /* already know a task may exist */
        case VW_EV_TIMEOUT:
            return false;
        default: return false;
        }
    case VW_BTN_SUCCESS:
        if (ev == VW_EV_RESET) { fsm->state = VW_BTN_IDLE; return true; }
        if (ev == VW_EV_PRESS) return false; /* settled: ignore stray presses */
        return false;
    case VW_BTN_FAILED:
        if (ev == VW_EV_PRESS) {
            /* explicit new attempt after failure gets a fresh key */
            fsm->idem[0] = '\0';
            fsm->task_id = 0;
            fsm->state = VW_BTN_EXECUTING;
            return true;
        }
        if (ev == VW_EV_RESET) { fsm->state = VW_BTN_IDLE; return true; }
        return false;
    }
    return false;
}

/* ---------------- single-flight registry ---------------- */

void vw_flights_init(VwFlights *f) { memset(f, 0, sizeof(*f)); }

uint64_t vw_flight_begin(VwFlights *f, const char *key, uint64_t next_task_id,
                         bool *duplicate) {
    *duplicate = false;
    for (int i = 0; i < 16; i++) {
        VwFlight *fl = &f->flights[i];
        if (fl->in_flight && strcmp(fl->key, key) == 0) {
            *duplicate = true;
            return fl->task_id; /* never starts a second task */
        }
    }
    for (int i = 0; i < 16; i++) {
        VwFlight *fl = &f->flights[i];
        if (!fl->in_flight && !fl->finished) {
            snprintf(fl->key, sizeof(fl->key), "%s", key);
            fl->task_id = next_task_id;
            fl->in_flight = true;
            fl->finished = false;
            fl->code = 0;
            return next_task_id;
        }
    }
    return 0;
}

void vw_flight_finish(VwFlights *f, const char *key, int code) {
    const VwFlight *r = vw_flight_find(f, key);
    (void)r;
    for (int i = 0; i < 16; i++) {
        VwFlight *fl = &f->flights[i];
        if (strcmp(fl->key, key) == 0) {
            fl->in_flight = false;
            fl->finished = true;
            fl->code = code;
            return;
        }
    }
}

const VwFlight *vw_flight_find(const VwFlights *f, const char *key) {
    for (int i = 0; i < 16; i++)
        if (f->flights[i].key[0] && strcmp(f->flights[i].key, key) == 0)
            return &f->flights[i];
    return NULL;
}

void vw_idem_key(char *out, int n, uint64_t device_id, uint64_t session,
                 uint64_t press_seq) {
    snprintf(out, (size_t)n, "d%llu-s%llu-p%llu",
             (unsigned long long)device_id, (unsigned long long)session,
             (unsigned long long)press_seq);
}

/* ---------------- start orchestrator ---------------- */

static void mark_uncertain(VwFlights *f, const char *key) {
    for (int i = 0; i < 16; i++) {
        VwFlight *fl = &f->flights[i];
        if (fl->key[0] && strcmp(fl->key, key) == 0) {
            fl->in_flight = false;
            fl->finished = false; /* outcome unknown; safe to retry key */
            return;
        }
    }
}

void vw_start_init(VwStart *s, VwTransport *t, uint64_t device_id,
                   uint64_t session) {
    memset(s, 0, sizeof(*s));
    vw_fsm_init(&s->fsm);
    vw_flights_init(&s->flights);
    s->transport = t;
    s->device_id = device_id;
    s->session = session;
}

int vw_start_press(VwStart *s) {
    bool safe_retry = s->fsm.state == VW_BTN_CONFIRMING && !s->acked;
    if ((s->fsm.state == VW_BTN_CONFIRMING && s->acked) ||
        s->fsm.state == VW_BTN_EXECUTING ||
        s->fsm.state == VW_BTN_SUCCESS) {
        /* pressed again while request in flight / after success: nothing new */
        return VW_OK_E;
    }
    if (safe_retry) {
        /* timeout happened earlier: a re-press retries the SAME idempotency
         * key, so the server cannot end up with two tasks */
        vw_fsm_event(&s->fsm, VW_EV_PRESS, 0); /* CONFIRMING -> EXECUTING */
    } else {
        if (s->fsm.idem[0] == '\0') {
            s->press_seq++;
            vw_idem_key(s->fsm.idem, sizeof(s->fsm.idem), s->device_id,
                        s->session, s->press_seq);
        }
        bool moved = vw_fsm_event(&s->fsm, VW_EV_PRESS, 0);
        (void)moved;
    }

    if (!s->transport || !s->transport->online)
        return VW_E_OFFLINE;

    /* deterministic local candidate id; server id replaces it on ack.
     * On a safe retry reuse the known task id instead of a new candidate. */
    uint64_t candidate = s->active_task
                             ? s->active_task
                             : s->session * 1000000ull + s->press_seq;
    bool dup = false;
    uint64_t tid = vw_flight_begin(&s->flights, s->fsm.idem, candidate, &dup);
    if (dup) {
        s->active_task = tid;
        return VW_OK_E; /* same task rebound, no second start */
    }
    uint64_t server_tid = 0;
    int rc = s->transport->start(s->transport, s->fsm.idem, &server_tid);
    if (rc == VW_E_TRANSPORT) {
        /* timeout: the server may have created the task. Move to CONFIRMING
         * and keep the idempotency key; next press retries the SAME key. */
        mark_uncertain(&s->flights, s->fsm.idem);
        vw_fsm_event(&s->fsm, VW_EV_TIMEOUT, 0);
        s->active_task = tid;
        s->acked = false;
        return VW_E_TRANSPORT;
    }
    if (rc != VW_OK_E) return rc;
    s->active_task = server_tid ? server_tid : tid;
    s->acked = true;
    vw_fsm_event(&s->fsm, VW_EV_ACK, s->active_task);
    return VW_OK_E;
}

bool vw_start_pump(VwStart *s) {
    if (!s->transport) return false;
    if (s->fsm.state != VW_BTN_CONFIRMING &&
        s->fsm.state != VW_BTN_EXECUTING)
        return false;
    int code = 0;
    int rc = s->transport->status(s->transport, s->active_task, &code);
    if (rc == VW_E_TRANSPORT) {
        vw_fsm_event(&s->fsm, VW_EV_TIMEOUT, 0);
        return false;
    }
    if (rc == VW_E_OFFLINE) return false;
    if (code == 0) {
        vw_flight_finish(&s->flights, s->fsm.idem, 0);
        return vw_fsm_event(&s->fsm, VW_EV_SUCCESS, s->active_task);
    }
    vw_flight_finish(&s->flights, s->fsm.idem, code);
    return vw_fsm_event(&s->fsm, VW_EV_FAILED, s->active_task);
}
