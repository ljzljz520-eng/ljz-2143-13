#!/usr/bin/env python3
"""Layout and command control service.

The service intentionally uses only the Python standard library plus SQLite.
It owns layout validation/publishing, per-device availability policy, command
idempotency, actual-device snapshots, and recovery/audit records.
"""

from __future__ import annotations

import json
import os
import sqlite3
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_ROOT = os.path.join(ROOT, "web")
DB_PATH = os.environ.get("CONTROL_DB", os.path.join(ROOT, "data", "control.db"))
HOST = os.environ.get("CONTROL_HOST", "0.0.0.0")
PORT = int(os.environ.get("CONTROL_PORT", "8080"))
DEVICE_ID = os.environ.get("DEVICE_ID", "terminal-01")
SCREEN_W = 1280
SCREEN_H = 720
MIN_TOUCH = 48
ACTIVE_STATUSES = ("QUEUED", "RUNNING", "CONFIRMING")
TERMINAL_STATUSES = ("SUCCESS", "FAILURE", "INTERRUPTED")

DEFAULT_LAYOUT = {
    "version": 1,
    "revision": 1,
    "title": "视觉窗口控制台",
    "title_font": {
        "family": "Noto Sans CJK SC, Noto Sans, DejaVu Sans, sans-serif",
        "size": 28,
        "fallback_chain": ["Noto Sans CJK SC", "Noto Sans", "DejaVu Sans", "embedded"],
    },
    "background": {"mode": "adaptive", "darken": 0.42},
    "safe_area": {"x": 16, "y": 16, "w": 1248, "h": 688},
    "title_rect": {"x": 24, "y": 20, "w": 900, "h": 48},
    "buttons": {
        "start": {"x": 432, "y": 600, "w": 168, "h": 52, "label": "开始"},
        "settings": {"x": 680, "y": 600, "w": 168, "h": 52, "label": "设置"},
        "exit": {"x": 1160, "y": 16, "w": 104, "h": 48, "label": "退出"},
    },
    "focus_order": ["start", "settings", "exit"],
}

POLICY_ACTIONS = ("start", "settings", "exit")


def now_ms() -> int:
    return int(time.time() * 1000)


def ensure_parent(path: str) -> None:
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)


def connect_db() -> sqlite3.Connection:
    ensure_parent(DB_PATH)
    conn = sqlite3.connect(DB_PATH, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_db() -> None:
    ensure_parent(DB_PATH)
    with connect_db() as db:
        db.executescript(
            """
            CREATE TABLE IF NOT EXISTS layouts (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                draft_json TEXT NOT NULL,
                draft_revision INTEGER NOT NULL,
                published_json TEXT NOT NULL,
                published_version INTEGER NOT NULL,
                published_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                last_seen INTEGER,
                actual_json TEXT,
                updated_at INTEGER
            );
            CREATE TABLE IF NOT EXISTS device_policy (
                device_id TEXT NOT NULL,
                action TEXT NOT NULL,
                enabled INTEGER NOT NULL,
                PRIMARY KEY (device_id, action)
            );
            CREATE TABLE IF NOT EXISTS commands (
                command_id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                action TEXT NOT NULL,
                layout_version INTEGER NOT NULL,
                status TEXT NOT NULL,
                progress INTEGER NOT NULL DEFAULT 0,
                request_json TEXT,
                result_json TEXT,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_commands_device_action
                ON commands(device_id, action, status);
            CREATE TABLE IF NOT EXISTS events (
                event_id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                kind TEXT NOT NULL,
                action TEXT,
                message TEXT NOT NULL,
                payload_json TEXT,
                created_at INTEGER NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_events_device_time
                ON events(device_id, created_at DESC);
            """
        )
        row = db.execute("SELECT id FROM layouts WHERE id = 1").fetchone()
        if row is None:
            default = json.dumps(DEFAULT_LAYOUT, ensure_ascii=False, separators=(",", ":"))
            db.execute(
                "INSERT INTO layouts(id, draft_json, draft_revision, published_json, "
                "published_version, published_at) VALUES (1, ?, 1, ?, 1, ?)",
                (default, default, now_ms()),
            )
        for action in POLICY_ACTIONS:
            db.execute(
                "INSERT OR IGNORE INTO device_policy(device_id, action, enabled) "
                "VALUES(?, ?, 1)",
                (DEVICE_ID, action),
            )
        db.execute(
            "INSERT OR IGNORE INTO devices(device_id, last_seen) VALUES(?, NULL)",
            (DEVICE_ID,),
        )


def row_to_dict(row: sqlite3.Row | None) -> dict | None:
    if row is None:
        return None
    return {k: row[k] for k in row.keys()}


def decode_json(raw: bytes) -> dict:
    if not raw:
        return {}
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ApiError(400, "invalid_json", f"请求体不是有效 JSON：{exc}")
    if not isinstance(value, dict):
        raise ApiError(400, "invalid_json", "请求体必须是 JSON 对象")
    return value


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str, details=None):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.details = details or {}


