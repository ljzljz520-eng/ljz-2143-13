#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define START_TASK_DURATION_MS 8000
#define NETWORK_EXIT_DEADLINE_MS 3000
#define EXIT_BUSINESS_GRACE_MS 12000
#define POLL_INTERVAL_ONLINE_MS 5000
#define POLL_INTERVAL_OFFLINE_MS 2500
#define ACTUAL_INTERVAL_MS 2000

static const char *device_id(void) {
    const char *id = getenv("DEVICE_ID");
    return (id != NULL && *id) ? id : "terminal-01";
}

static void set_status(VisualApp *app, const char *message) {
    snprintf(app->status_line, sizeof(app->status_line), "%s", message);
}

static void post_ui(VisualApp *app, UiEvent event) {
    SDL_LockMutex(app->ui_mutex);
    int next = (app->event_tail + 1) % EVENT_QUEUE_SIZE;
    if (next != app->event_head) {
        app->events[app->event_tail] = event;
        app->event_tail = next;
    }
    SDL_UnlockMutex(app->ui_mutex);
}

static bool pop_ui(VisualApp *app, UiEvent *event) {
    bool found = false;
    SDL_LockMutex(app->ui_mutex);
    if (app->event_head != app->event_tail) {
        *event = app->events[app->event_head];
        app->event_head = (app->event_head + 1) % EVENT_QUEUE_SIZE;
        found = true;
    }
    SDL_UnlockMutex(app->ui_mutex);
    return found;
}

static void queue_request(VisualApp *app, WorkerRequest request) {
    SDL_LockMutex(app->request_mutex);
    int next = (app->request_tail + 1) % EVENT_QUEUE_SIZE;
    if (next != app->request_head) {
        app->requests[app->request_tail] = request;
        app->request_tail = next;
        SDL_CondSignal(app->request_cond);
    }
    SDL_UnlockMutex(app->request_mutex);
}

static bool dequeue_request(VisualApp *app, WorkerRequest *request, int wait_ms) {
    bool found = false;
    SDL_LockMutex(app->request_mutex);
    if (app->request_head == app->request_tail) {
        SDL_CondWaitTimeout(app->request_cond, app->request_mutex, (Uint32)wait_ms);
    }
    if (app->request_head != app->request_tail) {
        *request = app->requests[app->request_head];
        app->request_head = (app->request_head + 1) % EVENT_QUEUE_SIZE;
        found = true;
    }
    SDL_UnlockMutex(app->request_mutex);
    return found;
}

static void queue_action(VisualApp *app, ControlAction action, const char *command_id) {
    WorkerRequest request = {
        .kind = WORKER_INVOKE_ACTION,
        .action = action,
        .layout_version = app->layout.version,
        .deadline_ms = SDL_GetTicks() + 60000,
    };
    snprintf(request.command_id, sizeof(request.command_id), "%s", command_id);
    queue_request(app, request);
}

static bool is_terminal_status(const char *status) {
    return strcmp(status, "SUCCESS") == 0 || strcmp(status, "FAILURE") == 0 ||
           strcmp(status, "INTERRUPTED") == 0;
}

static void update_button_states(VisualApp *app) {
    bool online = SDL_AtomicGet(&app->online) != 0;
    bool start_busy = app->active_command_action == ACTION_START &&
                      !is_terminal_status(app->active_status);

    for (int i = 0; i < ACTION_COUNT; ++i) {
        app->layout.buttons[i].focus = (app->focus_index == i);
        app->layout.buttons[i].pressed = (app->pressed_action == i);
    }

    bool start_server = online && app->server_policy.start;
    bool settings_server = online && app->server_policy.settings;
    app->layout.buttons[ACTION_START].enabled = start_server && !start_busy && !app->exiting;
    app->layout.buttons[ACTION_SETTINGS].enabled =
        (online ? settings_server : true) && !app->exiting;
    app->layout.buttons[ACTION_EXIT].enabled = true;

    ButtonControl *start = &app->layout.buttons[ACTION_START];
    if (!start->enabled && !start_busy && !app->exiting) start->state = BUTTON_DISABLED;
    else if (app->active_command_action == ACTION_START) {
        if (strcmp(app->active_status, "SUCCESS") == 0) start->state = BUTTON_SUCCESS;
        else if (strcmp(app->active_status, "FAILURE") == 0 ||
                 strcmp(app->active_status, "INTERRUPTED") == 0) start->state = BUTTON_FAILURE;
        else start->state = BUTTON_BUSY;
    } else if (start->enabled) {
        start->state = BUTTON_IDLE;
    }
    app->layout.buttons[ACTION_SETTINGS].state =
        app->layout.buttons[ACTION_SETTINGS].enabled ? BUTTON_IDLE : BUTTON_DISABLED;
    app->layout.buttons[ACTION_EXIT].state =
        app->exiting ? BUTTON_BUSY : BUTTON_IDLE;
}

