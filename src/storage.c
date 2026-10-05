#include "storage.h"

#include <jansson.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void make_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    mkdir(path, 0777);
}

void storage_init(Storage *storage) {
    const char *base = getenv("CONTROL_STATE_DIR");
    if (base == NULL || *base == '\0') base = "./data";
    make_directory(base);
    snprintf(storage->directory, sizeof(storage->directory), "%s", base);
    snprintf(storage->layout_path, sizeof(storage->layout_path), "%s/cached-layout.json", base);
    snprintf(storage->journal_path, sizeof(storage->journal_path), "%s/active-journal.json", base);
}

bool storage_load_layout(const Storage *storage, ControlLayout *layout) {
    FILE *file = fopen(storage->layout_path, "rb");
    if (file == NULL) return false;
    char buffer[16384];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[n] = '\0';
    fclose(file);

    json_error_t error;
    json_t *root = json_loadb(buffer, n, 0, &error);
    if (root == NULL) return false;
    json_t *rects = json_object_get(root, "rects");
    if (!json_is_array(rects) || json_array_size(rects) != 5) {
        json_decref(root);
        return false;
    }
    layout_set_defaults(layout);
    layout->version = (int)json_integer_value(json_object_get(root, "version"));
    layout->revision = (int)json_integer_value(json_object_get(root, "revision"));
    const char *title = json_string_value(json_object_get(root, "title"));
    const char *family = json_string_value(json_object_get(root, "family"));
    if (title != NULL) snprintf(layout->title, sizeof(layout->title), "%s", title);
    if (family != NULL) snprintf(layout->font_family, sizeof(layout->font_family), "%s", family);
    json_int_t font_size = json_integer_value(json_object_get(root, "font_size"));
    if (font_size >= 12 && font_size <= 96) layout->font_size = (int)font_size;
    layout->darken = json_number_value(json_object_get(root, "darken"));
    const char *mode = json_string_value(json_object_get(root, "mode"));
    if (mode && strcmp(mode, "light") == 0) layout->background_mode = BG_LIGHT;
    else if (mode && strcmp(mode, "dark") == 0) layout->background_mode = BG_DARK;
    else layout->background_mode = BG_ADAPTIVE;

    for (int group = 0; group < 5; ++group) {
        json_t *item = json_array_get(rects, (size_t)group);
        if (!json_is_array(item) || json_array_size(item) != 4) {
            json_decref(root);
            return false;
        }
        Rect r;
        r.x = (int)json_integer_value(json_array_get(item, 0));
        r.y = (int)json_integer_value(json_array_get(item, 1));
        r.w = (int)json_integer_value(json_array_get(item, 2));
        r.h = (int)json_integer_value(json_array_get(item, 3));
        if (group == 0) layout->safe_area = r;
        else if (group == 1) layout->title_rect = r;
        else layout->buttons[group - 2].visual = r;
    }
    json_decref(root);
    layout_rebuild_hits(layout);
    return true;
}

