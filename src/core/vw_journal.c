/*
 * vw_journal.c - line-oriented durable journal of operations.
 *
 * One record per line:  key|action|task_id|status
 * The same key can be appended again with a new status; replay resolves to the
 * latest state.  Used as the "unfinished operations recovery record" across
 * process restarts and after a forced offline exit.
 */
#include "vw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *fp;
} FileJournal;

static bool file_append(VwJournal *j, const VwJournalEntry *e) {
    FileJournal *f = (FileJournal *)j->userdata;
    if (!f || !f->fp) return false;
    if (fprintf(f->fp, "%s|%s|%llu|%d\n", e->key, e->action,
                (unsigned long long)e->task_id, (int)e->status) < 0)
        return false;
    fflush(f->fp);
    return true;
}

static bool file_update(VwJournal *j, const char *key,
                        VwJournalStatus status, uint64_t task_id) {
    VwJournalEntry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.key, sizeof(e.key), "%s", key);
    snprintf(e.action, sizeof(e.action), "?");
    e.task_id = task_id;
    e.status = status;
    return file_append(j, &e);
}

bool vw_journal_file_open(const char *path, VwJournal *j, void **handle_out) {
    FileJournal *f = calloc(1, sizeof(FileJournal));
    if (!f) return false;
    f->fp = fopen(path, "a+");
    if (!f->fp) { free(f); return false; }
    j->userdata = f;
    j->append = file_append;
    j->update = file_update;
    if (handle_out) *handle_out = f;
    return true;
}

void vw_journal_file_close(void *handle) {
    FileJournal *f = (FileJournal *)handle;
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

static VwJournalStatus parse_status(const char *s) {
    switch (atoi(s)) {
        case 1: return VW_J_DONE;
        case 2: return VW_J_FAILED;
        default: return VW_J_PENDING;
    }
}

bool vw_journal_replay(const char *path,
                       void (*visit)(const VwJournalEntry *, void *),
                       void *ctx, VwJournalStats *stats) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (stats) memset(stats, 0, sizeof(*stats));
        return true; /* nothing to replay is fine */
    }
    VwJournalEntry resolved[64];
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        VwJournalEntry e;
        memset(&e, 0, sizeof(e));
        char status[12] = {0};
        char tid[32] = {0};
        int fields = sscanf(line, "%32[^|]|%23[^|]|%31[^|]|%11[0-9]",
                            e.key, e.action, tid, status);
        if (fields < 4) continue; /* tolerate torn/partial lines */
        e.task_id = (uint64_t)strtoull(tid, NULL, 10);
        e.status = parse_status(status);
        int idx = -1;
        for (int i = 0; i < n; i++)
            if (strncmp(resolved[i].key, e.key, sizeof(e.key)) == 0) idx = i;
        if (idx >= 0) resolved[idx] = e;
        else if (n < 64) resolved[n++] = e;
    }
    fclose(fp);

    if (stats) memset(stats, 0, sizeof(*stats));
    for (int i = 0; i < n; i++) {
        if (visit) visit(&resolved[i], ctx);
        if (stats) {
            if (resolved[i].status == VW_J_PENDING) stats->pending++;
            else if (resolved[i].status == VW_J_DONE) stats->done++;
            else stats->failed++;
        }
    }
    return true;
}
