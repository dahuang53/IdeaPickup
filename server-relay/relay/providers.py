"""Immutable provider configuration and HTTPS requests pinned to public DNS answers."""
from __future__ import annotations

import hashlib
import http.client
import io
import ipaddress
import json
import socket
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request

from app import RelayError, RelayService
from config_broker.core import DEFAULTS, validate_config


def target_id(config):
    """An opaque destination identifier, not a secret or proof of account identity."""
    value = config["siyuan_url"] + "\0" + hashlib.sha256(config["siyuan_token"].encode()).hexdigest()
    return hashlib.sha256(value.encode()).hexdigest()


class _Response:
    def __init__(self, status, body):
        self.status = status
        self._body = io.BytesIO(body)

    def read(self, limit=-1):
        return self._body.read(limit)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self._body.close()


class PublicHTTPS:
    """Never uses environment proxies, follows redirects or re-resolves after validation."""
    def __init__(self, *, resolver=None, context=None):
        self.resolver = resolver or socket.getaddrinfo
        self.context = context or ssl.create_default_context()
        if not self.context.check_hostname or self.context.verify_mode != ssl.CERT_REQUIRED:
            raise ValueError("provider TLS verification cannot be disabled")

    def __call__(self, request, timeout=30):
        parts = urllib.parse.urlsplit(request.full_url)
        allowed = {DEFAULTS["qwen_url"], DEFAULTS["siyuan_url"]}
        normalized = urllib.parse.urlunsplit((parts.scheme, parts.hostname or "", parts.path, "", ""))
        if (parts.scheme != "https" or parts.username is not None or parts.password is not None or parts.query or parts.fragment or
                parts.port not in {None, 443} or normalized not in allowed):
            raise RelayError(400, "unsupported_endpoint", "only supported HTTPS provider endpoints are allowed")
        try:
            addresses = self.resolver(parts.hostname, 443, type=socket.SOCK_STREAM)
        except OSError as error:
            raise RelayError(502, "provider_dns", "provider DNS lookup failed") from error
        if not addresses or any(not ipaddress.ip_address(row[4][0]).is_global for row in addresses):
            raise RelayError(502, "provider_dns", "provider DNS must resolve exclusively to public addresses")
        last_error = None
        deadline = time.monotonic() + timeout
        for family, kind, protocol, _, address in addresses:
            connection = http.client.HTTPSConnection(parts.hostname, 443, timeout=timeout, context=self.context)
            raw = None
            try:
                raw = socket.socket(family, kind, protocol)
                raw.settimeout(timeout)
                raw.connect(address)
                connection.sock = self.context.wrap_socket(raw, server_hostname=parts.hostname)
                secured = connection.sock
                raw = None
                connection.request(request.get_method(), parts.path, body=request.data, headers=dict(request.header_items()))
                response = connection.getresponse()
                collected = bytearray()
                while not response.isclosed() and len(collected) <= 2 * 1024 * 1024:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("provider response deadline exceeded")
                    secured.settimeout(remaining)
                    part = response.read1(min(65536, 2 * 1024 * 1024 + 1 - len(collected)))
                    if not part:
                        break
                    collected.extend(part)
                # read1 closes the response file once a fixed-length body is
                # consumed. A Connection: close socket may then already be
                # closed too; do not set its timeout again. Premature EOF is
                # not a complete response, even when the file is now closed.
                if response.length is not None and response.length > 0 and response.isclosed():
                    raise http.client.IncompleteRead(bytes(collected), response.length)
                body = bytes(collected)
                if len(body) > 2 * 1024 * 1024:
                    raise RelayError(502, "provider_response_too_large", "provider response exceeds limit")
                status = response.status
                if not 200 <= status < 300:
                    # RelayService preserves the provider-specific distinction
                    # between explicit rejection and an uncertain delivery.
                    raise urllib.error.HTTPError(request.full_url, status, "provider rejected request", {}, io.BytesIO(body))
                return _Response(status, body)
            except urllib.error.HTTPError:
                raise
            except (OSError, http.client.HTTPException) as error:
                # A request may already have reached the provider. Do not try a
                # second IP and risk delivering the same text twice.
                last_error = error
                break
            finally:
                if raw is not None:
                    raw.close()
                connection.close()
        raise urllib.error.URLError(type(last_error).__name__ if last_error else "connection_failed")


