#!/usr/bin/env python3
import json
import os
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

os.environ['CONTROL_HOST'] = '127.0.0.1'
_fd, os.environ['CONTROL_DB'] = tempfile.mkstemp(prefix='control-test-', suffix='.db')
os.close(_fd)
os.unlink(os.environ['CONTROL_DB'])

from server import app as control_app  # noqa: E402


class ControlServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        control_app.init_db()
        cls.httpd = ThreadingHTTPServer(('127.0.0.1', 0), control_app.Handler)
        cls.port = cls.httpd.server_address[1]
        cls.base = f'http://127.0.0.1:{cls.port}'
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.thread.start()
        time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()
        cls.thread.join(timeout=2)
        for suffix in ('', '-wal', '-shm'):
            try: os.unlink(os.environ['CONTROL_DB'] + suffix)
            except FileNotFoundError: pass

    def request(self, path, method='GET', obj=None):
        data = None if obj is None else json.dumps(obj, ensure_ascii=False).encode()
        req = urllib.request.Request(
            self.base + path, data=data,
            headers={'Content-Type': 'application/json'}, method=method)
        try:
            with urllib.request.urlopen(req, timeout=2) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())

    def test_geometry_protects_exit_and_touch_area(self):
        _, draft = self.request('/api/layout/draft')
        bad = dict(draft)
        bad['title_rect'] = dict(bad['title_rect'], w=1150)
        status, payload = self.request('/api/layout/validate', 'POST', bad)
        self.assertEqual(status, 400)
        self.assertEqual(payload['error']['code'], 'title_covers_exit')

        small = dict(draft)
        small['buttons']['start'] = {'x': 16, 'y': 640, 'w': 96, 'h': 44}
        status, payload = self.request('/api/layout/validate', 'POST', small)
        self.assertEqual(status, 200)
        self.assertEqual(payload['layout']['touch_hits']['start']['h'], 48)

    def test_draft_revision_conflict(self):
        _, first = self.request('/api/layout/draft')
        revision = first['revision']
        self.request('/api/layout/draft', 'PUT',
                     {'base_revision': revision, 'layout': first})
        status, payload = self.request('/api/layout/draft', 'PUT',
                                       {'base_revision': revision, 'layout': first})
        self.assertEqual(status, 409)
        self.assertEqual(payload['error']['code'], 'draft_revision_conflict')

    def test_version_and_idempotent_command(self):
        status, command = self.request('/api/commands/cmdidempotent', 'POST', {
            'device_id': 'terminal-01', 'action': 'start', 'layout_version': 1})
        self.assertEqual(status, 202)
        self.assertIn(command['status'], ('QUEUED', 'RUNNING'))
        status, retry = self.request('/api/commands/cmdidempotent', 'POST', {
            'device_id': 'terminal-01', 'action': 'start', 'layout_version': 1})
        self.assertEqual(status, 202)
        self.assertEqual(retry['command_id'], 'cmdidempotent')
        status, duplicate = self.request('/api/commands/cmdother', 'POST', {
            'device_id': 'terminal-01', 'action': 'start', 'layout_version': 1})
        self.assertEqual(status, 409)
        self.assertEqual(duplicate['error']['code'], 'duplicate_active_command')
        status, stale = self.request('/api/commands/cmdstale', 'POST', {
            'device_id': 'terminal-01', 'action': 'settings', 'layout_version': 99})
        self.assertEqual(status, 409)
        self.assertEqual(stale['error']['code'], 'layout_version_stale')

    def test_policy_exit_override_and_recovery(self):
        status, policy = self.request('/api/policy', 'PUT', {
            'device_id': 'terminal-01',
            'actions': {'start': False, 'settings': False, 'exit': False}})
        self.assertEqual(status, 200)
        self.assertFalse(policy['server']['start'])
        self.assertFalse(policy['server']['settings'])
        self.assertTrue(policy['server']['exit'])
        status, recovery = self.request('/api/recovery', 'POST', {
            'device_id': 'terminal-01', 'kind': 'recovery', 'action': 'start',
            'message': '未完成操作恢复记录'})
        self.assertEqual(status, 201)
        self.assertTrue(recovery['event']['event_id'])
        # Restore policy for manual inspection.
        self.request('/api/policy', 'PUT', {'device_id': 'terminal-01',
                                            'actions': {'start': True, 'settings': True}})

    def test_actual_device_snapshot_and_recovery_list(self):
        snapshot = {'version': 1, 'online': True, 'actual': {
            'buttons': {'exit': {'x': 1160, 'y': 16, 'w': 104, 'h': 48,
                                 'state': 'idle', 'enabled': True}},
            'touch_hits': {'exit': {'x': 1160, 'y': 16, 'w': 104, 'h': 48}}}}
        status, payload = self.request('/api/devices/actual', 'PUT', snapshot)
        self.assertEqual(status, 200)
        status, payload = self.request('/api/devices/actual')
        self.assertEqual(status, 200)
        self.assertEqual(payload['actual']['version'], 1)
        status, events = self.request('/api/recovery')
        self.assertEqual(status, 200)
        self.assertIsInstance(events['events'], list)


if __name__ == '__main__':
    unittest.main()