def as_int(value, name: str, minimum: int | None = None, maximum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ApiError(400, "invalid_field", f"{name} 必须是整数", {"field": name})
    if minimum is not None and value < minimum:
        raise ApiError(400, "invalid_field", f"{name} 不能小于 {minimum}", {"field": name})
    if maximum is not None and value > maximum:
        raise ApiError(400, "invalid_field", f"{name} 不能大于 {maximum}", {"field": name})
    return value


def as_str(value, name: str, min_len: int = 1, max_len: int = 255) -> str:
    if not isinstance(value, str):
        raise ApiError(400, "invalid_field", f"{name} 必须是字符串", {"field": name})
    value = value.strip()
    if not (min_len <= len(value) <= max_len):
        raise ApiError(400, "invalid_field", f"{name} 长度必须在 {min_len}..{max_len}", {"field": name})
    return value


def rect(value, name: str) -> dict:
    if not isinstance(value, dict):
        raise ApiError(400, "invalid_field", f"{name} 必须是对象", {"field": name})
    result = {k: as_int(value.get(k), f"{name}.{k}", 0) for k in ("x", "y", "w", "h")}
    if result["w"] <= 0 or result["h"] <= 0:
        raise ApiError(400, "invalid_field", f"{name} 宽高必须为正数", {"field": name})
    return result


def intersects(a: dict, b: dict) -> bool:
    return not (
        a["x"] + a["w"] <= b["x"]
        or b["x"] + b["w"] <= a["x"]
        or a["y"] + a["h"] <= b["y"]
        or b["y"] + b["h"] <= a["y"]
    )


def contains(outer: dict, inner: dict) -> bool:
    return (
        inner["x"] >= outer["x"]
        and inner["y"] >= outer["y"]
        and inner["x"] + inner["w"] <= outer["x"] + outer["w"]
        and inner["y"] + inner["h"] <= outer["y"] + outer["h"]
    )


def touch_hit(visual: dict) -> dict:
    """Authoritative expanded touch target.

    The editor never supplies hit rectangles itself: the server expands the
    visual rect symmetrically to at least 48x48. This prevents visual edits
    from silently shrinking touch targets or changing keyboard/touch order.
    """
    w = max(visual["w"], MIN_TOUCH)
    h = max(visual["h"], MIN_TOUCH)
    # Keep a large visual rect fully covered; only small controls are
    # symmetrically expanded to the minimum touch target.
    x = visual["x"] if visual["w"] >= MIN_TOUCH else visual["x"] + visual["w"] // 2 - w // 2
    y = visual["y"] if visual["h"] >= MIN_TOUCH else visual["y"] + visual["h"] // 2 - h // 2
    return {"x": x, "y": y, "w": w, "h": h}


def validate_layout(data: dict) -> dict:
    if not isinstance(data, dict):
        raise ApiError(400, "invalid_layout", "布局必须是对象")
    title = as_str(data.get("title"), "title", 1, 80)
    font = data.get("title_font")
    if not isinstance(font, dict):
        raise ApiError(400, "invalid_field", "title_font 必须是对象", {"field": "title_font"})
    family = as_str(font.get("family"), "title_font.family", 1, 255)
    size = as_int(font.get("size"), "title_font.size", 12, 96)

    background = data.get("background")
    if not isinstance(background, dict):
        raise ApiError(400, "invalid_field", "background 必须是对象", {"field": "background"})
    mode = background.get("mode")
    if mode not in ("light", "dark", "adaptive"):
        raise ApiError(400, "invalid_field", "background.mode 必须是 light/dark/adaptive")
    try:
        darken = float(background.get("darken", 0))
    except (TypeError, ValueError):
        raise ApiError(400, "invalid_field", "background.darken 必须是 0..0.85 的数字")
    if not 0 <= darken <= 0.85:
        raise ApiError(400, "invalid_field", "background.darken 必须在 0..0.85")

    safe = rect(data.get("safe_area"), "safe_area")
    screen = {"x": 0, "y": 0, "w": SCREEN_W, "h": SCREEN_H}
    if not contains(screen, safe) or safe["w"] < 320 or safe["h"] < 240:
        raise ApiError(400, "safe_area_invalid", "safe_area 必须位于 1280x720 屏幕且至少 320x240")

    title_rect = rect(data.get("title_rect"), "title_rect")
    if not contains(screen, title_rect):
        raise ApiError(400, "title_outside_screen", "标题不得超出屏幕")

    raw_buttons = data.get("buttons")
    if not isinstance(raw_buttons, dict):
        raise ApiError(400, "invalid_field", "buttons 必须是对象")
    labels = {"start": "开始", "settings": "设置", "exit": "退出"}
    buttons: dict[str, dict] = {}
    for action in ("start", "settings", "exit"):
        item = raw_buttons.get(action)
        if not isinstance(item, dict):
            raise ApiError(400, "missing_button", f"缺少按钮 {action}", {"button": action})
        visual = rect(item, f"buttons.{action}")
        if visual["w"] < 96 or visual["h"] < 44:
            raise ApiError(400, "button_too_small", f"{action} 视觉尺寸至少 96x44", {"button": action})
        if not contains(safe, visual):
            raise ApiError(400, "button_outside_usable_area", f"{action} 必须位于可用区域", {"button": action})
        visual["label"] = labels[action]
        buttons[action] = visual

    hits = {action: touch_hit(visual) for action, visual in buttons.items()}
    for action, hit in hits.items():
        if not contains(safe, hit):
            raise ApiError(
                400,
                "touch_target_outside_usable_area",
                f"{action} 的 48x48 触摸命中区域超出可用区域",
                {"button": action, "hit_rect": hit},
            )
    actions = list(hits)
    for i, left in enumerate(actions):
        for right in actions[i + 1 :]:
            if intersects(hits[left], hits[right]):
                raise ApiError(
                    400,
                    "touch_target_overlap",
                    f"{left} 与 {right} 的触摸命中区域重叠",
                    {"left": hits[left], "right": hits[right]},
                )

    if intersects(title_rect, hits["exit"]):
        raise ApiError(
            400,
            "title_covers_exit",
            "长标题或标题框不得覆盖退出入口及其触摸命中区域",
            {"title_rect": title_rect, "exit_hit": hits["exit"]},
        )
    for action in ("start", "settings"):
        if intersects(title_rect, hits[action]):
            raise ApiError(400, "title_covers_button", f"标题不得覆盖 {action} 命中区域")

    focus = data.get("focus_order")
    if focus != ["start", "settings", "exit"] and focus != ["exit", "start", "settings"]:
        raise ApiError(400, "invalid_focus_order", "focus_order 必须包含 start/settings/exit 且不重复")
    # The tested conventional order is enforced; layouts must not make exit
    # unreachable, but the product specifically wants start -> settings -> exit.
    if focus != ["start", "settings", "exit"]:
        raise ApiError(400, "invalid_focus_order", "键盘焦点顺序必须为 start,settings,exit")

    return {
        "title": title,
        "title_font": {
            "family": family,
            "size": size,
            "fallback_chain": [part.strip() for part in family.split(",") if part.strip()]
            + ["DejaVu Sans", "embedded"],
        },
        "background": {"mode": mode, "darken": round(darken, 3)},
        "safe_area": safe,
        "title_rect": title_rect,
        "buttons": buttons,
        "touch_hits": hits,
        "focus_order": ["start", "settings", "exit"],
        "screen": {"w": SCREEN_W, "h": SCREEN_H},
        "min_touch_size": MIN_TOUCH,
    }


def get_layouts(db: sqlite3.Connection) -> sqlite3.Row:
    row = db.execute("SELECT * FROM layouts WHERE id=1").fetchone()
    if row is None:
        raise ApiError(500, "layout_missing", "布局尚未初始化")
    return row


def public_published(row: sqlite3.Row) -> dict:
    layout = json.loads(row["published_json"])
    layout["version"] = row["published_version"]
    layout["published_at"] = row["published_at"]
    return layout


def public_draft(row: sqlite3.Row) -> dict:
    layout = json.loads(row["draft_json"])
    layout["revision"] = row["draft_revision"]
    return layout


def get_policy(db: sqlite3.Connection, device_id: str) -> dict:
    rows = db.execute(
        "SELECT action, enabled FROM device_policy WHERE device_id=?", (device_id,)
    ).fetchall()
    policy = {action: False for action in POLICY_ACTIONS}
    for row in rows:
        policy[row["action"]] = bool(row["enabled"])
    # Safety override: exit must always be reachable even when policy is bad.
    policy["exit"] = True
    return policy


def command_to_dict(row: sqlite3.Row) -> dict:
    result = row_to_dict(row)
    for key in ("request_json", "result_json"):
        if result.get(key):
            try:
                result[key] = json.loads(result[key])
            except json.JSONDecodeError:
                pass
    result["enabled"] = result["status"] in ACTIVE_STATUSES
    return result


def record_event(db: sqlite3.Connection, device_id: str, kind: str, message: str,
                 action: str | None = None, payload: dict | None = None) -> dict:
    event = {
        "event_id": str(uuid.uuid4()),
        "device_id": device_id,
        "kind": kind,
        "action": action,
        "message": message,
        "payload_json": json.dumps(payload or {}, ensure_ascii=False),
        "created_at": now_ms(),
    }
    db.execute(
        "INSERT INTO events(event_id, device_id, kind, action, message, payload_json, created_at) "
        "VALUES(:event_id,:device_id,:kind,:action,:message,:payload_json,:created_at)",
        event,
    )
    event["payload"] = payload or {}
    del event["payload_json"]
    return event


class Handler(BaseHTTPRequestHandler):
    server_version = "ControlServer/1.0"

    def log_message(self, fmt: str, *args) -> None:
        if os.environ.get("CONTROL_QUIET") != "1":
            super().log_message(fmt, *args)

    def send_json(self, status: int, value: dict | list, extra_headers=None) -> None:
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def send_error_json(self, err: ApiError) -> None:
        self.send_json(err.status, {"error": {"code": err.code, "message": err.message, "details": err.details}})

    def read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length < 0 or length > 1024 * 1024:
            raise ApiError(413, "body_too_large", "请求体超过 1 MiB")
        return self.rfile.read(length) if length else b""

    def handle_api(self, method: str, parsed, body: dict):
        db = connect_db()
        try:
            return self.route(db, method, parsed.path, body, parse_qs(parsed.query))
        finally:
            db.close()

    def do_GET(self) -> None:
        self.route_generic("GET")

    def do_POST(self) -> None:
        self.route_generic("POST")

    def do_PUT(self) -> None:
        self.route_generic("PUT")

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type,If-Match")
        self.end_headers()

    def route_generic(self, method: str) -> None:
        parsed = urlparse(self.path)
        if parsed.path in ("/", "/index.html", "/app.js", "/style.css"):
            if method == "GET":
                self.serve_static(parsed.path)
                return
        if not parsed.path.startswith("/api/"):
            if method == "GET":
                if parsed.path == "/assets/background.png":
                    self.serve_asset_background()
                    return
                self.serve_static(parsed.path)
                return
            self.send_error_json(ApiError(404, "not_found", "路径不存在"))
            return
        try:
            body = decode_json(self.read_body()) if method in ("POST", "PUT") else {}
            result, status, headers = self.handle_api(method, parsed, body)
            self.send_json(status, result, headers)
        except ApiError as exc:
            self.send_error_json(exc)
        except sqlite3.Error as exc:
            self.send_error_json(ApiError(500, "database_error", f"数据库错误：{exc}"))
        except Exception as exc:  # keep GUI demo service observable
            self.send_error_json(ApiError(500, "internal_error", f"服务器内部错误：{exc}"))

    def serve_static(self, path: str) -> None:
        rel = "index.html" if path == "/" else path.lstrip("/")
        if rel not in ("index.html", "app.js", "style.css"):
            self.send_error_json(ApiError(404, "not_found", "静态资源不存在"))
            return
        file_path = os.path.normpath(os.path.join(WEB_ROOT, rel))
        if not file_path.startswith(WEB_ROOT + os.sep) or not os.path.isfile(file_path):
            self.send_error_json(ApiError(404, "not_found", "静态资源不存在"))
            return
        types = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8"}
        _, ext = os.path.splitext(file_path)
        data = open(file_path, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", types.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def serve_asset_background(self) -> None:
        path = os.path.join(ROOT, "assets", "background.png")
        if not os.path.isfile(path):
            self.send_error_json(ApiError(404, "not_found", "背景图不存在"))
            return
        data = open(path, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def route(self, db, method, path, body, query):
        if method == "GET" and path == "/api/health":
            return {"ok": True, "time": now_ms()}, 200, {}

        device_id = (body.get("device_id") or query.get("device_id", [DEVICE_ID])[0] or DEVICE_ID)
        device_id = str(device_id)[:64]

        if method == "POST" and path == "/api/devices/bootstrap":
            return self.bootstrap(db, device_id, body), 200, {}
        if method == "PUT" and path == "/api/devices/actual":
            return self.put_actual(db, device_id, body), 200, {}
        if method == "GET" and path == "/api/devices/actual":
            return self.get_actual(db, device_id), 200, {}

        if method == "GET" and path == "/api/layout/published":
            row = get_layouts(db)
            return public_published(row), 200, {}
        if method == "GET" and path == "/api/layout/draft":
            row = get_layouts(db)
            return public_draft(row), 200, {}
        if method == "PUT" and path == "/api/layout/draft":
            return self.put_draft(db, body), 200, {}
        if method == "POST" and path == "/api/layout/publish":
            return self.publish(db, body), 200, {}
        if method == "POST" and path == "/api/layout/validate":
            normalized = validate_layout(body.get("layout", body))
            return {"valid": True, "layout": normalized}, 200, {}

        if method == "POST" and path.startswith("/api/commands/"):
            parts = path.strip("/").split("/")
            if len(parts) == 3:
                return self.create_command(db, device_id, parts[2], body), 202, {}
        if method == "PUT" and path.startswith("/api/commands/"):
            parts = path.strip("/").split("/")
            if len(parts) == 4 and parts[3] == "status":
                return self.update_command(db, device_id, parts[2], body), 200, {}
        if method == "POST" and path == "/api/commands/reconcile":
            return self.reconcile(db, device_id, body), 200, {}
        if method == "GET" and path == "/api/commands":
            return self.list_commands(db, device_id), 200, {}

        if method == "PUT" and path == "/api/policy":
            return self.update_policy(db, device_id, body), 200, {}
        if method == "GET" and path == "/api/policy":
            return {"device_id": device_id, "server": get_policy(db, device_id)}, 200, {}

        if method == "POST" and path == "/api/recovery":
            message = as_str(body.get("message"), "message", 1, 500)
            action = body.get("action")
            if action is not None:
                action = as_str(action, "action", 1, 32)
            event = record_event(
                db, device_id, body.get("kind", "recovery"), message, action,
                body.get("payload") if isinstance(body.get("payload"), dict) else {},
            )
            db.commit()
            return {"ok": True, "event": event}, 201, {}
        if method == "GET" and path == "/api/recovery":
            rows = db.execute(
                "SELECT * FROM events WHERE device_id=? ORDER BY created_at DESC LIMIT 50",
                (device_id,),
            ).fetchall()
            return {"events": [row_to_dict(r) for r in rows]}, 200, {}

        raise ApiError(404, "not_found", f"未知接口：{path}")

    def bootstrap(self, db, device_id, body):
        ts = now_ms()
        db.execute(
            "INSERT INTO devices(device_id,last_seen,updated_at) VALUES(?,?,?) "
            "ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen",
            (device_id, ts, ts),
        )
        row = get_layouts(db)
        layout = public_published(row)
        local = body.get("local_command")
        reconciliation = None
        if isinstance(local, dict) and local.get("command_id"):
            reconciliation = self.reconcile(db, device_id, local)
        db.commit()
        return {
            "device_id": device_id,
            "server_time": ts,
            "layout_version": layout["version"],
            "layout": layout,
            "policy": {"server": get_policy(db, device_id)},
            "reconciliation": reconciliation,
        }

    def put_actual(self, db, device_id, body):
        actual = body.get("actual", body)
        if not isinstance(actual, dict):
            raise ApiError(400, "invalid_actual", "actual 必须是对象")
        # Preserve root telemetry alongside the geometry snapshot so the web
        # console can display both the published version and real device rects.
        for key in ("version", "online", "status_line", "background_mode",
                    "measured_luminance", "effective_darkness", "actual_fonts"):
            if key in body and key not in actual:
                actual[key] = body[key]
        ts = now_ms()
        db.execute(
            "INSERT INTO devices(device_id,last_seen,actual_json,updated_at) VALUES(?,?,?,?) "
            "ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen, "
            "actual_json=excluded.actual_json, updated_at=excluded.updated_at",
            (device_id, ts, json.dumps(actual, ensure_ascii=False), ts),
        )
        db.commit()
        return {"ok": True, "device_id": device_id, "updated_at": ts}

    def get_actual(self, db, device_id):
        row = db.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if row is None or not row["actual_json"]:
            return {"device_id": device_id, "actual": None}
        actual = json.loads(row["actual_json"])
        actual["last_seen"] = row["last_seen"]
        return {"device_id": device_id, "actual": actual}

    def put_draft(self, db, body):
        layout_in = body.get("layout", body)
        base_revision = body.get("base_revision", layout_in.get("revision"))
        if isinstance(base_revision, bool) or not isinstance(base_revision, int):
            raise ApiError(400, "missing_revision", "必须提供 base_revision 以检测草稿冲突")
        normalized = validate_layout(layout_in)
        row = get_layouts(db)
        if base_revision != row["draft_revision"]:
            conflict = {
                "code": "draft_revision_conflict",
                "message": "布局草稿已被另一个控制台修改，请比较后选择覆盖或放弃。",
                "base_revision": base_revision,
                "server_revision": row["draft_revision"],
                "server_draft": public_draft(row),
            }
            raise ApiError(409, "draft_revision_conflict", conflict["message"], conflict)
        revision = row["draft_revision"] + 1
        normalized.pop("version", None)
        db.execute(
            "UPDATE layouts SET draft_json=?, draft_revision=? WHERE id=1",
            (json.dumps(normalized, ensure_ascii=False), revision),
        )
        db.commit()
        row = get_layouts(db)
        return {"ok": True, "draft": public_draft(row)}

    def publish(self, db, body):
        device_id = str(body.get("device_id", DEVICE_ID))[:64]
        expected_revision = body.get("revision")
        row = get_layouts(db)
        if expected_revision is not None:
            if isinstance(expected_revision, bool) or not isinstance(expected_revision, int):
                raise ApiError(400, "invalid_revision", "revision 必须是整数")
            if expected_revision != row["draft_revision"]:
                raise ApiError(
                    409,
                    "draft_revision_conflict",
                    "发布前草稿已变化，请刷新后重新确认。",
                    {"server_revision": row["draft_revision"]},
                )
        layout = validate_layout(json.loads(row["draft_json"]))
        version = row["published_version"] + 1
        revision = row["draft_revision"]
        ts = now_ms()
        layout["version"] = version
        db.execute(
            "UPDATE layouts SET published_json=?, published_version=?, published_at=? WHERE id=1",
            (json.dumps(layout, ensure_ascii=False), version, ts),
        )
        record_event(db, device_id, "layout", f"布局发布版本 {version}（草稿 r{revision}）",
                     payload={"version": version, "revision": revision, "layout": layout})
        db.commit()
        return {"ok": True, "layout": layout, "version": version, "revision": revision, "published_at": ts}

    def create_command(self, db, device_id, command_id, body):
        command_id = as_str(command_id, "command_id", 8, 64)
        action = body.get("action")
        if action not in POLICY_ACTIONS:
            raise ApiError(400, "invalid_action", "action 必须是 start/settings/exit")
        layout_version = as_int(body.get("layout_version"), "layout_version", 1)
        row = get_layouts(db)
        current_version = row["published_version"]
        if layout_version != current_version:
            raise ApiError(
                409,
                "layout_version_stale",
                f"命令基于旧布局 v{layout_version}，当前为 v{current_version}，请抬起后按新布局操作。",
                {"client_version": layout_version, "server_version": current_version},
            )
        existing = db.execute(
            "SELECT * FROM commands WHERE command_id=?", (command_id,)
        ).fetchone()
        if existing is not None:
            if existing["device_id"] != device_id or existing["action"] != action:
                raise ApiError(409, "command_id_conflict", "命令 ID 已用于其他设备或动作")
            return command_to_dict(existing)

        active = db.execute(
            "SELECT * FROM commands WHERE device_id=? AND action=? AND status IN "
            "('QUEUED','RUNNING','CONFIRMING') ORDER BY created_at DESC LIMIT 1",
            (device_id, action),
        ).fetchone()
        if active is not None:
            same_device_client = active["command_id"] == command_id
            if same_device_client:
                # A retry after a lost HTTP response/timeout is idempotent.
                return command_to_dict(active)
            raise ApiError(
                409,
                "duplicate_active_command",
                f"{action} 已有活动任务；终端不会启动第二份任务，可挂接原任务。",
                {"active_command_id": active["command_id"], "status": active["status"]},
            )

        policy = get_policy(db, device_id)
        if not policy.get(action, False):
            raise ApiError(403, "action_forbidden", f"服务器策略当前禁止 {action}")
        ts = now_ms()
        db.execute(
            "INSERT INTO commands(command_id,device_id,action,layout_version,status,progress,"
            "request_json,result_json,created_at,updated_at) VALUES(?,?,?,?,?,0,?,?,?,?)",
            (command_id, device_id, action, layout_version, "RUNNING",
             json.dumps(body, ensure_ascii=False), None, ts, ts),
        )
        db.execute("UPDATE devices SET last_seen=? WHERE device_id=?", (ts, device_id))
        db.commit()
        made = db.execute("SELECT * FROM commands WHERE command_id=?", (command_id,)).fetchone()
        return command_to_dict(made)

    def update_command(self, db, device_id, command_id, body):
        status = body.get("status")
        if status not in ("RUNNING", "CONFIRMING", "SUCCESS", "FAILURE", "INTERRUPTED"):
            raise ApiError(400, "invalid_status", "状态不受支持")
        progress = as_int(body.get("progress", 0), "progress", 0, 100)
        result = body.get("result") if isinstance(body.get("result"), dict) else {}
        row = db.execute("SELECT * FROM commands WHERE command_id=?", (command_id,)).fetchone()
        if row is None:
            raise ApiError(404, "command_not_found", "命令不存在，恢复记录已由终端本地保留")
        if row["device_id"] != device_id:
            raise ApiError(403, "device_mismatch", "命令属于其他设备")
        if row["status"] in TERMINAL_STATUSES and status != row["status"]:
            return command_to_dict(row)
        ts = now_ms()
        db.execute(
            "UPDATE commands SET status=?, progress=?, result_json=?, updated_at=? WHERE command_id=?",
            (status, progress, json.dumps(result, ensure_ascii=False), ts, command_id),
        )
        if status == "INTERRUPTED":
            record_event(db, device_id, "recovery",
                         f"未完成操作 {row['action']} 已记录：{result.get('message', '等待恢复')}",
                         row["action"], {"command_id": command_id, "result": result, "progress": progress})
        db.commit()
        fresh = db.execute("SELECT * FROM commands WHERE command_id=?", (command_id,)).fetchone()
        return command_to_dict(fresh)

    def reconcile(self, db, device_id, body):
        command_id = as_str(body.get("command_id", ""), "command_id", 8, 64)
        action = body.get("action")
        if action not in POLICY_ACTIONS:
            raise ApiError(400, "invalid_action", "action 必须是 start/settings/exit")
        progress = as_int(body.get("progress", 0), "progress", 0, 100)
        local_status = body.get("local_status", "UNKNOWN")
        row = db.execute("SELECT * FROM commands WHERE command_id=?", (command_id,)).fetchone()
        if row is None:
            event = record_event(
                db, device_id, "recovery",
                f"恢复未完成操作 {action}：服务器无该命令（可能发生于断网），进度 {progress}%。",
                action, {"command_id": command_id, "local_status": local_status, "progress": progress},
            )
            db.commit()
            return {"result": "recovery_recorded", "command": None, "event": event}
        if row["device_id"] != device_id:
            raise ApiError(403, "device_mismatch", "命令属于其他设备")
        if row["status"] in ACTIVE_STATUSES and local_status in ("SUCCESS", "FAILURE", "INTERRUPTED"):
            result = {"message": "终端重启后与服务器对齐状态", "local_status": local_status}
            db.execute("UPDATE commands SET status=?, progress=?, result_json=?, updated_at=? WHERE command_id=?",
                       (local_status, progress, json.dumps(result, ensure_ascii=False), now_ms(), command_id))
            record_event(db, device_id, "recovery",
                         f"命令 {action} 从服务器活动状态对齐为 {local_status}", action,
                         {"command_id": command_id, "progress": progress})
            db.commit()
            row = db.execute("SELECT * FROM commands WHERE command_id=?", (command_id,)).fetchone()
        return {"result": "synchronized", "command": command_to_dict(row)}

    def update_policy(self, db, device_id, body):
        actions = body.get("actions", body)
        if not isinstance(actions, dict):
            raise ApiError(400, "invalid_policy", "actions 必须是 action -> bool 的对象")
        changed = []
        for action, enabled in actions.items():
            if action not in POLICY_ACTIONS:
                continue
            if not isinstance(enabled, bool):
                raise ApiError(400, "invalid_policy", f"{action} 必须是布尔值")
            if action == "exit" and not enabled:
                # Product safety requirement: service policy cannot hide the
                # terminal's exit entry, including during bad policy edits.
                enabled = True
            db.execute(
                "INSERT INTO device_policy(device_id, action, enabled) VALUES(?, ?, ?) "
                "ON CONFLICT(device_id, action) DO UPDATE SET enabled=excluded.enabled",
                (device_id, action, 1 if enabled else 0),
            )
            changed.append(action)
        record_event(db, device_id, "policy", f"服务器策略更新：{', '.join(changed) if changed else '无'}",
                     payload={"actions": actions})
        db.commit()
        return {"ok": True, "device_id": device_id, "server": get_policy(db, device_id)}

    def list_commands(self, db, device_id):
        active = db.execute(
            "SELECT * FROM commands WHERE device_id=? AND status IN ('RUNNING','CONFIRMING','QUEUED') "
            "ORDER BY updated_at DESC",
            (device_id,),
        ).fetchall()
        recent = db.execute(
            "SELECT * FROM commands WHERE device_id=? ORDER BY updated_at DESC LIMIT 20", (device_id,)
        ).fetchall()
        return {"active": [command_to_dict(r) for r in active], "recent": [command_to_dict(r) for r in recent]}


def main() -> None:
    init_db()
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"control server listening on http://{HOST}:{PORT}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
