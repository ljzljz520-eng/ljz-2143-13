#include "net.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_ID_MAX 64

static char g_server_base[256] = "http://127.0.0.1:8080";
static CURL *g_curl = NULL;

const char *net_server_base(void) {
    return g_server_base;
}

void http_global_init(void) {
    const char *server = getenv("CONTROL_SERVER");
    if (server != NULL && *server) {
        size_t len = strlen(server);
        while (len > 0 && server[len - 1] == '/') --len;
        snprintf(g_server_base, sizeof(g_server_base), "%.*s", (int)len, server);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
}

void http_global_cleanup(void) {
    if (g_curl != NULL) curl_easy_cleanup(g_curl);
    g_curl = NULL;
    curl_global_cleanup();
}

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *response = userdata;
    if (response->size + bytes + 1 > HTTP_BODY_MAX) {
        snprintf(response->error, sizeof(response->error), "响应超过 %d 字节", HTTP_BODY_MAX);
        return 0;
    }
    char *grown = realloc(response->body, response->size + bytes + 1);
    if (grown == NULL) return 0;
    response->body = grown;
    memcpy(response->body + response->size, ptr, bytes);
    response->size += bytes;
    response->body[response->size] = '\0';
    return bytes;
}

HttpResponse http_request(const char *method, const char *path, const char *json_body,
                          long timeout_ms) {
    HttpResponse response;
    memset(&response, 0, sizeof(response));
    char url[512];
    snprintf(url, sizeof(url), "%.255s%.255s", g_server_base, path);
    CURL *curl = g_curl ? curl_easy_duphandle(g_curl) : curl_easy_init();
    if (curl == NULL) {
        response.ok = false;
        snprintf(response.error, sizeof(response->error), "无法初始化 HTTP 客户端");
        return response;
    }
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (json_body != NULL) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    CURLcode code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (code != CURLE_OK) {
        snprintf(response.error, sizeof(response.error), "%s", curl_easy_strerror(code));
        response.ok = false;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        response.ok = response.status >= 200 && response.status < 300;
        if (!response.ok && response.error[0] == '\0') {
            snprintf(response.error, sizeof(response.error), "HTTP %ld", response.status);
        }
    }
    curl_easy_cleanup(curl);
    return response;
}

static void set_result(NetResult *result, bool loaded, long status, const char *error) {
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->loaded = loaded;
    result->http_status = status;
    if (error != NULL) snprintf(result->error, sizeof(result->error), "%s", error);
}

static Rect parse_rect(json_t *object, const char *key, Rect fallback) {
    json_t *value = json_object_get(object, key);
    if (!json_is_object(value)) return fallback;
    Rect r;
    r.x = (int)json_integer_value(json_object_get(value, "x"));
    r.y = (int)json_integer_value(json_object_get(value, "y"));
    r.w = (int)json_integer_value(json_object_get(value, "w"));
    r.h = (int)json_integer_value(json_object_get(value, "h"));
    if (r.w <= 0 || r.h <= 0) return fallback;
    return r;
}

static ControlAction parse_action(const char *name) {
    if (name != NULL && strcmp(name, "start") == 0) return ACTION_START;
    if (name != NULL && strcmp(name, "settings") == 0) return ACTION_SETTINGS;
    if (name != NULL && strcmp(name, "exit") == 0) return ACTION_EXIT;
    return ACTION_NONE;
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (src == NULL) return;
    snprintf(dst, dst_size, "%s", src);
}