static void handle_layout_event(VisualApp *app, const UiEvent *event) {
    (void)event;
    /* The worker writes a JSON-neutral copy through this event message. To
       avoid a second parser on the UI thread, successful bootstrap polls are
       represented by version + persisted cache reload. */
    ControlLayout updated;
    if (storage_load_layout(&app->storage, &updated)) {
        ButtonVisualState old_state[ACTION_COUNT];
        bool old_enabled[ACTION_COUNT];
        for (int i = 0; i < ACTION_COUNT; ++i) {
            old_state[i] = app->layout.buttons[i].state;
            old_enabled[i] = app->layout.buttons[i].enabled;
        }
        double luminance = app->scene.background_luminance;
        int old_version = app->layout.version;
        updated.measured_luminance = luminance;
        layout_update_effective_darkness(&updated, luminance);
        app->layout = updated;
        for (int i = 0; i < ACTION_COUNT; ++i) {
            app->layout.buttons[i].state = old_state[i];
            app->layout.buttons[i].enabled = old_enabled[i];
        }
        if (old_version != app->layout.version) {
            SDL_AtomicSet(&app->layout_version, app->layout.version);
            set_status(app, "布局已换版；按下但未抬起的旧版本点击已取消。");
            app->pressed_action = ACTION_NONE;
        }
        update_button_states(app);
    }
}

static void report_recovery_best_effort(VisualApp *app, ControlAction action,
                                        const char *message, const char *command_id,
                                        int progress) {
    storage_recovery_append(&app->storage, action, message, command_id, progress);
    NetResult result;
    if (!net_post_recovery(device_id(), action, message, command_id, progress, &result)) {
        storage_recovery_append(&app->storage, action,
                                "服务不可达，恢复记录保留在终端本地", command_id, progress);
    }
}

