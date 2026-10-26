"""LAN-facing G1 device API (pairing + authenticated WebSocket).

This app is intentionally separate from ``bridge.app``.  It exposes only the
pairing confirmation and device WebSocket; the PC admin UI/API is served from a
different, loopback-only listener by ``pager_bridge.serve``.

Run through the G1 launcher:
    python -m pager_bridge.serve
"""
from __future__ import annotations

from pathlib import Path
from typing import Any

from fastapi import FastAPI, Request, WebSocket
from fastapi.responses import JSONResponse

from . import tls as tlsutil
from .bridge import (
    CERT_DIR,
    DEVICE_HUB,
    DEVICE_PAIR_PATH,
    DEVICE_PORT,
    DEVICE_WS_PATH,
    PAIRING,
    DeviceHub,
    PairingManager,
    bridge,
    serve_bridge_ws,
)
from .security import PairingError


def _ip(request: Request | WebSocket) -> str:
    client = request.client
    return client.host if client else ""


def _protocols(ws: WebSocket) -> list[str]:
    raw = ws.headers.get("sec-websocket-protocol", "")
    return [part.strip() for part in raw.split(",") if part.strip()]


def _token_from_ws(ws: WebSocket) -> str:
    auth = ws.headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    header = ws.headers.get("x-pager-token", "").strip()
    if header:
        return header
    # Browser WebSocket cannot set Authorization.  It can send the token as a
    # subprotocol, which is a header and therefore not written to access logs.
    for protocol in _protocols(ws):
        if protocol.startswith("pager.token."):
            return protocol[len("pager.token."):]
    # Compatibility for simple scripts/tests; prefer a header over a query.
    return (ws.query_params.get("token") or "").strip()


def _info(manager: PairingManager, cert_dir: Path) -> dict[str, Any]:
    fp = tlsutil.read_fingerprint(cert_dir / tlsutil.CERT_NAME)
    return {
        "tlsPort": DEVICE_PORT,
        "wsPath": DEVICE_WS_PATH,
        "pairPath": DEVICE_PAIR_PATH,
        "fingerprint": fp,
        "fingerprintDisplay": tlsutil.format_fingerprint(fp),
        "mdnsService": "_pager._tcp.local.",
    }


def create_device_app(
    manager: PairingManager = PAIRING,
    *,
    hub: DeviceHub = DEVICE_HUB,
    cert_dir: Path = CERT_DIR,
    pair_path: str = DEVICE_PAIR_PATH,
    ws_path: str = DEVICE_WS_PATH,
) -> FastAPI:
    app = FastAPI(
        title="Pager G1 Device API",
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
    )

    @app.get("/health")
    async def health() -> dict[str, Any]:
        return {"ok": True, "service": "pager-device", "version": "g1"}

    @app.post(pair_path)
    async def pair_confirm(request: Request, payload: dict[str, Any]) -> JSONResponse:
        ip = _ip(request)
        expected = tlsutil.read_fingerprint(cert_dir / tlsutil.CERT_NAME)
        supplied = str(
            payload.get("certFingerprint")
            or payload.get("fingerprint")
            or ""
        ).strip()
        if expected and supplied and tlsutil.normalize_fingerprint(supplied) != expected:
            manager.audit("pair.confirm", ok=False, reason="cert_mismatch", ip=ip)
            return JSONResponse(
                status_code=400,
                content={
                    "ok": False,
                    "error": "cert_mismatch",
                    "message": "TLS 证书指纹与配对请求不一致",
                },
                headers={"Cache-Control": "no-store"},
            )
        try:
            grant = manager.confirm(
                payload.get("code"),
                name=str(payload.get("name") or payload.get("deviceName") or "Pager"),
                platform=str(payload.get("platform") or ""),
                cert_fingerprint=supplied or expected,
                ip=ip,
            )
        except PairingError as exc:
            return JSONResponse(
                status_code=exc.status_code,
                content={"ok": False, "error": exc.code, "message": exc.message},
                headers={"Cache-Control": "no-store"},
            )
        except Exception as exc:
            manager.audit("pair.confirm", ok=False, reason="internal_error", ip=ip)
            return JSONResponse(
                status_code=500,
                content={"ok": False, "error": "internal_error", "message": str(exc)},
                headers={"Cache-Control": "no-store"},
            )

        info = _info(manager, cert_dir)
        return JSONResponse(
            {
                "ok": True,
                "deviceId": grant.device_id,
                "token": grant.token,
                "tokenType": "Bearer",
                "pairedAt": grant.created_at,
                **info,
            },
            headers={"Cache-Control": "no-store", "Pragma": "no-cache"},
        )

    @app.websocket(ws_path)
    async def device_ws(ws: WebSocket) -> None:
        ip = _ip(ws)
        if manager.auth_rate_limited(ip):
            # Closing before accept makes uvicorn return HTTP 403 instead of
            # briefly opening an unauthenticated WebSocket.
            await ws.close(code=4429, reason="too many auth failures")
            return
        token = _token_from_ws(ws)
        record = manager.authenticate(token)
        if not record:
            manager.record_auth_failure(ip)
            await ws.close(code=4401, reason="invalid or revoked token")
            return

        manager.touch(record.id)
        manager.audit("device.ws.open", ok=True, device_id=record.id, ip=ip)
        hub.register(record.id, ws)
        protocol = "pager.v1" if "pager.v1" in _protocols(ws) else None
        try:
            await serve_bridge_ws(ws, subprotocol=protocol, remote=True)
        finally:
            hub.unregister(record.id, ws)
            manager.audit("device.ws.close", device_id=record.id, ip=ip)

    return app


device_app = create_device_app()
