#include "vw_net_client.h"
#include "core/vw.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct body_buf {
    char *data;
    size_t len;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct body_buf *b = ud;
    size_t n = size * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static void escape_json(char *dst, int n, const char *src) {
    int j = 0;
    for (int i = 0; src[i] && j < n - 2; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') {
            if (j < n - 3) dst[j++] = '\\';
        }
        dst[j++] = (char)ch;
    }
    dst[j] = '\0';
}

void vwn_init(VwNetClient *c, const char *base_url, const char *device) {
    memset(c, 0, sizeof(*c));
    snprintf(c->base_url, sizeof(c->base_url), "%s", base_url);
    snprintf(c->device, sizeof(c->device), "%s", device);
    c->timeout_ms = 1500;
    c->online = false;
}

static CURL *easy(VwNetClient *c, const char *path, struct body_buf *buf,
                  struct curl_slist *headers) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", c->base_url, path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    return curl;
}

bool vwn_ping(VwNetClient *c) {
    struct body_buf buf = {0};
    CURL *curl = easy(c, "/api/health", &buf, NULL);
    if (!curl) return false;
    CURLcode rc = curl_easy_perform(curl);
    c->online = (rc == CURLE_OK);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
                                          &c->last_http);
    free(buf.data);
    curl_easy_cleanup(curl);
    return c->online && c->last_http == 200;
}

int vwn_post_command(VwNetClient *c, const char *envelope_json) {
    struct body_buf buf = {0};
    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char path[96];
    snprintf(path, sizeof(path), "/api/devices/%s/commands", c->device);
    CURL *curl = easy(c, path, &buf, h);
    if (!curl) { curl_slist_free_all(h); return VW_E_TRANSPORT; }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, envelope_json);
    CURLcode rc = curl_easy_perform(curl);
    int result;
    if (rc != CURLE_OK) {
        c->online = false;
        result = VW_E_OFFLINE;
    } else {
        c->online = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &c->last_http);
        if (c->last_http == 409) result = VW_E_VERSION;
        else if (c->last_http >= 200 && c->last_http < 300) result = VW_OK_E;
        else result = VW_E_TRANSPORT;
    }
    free(buf.data);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return result;
}

int vwn_start_task(VwNetClient *c, const char *idempotency_key,
                   uint64_t *task_id, bool *replay) {
    struct body_buf buf = {0};
    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char path[96];
    snprintf(path, sizeof(path), "/api/devices/%s/tasks", c->device);
    char body[128], esc[VW_IDEMPOTENCY_BYTES + 8];
    escape_json(esc, sizeof(esc), idempotency_key);
    snprintf(body, sizeof(body), "{\"idempotency_key\":\"%s\"}", esc);
    CURL *curl = easy(c, path, &buf, h);
    if (!curl) { curl_slist_free_all(h); return VW_E_TRANSPORT; }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    CURLcode rc = curl_easy_perform(curl);
    int result;
    if (rc != CURLE_OK) {
        c->online = false;
        result = VW_E_OFFLINE;
    } else {
        c->online = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &c->last_http);
        long code = c->last_http;
        if (code == 200 || code == 201) {
            unsigned long long tid = 0;
            const char *mark = buf.data ? strstr(buf.data, "\"task_id\":") : NULL;
            if (mark) tid = strtoull(mark + strlen("\"task_id\":"), NULL, 10);
            if (task_id) *task_id = (uint64_t)tid;
            if (replay) *replay = (buf.data && strstr(buf.data, "true") &&
                                   strstr(buf.data, "idempotent_replay"));
            result = VW_OK_E;
        } else if (code == 403) {
            result = VW_E_DENIED;
        } else {
            result = VW_E_TRANSPORT;
        }
    }
    free(buf.data);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return result;
}