static void worker_run_command(VisualApp *app, const WorkerRequest *request) {
    Uint32 deadline_ms = request->deadline_ms;
    /* A fixed command id survives timeout/retry. The local active guard and
       the server active command table prevent two start tasks. */
    if (request->action == ACTION_START &&
        (SDL_AtomicGet(&app->start_confirming) ||
         (app->active_command_id[0] != '\0' &&
          app->active_command_action == ACTION_START &&
          !is_terminal_status(app->active_status)))) {
        UiEvent event = {.kind = UI_MESSAGE, .action = ACTION_START};
        snprintf(event.message, sizeof(event.message),
                 "开始任务仍在执行；忽略重复启动。");
        post_ui(app, event);
        return;
    }
    snprintf(app->active_command_id, sizeof(app->active_command_id), "%s", request->command_id);
    app->active_command_action = request->action;
    snprintf(app->active_status, sizeof(app->active_status), "CONFIRMING");
    app->active_progress = 0;
    app->active_command_unacked = false;

    ActiveJournal journal;
    journal_set(&journal, request->command_id, request->action, "CONFIRMING", 0);
    storage_save_journal(&app->storage, &journal);

    /* A queued request can outlive a late layout poll. Exit remains exempt;
       it is a safety action and is enforced locally as well. */
    if (request->action != ACTION_EXIT &&
        request->layout_version != SDL_AtomicGet(&app->layout_version)) {
        UiEvent stale = {.kind = UI_COMMAND_RESULT, .action = request->action};
        snprintf(stale.command_id, sizeof(stale.command_id), "%s", request->command_id);
        snprintf(stale.command_status, sizeof(stale.command_status), "FAILURE");
        snprintf(stale.message, sizeof(stale.message),
                 "命令执行前布局已换版，旧版本动作被取消。");
        storage_clear_journal(&app->storage);
        SDL_AtomicSet(&app->start_confirming, 0);
        post_ui(app, stale);
        return;
    }

    NetResult result;
    long command_timeout = request->action == ACTION_EXIT ? 1000L : 2000L;
    bool ok = net_apply_command(device_id(), request->action, request->layout_version,
                                request->command_id, command_timeout, &result);
    UiEvent accepted = {
        .kind = UI_COMMAND_RESULT,
        .action = request->action,
        .status_code = result.http_status,
        .progress = 0,
    };
    snprintf(accepted.command_id, sizeof(accepted.command_id), "%s", request->command_id);
    if (!ok) {
        if (result.http_status == 409 &&
            (strstr(result.error, "layout_version_stale") != NULL ||
             strstr(result.error, "旧布局") != NULL)) {
            snprintf(accepted.command_status, sizeof(accepted.command_status), "FAILURE");
            snprintf(accepted.message, sizeof(accepted.message),
                     "按下到抬起之间布局换版，旧版本命令未执行。");
            storage_clear_journal(&app->storage);
            snprintf(app->active_status, sizeof(app->active_status), "INTERRUPTED");
            app->active_command_action = ACTION_START;
            SDL_AtomicSet(&app->start_confirming, 0);
            report_recovery_best_effort(app, request->action, accepted.message,
                                        request->command_id, 0);
            post_ui(app, accepted);
            return;
        }
        if (request->action == ACTION_EXIT) {
            snprintf(accepted.command_status, sizeof(accepted.command_status), "CONFIRMING");
            snprintf(accepted.message, sizeof(accepted.message),
                     "退出确认服务不可达，等待受限保存期限后受控退出。");
            post_ui(app, accepted);
            while (SDL_GetTicks() < deadline_ms && SDL_AtomicGet(&app->running)) {
                SDL_Delay(50);
            }
            report_recovery_best_effort(app, ACTION_EXIT,
                                        "服务不可达，已达到受控退出期限；保存待恢复",
                                        request->command_id, 0);
            UiEvent exit_event = {.kind = UI_EXIT_READY, .action = ACTION_EXIT};
            snprintf(exit_event.command_id, sizeof(exit_event.command_id), "%s", request->command_id);
            snprintf(exit_event.command_status, sizeof(exit_event.command_status), "INTERRUPTED");
            post_ui(app, exit_event);
            return;
        }
        if (result.http_status == 409 &&
            strstr(result.error, "duplicate_active_command") != NULL) {
            if (result.active_command_id[0] != '\0') {
                snprintf(app->active_command_id, sizeof(app->active_command_id),
                         "%s", result.active_command_id);
            }
            snprintf(result.status, sizeof(result.status), "RUNNING");
            result.loaded = true;
            result.ok = true;
            snprintf(accepted.command_status, sizeof(accepted.command_status), "RUNNING");
            snprintf(accepted.message, sizeof(accepted.message),
                     "服务器已有同一活动任务；已挂接原任务，不会启动第二份。");
            app->active_command_action = ACTION_START;
            SDL_AtomicSet(&app->start_confirming, 0);
            goto command_accepted;
        }
        snprintf(accepted.command_status, sizeof(accepted.command_status), "FAILURE");
        snprintf(accepted.message, sizeof(accepted.message),
                 "网络超时或服务器拒绝；可重按“开始”重试同一命令，不会启动两份任务。");
        report_recovery_best_effort(app, request->action, accepted.message,
                                    request->command_id, 0);
        storage_clear_journal(&app->storage);
        /* Keep the command id for a retry. If the first request actually
           reached the server but the response timed out, retrying this id is
           idempotent; the active-command table rejects a different id. */
        app->active_command_action = ACTION_START;
        snprintf(app->pending_command_id, sizeof(app->pending_command_id),
                 "%s", app->active_command_id);
        SDL_AtomicSet(&app->start_confirming, 0);
        post_ui(app, accepted);
        return;
    }

command_accepted:
    if (result.status[0]) snprintf(app->active_status, sizeof(app->active_status), "%s", result.status);
    if (result.action[0]) app->active_command_action = request->action;
    snprintf(accepted.command_status, sizeof(accepted.command_status), "%s", app->active_status);
    post_ui(app, accepted);

    if (request->action == ACTION_START) {
        Uint32 started = SDL_GetTicks();
        Uint32 last_progress_update = 0;
        bool cancelled = false;
        const char *final_status = "SUCCESS";
        while (SDL_AtomicGet(&app->running)) {
            int elapsed = (int)(SDL_GetTicks() - started);
            int progress = elapsed * 100 / START_TASK_DURATION_MS;
            if (progress > 99) progress = 99;
            if (SDL_AtomicGet(&app->exit_requested)) {
                cancelled = true;
                final_status = "INTERRUPTED";
                break;
            }
            if (elapsed >= START_TASK_DURATION_MS) {
                progress = 100;
                final_status = "SUCCESS";
                break;
            }
            app->active_progress = progress;
            if (elapsed - (int)last_progress_update >= 1000 || last_progress_update == 0) {
                last_progress_update = (Uint32)elapsed;
                UiEvent tick = {.kind = UI_PROGRESS, .action = ACTION_START, .progress = progress};
                snprintf(tick.command_id, sizeof(tick.command_id), "%s", request->command_id);
                post_ui(app, tick);
                NetResult update_result;
                char update_message[128];
                snprintf(update_message, sizeof(update_message), "任务进度 %d%%", progress);
                if (!net_update_command(device_id(), request->command_id, "RUNNING",
                                        progress, update_message, &update_result)) {
                    /* Offline during a long accepted task is allowed to
                       continue, but the journal marks it for reconciliation. */
                    app->active_command_unacked = true;
                    journal_set(&journal, request->command_id, ACTION_START,
                                "RUNNING", progress);
                    storage_save_journal(&app->storage, &journal);
                }
            }
            /* 100 ms checkpoints make an exit click responsive; status is
               still only pushed to the server about once per second. */
            SDL_Delay(100);
        }
        app->active_progress = strcmp(final_status, "SUCCESS") == 0 ? 100 : app->active_progress;
        NetResult finish;
        char message[160];
        snprintf(message, sizeof(message), "%s%s",
                 cancelled ? "长任务期间退出，未完成操作已记录：" : "业务保存确认完成。",
                 cancelled ? "等待恢复" : "");
        net_update_command(device_id(), request->command_id, final_status,
                           app->active_progress, message, &finish);
        if (cancelled || app->active_command_unacked) {
            report_recovery_best_effort(app, ACTION_START, message,
                                        request->command_id, app->active_progress);
            if (!cancelled) final_status = "FAILURE";
        }
        UiEvent done = {.kind = UI_COMMAND_RESULT, .action = ACTION_START,
                        .progress = app->active_progress};
        snprintf(done.command_id, sizeof(done.command_id), "%s", request->command_id);
        snprintf(done.command_status, sizeof(done.command_status), "%s", final_status);
        snprintf(done.message, sizeof(done.message), "%s", message);
        post_ui(app, done);
        if (cancelled) {
            UiEvent note = {.kind = UI_MESSAGE, .action = ACTION_START};
            snprintf(note.message, sizeof(note.message),
                     "长任务已中止并记录；继续执行退出保存流程。");
            post_ui(app, note);
        } else if (app->active_command_unacked) {
            journal_set(&journal, request->command_id, ACTION_START,
                        "INTERRUPTED", app->active_progress);
            storage_save_journal(&app->storage, &journal);
        } else {
            storage_clear_journal(&app->storage);
        }
        return;
    }

    if (request->action == ACTION_SETTINGS) {
        NetResult done;
        net_update_command(device_id(), request->command_id, "SUCCESS", 100,
                           "设置面板已打开", &done);
        UiEvent done_event = {.kind = UI_COMMAND_RESULT, .action = ACTION_SETTINGS, .progress = 100};
        snprintf(done_event.command_id, sizeof(done_event.command_id), "%s", request->command_id);
        snprintf(done_event.command_status, sizeof(done_event.command_status), "SUCCESS");
        snprintf(done_event.message, sizeof(done_event.message), "设置命令已确认。");
        post_ui(app, done_event);
        app->active_command_action = ACTION_NONE;
        app->active_status[0] = '\0';
        app->active_command_id[0] = '\0';
        storage_clear_journal(&app->storage);
        return;
    }

    if (request->action == ACTION_EXIT) {
        Uint32 exit_save_deadline = SDL_GetTicks() + NETWORK_EXIT_DEADLINE_MS;
        if (deadline_ms > exit_save_deadline) deadline_ms = exit_save_deadline;
        bool saved = false;
        while (SDL_GetTicks() < deadline_ms) {
            NetResult save;
            if (net_update_command(device_id(), request->command_id, "RUNNING",
                                   50, "等待业务确认保存与渲染资源释放", &save)) {
                saved = true;
                break;
            }
            SDL_Delay(200);
        }
        const char *final_status = saved ? "SUCCESS" : "INTERRUPTED";
        NetResult finish;
        net_update_command(device_id(), request->command_id, final_status,
                           saved ? 100 : 50,
                           saved ? "保存完成，允许释放资源" : "服务不可达，受控退出并保留恢复记录",
                           &finish);
        if (!saved) {
            report_recovery_best_effort(app, ACTION_EXIT,
                                        "服务不可达，按超时受控退出；业务保存待恢复",
                                        request->command_id, 50);
        } else {
            storage_clear_journal(&app->storage);
        }
        UiEvent ready = {.kind = UI_EXIT_READY, .action = ACTION_EXIT,
                         .progress = saved ? 100 : 50};
        snprintf(ready.command_id, sizeof(ready.command_id), "%s", request->command_id);
        snprintf(ready.command_status, sizeof(ready.command_status), "%s", final_status);
        post_ui(app, ready);
    }
}