static void parse_layout_json(json_t *root, ControlLayout *layout) {
    ControlLayout defaults;
    layout_set_defaults(&defaults);
    for (int i = 0; i < ACTION_COUNT; ++i) {
        defaults.buttons[i].state = layout->buttons[i].state;
        defaults.buttons[i].enabled = layout->buttons[i].enabled;
        defaults.buttons[i].pressed = layout->buttons[i].pressed;
        defaults.buttons[i].focus = layout->buttons[i].focus;
    }
    defaults.measured_luminance = layout->measured_luminance;
    defaults.effective_darkness = layout->effective_darkness;
    copy_string(defaults.actual_fonts, sizeof(defaults.actual_fonts), layout->actual_fonts);
    *layout = defaults;

    layout->version = (int)json_integer_value(json_object_get(root, "version"));
    if (json_integer_value(json_object_get(root, "revision")) > 0)
        layout->revision = (int)json_integer_value(json_object_get(root, "revision"));
    const char *title = json_string_value(json_object_get(root, "title"));
    if (title != NULL) copy_string(layout->title, sizeof(layout->title), title);

    json_t *font = json_object_get(root, "title_font");
    if (json_is_object(font)) {
        const char *family = json_string_value(json_object_get(font, "family"));
        if (family != NULL) copy_string(layout->font_family, sizeof(layout->font_family), family);
        json_int_t size = json_integer_value(json_object_get(font, "size"));
        if (size >= 12 && size <= 96) layout->font_size = (int)size;
    }
    json_t *background = json_object_get(root, "background");
    if (json_is_object(background)) {
        const char *mode = json_string_value(json_object_get(background, "mode"));
        if (mode != NULL && strcmp(mode, "light") == 0) layout->background_mode = BG_LIGHT;
        else if (mode != NULL && strcmp(mode, "dark") == 0) layout->background_mode = BG_DARK;
        else layout->background_mode = BG_ADAPTIVE;
        json_t *darken = json_object_get(background, "darken");
        if (json_is_number(darken)) layout->darken = json_number_value(darken);
    }
    layout->safe_area = parse_rect(root, "safe_area", layout->safe_area);
    layout->title_rect = parse_rect(root, "title_rect", layout->title_rect);
    json_t *buttons = json_object_get(root, "buttons");
    if (json_is_object(buttons)) {
        const char *names[ACTION_COUNT] = {"start", "settings", "exit"};
        for (int i = 0; i < ACTION_COUNT; ++i) {
            json_t *item = json_object_get(buttons, names[i]);
            if (json_is_object(item)) layout->buttons[i].visual = parse_rect(item, "x", layout->buttons[i].visual);
        }
    }
    json_t *hits = json_object_get(root, "touch_hits");
    if (json_is_object(hits)) {
        const char *names[ACTION_COUNT] = {"start", "settings", "exit"};
        for (int i = 0; i < ACTION_COUNT; ++i) {
            layout->buttons[i].hit = parse_rect(hits, names[i], layout->buttons[i].hit);
        }
    } else {
        layout_rebuild_hits(layout);
    }
    json_t *focus = json_object_get(root, "focus_order");
    if (json_is_array(focus) && json_array_size(focus) == ACTION_COUNT) {
        for (size_t i = 0; i < ACTION_COUNT; ++i) {
            layout->focus_order[i] = parse_action(json_string_value(json_array_get(focus, i)));
        }
    }
    snprintf(layout->actual_fonts, sizeof(layout->actual_fonts),
             "%s -> Noto Sans CJK SC -> Noto Sans -> DejaVu Sans -> embedded",
             layout->font_family);
}

static void parse_policy_json(json_t *root, ServerPolicy *policy) {
    policy->start = json_boolean_value(json_object_get(root, "start"));
    policy->settings = json_boolean_value(json_object_get(root, "settings"));
    policy->exit_allowed = json_boolean_value(json_object_get(root, "exit"));
}

static char *json_escape(const char *value) {
    json_t *string = json_string(value);
    char *encoded = json_dumps(string, JSON_COMPACT);
    json_decref(string);
    return encoded;
}

static char *device_json(char *buffer, size_t size) {
    const char *device = getenv("DEVICE_ID");
    if (device == NULL || *device == '\0') device = "terminal-01";
    char *escaped = json_escape(device);
    snprintf(buffer, size, "{\"device_id\":%s}", escaped ? escaped : "\"terminal-01\"");
    free(escaped);
    return buffer;
}

bool net_bootstrap(ControlLayout *layout, ServerPolicy *policy, const char *device_id,
                   NetResult *result) {
    (void)device_id;
    char body[128];
    device_json(body, sizeof(body));
    HttpResponse response = http_request("POST", "/api/devices/bootstrap", body, 2000);
    if (!response.ok || response.body == NULL) {
        set_result(result, false, response.status, response.error[0] ? response.error : "bootstrap failed");
        free(response.body);
        return false;
    }
    json_error_t error;
    json_t *root = json_loads(response.body, 0, &error);
    if (root == NULL) {
        set_result(result, false, response.status, error.text);
        free(response.body);
        return false;
    }
    parse_layout_json(json_object_get(root, "layout"), layout);
    parse_policy_json(json_object_get(
        json_object_get(json_object_get(root, "policy"), "server")), policy);
    set_result(result, true, response.status, NULL);
    json_decref(root);
    free(response.body);
    return true;
}