static bool write_atomic(const char *path, const char *text) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    size_t len = strlen(text);
    ssize_t written = write(fd, text, len);
    close(fd);
    if (written != (ssize_t)len) return false;
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static void json_escape_file(char *dst, size_t size, const char *src) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 2 < size; ++i) {
        if (src[i] == '"' || src[i] == '\\') {
            if (out + 2 >= size) break;
            dst[out++] = '\\';
            dst[out++] = src[i];
        } else if ((unsigned char)src[i] < 32) {
            if (out + 7 >= size) break;
            out += (size_t)snprintf(dst + out, size - out, "\\u%04x", (unsigned char)src[i]);
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

bool storage_save_layout(const Storage *storage, const ControlLayout *layout) {
    char title[TITLE_TEXT_MAX * 2 + 4];
    char family[FONT_FAMILY_MAX * 2 + 4];
    json_escape_file(title, sizeof(title), layout->title);
    json_escape_file(family, sizeof(family), layout->font_family);
    char text[2048];
    snprintf(text, sizeof(text),
        "{\"version\":%d,\"revision\":%d,\"title\":\"%s\",\"family\":\"%s\","
        "\"font_size\":%d,\"mode\":\"%s\",\"darken\":%d,\"rects\":[[%d,%d,%d,%d],"
        "[%d,%d,%d,%d],[%d,%d,%d,%d],[%d,%d,%d,%d],[%d,%d,%d,%d]]}\n",
        layout->version, layout->revision, title, family, layout->font_size,
        layout_background_mode_name(layout->background_mode),
        (int)(layout->darken * 1000.0),
        layout->safe_area.x,layout->safe_area.y,layout->safe_area.w,layout->safe_area.h,
        layout->title_rect.x,layout->title_rect.y,layout->title_rect.w,layout->title_rect.h,
        layout->buttons[0].visual.x,layout->buttons[0].visual.y,layout->buttons[0].visual.w,layout->buttons[0].visual.h,
        layout->buttons[1].visual.x,layout->buttons[1].visual.y,layout->buttons[1].visual.w,layout->buttons[1].visual.h,
        layout->buttons[2].visual.x,layout->buttons[2].visual.y,layout->buttons[2].visual.w,layout->buttons[2].visual.h);
    return write_atomic(storage->layout_path, text);
}

bool storage_load_journal(const Storage *storage, ActiveJournal *journal) {
    memset(journal, 0, sizeof(*journal));
    FILE *file = fopen(storage->journal_path, "rb");
    if (file == NULL) return false;
    char buffer[1024];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[n] = '\0';
    fclose(file);
    int matched = sscanf(buffer, "{\"command_id\":\"%79[^\"]\",\"action\":\"%23[^\"]\","
                         "\"status\":\"%23[^\"]\",\"progress\":%d,\"updated_at\":%lld",
                         journal->command_id, journal->action, journal->status,
                         &journal->progress, &journal->updated_at);
    return matched >= 4;
}

bool storage_save_journal(const Storage *storage, const ActiveJournal *journal) {
    char text[512];
    snprintf(text, sizeof(text),
             "{\"command_id\":\"%s\",\"action\":\"%s\",\"status\":\"%s\",\"progress\":%d,\"updated_at\":%lld}\n",
             journal->command_id, journal->action, journal->status,
             journal->progress, journal->updated_at);
    return write_atomic(storage->journal_path, text);
}

bool storage_clear_journal(const Storage *storage) {
    return unlink(storage->journal_path) == 0 || errno == ENOENT;
}

void journal_set(ActiveJournal *journal, const char *command_id, ControlAction action,
                 const char *status, int progress) {
    snprintf(journal->command_id, sizeof(journal->command_id), "%s", command_id);
    snprintf(journal->action, sizeof(journal->action), "%s", control_action_name(action));
    snprintf(journal->status, sizeof(journal->status), "%s", status);
    journal->progress = progress;
    journal->updated_at = (long long)time(NULL);
}

bool storage_recovery_append(const Storage *storage, ControlAction action,
                             const char *message, const char *command_id, int progress) {
    char path[600];
    snprintf(path, sizeof(path), "%s/recovery.log", storage->directory);
    FILE *file = fopen(path, "ab");
    if (file == NULL) return false;
    fprintf(file, "%lld %s %s progress=%d: %s\n", (long long)time(NULL),
            control_action_name(action), command_id, progress, message);
    fclose(file);
    return true;
}

char *storage_recovery_read(const Storage *storage) {
    char path[600];
    snprintf(path, sizeof(path), "%s/recovery.log", storage->directory);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return strdup("");
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    if (size > 4096) {
        fseek(file, -4096, SEEK_END);
        size = 4096;
    } else {
        fseek(file, 0, SEEK_SET);
    }
    char *text = calloc((size_t)size + 1, 1);
    if (text != NULL && fread(text, 1, (size_t)size, file) != (size_t)size) {
        /* Reading a concurrently rotated file is best effort. */
    }
    fclose(file);
    return text ? text : strdup("");
}

void storage_generate_command_id(char *output, size_t output_size) {
    unsigned int seed = (unsigned int)(time(NULL) ^ getpid());
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom != NULL) {
        unsigned int extra = 0;
        if (fread(&extra, sizeof(extra), 1, urandom) == 1) seed ^= extra;
        fclose(urandom);
    }
    srand(seed);
    snprintf(output, output_size, "cmd-%08x-%08x-%04x",
             rand(), rand(), rand() & 0xffff);
}