static bool worker_has_active_start(VisualApp *app) {
    return app->active_command_action == ACTION_START &&
           app->active_command_id[0] != '\0' &&
           !is_terminal_status(app->active_status);
}

static int worker_thread(void *opaque) {
    VisualApp *app = opaque;
    ServerPolicy policy = {.start = true, .settings = true, .exit_allowed = true};
    ControlLayout remote;
    NetResult bootstrap;
    bool ok = net_bootstrap(&remote, &policy, device_id(), &bootstrap);
    UiEvent boot = {.kind = UI_BOOTSTRAP, .online = ok, .status_code = bootstrap.http_status};
    if (ok) {
        layout_update_effective_darkness(&remote, app->scene.background_luminance);
        storage_save_layout(&app->storage, &remote);
        app->server_policy = policy;
        boot.version = remote.version;
        snprintf(boot.message, sizeof(boot.message), "已从服务器获取布局 v%d。", remote.version);
    } else {
        snprintf(boot.message, sizeof(boot.message),
                 "服务不可达，使用终端缓存；断网下仅允许设置与受控退出。");
    }
    post_ui(app, boot);
    if (ok) {
        NetResult initial_actual;
        net_post_actual(device_id(), &remote, "终端已上报设备实际布局。", true, &initial_actual);
    }

    Uint32 last_poll = 0;
    while (SDL_AtomicGet(&app->running)) {
        WorkerRequest request;
        if (dequeue_request(app, &request, 200)) {
            if (request.kind == WORKER_EXIT) break;
            if (request.kind == WORKER_INVOKE_ACTION) {
                if (request.action == ACTION_START &&
                    (worker_has_active_start(app) ||
                     SDL_AtomicGet(&app->start_confirming))) {
                    UiEvent duplicate = {.kind = UI_MESSAGE, .action = ACTION_START};
                    snprintf(duplicate.message, sizeof(duplicate.message),
                             "开始任务已存在；忽略第二份启动请求。");
                    post_ui(app, duplicate);
                } else {
                    worker_run_command(&app, &request);
                }
            }
            continue;
        }
        Uint32 now = SDL_GetTicks();
        Uint32 interval = SDL_AtomicGet(&app->online) ? POLL_INTERVAL_ONLINE_MS
                                                      : POLL_INTERVAL_OFFLINE_MS;
        if (now - last_poll < interval) continue;
        last_poll = now;
        NetResult poll;
        bool poll_ok = net_bootstrap(&remote, &policy, device_id(), &poll);
        UiEvent online_event = {.kind = UI_ONLINE, .online = poll_ok,
                                .status_code = poll.http_status};
        if (poll_ok) {
            layout_update_effective_darkness(&remote, app->scene.background_luminance);
            bool changed = remote.version != SDL_AtomicGet(&app->layout_version);
            if (remote.version < SDL_AtomicGet(&app->layout_version) && SDL_AtomicGet(&app->layout_version) > 0) {
                changed = false;
            }
            storage_save_layout(&app->storage, &remote);
            app->server_policy = policy;
            online_event.kind = UI_LAYOUT;
            online_event.version = remote.version;
            snprintf(online_event.message, sizeof(online_event.message),
                     changed ? "检测到新版本布局。" : "服务器在线。");
        } else {
            snprintf(online_event.message, sizeof(online_event.message), "服务器不可达。");
        }
        post_ui(app, online_event);
        if (poll_ok) {
            NetResult actual;
            char actual_status[192];
            snprintf(actual_status, sizeof(actual_status),
                     "终端正在执行布局 v%d，暗化 %.2f，字体链 %s",
                     remote.version, remote.effective_darkness, remote.actual_fonts);
            net_post_actual(device_id(), &remote, actual_status, true, &actual);
        }
    }
    return 0;
}

