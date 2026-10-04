/*
 * vw_exit.c - controlled shutdown coordinator.
 *
 * Exit waits for (1) business confirmation that work is saved and (2) renderer
 * resource release.  A network flush is attempted with a bounded budget; when
 * the server is unreachable the flush is skipped (journal keeps the recovery
 * record) and release still runs.  A second exit press during a long task is
 * the controlled force path.
 */
#include "vw.h"

#include <string.h>

#define VW_SAVE_BUDGET_MS 5000
#define VW_FLUSH_BUDGET_MS 3000
/* release is local and mandatory; give it its own bounded budget below. */
#define VW_RELEASE_BUDGET_MS 2000

const char *vw_exit_state_name(VwExitState s) {
    static const char *names[] = {"idle", "saving", "flushing", "releasing",
                                  "done"};
    return names[(int)s];
}

void vw_exit_init(VwExitCoord *c, VwExitStep save, VwExitStep flush,
                  VwExitStep release, void *ctx, int deadline_ms) {
    memset(c, 0, sizeof(*c));
    c->state = VW_EXIT_IDLE;
    c->save = save;
    c->flush = flush;
    c->release = release;
    c->ctx = ctx;
    c->deadline_ms = deadline_ms > 0 ? deadline_ms
                                     : VW_SAVE_BUDGET_MS + VW_FLUSH_BUDGET_MS;
    c->save_budget_ms = VW_SAVE_BUDGET_MS;
    c->flush_budget_ms = VW_FLUSH_BUDGET_MS;
}

void vw_exit_request(VwExitCoord *c) {
    if (c->state == VW_EXIT_IDLE) {
        c->state = VW_EXIT_SAVING;
        c->elapsed_ms = 0;
    } else if (c->state != VW_EXIT_DONE) {
        /* second press: controlled force; skip waiting phases */
        c->forced = true;
    }
}

bool vw_exit_tick(VwExitCoord *c, int delta_ms) {
    if (c->state == VW_EXIT_IDLE || c->state == VW_EXIT_DONE)
        return false;

    c->elapsed_ms += delta_ms;
    bool changed = false;

    /* Each tick advances at most one step per phase. Phases that need to wait
     * (business save, renderer teardown) return here and resume on the next
     * embedder tick; forced exit overrides the waits. */
    int guard = 4;
    while (guard-- > 0 && c->state != VW_EXIT_DONE) {
        switch (c->state) {
        case VW_EXIT_SAVING:
            if (c->save && !c->deps.save_confirmed)
                c->deps.save_confirmed = c->save(&c->deps, c->ctx);
            if (c->deps.save_confirmed || c->forced ||
                c->elapsed_ms > c->save_budget_ms) {
                c->deps.save_confirmed = true;
                c->state = VW_EXIT_FLUSHING;
                changed = true;
                /* fall through and attempt the flush on the same tick */
            } else {
                return changed; /* wait for business confirmation */
            }
            break;

        case VW_EXIT_FLUSHING:
            if (!c->flush_attempted) {
                c->flush_attempted = true;
                int rc = 1;
                if (c->flush) rc = c->flush(&c->deps, c->ctx) ? 0 : 1;
                c->deps.flush_code = rc;
                /* delivered or server unreachable alike proceed; when rc != 0
                 * the journal is the durable recovery record */
            }
            c->state = VW_EXIT_RELEASING;
            changed = true;
            break;

        case VW_EXIT_RELEASING:
            if (!c->release_ran || !c->deps.resources_released) {
                c->release_ran = true;
                if (c->release)
                    c->deps.resources_released =
                        c->release(&c->deps, c->ctx) || c->forced;
                else
                    c->deps.resources_released = true;
            }
            if (c->deps.resources_released || c->forced ||
                c->elapsed_ms > c->save_budget_ms + c->flush_budget_ms +
                                    VW_RELEASE_BUDGET_MS) {
                c->deps.resources_released = true;
                c->state = VW_EXIT_DONE;
                changed = true;
            }
            return changed; /* renderer gets the next tick unless forced */

        default:
            return changed;
        }
    }
    return changed;
}
