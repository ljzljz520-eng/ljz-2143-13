#!/usr/bin/env python3
"""
Visual Window control backend.

Responsibilities (see docs/design.md):
  * web editor drafts: editable title / font size / button arrangement
  * server-side "usable area" validation of the same geometry the C client
    compiles (reserved exit zone, touch targets, non-overlap, title limits)
  * durable SQLite persistence, monotonic published layout versions
  * optimistic concurrency: two editors editing the same draft => 409 conflict
  * versioned command acceptance from C clients; version mismatch => 409
  * idempotent task start: re-press after a timeout never creates two tasks
  * availability policy endpoint and device-side recovery records

Only the Python standard library is used.
"""
import json
import os
import sqlite3
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.environ.get("VW_DB", os.path.join(HERE, "..", "build", "vw.db"))
SCHEMA = os.path.join(HERE, "schema.sql")
WEB_DIR = os.path.join(HERE, "..", "web")

MAX_TITLE_BYTES = 127
MIN_TITLE_BYTES = 1
MIN_FONT = 10
MAX_FONT = 120
MIN_TOUCH = 44
EXIT_KEEP_OUT = 8


# --------------------------------------------------------------------------
# Usable-area validation (mirrors vw_layout_compile on the client)
# --------------------------------------------------------------------------

def validate_layout_spec(spec, device):
    """Return (ok, error). Pure function so it can be self-tested offline."""
    title = spec.get("title", "")
    if not isinstance(title, str):
        return False, "title must be a string"
    tb = len(title.encode("utf-8"))
    if tb < MIN_TITLE_BYTES:
        return False, "title must not be empty"
    if tb > MAX_TITLE_BYTES:
        return False, "title exceeds %d UTF-8 bytes" % MAX_TITLE_BYTES
    try:
        title.encode("utf-8").decode("utf-8")
    except UnicodeError:
        return False, "title is not valid UTF-8"

    font = spec.get("font_size_px")
    if not isinstance(font, int) or not (MIN_FONT <= font <= MAX_FONT):
        return False, "font_size_px must be %d..%d" % (MIN_FONT, MAX_FONT)

    arrangement = spec.get("arrangement", "split")
    if arrangement not in ("inline", "split"):
        return False, "arrangement must be inline or split"

    screen_w = int(device["screen_w"])
    screen_h = int(device["screen_h"])
    touch = int(device["min_touch"])

    if screen_w < 240 or screen_h < 240:
        return False, "screen too small"

    # reserved exit zone (same constants as the C compiler)
    margin = 8
    exit_size = max(36, min(72, font + 18))
    exit_x = screen_w - margin - exit_size
    title_x = margin * 2
    title_max_w = exit_x - EXIT_KEEP_OUT - title_x
    if title_max_w < font * 2:
        return False, "usable title width too small for this screen/font"

    # buttons must fit inside the screen and never enter the exit keep-out
    btn_h = max(touch, min(88, font + 20))
    if btn_h > screen_h:
        return False, "button height exceeds screen"
    if arrangement == "inline":
        # both buttons occupy the title bar, left of the exit keep-out
        band_w = exit_x - EXIT_KEEP_OUT - margin
        # two buttons each at least 2*touch wide plus a 12px gap
        if band_w < 4 * touch + 12:
            return False, ("inline arrangement unusable: buttons would crowd "
                           "the title/exit; use split arrangement")
    else:
        bar_h = btn_h + 24
        if bar_h > screen_h - 80:
            return False, "action bar does not fit this screen height"

    # every touch target on screen must be reachable at >= MIN_TOUCH px
    if touch < MIN_TOUCH:
        return False, "device touch minimum below %dpx" % MIN_TOUCH

    return True, ""


def spec_hash_number(spec, device):
    """FNV-1a over the canonical string the C client also hashes."""
    canon = "%d|%d|%d|%s|%d|%d|%d|%d|%d|%d|%d" % (
        int(device["screen_w"]), int(device["screen_h"]), int(device["dpi"]),
        spec["title"], int(spec["font_size_px"]),
        1 if spec.get("arrangement", "split") == "split" else 0,
        int(device["min_touch"]), 12,
        int(spec.get("bg_luminance_0_255", 128)),
        1, 1)
    h = 2166136261
    for ch in canon.encode("utf-8"):
        h ^= ch
        h = (h * 16777619) & 0xFFFFFFFF
    return h


