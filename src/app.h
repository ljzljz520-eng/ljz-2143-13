#ifndef APP_H
#define APP_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

#include "layout.h"
#include "net.h"
#include "renderer.h"
#include "storage.h"
#include "window.h"

#define EVENT_QUEUE_SIZE 32

typedef enum {
    WORKER_NOOP = 0,
    WORKER_INVOKE_ACTION,
    WORKER_POLL,
    WORKER_EXIT,
    WORKER_RECONCILE_ONLY
} WorkerRequestKind;

typedef enum {
    UI_BOOTSTRAP,
    UI_LAYOUT,
    UI_COMMAND_RESULT,
    UI_PROGRESS,
    UI_EXIT_READY,
    UI_ONLINE,
    UI_MESSAGE
} UiEventKind;

typedef struct {
    WorkerRequestKind kind;
    ControlAction action;
    char command_id[80];
    int layout_version;
    Uint32 deadline_ms;
} WorkerRequest;

typedef struct {
    UiEventKind kind;
    ControlAction action;
    int status_code;
    int progress;
    int version;
    bool online;
    char command_id[80];
    char command_status[24];
    char message[256];
} UiEvent;

typedef struct {
    AppWindow window;
    SceneRenderer scene;
    Storage storage;
    ControlLayout layout;
    ServerPolicy server_policy;
    SDL_Thread *thread;
    SDL_mutex *request_mutex;
    SDL_cond *request_cond;
    SDL_mutex *ui_mutex;
    WorkerRequest requests[EVENT_QUEUE_SIZE];
    int request_head;
    int request_tail;
    UiEvent events[EVENT_QUEUE_SIZE];
    int event_head;
    int event_tail;
    SDL_atomic_t running;
    SDL_atomic_t online;
    SDL_atomic_t exit_requested;
    SDL_atomic_t start_confirming;
    SDL_atomic_t layout_version;
    ControlAction pressed_action;
    int pressed_version;
    int focus_index;
    bool settings_open;
    bool exiting;
    bool exit_ready;
    char queued_exit_command_id[80];
    char pending_command_id[80];
    char active_command_id[80];
    ControlAction active_command_action;
    char active_status[24];
    int active_progress;
    bool active_command_unacked;
    char status_line[256];
    char exit_message[256];
    Uint32 last_poll_ms;
    Uint32 last_actual_ms;
} VisualApp;

int app_run(VisualApp *app);

#endif
