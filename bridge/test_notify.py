"""Tests for notify / toast / rate limits (no network)."""
from __future__ import annotations

import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from pager_bridge import bridge as bridge_mod  # noqa: E402
from pager_bridge.bridge import Bridge, _notify_rate, load_macros, public_macros  # noqa: E402


class ToastTests(unittest.TestCase):
    def setUp(self) -> None:
        self.loop = asyncio.new_event_loop()
        self.b = Bridge()
        self.b.attach_loop(self.loop)
        self.q = self.b.subscribe()

    def _drain(self) -> dict:
        async def _tick() -> None:
            await asyncio.sleep(0)

        self.loop.run_until_complete(_tick())
        return self.q.get_nowait()

    def tearDown(self) -> None:
        self.b.unsubscribe(self.q)

    def test_push_toast_shape(self) -> None:
        self.b.push_toast("构建", "完成", level="ok", ttl_ms=5000)
        event = self._drain()
        self.assertEqual(event["type"], "toast")
        self.assertEqual(event["title"], "构建")
        self.assertEqual(event["body"], "完成")
        self.assertEqual(event["level"], "ok")
        self.assertEqual(event["ttlMs"], 5000)
        self.assertIn("seq", event)
        self.assertIn("ts", event)

    def test_ring_device(self) -> None:
        self.b.ring_device("short", 2)
        event = self._drain()
        self.assertEqual(event["type"], "ring_device")
        self.assertEqual(event["pattern"], "short")
        self.assertEqual(event["repeat"], 2)

    def test_macros_load(self) -> None:
        macros = load_macros()
        self.assertIn("echo_ok", macros)
        pub = public_macros()
        self.assertTrue(any(m["id"] == "echo_ok" for m in pub))

    def test_notify_rate_limit(self) -> None:
        _notify_rate._events.clear()
        key = "test-ip"
        for _ in range(10):
            self.assertTrue(_notify_rate.allow(key))
        self.assertFalse(_notify_rate.allow(key))


if __name__ == "__main__":
    unittest.main()
