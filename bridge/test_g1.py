"""G1 regression tests: pairing, token hashing, profiles, TLS and device API.

Run:
    python -m unittest pager_bridge.test_g1 -v
"""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fastapi.testclient import TestClient

from . import tls as tlsutil
from .acp_client import ACPClient
from .bridge import pick_login_method, transport_dead_text, visible_transcript
from .device_api import create_device_app
from .profiles import ProfileError, ProfileRegistry
from .security import DeviceStore, PairingError, PairingManager


class FakeClock:
    def __init__(self, start: float = 1_700_000_000.0) -> None:
        self.now = start

    def __call__(self) -> float:
        return self.now


class PairingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.store = DeviceStore(self.root / "devices.json")
        self.clock = FakeClock()
        self.manager = PairingManager(
            self.store,
            ttl=120,
            audit_path=self.root / "audit.log",
            clock=self.clock,
            code_factory=lambda: "654321",
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_pair_returns_token_once_and_stores_only_hash(self) -> None:
        session = self.manager.start(name_hint="Pager", ip="127.0.0.1")
        grant = self.manager.confirm(
            "654 321",
            name="Desk Pager",
            platform="esp32s3",
            ip="192.168.1.20",
        )
        self.assertEqual(grant.device_id, self.store.get(grant.device_id).id)
        self.assertTrue(grant.token.startswith("pager_"))
        self.assertIsNotNone(self.manager.authenticate(grant.token))
        self.assertIsNone(self.manager.authenticate(grant.token + "x"))

        public = self.manager.public_devices()
        self.assertEqual(len(public), 1)
        self.assertNotIn("token", public[0])
        self.assertNotIn("tokenHash", public[0])
        raw = (self.root / "devices.json").read_text(encoding="utf-8")
        self.assertNotIn(grant.token, raw)
        self.assertIn("tokenHash", raw)

    def test_code_is_single_use(self) -> None:
        session = self.manager.start(ip="127.0.0.1")
        self.manager.confirm(session.code, name="One", ip="192.168.1.20")
        with self.assertRaises(PairingError) as ctx:
            self.manager.confirm(session.code, name="Two", ip="192.168.1.21")
        self.assertEqual(ctx.exception.code, "invalid_code")

    def test_expired_code(self) -> None:
        session = self.manager.start(ip="127.0.0.1")
        self.clock.now += 121
        with self.assertRaises(PairingError) as ctx:
            self.manager.confirm(session.code, name="Late", ip="192.168.1.20")
        self.assertEqual(ctx.exception.code, "expired_code")

    def test_invalid_code_rate_limit(self) -> None:
        self.manager.start(ip="127.0.0.1")
        for index in range(20):
            with self.assertRaises(PairingError):
                self.manager.confirm(f"{index:06d}", name="Bad", ip="192.168.1.20")
        with self.assertRaises(PairingError) as ctx:
            self.manager.confirm("654321", name="Good", ip="192.168.1.20")
        self.assertEqual(ctx.exception.code, "rate_limited")
        self.assertEqual(ctx.exception.status_code, 429)

    def test_revoke_invalidates_token(self) -> None:
        session = self.manager.start(ip="127.0.0.1")
        grant = self.manager.confirm(session.code, name="Revoked", ip="192.168.1.20")
        self.assertIsNotNone(self.manager.authenticate(grant.token))
        record = self.manager.revoke(grant.device_id, ip="127.0.0.1")
        self.assertIsNotNone(record)
        self.assertIsNone(self.manager.authenticate(grant.token))
        self.assertTrue(self.manager.public_devices()[0]["revoked"])

    def test_persistence_reloads_device_registry(self) -> None:
        session = self.manager.start(ip="127.0.0.1")
        grant = self.manager.confirm(session.code, name="Persistent", ip="192.168.1.20")
        reloaded = DeviceStore(self.root / "devices.json")
        manager2 = PairingManager(reloaded, clock=self.clock)
        self.assertIsNotNone(manager2.authenticate(grant.token))

    def test_audit_does_not_contain_code_or_token(self) -> None:
        session = self.manager.start(ip="127.0.0.1")
        grant = self.manager.confirm(session.code, name="Audit", ip="192.168.1.20")
        audit = (self.root / "audit.log").read_text(encoding="utf-8")
        self.assertNotIn(session.code, audit)
        self.assertNotIn(grant.token, audit)
        self.assertIn("pair.confirm", audit)


class ProfileTests(unittest.TestCase):
    def test_default_profile_is_hermes_and_public_is_safe(self) -> None:
        registry = ProfileRegistry.default_registry()
        self.assertEqual(registry.default_id, "hermes")
        public = registry.public_list()[0]
        self.assertEqual(public["id"], "hermes")
        self.assertNotIn("command", public)
        self.assertNotIn("env", public)

    def test_load_relative_cwd_and_args(self) -> None:
        raw = {
            "default": "second",
            "profiles": [
                {
                    "id": "first",
                    "name": "First",
                    "command": "first-agent",
                    "args": ["--acp"],
                    "cwd": "pager_workspace",
                },
                {
                    "id": "second",
                    "name": "Second",
                    "command": "second-agent",
                    "enabled": True,
                },
                {
                    "id": "disabled",
                    "name": "Disabled",
                    "command": "disabled-agent",
                    "enabled": False,
                },
            ],
        }
        registry = ProfileRegistry.from_dict(raw)
        self.assertEqual(registry.default_id, "second")
        first = registry.get("first")
        self.assertEqual(first.args, ("--acp",))
        self.assertTrue(str(first.cwd).endswith("pager_workspace"))
        self.assertEqual(len(registry.public_list()), 3)

    def test_invalid_profile_kind_rejected(self) -> None:
        with self.assertRaises(ProfileError):
            ProfileRegistry.from_dict({
                "profiles": [{"id": "x", "name": "X", "kind": "http", "command": "x"}],
            })

    def test_auth_and_login_fields(self) -> None:
        registry = ProfileRegistry.from_dict({
            "profiles": [{
                "id": "x",
                "name": "X",
                "command": "agent",
                "authMethod": "cursor_login",
                "loginCommand": "login.exe",
                "loginArgs": ["auth", "login"],
                "loginCheckArgs": ["auth", "status"],
            }],
        })
        profile = registry.get("x")
        self.assertEqual(profile.auth_method, "cursor_login")
        self.assertEqual(profile.login_command, "login.exe")
        self.assertEqual(profile.login_args, ("auth", "login"))
        self.assertEqual(profile.login_check_args, ("auth", "status"))
        self.assertEqual(profile.transport, "stdio")

    def test_workbuddy_http_transport(self) -> None:
        registry = ProfileRegistry.from_dict({
            "profiles": [{
                "id": "workbuddy",
                "name": "WorkBuddy",
                "command": "node.exe",
                "args": ["codebuddy", "--serve", "--acp"],
                "transport": "workbuddy-http",
            }],
        })
        self.assertEqual(registry.get("workbuddy").transport, "workbuddy-http")
        with self.assertRaises(ProfileError):
            ProfileRegistry.from_dict({
                "profiles": [{
                    "id": "x",
                    "name": "X",
                    "command": "agent",
                    "transport": "websocket",
                }],
            })

    def test_login_args_require_login_command(self) -> None:
        with self.assertRaises(ProfileError):
            ProfileRegistry.from_dict({
                "profiles": [{
                    "id": "x",
                    "name": "X",
                    "command": "agent",
                    "loginArgs": ["auth", "login"],
                }],
            })

    def test_acp_client_accepts_profile_args(self) -> None:
        client = ACPClient(
            "agent.exe",
            ".",
            on_update=lambda _: None,
            on_permission=lambda *_: None,
            args=["acp", "--stdio"],
        )
        self.assertEqual(client.args, ["acp", "--stdio"])
        self.assertEqual(client.exe, "agent.exe")

    def test_acp_authenticate_uses_method_id(self) -> None:
        client = ACPClient(
            "agent.exe",
            ".",
            on_update=lambda _: None,
            on_permission=lambda *_: None,
        )
        seen: dict[str, object] = {}

        def fake_request(method: str, params: dict, timeout: float = 120.0) -> dict:
            seen.update(method=method, params=params, timeout=timeout)
            return {"ok": True}

        client.request = fake_request  # type: ignore[method-assign]
        result = client.authenticate("cursor_login", timeout=42)
        self.assertEqual(result, {"ok": True})
        self.assertEqual(seen, {
            "method": "authenticate",
            "params": {"methodId": "cursor_login"},
            "timeout": 42,
        })

    def test_transport_dead_text_matches_cursor_http2_failures(self) -> None:
        self.assertTrue(transport_dead_text("Error: RetriableError: [unavailable] PING timed out"))
        self.assertTrue(transport_dead_text("Error: RetriableError: [aborted] read ECONNRESET"))
        self.assertFalse(transport_dead_text("当前用的是 Cursor。"))
        self.assertFalse(transport_dead_text(""))

    def test_visible_transcript_keeps_disk_history_and_live_tail(self) -> None:
        saved = [
            {"ts": 10, "role": "user", "text": "测试", "profile": "cursor", "profileName": "Cursor"},
            {"ts": 20, "role": "assistant", "text": "在。", "profile": "cursor", "profileName": "Cursor"},
        ]
        memory = [
            {"id": "u1", "role": "user", "text": "测试", "ts": 10},
            {"id": "a1", "role": "assistant", "text": "在。", "ts": 11},
            {"id": "a2", "role": "assistant", "text": "正在写", "ts": 30},
        ]
        shown = visible_transcript(memory, saved, limit=60)
        self.assertEqual([row["text"] for row in shown], ["测试", "在。", "正在写"])

    def test_pick_login_method_prefers_browser_login(self) -> None:
        methods = [
            {"id": "api-key", "name": "API Key"},
            {"id": "chat-gpt", "name": "ChatGPT"},
        ]
        self.assertEqual(pick_login_method(methods), "chat-gpt")
        self.assertEqual(pick_login_method(methods, "chat-gpt"), "chat-gpt")

    def test_pick_login_method_skips_terminal_setup(self) -> None:
        methods = [
            {"id": "deepseek"},
            {"id": "hermes-setup", "type": "terminal"},
        ]
        self.assertEqual(pick_login_method(methods), "deepseek")

    def test_pick_workbuddy_wechat_login(self) -> None:
        methods = [
            {"id": "iOA"},
            {"id": "external"},
            {"id": "internal"},
            {"id": "selfhosted"},
        ]
        self.assertEqual(pick_login_method(methods), "internal")
        self.assertEqual(pick_login_method(methods, "internal"), "internal")


class TlsTests(unittest.TestCase):
    def test_generate_and_reuse_certificate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = tlsutil.ensure_cert(root)
            self.assertTrue(first.cert_path.exists())
            self.assertTrue(first.key_path.exists())
            self.assertEqual(len(first.fingerprint_sha256), 64)
            self.assertIn("127.0.0.1", first.sans)
            self.assertIn("pager.local", first.sans)
            second = tlsutil.ensure_cert(root)
            self.assertEqual(first.fingerprint_sha256, second.fingerprint_sha256)
            self.assertEqual(
                tlsutil.read_fingerprint(first.cert_path),
                first.fingerprint_sha256,
            )
            self.assertIn(":", tlsutil.format_fingerprint(first.fingerprint_sha256))


class DeviceApiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.store = DeviceStore(self.root / "devices.json")
        self.manager = PairingManager(
            self.store,
            audit_path=self.root / "audit.log",
            code_factory=lambda: "112233",
        )
        self.client = TestClient(create_device_app(self.manager, cert_dir=self.root / "certs"))

    def tearDown(self) -> None:
        self.client.close()
        self.tmp.cleanup()

    def test_pair_confirm_over_device_api(self) -> None:
        self.manager.start(ip="127.0.0.1")
        response = self.client.post(
            "/device/pair/confirm",
            json={"code": "112233", "name": "ESP Pager", "platform": "esp32s3"},
        )
        self.assertEqual(response.status_code, 200)
        body = response.json()
        self.assertTrue(body["ok"])
        self.assertTrue(body["token"].startswith("pager_"))
        self.assertEqual(body["tokenType"], "Bearer")
        self.assertIsNotNone(self.manager.authenticate(body["token"]))

    def test_bad_pair_code_is_not_accepted(self) -> None:
        self.manager.start(ip="127.0.0.1")
        response = self.client.post(
            "/device/pair/confirm",
            json={"code": "000000", "name": "Bad"},
        )
        self.assertEqual(response.status_code, 400)
        self.assertEqual(response.json()["error"], "invalid_code")


if __name__ == "__main__":
    unittest.main()