static void parse_command_result(NetResult *result, const char *body) {
    json_error_t error;
    json_t *root = json_loads(body, 0, &error);
    if (root == NULL) {
        snprintf(result->error, sizeof(result->error), "%s", error.text);
        result->loaded = false;
        return;
    }
    copy_string(result->command_id, sizeof(result->command_id),
                json_string_value(json_object_get(root, "command_id")));
    copy_string(result->action, sizeof(result->action),
                json_string_value(json_object_get(root, "action")));
    copy_string(result->status, sizeof(result->status),
                json_string_value(json_object_get(root, "status")));
    result->progress = (int)json_integer_value(json_object_get(root, "progress"));
    result->loaded = true;
    json_decref(root);
}

bool net_apply_command(const char *device_id, ControlAction action,
                       int layout_version, const char *command_id,
                       long timeout_ms, NetResult *result) {
    char path[128];
    char body[256];
    char *escaped_device = json_escape(device_id);
    char *escaped_id = json_escape(command_id);
    snprintf(path, sizeof(path), "/api/commands/%s", command_id);
    snprintf(body, sizeof(body),
             "{\"device_id\":%s,\"action\":\"%s\",\"layout_version\":%d}",
             escaped_device, control_action_name(action), layout_version);
    HttpResponse response = http_request("POST", path, body, timeout_ms);
    free(escaped_device);
    free(escaped_id);
    if (!response.ok && response.body != NULL) {
        json_error_t json_error;
        json_t *error_root = json_loads(response.body, 0, &json_error);
        json_t *error_object = error_root ? json_object_get(error_root, "error") : NULL;
        json_t *code = error_object ? json_object_get(error_object, "code") : NULL;
        json_t *message = error_object ? json_object_get(error_object, "message") : NULL;
        json_t *details = error_object ? json_object_get(error_object, "details") : NULL;
        json_t *active_id = details ? json_object_get(details, "active_command_id") : NULL;
        if (json_is_string(active_id)) {
            snprintf(result->active_command_id, sizeof(result->active_command_id),
                     "%s", json_string_value(active_id));
        }
        snprintf(result->error, sizeof(result->error), "%s: %s",
                 json_string_value(code) ? json_string_value(code) : "command_failed",
                 json_string_value(message) ? json_string_value(message) :
                 (response.error[0] ? response.error : "command failed"));
        result->status = response.status;
        result->ok = false;
        result->loaded = false;
        if (error_root) json_decref(error_root);
        free(response.body);
        return false;
    }
    set_result(result, response.ok, response.status,
               response.error[0] ? response.error : "command failed");
    if (response.body != NULL) parse_command_result(result, response.body);
    free(response.body);
    return response.ok;
}

bool net_update_command(const char *device_id, const char *command_id,
                        const char *status, int progress, const char *message,
                        NetResult *result) {
    char path[128];
    char body[512];
    char *escaped_device = json_escape(device_id);
    char *escaped_id = json_escape(command_id);
    char *escaped_status = json_escape(status);
    char *escaped_message = json_escape(message ? message : "");
    snprintf(path, sizeof(path), "/api/commands/%s/status", command_id);
    snprintf(body, sizeof(body),
             "{\"device_id\":%s,\"status\":%s,\"progress\":%d,\"result\":{\"message\":%s}}",
             escaped_device, escaped_status, progress, escaped_message);
    HttpResponse response = http_request("PUT", path, body, 1500);
    free(escaped_device);
    free(escaped_id);
    free(escaped_status);
    free(escaped_message);
    set_result(result, response.ok, response.status, response.error[0] ? response.error : "status update failed");
    if (response.body != NULL) parse_command_result(result, response.body);
    free(response.body);
    return response.ok;
}