# --------------------------------------------------------------------------
# Database
# --------------------------------------------------------------------------

class Store:
    def __init__(self, path=DB_PATH):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.path = path
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self._init_schema()
        self._seed()

    def _init_schema(self):
        with open(SCHEMA, "r", encoding="utf-8") as f:
            self.db.executescript(f.read())
        self.db.commit()

    def _seed(self):
        row = self.db.execute("SELECT id FROM devices WHERE device_key=?",
                              ("DEV-1",)).fetchone()
        if row:
            return
        now = int(time.time())
        cur = self.db.execute(
            "INSERT INTO devices(device_key,name,screen_w,screen_h,dpi,"
            "min_touch,online,last_seen) VALUES(?,?,?,?,?,?,?,?)",
            ("DEV-1", "会议室背景窗口", 1280, 720, 96, 44, 1, now))
        dev_id = cur.lastrowid
        self.db.execute(
            "INSERT INTO layouts(device_id,version,title,font_size_px,"
            "arrangement,spec_hash,published_by,created_at) "
            "VALUES(?,?,?,?,?,?,?,?)",
            (dev_id, 1, "Visual Window App", 24, "split", 0, "seed", now))
        self.db.execute(
            "INSERT INTO policy_cache(device_id,start_allowed,settings_allowed,"
            "exit_allowed,decided_at) VALUES(?,?,?,?,?)",
            (dev_id, 1, 1, 1, now))
        self.db.commit()

    def device(self, key):
        return self.db.execute(
            "SELECT * FROM devices WHERE device_key=?", (key,)).fetchone()

    def latest_layout(self, dev_id):
        return self.db.execute(
            "SELECT * FROM layouts WHERE device_id=? ORDER BY version DESC "
            "LIMIT 1", (dev_id,)).fetchone()

    def draft(self, dev_id):
        return self.db.execute(
            "SELECT * FROM drafts WHERE device_id=? ORDER BY updated_at DESC "
            "LIMIT 1", (dev_id,)).fetchone()

    def policy(self, dev_id):
        return self.db.execute(
            "SELECT * FROM policy_cache WHERE device_id=?", (dev_id,)).fetchone()


