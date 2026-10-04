-- Visual Window control backend schema (SQLite)

CREATE TABLE IF NOT EXISTS devices (
    id              INTEGER PRIMARY KEY,
    device_key      TEXT UNIQUE NOT NULL,
    name            TEXT NOT NULL,
    screen_w        INTEGER NOT NULL,
    screen_h        INTEGER NOT NULL,
    dpi             INTEGER NOT NULL DEFAULT 96,
    min_touch       INTEGER NOT NULL DEFAULT 44,
    online          INTEGER NOT NULL DEFAULT 0,
    last_seen       INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS drafts (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       INTEGER NOT NULL REFERENCES devices(id),
    title           TEXT NOT NULL,
    font_size_px    INTEGER NOT NULL,
    arrangement     TEXT NOT NULL CHECK (arrangement IN ('inline','split')),
    base_version    INTEGER NOT NULL,          -- published-layout base
    rev             INTEGER NOT NULL DEFAULT 1,-- monotonic draft revision
    editor          TEXT NOT NULL DEFAULT '',
    updated_by      TEXT NOT NULL DEFAULT '',
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_drafts_device ON drafts(device_id);

CREATE TABLE IF NOT EXISTS layouts (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       INTEGER NOT NULL REFERENCES devices(id),
    version         INTEGER NOT NULL,
    title           TEXT NOT NULL,
    font_size_px    INTEGER NOT NULL,
    arrangement     TEXT NOT NULL,
    spec_hash       INTEGER NOT NULL,
    published_by    TEXT NOT NULL DEFAULT '',
    created_at      INTEGER NOT NULL,
    UNIQUE(device_id, version)
);

CREATE TABLE IF NOT EXISTS commands (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       INTEGER NOT NULL REFERENCES devices(id),
    idempotency_key TEXT NOT NULL,
    widget          TEXT NOT NULL,
    layout_version  INTEGER NOT NULL,
    spec_hash       INTEGER NOT NULL,
    payload         TEXT NOT NULL,
    accepted        INTEGER NOT NULL DEFAULT 0,
    reject_reason   TEXT NOT NULL DEFAULT '',
    created_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_commands_key ON commands(idempotency_key);

CREATE TABLE IF NOT EXISTS tasks (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       INTEGER NOT NULL REFERENCES devices(id),
    idempotency_key TEXT UNIQUE NOT NULL,      -- single task per press key
    state           TEXT NOT NULL CHECK (state IN ('running','done','failed')),
    code            INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    finished_at     INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS device_journal (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       INTEGER NOT NULL REFERENCES devices(id),
    idempotency_key TEXT NOT NULL,
    action          TEXT NOT NULL,
    task_id         INTEGER NOT NULL DEFAULT 0,
    status          TEXT NOT NULL CHECK (status IN ('pending','done','failed')),
    received_at     INTEGER NOT NULL,
    UNIQUE(device_id, idempotency_key, status)
);

CREATE TABLE IF NOT EXISTS policy_cache (
    device_id       INTEGER PRIMARY KEY REFERENCES devices(id),
    start_allowed   INTEGER NOT NULL DEFAULT 1,
    settings_allowed INTEGER NOT NULL DEFAULT 1,
    exit_allowed    INTEGER NOT NULL DEFAULT 1,
    decided_at      INTEGER NOT NULL
);
