"""Self-signed TLS material for the G1 device listener.

The PC admin surface stays on plain HTTP at 127.0.0.1.  The LAN device surface
uses a locally generated certificate; its SHA-256 fingerprint is shown in the
pairing response and advertised in mDNS so an ESP32 can pin it.

This is transport identity, not a public CA certificate.  Browsers will show a
warning; the device firmware should compare the pin/fingerprint explicitly.
"""
from __future__ import annotations

import ipaddress
import os
import socket
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

DEFAULT_CERT_DIR = Path(__file__).resolve().parent / "certs"
CERT_NAME = "pager.crt"
KEY_NAME = "pager.key"


class TlsError(RuntimeError):
    pass


@dataclass(frozen=True)
class CertInfo:
    cert_path: Path
    key_path: Path
    fingerprint_sha256: str
    fingerprint_display: str
    not_after: float
    sans: tuple[str, ...]


def normalize_fingerprint(value: str | None) -> str:
    return "".join(ch for ch in str(value or "").lower() if ch in "0123456789abcdef")


def format_fingerprint(value: str | None) -> str:
    raw = normalize_fingerprint(value)
    return ":".join(raw[i:i + 2].upper() for i in range(0, len(raw), 2))


def read_fingerprint(cert_path: str | Path) -> str:
    """Return the SHA-256 certificate fingerprint as lowercase hex."""
    path = Path(cert_path)
    if not path.exists():
        return ""
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes

        cert = x509.load_pem_x509_certificate(path.read_bytes())
        return cert.fingerprint(hashes.SHA256()).hex()
    except Exception:
        return ""


def _local_ipv4_addresses() -> list[str]:
    found: list[str] = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip and not ip.startswith("127.") and ip not in found:
                found.append(ip)
    except Exception:
        pass
    try:
        # Does not send traffic; asks the OS which local address would be used.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            ip = sock.getsockname()[0]
            if ip and not ip.startswith("127.") and ip not in found:
                found.insert(0, ip)
    except Exception:
        pass
    return found


def certificate_hosts(extra_hosts: Iterable[str] = ()) -> tuple[list[str], list[str]]:
    dns: list[str] = ["pager.local", "localhost", socket.gethostname()]
    ips: list[str] = ["127.0.0.1", "::1"]
    for host in extra_hosts:
        host = str(host or "").strip()
        if not host:
            continue
        try:
            ipaddress.ip_address(host)
            if host not in ips:
                ips.append(host)
        except ValueError:
            if host not in dns:
                dns.append(host)
    for ip in _local_ipv4_addresses():
        if ip not in ips:
            ips.append(ip)
    return dns, ips


def _cert_sans(cert: object) -> tuple[str, ...]:
    try:
        from cryptography import x509

        ext = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        values: list[str] = []
        for entry in ext:
            if isinstance(entry, x509.DNSName):
                values.append(str(entry.value))
            elif isinstance(entry, x509.IPAddress):
                values.append(str(entry.value))
        return tuple(values)
    except Exception:
        return ()


def _load_cert_info(cert_path: Path, key_path: Path) -> CertInfo:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes

    cert = x509.load_pem_x509_certificate(cert_path.read_bytes())
    fp = cert.fingerprint(hashes.SHA256()).hex()
    not_after_dt = getattr(cert, "not_valid_after_utc", None)
    if not_after_dt is None:
        from datetime import timezone
        not_after_dt = cert.not_valid_after.replace(tzinfo=timezone.utc)
    return CertInfo(
        cert_path=cert_path,
        key_path=key_path,
        fingerprint_sha256=fp,
        fingerprint_display=format_fingerprint(fp),
        not_after=not_after_dt.timestamp(),
        sans=_cert_sans(cert),
    )


def ensure_cert(
    cert_dir: str | Path = DEFAULT_CERT_DIR,
    *,
    extra_hosts: Iterable[str] = (),
    force: bool = False,
) -> CertInfo:
    """Create/reuse the self-signed certificate used by the device listener."""
    directory = Path(cert_dir)
    directory.mkdir(parents=True, exist_ok=True)
    cert_path = directory / CERT_NAME
    key_path = directory / KEY_NAME

    if not force and cert_path.exists() and key_path.exists():
        try:
            info = _load_cert_info(cert_path, key_path)
            if info.not_after > time.time() + 7 * 86400:
                return info
        except Exception:
            pass

    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
    except Exception as exc:  # pragma: no cover - dependency is declared
        raise TlsError("生成 TLS 证书需要 cryptography：pip install cryptography") from exc

    dns_names, ip_names = certificate_hosts(extra_hosts)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "Pager Bridge"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Pager Local"),
    ])
    now = time.time()
    try:
        not_before = now - 86400
        not_after = now + 397 * 86400
        san_entries: list[x509.GeneralName] = []
        for name in dns_names:
            try:
                san_entries.append(x509.DNSName(name))
            except ValueError:
                pass
        for ip in ip_names:
            try:
                san_entries.append(x509.IPAddress(ipaddress.ip_address(ip)))
            except ValueError:
                pass
        cert = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(_utc(not_before))
            .not_valid_after(_utc(not_after))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(
                x509.KeyUsage(
                    digital_signature=True,
                    content_commitment=False,
                    key_encipherment=True,
                    data_encipherment=False,
                    key_agreement=False,
                    key_cert_sign=False,
                    crl_sign=False,
                    encipher_only=False,
                    decipher_only=False,
                ),
                critical=True,
            )
            .add_extension(
                x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
                critical=False,
            )
            .add_extension(x509.SubjectAlternativeName(san_entries), critical=False)
            .sign(key, hashes.SHA256())
        )
    except Exception as exc:
        raise TlsError(f"生成 TLS 证书失败：{exc}") from exc

    key_bytes = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.TraditionalOpenSSL,
        encryption_algorithm=serialization.NoEncryption(),
    )
    cert_bytes = cert.public_bytes(serialization.Encoding.PEM)
    tmp_key = key_path.with_name(f".{key_path.name}.{os.getpid()}.tmp")
    tmp_cert = cert_path.with_name(f".{cert_path.name}.{os.getpid()}.tmp")
    tmp_key.write_bytes(key_bytes)
    tmp_cert.write_bytes(cert_bytes)
    try:
        os.chmod(tmp_key, 0o600)
    except OSError:
        pass
    os.replace(tmp_key, key_path)
    os.replace(tmp_cert, cert_path)
    return _load_cert_info(cert_path, key_path)


def _utc(timestamp: float):
    from datetime import datetime, timezone

    return datetime.fromtimestamp(timestamp, tz=timezone.utc)