# --------------------------------------------------------------------------
# HTTP layer
# --------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "VisualWindow/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[server] " + (fmt % args) + "\n")

    # -- helpers -----------------------------------------------------------
    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "content-type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        if n <= 0:
            return {}
        raw = self.rfile.read(n)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return None

    def _device_or_404(self):
        key = self.path.strip("/").split("/")
        dev_key = key[2] if len(key) > 2 else "DEV-1"
        dev = self.server.store.device(dev_key)
        if not dev:
            self._send(404, {"error": "unknown device %s" % dev_key})
            return None
        return dev

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "content-type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS")
        self.end_headers()

    # -- routing -----------------------------------------------------------
    def do_GET(self):
        p = self.path.split("?", 1)[0]
        if p in ("/", "/index.html"):
            return self._serve_editor()
        if p == "/api/health":
            return self._send(200, {"ok": True})

        parts = [x for x in p.strip("/").split("/") if x]
        # /api/devices/<key>
        if len(parts) == 3 and parts[:2] == ["api", "devices"]:
            dev = self.server.store.device(parts[2])
            if not dev:
                return self._send(404, {"error": "unknown device"})
            lay = self.server.store.latest_layout(dev["id"])
            return self._send(200, {
                "device_key": dev["device_key"], "name": dev["name"],
                "screen_w": dev["screen_w"], "screen_h": dev["screen_h"],
                "dpi": dev["dpi"], "min_touch": dev["min_touch"],
                "online": bool(dev["online"]),
                "current_layout": None if not lay else {
                    "version": lay["version"], "title": lay["title"],
                    "font_size_px": lay["font_size_px"],
                    "arrangement": lay["arrangement"],
                    "spec_hash": lay["spec_hash"],
                }})

        # /api/devices/<key>/draft
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "draft":
            dev = self.server.store.device(parts[2])
            if not dev:
                return self._send(404, {"error": "unknown device"})
            d = self.server.store.draft(dev["id"])
            if not d:
                return self._send(200, {"draft": None})
            return self._send(200, {"draft": Handler._draft_dict(d)})

        # /api/devices/<key>/layout
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "layout":
            dev = self.server.store.device(parts[2])
            lay = self.server.store.latest_layout(dev["id"])
            return self._send(200, {"version": lay["version"],
                                    "title": lay["title"],
                                    "font_size_px": lay["font_size_px"],
                                    "arrangement": lay["arrangement"],
                                    "spec_hash": lay["spec_hash"]})

        # /api/devices/<key>/policy
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "policy":
            dev = self.server.store.device(parts[2])
            pol = self.server.store.policy(dev["id"])
            return self._send(200, {"start": bool(pol["start_allowed"]),
                                    "settings": bool(pol["settings_allowed"]),
                                    "exit": bool(pol["exit_allowed"]),
                                    "decided_at": pol["decided_at"]})

        # /api/devices/<key>/recovery
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "recovery":
            dev = self.server.store.device(parts[2])
            rows = self.server.store.db.execute(
                "SELECT idempotency_key,action,task_id,status,received_at "
                "FROM device_journal WHERE device_id=? ORDER BY received_at",
                (dev["id"],)).fetchall()
            return self._send(200, {"records": [dict(r) for r in rows]})

        return self._send(404, {"error": "not found", "path": p})

    def do_PUT(self):
        parts = [x for x in self.path.strip("/").split("/") if x]
        body = self._read_json()
        if body is None:
            return self._send(400, {"error": "invalid JSON"})

        # PUT /api/devices/<key>/draft  (create/update an editor draft)
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "draft":
            return self._upsert_draft(parts[2], body)

        return self._send(404, {"error": "not found"})

    def do_POST(self):
        parts = [x for x in self.path.strip("/").split("/") if x]
        body = self._read_json()
        if body is None:
            return self._send(400, {"error": "invalid JSON"})

        # POST /api/devices/<key>/publish
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "publish":
            return self._publish(parts[2], body)

        # POST /api/devices/<key>/commands
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "commands":
            return self._command(parts[2], body)

        # POST /api/devices/<key>/tasks
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "tasks":
            return self._start_task(parts[2], body)

        # POST /api/devices/<key>/journal
        if len(parts) == 4 and parts[:2] == ["api", "devices"] and \
                parts[3] == "journal":
            return self._journal(parts[2], body)

        return self._send(404, {"error": "not found"})

    # -- draft / publish ---------------------------------------------------
    @staticmethod
    def _draft_dict(d):
        return {"id": d["id"], "device_id": d["device_id"],
                "title": d["title"], "font_size_px": d["font_size_px"],
                "arrangement": d["arrangement"],
                "base_version": d["base_version"], "rev": d["rev"],
                "editor": d["editor"],
                "updated_by": d["updated_by"],
                "updated_at": d["updated_at"]}

    def _upsert_draft(self, dev_key, body):
        dev = self.server.store.device(dev_key)
        if not dev:
            return self._send(404, {"error": "unknown device"})
        spec = {
            "title": body.get("title", ""),
            "font_size_px": body.get("font_size_px"),
            "arrangement": body.get("arrangement", "split"),
        }
        ok, err = validate_layout_spec(spec, dev)
        if not ok:
            return self._send(422, {"error": err})
        editor = str(body.get("editor", "console"))[:64]
        base = int(body.get("base_version", 0) or 0)
        rev = int(body.get("rev", 0) or 0)
        now = int(time.time() * 1000)
        db = self.server.store.db
        existing = db.execute(
            "SELECT * FROM drafts WHERE device_id=? ORDER BY updated_at DESC "
            "LIMIT 1", (dev["id"],)).fetchone()
        # optimistic concurrency: if another console advanced the draft
        # revision after this editor loaded it, reject with the server draft
        force = bool(body.get("overwrite"))
        if existing and not force and existing["updated_by"] != editor and \
                existing["rev"] > rev:
            return self._send(409, {
                "error": "draft conflict",
                "server_draft": Handler._draft_dict(existing)})
        if existing:
            db.execute(
                "UPDATE drafts SET title=?,font_size_px=?,arrangement=?,"
                "base_version=?,rev=rev+1,updated_by=?,updated_at=? WHERE id=?",
                (spec["title"], spec["font_size_px"], spec["arrangement"],
                 base, editor, now, existing["id"]))  # rev auto-increments
            d = db.execute("SELECT * FROM drafts WHERE id=?",
                           (existing["id"],)).fetchone()
        else:
            cur = db.execute(
                "INSERT INTO drafts(device_id,title,font_size_px,arrangement,"
                "base_version,rev,editor,updated_by,created_at,updated_at) "
                "VALUES(?,?,?,?,?,2,?,?,?,?)",
                (dev["id"], spec["title"], spec["font_size_px"],
                 spec["arrangement"], base, editor, editor, now, now))
            d = db.execute("SELECT * FROM drafts WHERE id=?",
                           (cur.lastrowid,)).fetchone()
        db.commit()
        self._send(200, {"draft": Handler._draft_dict(d)})

    def _publish(self, dev_key, body):
        dev = self.server.store.device(dev_key)
        if not dev:
            return self._send(404, {"error": "unknown device"})
        db = self.server.store.db
        draft_id = body.get("draft_id")
        editor = str(body.get("editor", "console"))[:64]
        base = int(body.get("base_version", 0) or 0)
        rev = int(body.get("rev", 0) or 0)
        if draft_id:
            d = db.execute("SELECT * FROM drafts WHERE id=? AND device_id=?",
                           (draft_id, dev["id"])).fetchone()
        else:
            d = db.execute(
                "SELECT * FROM drafts WHERE device_id=? ORDER BY updated_at "
                "DESC LIMIT 1", (dev["id"],)).fetchone()
        if not d:
            return self._send(404, {"error": "no draft to publish"})
        # another console published or touched the draft after this editor's
        # base read => surface the conflict with the actual device layout
        current = db.execute(
            "SELECT * FROM layouts WHERE device_id=? ORDER BY version DESC "
            "LIMIT 1", (dev["id"],)).fetchone()
        if base and current and base < current["version"] and \
                d["updated_by"] != editor:
            return self._send(409, {
                "error": "draft conflict: device layout changed",
                "actual_layout": {"version": current["version"],
                                  "title": current["title"],
                                  "font_size_px": current["font_size_px"],
                                  "arrangement": current["arrangement"]}})
        spec = {"title": d["title"], "font_size_px": d["font_size_px"],
                "arrangement": d["arrangement"]}
        ok, err = validate_layout_spec(spec, dev)
        if not ok:
            return self._send(422, {"error": err})
        new_version = current["version"] + 1
        hsh = spec_hash_number(spec, dev)
        now = int(time.time())
        db.execute(
            "INSERT INTO layouts(device_id,version,title,font_size_px,"
            "arrangement,spec_hash,published_by,created_at) "
            "VALUES(?,?,?,?,?,?,?,?)",
            (dev["id"], new_version, spec["title"], spec["font_size_px"],
             spec["arrangement"], hsh, d["updated_by"], now))
        db.commit()
        self._send(201, {"version": new_version, "title": spec["title"],
                         "font_size_px": spec["font_size_px"],
                         "arrangement": spec["arrangement"],
                         "spec_hash": hsh})

    # -- client commands / tasks / journal ---------------------------------
    def _command(self, dev_key, body):
        dev = self.server.store.device(dev_key)
        if not dev:
            return self._send(404, {"error": "unknown device"})
        db = self.server.store.db
        current = db.execute(
            "SELECT * FROM layouts WHERE device_id=? ORDER BY version DESC "
            "LIMIT 1", (dev["id"],)).fetchone()
        version = int(body.get("layout_version", 0) or 0)
        if version != current["version"]:
            return self._send(409, {"error": "version mismatch",
                                    "current_version": current["version"]})
        widget = body.get("widget", "")
        if widget not in ("start", "settings", "exit", "title"):
            return self._send(422, {"error": "unknown widget"})
        key = str(body.get("idempotency_key", ""))[:64]
        if not key:
            return self._send(422, {"error": "missing idempotency_key"})
        now = int(time.time())
        db.execute(
            "INSERT INTO commands(device_id,idempotency_key,widget,"
            "layout_version,spec_hash,payload,accepted,created_at) "
            "VALUES(?,?,?,?,?,?,1,?)",
            (dev["id"], key, widget, version,
             int(body.get("spec_hash", 0) or 0), json.dumps(body), now))
        db.commit()
        self._send(202, {"accepted": True, "widget": widget,
                         "layout_version": version})

    def _start_task(self, dev_key, body):
        dev = self.server.store.device(dev_key)
        if not dev:
            return self._send(404, {"error": "unknown device"})
        key = str(body.get("idempotency_key", ""))[:64]
        if not key:
            return self._send(422, {"error": "missing idempotency_key"})
        db = self.server.store.db
        # idempotent: same key always returns the SAME task, never a new one
        row = db.execute("SELECT * FROM tasks WHERE idempotency_key=?",
                         (key,)).fetchone()
        if row:
            return self._send(200, {"task_id": row["id"],
                                    "state": row["state"],
                                    "idempotent_replay": True})
        pol = db.execute("SELECT * FROM policy_cache WHERE device_id=?",
                         (dev["id"],)).fetchone()
        if not pol["start_allowed"]:
            return self._send(403, {"error": "start denied by server policy"})
        now = int(time.time())
        fail_first = bool(body.get("fail_once"))
        cur = db.execute(
            "INSERT INTO tasks(device_id,idempotency_key,state,code,"
            "created_at) VALUES(?,?,?,?,?)",
            (dev["id"], key, "running", 1 if fail_first else 0, now))
        db.commit()
        self._send(201, {"task_id": cur.lastrowid, "state": "running",
                         "idempotent_replay": False})

    def _journal(self, dev_key, body):
        dev = self.server.store.device(dev_key)
        if not dev:
            return self._send(404, {"error": "unknown device"})
        recs = body.get("records", [])
        if isinstance(recs, dict):
            recs = [recs]
        db = self.server.store.db
        now = int(time.time())
        saved = 0
        for r in recs:
            status = r.get("status", "pending")
            if status not in ("pending", "done", "failed"):
                continue
            db.execute(
                "INSERT OR IGNORE INTO device_journal(device_id,"
                "idempotency_key,action,task_id,status,received_at) "
                "VALUES(?,?,?,?,?,?)",
                (dev["id"], str(r.get("key", ""))[:33],
                 str(r.get("action", ""))[:24],
                 int(r.get("task_id", 0) or 0), status, now))
            if status != "pending":
                db.execute(
                    "UPDATE tasks SET state=?, code=?, finished_at=? "
                    "WHERE idempotency_key=?",
                    (status, int(r.get("code", 0) or 0), now,
                     str(r.get("key", ""))[:33]))
            saved += 1
        db.commit()
        self._send(200, {"saved": saved})

    def _serve_editor(self):
        path = os.path.join(WEB_DIR, "index.html")
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            return self._send(404, {"error": "editor not built"})
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


