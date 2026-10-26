"""mDNS advertisement for the Pager device endpoint.

Service type: ``_pager._tcp.local.``
TXT records:
    version=1
    path=/device/ws
    pair=/device/pair/confirm
    tls=1
    fp=<sha256 certificate fingerprint>

mDNS is discovery only.  It is not trusted: the six-digit code and the
fingerprint shown by the PC remain the pairing authority.
"""
from __future__ import annotations

import logging
import os
import socket
from dataclasses import dataclass
from typing import Any

SERVICE_TYPE = "_pager._tcp.local."
DEFAULT_NAME = "Pager Bridge"
LOG = logging.getLogger("pager.discovery")


@dataclass
class MdnsAdvertisement:
    zeroconf: Any
    info: Any

    @property
    def name(self) -> str:
        return str(getattr(self.info, "name", ""))


def local_ipv4() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            ip = sock.getsockname()[0]
            if ip and not ip.startswith("127."):
                return ip
    except Exception:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip and not ip.startswith("127."):
                return ip
    except Exception:
        pass
    return ""


def instance_name() -> str:
    configured = os.environ.get("PAGER_MDNS_NAME", "").strip()
    if configured:
        return configured
    host = socket.gethostname().split(".")[0] or "pager"
    return f"Pager {host}"


def start_mdns(
    port: int,
    fingerprint: str,
    *,
    path: str = "/device/ws",
    pair_path: str = "/device/pair/confirm",
    tls: bool = True,
) -> MdnsAdvertisement | None:
    """Register the service. Returns None when mDNS is unavailable/disabled."""
    if os.environ.get("PAGER_MDNS", "1") not in ("1", "true", "TRUE", "yes", "on"):
        LOG.info("mDNS disabled by PAGER_MDNS")
        return None
    try:
        from zeroconf import ServiceInfo, Zeroconf
    except Exception as exc:
        LOG.warning("mDNS unavailable (install zeroconf): %s", exc)
        return None

    ip = local_ipv4()
    if not ip:
        LOG.warning("mDNS skipped: no non-loopback IPv4 address")
        return None
    instance = instance_name()
    host = socket.gethostname().split(".")[0].replace(" ", "-") or "pager"
    zc = None
    try:
        info = ServiceInfo(
            SERVICE_TYPE,
            f"{instance}.{SERVICE_TYPE}",
            addresses=[socket.inet_aton(ip)],
            port=int(port),
            properties={
                "version": "1",
                "path": path,
                "pair": pair_path,
                "tls": "1" if tls else "0",
                "fp": fingerprint,
            },
            server=f"{host}.local.",
        )
        zc = Zeroconf()
        # If another bridge instance (or a stale record) already owns the
        # friendly name, let zeroconf pick a unique suffix instead of failing
        # the whole G1 startup.
        zc.register_service(info, allow_name_change=True)
    except Exception as exc:
        LOG.warning("mDNS registration failed: %r", exc)
        try:
            zc.close()
        except Exception:
            pass
        return None
    LOG.info("mDNS advertised %s on %s:%s", info.name, ip, port)
    return MdnsAdvertisement(zeroconf=zc, info=info)


def stop_mdns(advertisement: MdnsAdvertisement | None) -> None:
    if not advertisement:
        return
    try:
        advertisement.zeroconf.unregister_service(advertisement.info)
    except Exception:
        pass
    try:
        advertisement.zeroconf.close()
    except Exception:
        pass