static ControlAction hit_action(VisualApp *app, int x, int y) {
    for (int i = ACTION_COUNT - 1; i >= 0; --i) {
        if (point_in_rect(x, y, app->layout.buttons[i].hit)) return (ControlAction)i;
    }
    return ACTION_NONE;
}

static void activate_action(VisualApp *app, ControlAction action) {
    if (action == ACTION_SETTINGS && app->exiting) return;
    if (action == ACTION_SETTINGS) {
        app->settings_open = !app->settings_open;
        char command_id[80];
        storage_generate_command_id(command_id, sizeof(command_id));
        if (!app->settings_open) {
            set_status(app, "设置面板已关闭。");
            return;
        }
        if (SDL_AtomicGet(&app->online) && app->server_policy.settings) {
            set_status(app, "设置命令确认中…");
            queue_action(app, ACTION_SETTINGS, command_id);
        } else if (!SDL_AtomicGet(&app->online)) {
            set_status(app, "断网本地策略：允许查看设置；命令不会写入服务器。");
        } else {
            set_status(app, "服务器策略禁止设置。");
            app->settings_open = false;
        }
        return;
    }
    if (action == ACTION_EXIT) {
        if (app->exiting) return;
        app->exiting = true;
        app->settings_open = false;
        app->pressed_action = ACTION_NONE;
        SDL_AtomicSet(&app->exit_requested, 1);
        char command_id[80];
        storage_generate_command_id(command_id, sizeof(command_id));
        snprintf(app->queued_exit_command_id, sizeof(app->queued_exit_command_id),
                 "%s", command_id);
        snprintf(app->exit_message, sizeof(app->exit_message),
                 "等待业务确认保存；普通退出最多 %d ms，长任务场景最多 %d ms。",
                 NETWORK_EXIT_DEADLINE_MS, EXIT_BUSINESS_GRACE_MS);
        set_status(app, "退出流程开始：等待长任务/保存与资源释放。");
        WorkerRequest exit_request = {
            .kind = WORKER_INVOKE_ACTION,
            .action = ACTION_EXIT,
            .layout_version = app->layout.version,
            .deadline_ms = SDL_GetTicks() + EXIT_BUSINESS_GRACE_MS,
        };
        snprintf(exit_request.command_id, sizeof(exit_request.command_id), "%s", command_id);
        queue_request(app, exit_request);
        return;
    }
    if (action == ACTION_START) {
        if (!app->layout.buttons[ACTION_START].enabled) {
            set_status(app, "开始当前不可用：服务器策略禁止、服务不可达或已有任务执行中。");
            return;
        }
        bool reusable_timeout = app->active_command_id[0] != '\0' &&
            app->active_command_action == ACTION_START &&
            strcmp(app->active_status, "FAILURE") == 0 &&
            SDL_AtomicGet(&app->start_confirming) == 0;
        if (!reusable_timeout) {
            storage_generate_command_id(app->pending_command_id,
                                        sizeof(app->pending_command_id));
        } else {
            snprintf(app->pending_command_id, sizeof(app->pending_command_id),
                     "%s", app->active_command_id);
        }
        app->active_command_action = ACTION_START;
        snprintf(app->active_status, sizeof(app->active_status), "CONFIRMING");
        app->active_progress = 0;
        SDL_AtomicSet(&app->start_confirming, 1);
        set_status(app, "开始命令确认中；超时重按使用同一命令 ID 幂等去重…");
        queue_action(app, ACTION_START, app->pending_command_id);
    }
}

