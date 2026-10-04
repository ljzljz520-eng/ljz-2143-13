/*
 * vw_command.c - versioned command envelope and press/release capture.
 *
 * A press is captured against the layout artifact currently on screen.  The
 * command is only emitted on release if the SAME layout version is still
 * active: if a new published layout is swapped in between press and release
 * the interaction is cancelled ("mid-press version swap" acceptance case).
 */
#include "vw.h"

#include <stdio.h>
#include <string.h>

static const char *widget_name(VwWidgetId id) {
    switch (id) {
        case VW_WIDGET_START: return "start";
        case VW_WIDGET_SETTINGS: return "settings";
        case VW_WIDGET_EXIT: return "exit";
        default: return "title";
    }
}

int vw_command_serialize(const VwCommand *cmd, char *buf, int n) {
    return snprintf(buf, (size_t)n,
        "{\"v\":1,\"widget\":\"%s\",\"layout_version\":%llu,\"spec_hash\":%u,"
        "\"x\":%d,\"y\":%d}",
        widget_name(cmd->id), (unsigned long long)cmd->version,
        cmd->spec_hash, cmd->x, cmd->y);
}

void vw_capture_init(VwCapture *c) {
    if (c) memset(c, 0, sizeof(*c));
}

static VwGrab *find_active(VwCapture *c, int pointer_id) {
    for (int i = 0; i < VW_MAX_GRABS; i++)
        if (c->grabs[i].active && c->grabs[i].pointer_id == pointer_id)
            return &c->grabs[i];
    return NULL;
}

static VwGrab *free_slot(VwCapture *c) {
    for (int i = 0; i < VW_MAX_GRABS; i++)
        if (!c->grabs[i].active) return &c->grabs[i];
    return NULL;
}

bool vw_capture_press(VwCapture *c, const VwLayoutArtifact *a,
                      int pointer_id, int x, int y) {
    if (!c || !a) return false;
    if (find_active(c, pointer_id)) return false;
    VwWidgetId id;
    if (!vw_layout_hit(a, x, y, &id)) return false;
    VwGrab *g = free_slot(c);
    if (!g) return false;
    g->id = id;
    g->version = a->version;
    g->x = x;
    g->y = y;
    g->pointer_id = pointer_id;
    g->active = true;
    return true;
}

bool vw_capture_release(VwCapture *c, const VwLayoutArtifact *current,
                        int pointer_id, int x, int y,
                        VwCommand *cmd, bool *cancelled) {
    if (cancelled) *cancelled = false;
    if (!c || !current) return false;
    VwGrab *g = find_active(c, pointer_id);
    if (!g) return false;

    VwWidgetId released_id;
    bool still_over = vw_layout_hit(current, x, y, &released_id) &&
                      released_id == g->id;
    bool version_swapped = g->version != current->version;
    bool enabled_now = false;
    for (int i = 0; i < current->widget_count; i++)
        if (current->widgets[i].id == g->id)
            enabled_now = current->widgets[i].enabled;

    if (!still_over || version_swapped || !enabled_now) {
        g->active = false;
        if (cancelled) *cancelled = true;
        return false;
    }

    cmd->id = g->id;
    cmd->version = current->version;
    cmd->spec_hash = current->spec_hash;
    cmd->x = x;
    cmd->y = y;
    g->active = false;
    return true;
}
