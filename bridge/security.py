"""G1 device identity: pairing codes, hashed bearer tokens, audit and live sockets.

The module deliberately contains no FastAPI/uvicorn imports so it can be tested
in isolation and reused by a future ESP provisioning tool.

Security model
--------------
* Pairing starts on the localhost-only admin surface. A six-digit code is valid
  for a short TTL, is one-shot and is never written to the audit log.
* The device exchanges the code for a high-entropy bearer token. Only a SHA-256
  hash of the token is persisted; the plaintext is returned exactly once.
* Revocation clears the stored hash and closes any live device socket.
* Audit records contain identifiers and outcomes, never codes or tokens.
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import os
import re
import secrets
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable

TOKEN_PREFIX = "pager_"


class PairingError(RuntimeError):
    """A pairing attempt that must be reported to the device/admin client."""

    def __init__(self, code: str, message: str, status_code: int = 400) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status_code = status_code


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def normalize_code(value: Any) -> str:
    digits = re.sub(r"\D", "", str(value or ""))
    return digits if len(digits) == 6 else ""


def clean_name(value: Any, fallback: str = "Pager") -> str:
    text = re.sub(r"[\x00-\x1f\x7f]+", " ", str(value or "")).strip()
    text = re.sub(r"\s+", " ", text)
    return (text[:64] or fallback)


@dataclass
class DeviceRecord:
    id: str
    name: str
    token_hash: str
    created_at: float
    last_seen_at: float | None = None
    revoked_at: float | None = None
    platform: str = ""
    cert_fingerprint: str = ""

    @property
    def revoked(self) -> bool:
        return self.revoked_at is not None

    def to_json(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "name": self.name,
            "tokenHash": self.token_hash,
            "createdAt": self.created_at,
            "lastSeenAt": self.last_seen_at,
            "revokedAt": self.revoked_at,
            "platform": self.platform,
            "certFingerprint": self.cert_fingerprint,
        }

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> "DeviceRecord":
        return cls(
            id=str(data["id"]),
            name=str(data.get("name") or "Pager"),
            token_hash=str(data.get("tokenHash") or ""),
            created_at=float(data.get("createdAt") or 0),
            last_seen_at=float(data["lastSeenAt"]) if data.get("lastSeenAt") is not None else None,
            revoked_at=float(data["revokedAt"]) if data.get("revokedAt") is not None else None,
            platform=str(data.get("platform") or ""),
            cert_fingerprint=str(data.get("certFingerprint") or ""),
        )

    def public(self, connected: bool = False) -> dict[str, Any]:
        return {
            "id": self.id,
            "name": self.name,
            "createdAt": self.created_at,
            "lastSeenAt": self.last_seen_at,
            "revokedAt": self.revoked_at,
            "revoked": self.revoked,
            "platform": self.platform,
            "certFingerprint": self.cert_fingerprint,
            "connected": connected,
        }


class DeviceStore:
    """Small JSON-backed device registry with atomic writes."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._lock = threading.RLock()
        self._devices: dict[str, DeviceRecord] = {}
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._load()

    def _load(self) -> None:
        if not self.path.exists():
            return
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            items = raw.get("devices", []) if isinstance(raw, dict) else raw
            if not isinstance(items, list):
                raise ValueError("devices must be a list")
            loaded: dict[str, DeviceRecord] = {}
            for item in items:
                if isinstance(item, dict) and item.get("id"):
                    record = DeviceRecord.from_json(item)
                    loaded[record.id] = record
            self._devices = loaded
        except Exception:
            # Do not silently destroy an unreadable registry. Keep a timestamped
            # copy for recovery, then start with an empty registry.
            backup = self.path.with_name(
                f"{self.path.name}.corrupt-{int(time.time())}"
            )
            try:
                os.replace(self.path, backup)
            except OSError:
                pass
            self._devices = {}

    def _save(self) -> None:
        payload = {
            "version": 1,
            "devices": [record.to_json() for record in self._devices.values()],
        }
        tmp = self.path.with_name(f".{self.path.name}.{os.getpid()}.tmp")
        tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass
        os.replace(tmp, self.path)

    def add(
        self,
        *,
        name: str,
        token_hash: str,
        device_id: str | None = None,
        platform: str = "",
        cert_fingerprint: str = "",
        created_at: float | None = None,
    ) -> DeviceRecord:
        with self._lock:
            did = device_id or self._new_id()
            if did in self._devices:
                raise ValueError("device id collision")
            record = DeviceRecord(
                id=did,
                name=clean_name(name),
                token_hash=token_hash,
                created_at=created_at if created_at is not None else time.time(),
                platform=clean_name(platform, "") if platform else "",
                cert_fingerprint=str(cert_fingerprint or ""),
            )
            self._devices[did] = record
            try:
                self._save()
            except Exception:
                self._devices.pop(did, None)
                raise
            return record

    def _new_id(self) -> str:
        while True:
            did = "dev_" + secrets.token_hex(6)
            if did not in self._devices:
                return did

    def get(self, device_id: str) -> DeviceRecord | None:
        with self._lock:
            return self._devices.get(device_id)

    def all(self) -> list[DeviceRecord]:
        with self._lock:
            return sorted(self._devices.values(), key=lambda d: d.created_at, reverse=True)

    def touch(self, device_id: str, when: float | None = None) -> None:
        with self._lock:
            record = self._devices.get(device_id)
            if not record or record.revoked:
                return
            record.last_seen_at = when if when is not None else time.time()
            try:
                self._save()
            except Exception:
                pass

    def revoke(self, device_id: str, when: float | None = None) -> DeviceRecord | None:
        with self._lock:
            record = self._devices.get(device_id)
            if not record:
                return None
            record.revoked_at = when if when is not None else time.time()
            record.token_hash = ""
            self._save()
            return record

    def public_list(self, connected_ids: Iterable[str] = ()) -> list[dict[str, Any]]:
        connected = set(connected_ids)
        return [record.public(record.id in connected) for record in self.all()]