static void handle_mouse_down(VisualApp *app, const SDL_MouseButtonEvent *event) {
    if (event->button != SDL_BUTTON_LEFT || app->exiting) return;
    ControlAction action = hit_action(app, event->x, event->y);
    if (action != ACTION_NONE) {
        app->pressed_action = action;
        app->pressed_version = app->layout.version;
    }
}

static void handle_mouse_up(VisualApp *app, const SDL_MouseButtonEvent *event) {
    if (event->button != SDL_BUTTON_LEFT) return;
    ControlAction pressed = app->pressed_action;
    app->pressed_action = ACTION_NONE;
    if (pressed == ACTION_NONE) return;
    ControlAction released = hit_action(app, event->x, event->y);
    if (released != pressed) {
        set_status(app, "抬起位置离开控件，命令取消。");
        return;
    }
    if (app->pressed_version != app->layout.version) {
        set_status(app, "按下到抬起之间布局换版，请按新布局重新操作。");
        return;
    }
    activate_action(app, pressed);
}

static void advance_focus(VisualApp *app, int direction) {
    for (int step = 1; step <= ACTION_COUNT; ++step) {
        app->focus_index = (app->focus_index + direction + ACTION_COUNT) % ACTION_COUNT;
        ControlAction action = app->layout.focus_order[app->focus_index];
        if (app->layout.buttons[action].enabled) break;
    }
}

