"""Agent profiles for the Pager bridge.

G1 keeps the existing ACP transport but removes the hard-coded Hermes path from
``bridge.py``.  A profile is a small, auditable description of one local agent
process:

    {
      "id": "hermes",
      "name": "Hermes",
      "kind": "acp",
      "command": "%LOCALAPPDATA%\\hermes\\bin\\hermes-acp.exe",
      "args": [],
      "cwd": "pager_workspace",
      "env": {},
      "enabled": true
    }

Only the ``acp`` kind is supported by G1.  Cursor/Codex/Claude adapters can be
added later without changing the bridge event model.
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

BASE = Path(__file__).resolve().parent
PROJECT_ROOT = BASE.parent
DEFAULT_PROFILES_FILE = BASE / "agents.json"
DEFAULT_WORKSPACE = PROJECT_ROOT / "pager_workspace"
DEFAULT_HERMES = Path(os.environ.get("LOCALAPPDATA", "")) / "hermes" / "bin" / "hermes-acp.exe"
_PROFILE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")


class ProfileError(RuntimeError):
    pass


def _expand(value: Any) -> str:
    text = str(value or "")
    return os.path.expandvars(os.path.expanduser(text))


def _string_list(value: Any, pid: str, field_name: str) -> tuple[str, ...]:
    raw = value or []
    if not isinstance(raw, list) or any(not isinstance(item, str) for item in raw):
        raise ProfileError(f"Profile {pid}: {field_name} 必须是字符串数组")
    return tuple(_expand(item) for item in raw)


def _resolve_cwd(value: Any) -> Path:
    p = Path(_expand(value or DEFAULT_WORKSPACE))
    if not p.is_absolute():
        p = PROJECT_ROOT / p
    return p.resolve(strict=False)


@dataclass(frozen=True)
class AgentProfile:
    id: str
    name: str
    command: str
    args: tuple[str, ...] = ()
    cwd: Path = DEFAULT_WORKSPACE
    env: dict[str, str] = field(default_factory=dict)
    kind: str = "acp"
    description: str = ""
    enabled: bool = True
    auth_method: str = ""
    login_command: str = ""
    login_args: tuple[str, ...] = ()
    login_check_args: tuple[str, ...] = ()
    transport: str = "stdio"

    def public(self, *, default: bool = False) -> dict[str, Any]:
        """Safe metadata for the admin UI (never includes env or command)."""
        return {
            "id": self.id,
            "name": self.name,
            "kind": self.kind,
            "description": self.description,
            "enabled": self.enabled,
            "default": default,
        }

    def runtime_public(self) -> dict[str, Any]:
        """Metadata safe to include in a snapshot sent to a device."""
        return {
            "id": self.id,
            "name": self.name,
            "kind": self.kind,
        }


@dataclass
class ProfileRegistry:
    profiles: dict[str, AgentProfile]
    default_id: str
    source: Path | None = None
    error: str | None = None

    @classmethod
    def default_registry(cls) -> "ProfileRegistry":
        profile = AgentProfile(
            id="hermes",
            name="Hermes",
            command=_expand(os.environ.get("PAGER_HERMES_ACP", str(DEFAULT_HERMES))),
            cwd=_resolve_cwd(os.environ.get("PAGER_WORKSPACE", str(DEFAULT_WORKSPACE))),
            description="Hermes ACP (G0 regression baseline)",
        )
        return cls(profiles={profile.id: profile}, default_id=profile.id)

    @classmethod
    def load(cls, path: str | Path | None = None) -> "ProfileRegistry":
        source = Path(path or os.environ.get("PAGER_PROFILES_FILE", str(DEFAULT_PROFILES_FILE)))
        if not source.exists():
            registry = cls.default_registry()
            registry.source = None
            return registry
        try:
            raw = json.loads(source.read_text(encoding="utf-8"))
        except Exception as exc:
            raise ProfileError(f"无法读取 Agent Profile 配置：{source}（{exc}）") from exc
        return cls.from_dict(raw, source=source)

    @classmethod
    def from_dict(cls, raw: Any, *, source: Path | None = None) -> "ProfileRegistry":
        if isinstance(raw, list):
            items = raw
            default_id = ""
        elif isinstance(raw, dict):
            items = raw.get("profiles")
            default_id = str(raw.get("default") or "")
        else:
            raise ProfileError("Agent Profile 配置必须是对象或数组")
        if not isinstance(items, list) or not items:
            raise ProfileError("Agent Profile 配置缺少 profiles")

        profiles: dict[str, AgentProfile] = {}
        for index, item in enumerate(items):
            if not isinstance(item, dict):
                raise ProfileError(f"profiles[{index}] 必须是对象")
            pid = str(item.get("id") or "").strip()
            if not _PROFILE_ID_RE.match(pid):
                raise ProfileError(f"profiles[{index}] 的 id 无效：{pid!r}")
            if pid in profiles:
                raise ProfileError(f"Agent Profile id 重复：{pid}")
            name = str(item.get("name") or pid).strip()[:64]
            kind = str(item.get("kind") or "acp").strip().lower()
            if kind != "acp":
                raise ProfileError(f"Profile {pid}: G1 仅支持 kind=acp，收到 {kind!r}")
            command = _expand(item.get("command") or "")
            if not command:
                raise ProfileError(f"Profile {pid}: command 不能为空")
            args_raw = item.get("args") or []
            if not isinstance(args_raw, list) or any(not isinstance(a, str) for a in args_raw):
                raise ProfileError(f"Profile {pid}: args 必须是字符串数组")
            args = tuple(_expand(a) for a in args_raw)
            login_command = _expand(item.get("loginCommand") or "")
            login_args = _string_list(item.get("loginArgs"), pid, "loginArgs")
            login_check_args = _string_list(item.get("loginCheckArgs"), pid, "loginCheckArgs")
            if (login_args or login_check_args) and not login_command:
                raise ProfileError(f"Profile {pid}: 配置了登录参数但没有 loginCommand")
            transport = str(item.get("transport") or "stdio").strip().lower()
            if transport not in {"stdio", "workbuddy-http"}:
                raise ProfileError(f"Profile {pid}: 不支持的 transport {transport!r}")
            env_raw = item.get("env") or {}
            if not isinstance(env_raw, dict):
                raise ProfileError(f"Profile {pid}: env 必须是对象")
            env = {str(k): _expand(v) for k, v in env_raw.items() if str(k)}
            profiles[pid] = AgentProfile(
                id=pid,
                name=name or pid,
                command=command,
                args=args,
                cwd=_resolve_cwd(item.get("cwd") or DEFAULT_WORKSPACE),
                env=env,
                kind=kind,
                description=str(item.get("description") or "")[:240],
                enabled=bool(item.get("enabled", True)),
                auth_method=str(item.get("authMethod") or "").strip()[:64],
                login_command=login_command,
                login_args=login_args,
                login_check_args=login_check_args,
                transport=transport,
            )

        enabled = [p for p in profiles.values() if p.enabled]
        if not enabled:
            raise ProfileError("Agent Profile 配置中没有启用的 profile")
        if default_id and default_id in profiles and profiles[default_id].enabled:
            chosen = default_id
        else:
            chosen = enabled[0].id
        return cls(profiles=profiles, default_id=chosen, source=source)

    @property
    def default(self) -> AgentProfile:
        return self.profiles[self.default_id]

    def get(self, profile_id: str) -> AgentProfile:
        profile = self.profiles.get(profile_id)
        if not profile:
            raise ProfileError(f"未知 Agent Profile：{profile_id}")
        return profile

    def enabled(self) -> list[AgentProfile]:
        return [p for p in self.profiles.values() if p.enabled]

    def public_list(self) -> list[dict[str, Any]]:
        return [
            p.public(default=(p.id == self.default_id))
            for p in sorted(self.profiles.values(), key=lambda x: (not x.enabled, x.name.lower()))
        ]
