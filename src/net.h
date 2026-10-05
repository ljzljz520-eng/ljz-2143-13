#ifndef NET_H
#define NET_H

#include <stdbool.h>

#include "layout.h"

#define HTTP_BODY_MAX (256 * 1024)

typedef struct {
    long status;
    bool ok;
    char *body;
    size_t size;
    char error[512];
    char active_command_id[80];
} HttpResponse;

typedef struct {
    bool start;
    bool settings;
    bool exit_allowed;
} ServerPolicy;

typedef struct {
    bool loaded;
    bool online;
    long http_status;
    char error[512];
    char active_command_id[80];
    char command_id[80];
    char action[24];
    char status[24];
    int progress;
} NetResult;

void http_global_init(void);
void http_global_cleanup(void);
HttpResponse http_request(const char *method, const char *path, const char *json_body,
                          long timeout_ms);
bool net_bootstrap(ControlLayout *layout, ServerPolicy *policy, const char *device_id,
                   NetResult *result);
bool net_apply_command(const char *device_id, ControlAction action,
                       int layout_version, const char *command_id,
                       NetResult *result);
bool net_update_command(const char *device_id, const char *command_id,
                        const char *status, int progress, const char *message,
                        NetResult *result);
bool net_post_actual(const char *device_id, const ControlLayout *layout,
                     const char *status_line, bool online, NetResult *result);
bool net_post_recovery(const char *device_id, ControlAction action,
                       const char *message, const char *command_id,
                       int progress, NetResult *result);
bool net_reconcile_command(const char *device_id, const char *command_id,
                           ControlAction action, const char *local_status,
                           int progress, NetResult *result);
const char *net_server_base(void);

#endif