static void process_ui_event(VisualApp *app, const UiEvent *event) {
    switch (event->kind) {
        case UI_BOOTSTRAP:
        case UI_LAYOUT:
            SDL_AtomicSet(&app->online, event->online ? 1 : 0);
            if (event->online || event->kind == UI_LAYOUT) handle_layout_event(app, event);
            set_status(app, event->message);
            break;
        case UI_ONLINE:
            SDL_AtomicSet(&app->online, event->online ? 1 : 0);
            if (!event->online) set_status(app, event->message);
            break;
        case UI_PROGRESS:
            app->active_progress = event->progress;
            if (event->command_id[0] &&
                (app->active_command_id[0] == '\0' ||
                 strcmp(event->command_id, app->active_command_id) == 0 ||
                 SDL_AtomicGet(&app->start_confirming))) {
                snprintf(app->active_command_id, sizeof(app->active_command_id), "%s", event->command_id);
                app->active_command_action = ACTION_START;
                snprintf(app->active_status, sizeof(app->active_status), "RUNNING");
                SDL_AtomicSet(&app->start_confirming, 0);
                char status[256];
                snprintf(status, sizeof(status), "开始执行中：%d%%", event->progress);
                set_status(app, status);
            }
            break;
        case UI_COMMAND_RESULT:
            if (event->command_id[0]) {
                snprintf(app->active_command_id, sizeof(app->active_command_id), "%s", event->command_id);
                app->active_command_action = event->action;
                if (event->action == ACTION_START &&
                    strcmp(event->command_status, "RUNNING") == 0) {
                    SDL_AtomicSet(&app->start_confirming, 0);
                }
                if (event->command_status[0]) {
                    snprintf(app->active_status, sizeof(app->active_status), "%s", event->command_status);
                }
                app->active_progress = event->progress;
                if (event->action == ACTION_SETTINGS &&
                    strcmp(event->command_status, "SUCCESS") == 0) {
                    app->settings_open = true;
                    app->active_command_action = ACTION_NONE;
                    app->active_status[0] = '\0';
                    app->active_command_id[0] = '\0';
                } else if (event->action == ACTION_SETTINGS &&
                           is_terminal_status(event->command_status)) {
                    app->settings_open = false;
                }
                if (event->action == ACTION_START) {
                    if (strcmp(event->command_status, "SUCCESS") == 0) {
                        app->active_command_action = ACTION_NONE;
                        app->active_status[0] = '\0';
                        app->active_command_id[0] = '\0';
                        app->pending_command_id[0] = '\0';
                        SDL_AtomicSet(&app->start_confirming, 0);
                    } else if (is_terminal_status(event->command_status)) {
                        app->active_command_action = ACTION_START;
                        SDL_AtomicSet(&app->start_confirming, 0);
                    }
                }
            }
            if (event->message[0]) set_status(app, event->message);
            break;
        case UI_EXIT_READY:
            app->exit_ready = 1;
            set_status(app, "退出保存阶段完成，正在释放渲染资源…");
            break;
        case UI_MESSAGE:
            set_status(app, event->message);
            break;
    }
    update_button_states(app);
}

