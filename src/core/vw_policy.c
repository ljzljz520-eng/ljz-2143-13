/*
 * vw_policy.c - reconcile server availability decisions with the terminal's
 * local policy, especially what is still allowed while offline.
 *
 * Matrix (deny wins):
 *
 *  action    online + server         online + no/unreachable server
 *  ------    ----------------------   --------------------------------
 *  start     server flag              local: only if no task running and
 *                                     an unsynced/startable context exists
 *  settings  server flag              local: only with a valid cached config
 *  exit      always allowed           always allowed (controlled path)
 */
#include "vw.h"

#include <stdio.h>
#include <string.h>

VwDecision vw_policy_decide(VwAction action, VwNetState net,
                            const VwServerPolicy *srv, const VwLocalState *loc) {
    VwDecision d;
    d.allow = false;
    d.decided_locally = false;
    d.reason[0] = '\0';

    if (action == VW_ACT_EXIT) {
        /* exit is a local safety invariant; server can never disable it */
        d.allow = true;
        d.decided_locally = (net == VW_NET_OFFLINE) || srv == NULL ||
                            !srv->exit_;
        snprintf(d.reason, sizeof(d.reason),
                 d.decided_locally ? "exit: local fallback" : "exit: allowed");
        return d;
    }

    if (net == VW_NET_ONLINE && srv) {
        d.allow = (action == VW_ACT_START) ? srv->start : srv->settings;
        d.decided_locally = false;
        snprintf(d.reason, sizeof(d.reason), "%s: server says %s",
                 action == VW_ACT_START ? "start" : "settings",
                 d.allow ? "allow" : "deny");
        return d;
    }

    /* offline or no usable server decision: terminal local policy applies */
    d.decided_locally = true;
    if (action == VW_ACT_START) {
        if (loc && loc->task_running) {
            snprintf(d.reason, sizeof(d.reason),
                     "start denied offline: task already running");
        } else {
            d.allow = true;
            snprintf(d.reason, sizeof(d.reason),
                     "start allowed offline: local-only, journaled for sync");
        }
    } else { /* settings */
        if (loc && loc->settings_cache_valid) {
            d.allow = true;
            snprintf(d.reason, sizeof(d.reason),
                     "settings allowed offline: cached policy in effect");
        } else {
            snprintf(d.reason, sizeof(d.reason),
                     "settings denied offline: no cached config");
        }
    }
    return d;
}