class ConfiguredService(RelayService):
    """Each instance owns one durable task's immutable configuration snapshot."""
    def __init__(self, store, config, *, progress=None, transport=None):
        config = validate_config(dict(config))
        if not config["qwen_api_key"] or not config["siyuan_token"]:
            raise RelayError(503, "not_configured", "ASR and delivery credentials are required")
        super().__init__(store, config["qwen_api_key"], config["siyuan_token"],
                         qwen_url=config["qwen_url"], siyuan_url=config["siyuan_url"], model=config["qwen_model"],
                         http_open=transport or PublicHTTPS(), progress=progress)

    def _call_qwen(self, data, duration):
        try:
            return super()._call_qwen(data, duration)
        except RelayError as error:
            message = "empty" if error.code == "transcript_rejected" and str(error) == "empty" else error.code
            raise RelayError(error.status, error.code, message) from None

    def _send_siyuan(self, title, content):
        try:
            return super()._send_siyuan(title, content)
        except RelayError as error:
            if error.code == "provider_response_too_large":
                # PublicHTTPS has already sent the POST before bounding its
                # response. The provider may have accepted this delivery.
                raise RelayError(503, "siyuan_uncertain", "provider_response_too_large") from None
            raise RelayError(error.status, error.code, error.code) from None


def probe_config(config, provider, mode="validate", allow_side_effect=False, *, transport=None):
    if provider not in {"qwen", "siyuan", "all"} or mode not in {"validate", "tls", "end_to_end"}:
        raise RelayError(400, "invalid_probe", "unsupported provider or probe mode")
    if mode == "end_to_end" and allow_side_effect is not True:
        raise RelayError(400, "probe_confirmation_required", "explicit synthetic ASR or test-entry confirmation is required")
    config = validate_config(dict(config))
    providers = ("qwen", "siyuan") if provider == "all" else (provider,)
    results = {}
    transport = transport or PublicHTTPS()
    for name in providers:
        configured = bool(config["qwen_api_key" if name == "qwen" else "siyuan_token"])
        result = {"validation": "pass" if configured else "missing_credential", "tls": "not_checked",
                  "auth": "not_checked", "delivery": "not_checked", "end_to_end": "not_checked"}
        results[name] = result
        if mode == "validate":
            continue
        if mode == "tls":
            try:
                with transport(urllib.request.Request(config[name + "_url"], method="HEAD"), timeout=15):
                    result["tls"] = "pass"
            except urllib.error.HTTPError as error:
                error.close()
                result["tls"] = "pass"  # HTTP rejection still establishes a validated TLS connection.
            except Exception as error:
                result["tls"] = "fail"
                result["reason"] = getattr(error, "code", "transport_failed")
            continue
        if not configured:
            continue
        # No mutable store or task state is involved in the explicitly requested
        # fixed-content probe. The caller has pinned the selected revision.
        service = RelayService(None, config["qwen_api_key"], config["siyuan_token"],
                               qwen_url=config["qwen_url"], siyuan_url=config["siyuan_url"],
                               model=config["qwen_model"], http_open=transport)
        try:
            if name == "qwen":
                import wave
                output = io.BytesIO()
                with wave.open(output, "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(16000)
                    wav.writeframes(b"\0\0" * 16000)
                try:
                    service._call_qwen(output.getvalue(), 1000)
                except RelayError as error:
                    if not (error.code == "transcript_rejected" and str(error) == "empty"):
                        raise
                result["auth"], result["end_to_end"] = "pass", "pass_synthetic_silence"
            else:
                service._send_siyuan("Voice Memo deployment test", "Fixed synthetic connectivity test. No user audio or transcript is included.")
                result["auth"], result["delivery"], result["end_to_end"] = "pass", "pass_test_entry_created", "pass"
            result["tls"] = "pass"
        except RelayError as error:
            result["end_to_end"] = "fail"
            result["reason"] = error.code
            if error.code.endswith("uncertain"):
                result["delivery"] = "uncertain_do_not_repeat_automatically"
    return {"mode": mode, "providers": results, "synthetic_side_effect_requested": mode == "end_to_end"}
