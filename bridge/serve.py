"""G1 launcher: localhost admin UI + LAN HTTPS/WSS device endpoint.

    python -m pager_bridge.serve

Ports:
    127.0.0.1:8765   PC UI / localhost admin API (plain HTTP)
    0.0.0.0:8766     device pairing + authenticated WSS (TLS by default)

Both listeners share one asyncio loop, so the bridge event bus and live device
sockets can interact safely.  The admin listener is deliberately hard-coded to
loopback; only the device app is bound to the LAN interface.
"""
from __future__ import annotations

import asyncio
import logging
import os

import uvicorn

from . import tls as tlsutil
from .bridge import CERT_DIR, DEVICE_PORT, app
from .device_api import device_app
from .discovery import local_ipv4, start_mdns, stop_mdns


def _extra_hosts() -> list[str]:
    raw = os.environ.get("PAGER_TLS_HOSTS", "")
    return [part.strip() for part in raw.split(",") if part.strip()]


async def run() -> None:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    insecure = os.environ.get("PAGER_INSECURE_DEVICE", "") in ("1", "true", "TRUE", "yes", "on")
    admin_port = int(os.environ.get("PAGER_PORT", "8765"))
    device_bind = os.environ.get("PAGER_DEVICE_BIND", "0.0.0.0")
    device_port = DEVICE_PORT

    if insecure:
        cert = None
        print("WARNING: PAGER_INSECURE_DEVICE=1 — device endpoint is HTTP, not WSS")
    else:
        cert = tlsutil.ensure_cert(CERT_DIR, extra_hosts=_extra_hosts())
        print(f"TLS SHA-256: {cert.fingerprint_sha256}")
        print(f"TLS display: {cert.fingerprint_display}")

    admin_config = uvicorn.Config(
        app,
        host="127.0.0.1",
        port=admin_port,
        log_level="info",
    )
    device_kwargs = {
        "host": device_bind,
        "port": device_port,
        "log_level": "info",
        "access_log": False,
    }
    if cert is not None:
        device_kwargs.update({
            "ssl_certfile": str(cert.cert_path),
            "ssl_keyfile": str(cert.key_path),
            # uvicorn defaults ssl_ciphers to "TLSv1" (TLS 1.0 suites). TLS 1.3
            # ignores that list, but the watch is TLS 1.2 and offers only
            # ECDHE-RSA-AES-GCM, so the handshake is closed before HTTP.
            "ssl_ciphers": "ECDHE+AESGCM:ECDHE+CHACHA20:@SECLEVEL=1",
        })
    device_config = uvicorn.Config(device_app, **device_kwargs)

    admin_server = uvicorn.Server(admin_config)
    device_server = uvicorn.Server(device_config)

    ip = local_ipv4() or "127.0.0.1"
    scheme = "http" if insecure else "https"
    ws_scheme = "ws" if insecure else "wss"
    print(f"PC UI:       http://127.0.0.1:{admin_port}/")
    print(f"Device API:  {scheme}://{ip}:{device_port}/device/pair/confirm")
    print(f"Device WS:   {ws_scheme}://{ip}:{device_port}/device/ws")

    mdns_holder: list[object | None] = [None]

    async def advertise_when_ready() -> None:
        while not (admin_server.started and device_server.started):
            if admin_task.done() or device_task.done():
                return
            await asyncio.sleep(0.1)
        fp = cert.fingerprint_sha256 if cert is not None else ""
        # python-zeroconf refuses to start on a thread that is running an
        # asyncio loop ("EventLoopBlocked").  Run only the advertisement setup
        # in a worker thread; the returned Zeroconf object keeps its own loop.
        mdns_holder[0] = await asyncio.to_thread(
            start_mdns,
            device_port,
            fp,
            path="/device/ws",
            pair_path="/device/pair/confirm",
            tls=cert is not None,
        )
        if mdns_holder[0] is not None:
            print(f"mDNS:        {mdns_holder[0].name}")  # type: ignore[union-attr]
        else:
            print("mDNS:        not advertised")

    # Run the admin server through the public serve() entry point so it owns
    # Ctrl+C handling, and run the device server with the same loop but without
    # installing a second signal handler.  Either server exiting stops both.
    admin_task = asyncio.create_task(admin_server.serve())
    device_task = asyncio.create_task(device_server._serve())
    advertise_task = asyncio.create_task(advertise_when_ready())
    try:
        await asyncio.wait({admin_task, device_task}, return_when=asyncio.FIRST_COMPLETED)
        admin_server.should_exit = True
        device_server.should_exit = True
        await asyncio.gather(admin_task, device_task, return_exceptions=True)
    finally:
        advertise_task.cancel()
        try:
            await advertise_task
        except (asyncio.CancelledError, Exception):
            pass
        if mdns_holder[0] is not None:
            await asyncio.to_thread(stop_mdns, mdns_holder[0])  # type: ignore[arg-type]


def main() -> None:
    asyncio.run(run())


if __name__ == "__main__":
    main()
