/*
 * vw_net_client.h - tiny libcurl-backed HTTP client for the visual window
 * terminal. Short timeouts by design: an unreachable server must quickly defer
 * to the terminal local policy instead of blocking the UI/exit path.
 */
#ifndef VW_NET_CLIENT_H
#define VW_NET_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char base_url[128]; /* e.g. http://127.0.0.1:8080 */
    char device[32];    /* e.g. DEV-1 */
    long timeout_ms;
    bool online;        /* updated by every call; also set by reachability */
    int last_http;      /* last HTTP status code */
} VwNetClient;

void vwn_init(VwNetClient *c, const char *base_url, const char *device);

/* Fire-and-check reachability; updates c->online. */
bool vwn_ping(VwNetClient *c);

/* POST a versioned command envelope JSON.
 * Returns 0 on HTTP 2xx (accept), VW_E_VERSION on 409, VW_E_OFFLINE when the
 * server cannot be reached, VW_E_TRANSPORT on other failures. */
int vwn_post_command(VwNetClient *c, const char *envelope_json);

/* Start (idempotent) a task. On success *task_id is the server task id and
 * *replay is true when the server returned an existing task for this key. */
int vwn_start_task(VwNetClient *c, const char *idempotency_key,
                   uint64_t *task_id, bool *replay);

#ifdef __cplusplus
}
#endif
#endif