@dataclass
class PairingSession:
    id: str
    code: str
    created_at: float
    expires_at: float
    name_hint: str = ""
    attempts: int = 0

    @property
    def ttl_remaining(self) -> int:
        return max(0, int(round(self.expires_at - time.time())))


@dataclass
class PairingGrant:
    device_id: str
    name: str
    token: str
    created_at: float
    platform: str
    cert_fingerprint: str


class PairingManager:
    """Short-lived pairing codes plus long-lived hashed device tokens."""

    def __init__(
        self,
        store: DeviceStore,
        *,
        ttl: float = 120.0,
        max_attempts: int = 5,
        max_active: int = 12,
        audit_path: str | Path | None = None,
        clock: Callable[[], float] = time.time,
        code_factory: Callable[[], str] | None = None,
    ) -> None:
        self.store = store
        self.ttl = ttl
        self.max_attempts = max_attempts
        self.max_active = max_active
        self.audit_path = Path(audit_path) if audit_path else None
        self._clock = clock
        self._code_factory = code_factory or (lambda: f"{secrets.randbelow(1_000_000):06d}")
        self._sessions: dict[str, PairingSession] = {}
        self._failures: dict[str, list[float]] = {}
        self._lock = threading.RLock()
        self._seq = 0
        if self.audit_path:
            self.audit_path.parent.mkdir(parents=True, exist_ok=True)

    # ---- audit -------------------------------------------------------------

    def audit(self, event: str, **fields: Any) -> None:
        if not self.audit_path:
            return
        record = {"ts": self._clock(), "event": event, **fields}
        try:
            with self._lock:
                with self.audit_path.open("a", encoding="utf-8") as fh:
                    fh.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
        except Exception:
            # Auditing must never break pairing or a live device connection.
            pass

    # ---- pairing -----------------------------------------------------------

    def _prune_failures(self, now: float) -> None:
        for ip, stamps in list(self._failures.items()):
            recent = [t for t in stamps if now - t < 60]
            if recent:
                self._failures[ip] = recent
            else:
                self._failures.pop(ip, None)

    def _prune_sessions(self, now: float) -> None:
        expired = [code for code, s in self._sessions.items() if s.expires_at <= now]
        for code in expired:
            self._sessions.pop(code, None)

    def _record_failure(self, ip: str, now: float) -> None:
        if not ip:
            return
        stamps = self._failures.setdefault(ip, [])
        stamps.append(now)

    def _check_rate(self, ip: str, now: float) -> None:
        self._prune_failures(now)
        if ip and len(self._failures.get(ip, [])) >= 20:
            self.audit("pair.rate_limited", ip=ip)
            raise PairingError("rate_limited", "尝试次数过多，请稍后再试", 429)

    def start(self, *, name_hint: str = "", ip: str = "") -> PairingSession:
        with self._lock:
            now = self._clock()
            self._prune_failures(now)
            self._prune_sessions(now)
            if len(self._sessions) >= self.max_active:
                raise PairingError("pairing_busy", "当前配对码过多，请稍后再试", 429)
            code = ""
            for _ in range(100):
                candidate = normalize_code(self._code_factory())
                if candidate and candidate not in self._sessions:
                    code = candidate
                    break
            if not code:
                raise PairingError("pairing_error", "无法生成配对码", 500)
            self._seq += 1
            session = PairingSession(
                id=f"pair-{self._seq}",
                code=code,
                created_at=now,
                expires_at=now + self.ttl,
                name_hint=clean_name(name_hint, "") if name_hint else "",
            )
            self._sessions[code] = session
        self.audit(
            "pair.start",
            pairing_id=session.id,
            ip=ip,
            expires_at=session.expires_at,
        )
        return session

    def confirm(
        self,
        code: Any,
        *,
        name: str = "Pager",
        platform: str = "",
        cert_fingerprint: str = "",
        ip: str = "",
    ) -> PairingGrant:
        normalized = normalize_code(code)
        with self._lock:
            now = self._clock()
            self._check_rate(ip, now)
            session = self._sessions.get(normalized) if normalized else None
            if not session:
                self._record_failure(ip, now)
                self.audit("pair.confirm", ok=False, reason="invalid_code", ip=ip)
                raise PairingError("invalid_code", "配对码无效或已过期", 400)
            if session.expires_at <= now:
                self._sessions.pop(normalized, None)
                self._record_failure(ip, now)
                self.audit("pair.confirm", ok=False, reason="expired", pairing_id=session.id, ip=ip)
                raise PairingError("expired_code", "配对码已过期，请在电脑上重新生成", 400)
            if session.attempts >= self.max_attempts:
                self._sessions.pop(normalized, None)
                self._record_failure(ip, now)
                self.audit("pair.confirm", ok=False, reason="too_many_attempts", pairing_id=session.id, ip=ip)
                raise PairingError("too_many_attempts", "配对尝试次数过多，请重新生成配对码", 429)

            device_name = clean_name(name or session.name_hint)
            device_id = "dev_" + secrets.token_hex(6)
            token = f"{TOKEN_PREFIX}{device_id}.{secrets.token_urlsafe(32)}"
            try:
                record = self.store.add(
                    name=device_name,
                    token_hash=hash_token(token),
                    device_id=device_id,
                    platform=platform,
                    cert_fingerprint=cert_fingerprint,
                    created_at=now,
                )
            except Exception:
                self._record_failure(ip, now)
                self.audit("pair.confirm", ok=False, reason="store_error", pairing_id=session.id, ip=ip)
                raise
            self._sessions.pop(normalized, None)
            self._failures.pop(ip, None)
        self.audit(
            "pair.confirm",
            ok=True,
            pairing_id=session.id,
            device_id=record.id,
            device_name=record.name,
            platform=record.platform,
            ip=ip,
        )
        return PairingGrant(
            device_id=record.id,
            name=record.name,
            token=token,
            created_at=record.created_at,
            platform=record.platform,
            cert_fingerprint=record.cert_fingerprint,
        )

    # ---- bearer auth -------------------------------------------------------

    def authenticate(self, token: str | None) -> DeviceRecord | None:
        if not token or not token.startswith(TOKEN_PREFIX):
            return None
        body = token[len(TOKEN_PREFIX):]
        device_id, sep, _secret = body.partition(".")
        if not sep or not device_id:
            return None
        record = self.store.get(device_id)
        if not record or record.revoked or not record.token_hash:
            return None
        if not hmac.compare_digest(hash_token(token), record.token_hash):
            return None
        return record

    def record_auth_failure(self, ip: str, reason: str = "invalid_token") -> None:
        with self._lock:
            now = self._clock()
            self._record_failure(ip, now)
        self.audit("device.auth", ok=False, reason=reason, ip=ip)

    def auth_rate_limited(self, ip: str) -> bool:
        with self._lock:
            now = self._clock()
            self._prune_failures(now)
            return bool(ip and len(self._failures.get(ip, [])) >= 20)

    def touch(self, device_id: str) -> None:
        self.store.touch(device_id)

    def revoke(self, device_id: str, *, ip: str = "") -> DeviceRecord | None:
        record = self.store.revoke(device_id)
        if record:
            self.audit("device.revoke", ok=True, device_id=device_id, ip=ip)
        else:
            self.audit("device.revoke", ok=False, reason="not_found", device_id=device_id, ip=ip)
        return record

    def public_devices(self, connected_ids: Iterable[str] = ()) -> list[dict[str, Any]]:
        return self.store.public_list(connected_ids)


class DeviceHub:
    """Tracks live device WebSockets so revocation takes effect immediately."""

    def __init__(self) -> None:
        self._sessions: dict[str, set[Any]] = {}
        self._lock = threading.RLock()

    def register(self, device_id: str, websocket: Any) -> None:
        with self._lock:
            self._sessions.setdefault(device_id, set()).add(websocket)

    def unregister(self, device_id: str, websocket: Any) -> None:
        with self._lock:
            sockets = self._sessions.get(device_id)
            if not sockets:
                return
            sockets.discard(websocket)
            if not sockets:
                self._sessions.pop(device_id, None)

    def connected_ids(self) -> set[str]:
        with self._lock:
            return set(self._sessions)

    def is_connected(self, device_id: str) -> bool:
        with self._lock:
            return bool(self._sessions.get(device_id))

    async def close_device(self, device_id: str, code: int = 4403, reason: str = "device revoked") -> None:
        with self._lock:
            sockets = list(self._sessions.get(device_id, ()))
        for websocket in sockets:
            try:
                # The admin endpoint must stay responsive even if a device is
                # wedged or no longer reading its close frame.
                await asyncio.wait_for(
                    websocket.close(code=code, reason=reason),
                    timeout=1.5,
                )
            except Exception:
                pass