static void reconcile_stale_journal(VisualApp *app) {
    ActiveJournal journal;
    if (!storage_load_journal(&app->storage, &journal)) return;
    ControlAction action = ACTION_NONE;
    if (strcmp(journal.action, "start") == 0) action = ACTION_START;
    else if (strcmp(journal.action, "settings") == 0) action = ACTION_SETTINGS;
    else if (strcmp(journal.action, "exit") == 0) action = ACTION_EXIT;
    char message[300];
    snprintf(message, sizeof(message),
             "检测到未完成操作（%s，%s，%d%%）；已形成恢复记录。",
             journal.action, journal.status, journal.progress);
    if (action != ACTION_NONE) {
        NetResult result;
        if (net_reconcile_command(device_id(), journal.command_id, action,
                                  "INTERRUPTED", journal.progress, &result)) {
            snprintf(message, sizeof(message), "已与服务器对齐未完成操作 %s。", journal.action);
        }
    }
    report_recovery_best_effort(app, action == ACTION_NONE ? ACTION_EXIT : action,
                                message, journal.command_id, journal.progress);
    storage_clear_journal(&app->storage);
    set_status(app, message);
}

int app_run(VisualApp *app) {
    memset(app, 0, sizeof(*app));
    SDL_AtomicSet(&app->running, 1);
    SDL_AtomicSet(&app->online, 0);
    SDL_AtomicSet(&app->start_confirming, 0);
    app->pressed_action = ACTION_NONE;
    app->focus_index = 0;
    app->request_head = app->request_tail = 0;
    app->event_head = app->event_tail = 0;
    storage_init(&app->storage);
    layout_set_defaults(&app->layout);
    renderer_init_scene(&app->scene);

    if (!window_init(&app->window, "背景窗口控制台", SCREEN_WIDTH, SCREEN_HEIGHT)) {
        renderer_destroy(&app->scene);
        return 1;
    }
    if (!renderer_load_background(&app->scene, app->window.renderer,
                                  getenv("BACKGROUND_IMAGE_PATH") ?
                                  getenv("BACKGROUND_IMAGE_PATH") : "assets/background.png")) {
        window_destroy(&app->window);
        renderer_destroy(&app->scene);
        return 1;
    }
    if (storage_load_layout(&app->storage, &app->layout)) {
        set_status(app, "已载入本地缓存布局，正在连接服务器。");
    } else {
        set_status(app, "使用内置安全布局，正在连接服务器。");
    }
    layout_update_effective_darkness(&app->layout, app->scene.background_luminance);
    SDL_AtomicSet(&app->layout_version, app->layout.version);

    app->request_mutex = SDL_CreateMutex();
    app->request_cond = SDL_CreateCond();
    app->ui_mutex = SDL_CreateMutex();
    http_global_init();
    reconcile_stale_journal(app);
    app->thread = SDL_CreateThread(worker_thread, "control-worker", app);
    update_button_states(app);

    Uint32 last_actual = 0;
    while (SDL_AtomicGet(&app->running)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT &&
                 event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                activate_action(app, ACTION_EXIT);
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                handle_mouse_down(app, &event.button);
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                handle_mouse_up(app, &event.button);
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_TAB) {
                    advance_focus(app, event.key.keysym.mod & KMOD_SHIFT ? -1 : 1);
                } else if (event.key.keysym.sym == SDLK_RETURN ||
                           event.key.keysym.sym == SDLK_SPACE) {
                    ControlAction focused = app->layout.focus_order[app->focus_index];
                    if (app->layout.buttons[focused].enabled) activate_action(app, focused);
                } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                    app->settings_open = false;
                }
            }
        }

        UiEvent ui_event;
        while (pop_ui(app, &ui_event)) process_ui_event(app, &ui_event);
        update_button_states(app);

        renderer_draw_scene(&app->scene, app->window.renderer, &app->layout,
                            app->window.width, app->window.height, app->status_line,
                            app->settings_open, app->exiting, app->exit_message);
        if (app->exit_ready) {
            SDL_Delay(500);
            SDL_AtomicSet(&app->running, 0);
        }
        SDL_Delay(16);
    }

    SDL_AtomicSet(&app->running, 0);
    if (app->thread != NULL) {
        WorkerRequest stop = {.kind = WORKER_EXIT};
        queue_request(app, stop);
        SDL_WaitThread(app->thread, NULL);
    }
    renderer_destroy(&app->scene);
    window_destroy(&app->window);
    http_global_cleanup();
    SDL_DestroyMutex(app->request_mutex);
    SDL_DestroyMutex(app->ui_mutex);
    SDL_DestroyCond(app->request_cond);
    return 0;
}
