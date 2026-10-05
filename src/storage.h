#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>

#include "layout.h"
#include "net.h"

#define STORAGE_PATH_MAX 512

typedef struct {
    char command_id[80];
    char action[24];
    char status[24];
    int progress;
    long long updated_at;
} ActiveJournal;

typedef struct {
    char directory[STORAGE_PATH_MAX];
    char layout_path[STORAGE_PATH_MAX];
    char journal_path[STORAGE_PATH_MAX];
} Storage;

void storage_init(Storage *storage);
bool storage_load_layout(const Storage *storage, ControlLayout *layout);
bool storage_save_layout(const Storage *storage, const ControlLayout *layout);
bool storage_load_journal(const Storage *storage, ActiveJournal *journal);
bool storage_save_journal(const Storage *storage, const ActiveJournal *journal);
bool storage_clear_journal(const Storage *storage);
void journal_set(ActiveJournal *journal, const char *command_id, ControlAction action,
                 const char *status, int progress);
bool storage_recovery_append(const Storage *storage, ControlAction action,
                             const char *message, const char *command_id, int progress);
char *storage_recovery_read(const Storage *storage);
void storage_generate_command_id(char *output, size_t output_size);

#endif