static char *build_actual_json(const char *device_id, const ControlLayout *layout,
                               const char *status_line, bool online) {
    json_t *root = json_object();
    json_object_set_new(root, "device_id", json_string(device_id));
    json_object_set_new(root, "version", json_integer(layout->version));
    json_object_set_new(root, "online", json_boolean(online));
    json_object_set_new(root, "status_line", json_string(status_line));
    json_object_set_new(root, "background_mode",
                        json_string(layout_background_mode_name(layout->background_mode)));
    json_object_set_new(root, "measured_luminance", json_real(layout->measured_luminance));
    json_object_set_new(root, "effective_darkness", json_real(layout->effective_darkness));
    json_object_set_new(root, "actual_fonts", json_string(layout->actual_fonts));
    json_t *actual = json_object();
    json_t *buttons = json_object();
    json_t *hits = json_object();
    const char *names[ACTION_COUNT] = {"start", "settings", "exit"};
    for (int i = 0; i < ACTION_COUNT; ++i) {
        json_t *b = json_object();
        json_object_set_new(b, "x", json_integer(layout->buttons[i].visual.x));
        json_object_set_new(b, "y", json_integer(layout->buttons[i].visual.y));
        json_object_set_new(b, "w", json_integer(layout->buttons[i].visual.w));
        json_object_set_new(b, "h", json_integer(layout->buttons[i].visual.h));
        json_object_set_new(b, "state", json_string(button_state_name(layout->buttons[i].state)));
        json_object_set_new(b, "enabled", json_boolean(layout->buttons[i].enabled));
        json_object_set_new(buttons, names[i], b);
        json_t *h = json_pack("{s:i,s:i,s:i,s:i}", "x", layout->buttons[i].hit.x,
                              "y", layout->buttons[i].hit.y, "w", layout->buttons[i].hit.w,
                              "h", layout->buttons[i].hit.h);
        json_object_set_new(hits, names[i], h);
    }
    json_object_set_new(actual, "buttons", buttons);
    json_object_set_new(actual, "touch_hits", hits);
    json_object_set_new(actual, "title_rect",
                        json_pack("{s:i,s:i,s:i,s:i}", "x", layout->title_rect.x,
                                  "y", layout->title_rect.y, "w", layout->title_rect.w,
                                  "h", layout->title_rect.h));
    json_object_set_new(root, "actual", actual);
    char *text = json_dumps(root, JSON_COMPACT | JSON_UNESCAPED_UNICODE);
    json_decref(root);
    return text;
}

bool net_post_actual(const char *device_id, const ControlLayout *layout,
                     const char *status_line, bool online, NetResult *result) {
    char *actual_json = build_actual_json(device_id, layout, status_line, online);
    HttpResponse response = http_request("PUT", "/api/devices/actual", actual_json, 800);
    free(actual_json);
    set_result(result, response.ok, response.status, response.error[0] ? response.error : "actual failed");
    free(response.body);
    return response.ok;
}

bool net_post_recovery(const char *device_id, ControlAction action,
                       const char *message, const char *command_id,
                       int progress, NetResult *result) {
    char body[1024];
    char *device = json_escape(device_id);
    char *msg = json_escape(message);
    char *cid = json_escape(command_id);
    snprintf(body, sizeof(body),
             "{\"device_id\":%s,\"kind\":\"recovery\",\"action\":\"%s\",\"message\":%s,"
             "\"payload\":{\"command_id\":%s,\"progress\":%d}}",
             device, control_action_name(action), msg, cid, progress);
    HttpResponse response = http_request("POST", "/api/recovery", body, 1200);
    free(device);
    free(msg);
    free(cid);
    set_result(result, response.ok, response.status, response.error[0] ? response.error : "recovery failed");
    free(response.body);
    return response.ok;
}

bool net_reconcile_command(const char *device_id, const char *command_id,
                           ControlAction action, const char *local_status,
                           int progress, NetResult *result) {
    char body[512];
    char *device = json_escape(device_id);
    char *cid = json_escape(command_id);
    char *lstatus = json_escape(local_status);
    snprintf(body, sizeof(body),
             "{\"device_id\":%s,\"command_id\":%s,\"action\":\"%s\",\"local_status\":%s,\"progress\":%d}",
             device, cid, control_action_name(action), lstatus, progress);
    HttpResponse response = http_request("POST", "/api/commands/reconcile", body, 1500);
    free(device);
    free(cid);
    free(lstatus);
    set_result(result, response.ok, response.status, response.error[0] ? response.error : "reconcile failed");
    if (response.body != NULL) parse_command_result(result, response.body);
    free(response.body);
    return response.ok;
}