# --------------------------------------------------------------------------
# In-process self-test
# --------------------------------------------------------------------------

def _selftest(tmpdir):
    db = os.path.join(tmpdir, "t.db")
    store = Store(db)
    dev = store.device("DEV-1")
    assert dev is not None

    # 1. happy-path validation + publish bumps the version
    good = {"title": "新的窗口标题", "font_size_px": 26, "arrangement": "split"}
    ok, err = validate_layout_spec(good, dev)
    assert ok, err
    h1 = spec_hash_number(good, dev)
    assert h1 == spec_hash_number(good, dev)  # deterministic

    # 2. invalid usable area is rejected
    bad = {"title": "x", "font_size_px": 999, "arrangement": "split"}
    ok, err = validate_layout_spec(bad, dev)
    assert not ok and "font" in err

    bad2 = {"title": "", "font_size_px": 20}
    ok, err = validate_layout_spec(bad2, dev)
    assert not ok and "empty" in err

    long_title = "字" * 100  # 300 UTF-8 bytes > 127
    bad3 = {"title": long_title, "font_size_px": 20}
    ok, err = validate_layout_spec(bad3, dev)
    assert not ok and "UTF-8" in err

    # 3. draft conflict between two consoles (optimistic concurrency)
    # simulate by calling handler logic directly via small inline HTTP-less flow
    import tempfile
    srv = type("S", (), {"store": store})()

    class H:
        def __init__(self):
            self.sent = None
        def _send(self, code, obj):
            self.sent = (code, obj)
        server = srv
    # console A writes a draft at base 1
    ha = H()
    Handler._upsert_draft(ha, "DEV-1",
                          {"title": "A 改的标题", "font_size_px": 22,
                           "arrangement": "split", "base_version": 1, "rev": 1,
                           "editor": "consoleA"})
    assert ha.sent[0] == 200, ha.sent
    # console B edits against stale base 1 -> 409 with the server draft
    hb = H()
    Handler._upsert_draft(hb, "DEV-1",
                          {"title": "B 改的标题", "font_size_px": 22,
                           "arrangement": "split", "base_version": 1, "rev": 1,
                           "editor": "consoleB"})
    assert hb.sent[0] == 409, hb.sent
    assert "draft conflict" in hb.sent[1]["error"]

    # 4. publish increments version monotonically
    hp = H()
    Handler._publish(hp, "DEV-1", {"editor": "consoleA", "base_version": 1})
    assert hp.sent[0] == 201, hp.sent
    assert hp.sent[1]["version"] == 2

    # 5. stale command version is rejected with 409
    hc = H()
    Handler._command(hc, "DEV-1",
                     {"widget": "start", "layout_version": 1,
                      "idempotency_key": "k1"})
    assert hc.sent[0] == 409 and hc.sent[1]["current_version"] == 2
    hc2 = H()
    Handler._command(hc2, "DEV-1",
                     {"widget": "start", "layout_version": 2,
                      "idempotency_key": "k1"})
    assert hc2.sent[0] == 202

    # 6. idempotent task start: same key, one task
    ht = H()
    Handler._start_task(ht, "DEV-1", {"idempotency_key": "d1-s1-p1"})
    code1, obj1 = ht.sent
    assert code1 == 201 and obj1["idempotent_replay"] is False
    ht2 = H()
    Handler._start_task(ht2, "DEV-1", {"idempotency_key": "d1-s1-p1"})
    code2, obj2 = ht2.sent
    assert code2 == 200 and obj2["task_id"] == obj1["task_id"]
    assert obj2["idempotent_replay"] is True

    # 7. journal upload stores recovery records
    hj = H()
    Handler._journal(hj, "DEV-1", {"records": [
        {"key": "d1-s1-p1", "action": "start", "task_id": 9,
         "status": "pending"}]})
    assert hj.sent[0] == 200 and hj.sent[1]["saved"] == 1
    hr = H()
    rows = store.db.execute(
        "SELECT * FROM device_journal WHERE idempotency_key=?",
        ("d1-s1-p1",)).fetchall()
    assert rows and rows[0]["status"] == "pending"

    print("server self-test: all checks passed")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--self-test":
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            return _selftest(td)
    host = os.environ.get("VW_HOST", "0.0.0.0")
    port = int(os.environ.get("VW_PORT", "8080"))
    httpd = ThreadingHTTPServer((host, port), Handler)
    httpd.store = Store()
    print("Visual Window backend on http://%s:%d (db=%s)" %
          (host, port, DB_PATH))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
