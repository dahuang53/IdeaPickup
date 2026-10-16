"""Isolated HTTPS UI fixture, adapted from R02's private browser fixture.

Only this test process substitutes provider I/O and broker ownership. No test
routes, TLS bypasses or fixture switches are added to the production application.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server-relay"))
import uvicorn
from app import RelayError
from relay import runtime as runtime_module
from relay.providers import ConfiguredService, probe_config, _Response
from relay.runtime import Runtime
from relay.web import create_app
from test_support import LocalBroker


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--lan-ip", help="test-only RFC1918 local interface; serve actual production HTTP boundary")
    args = parser.parse_args()
    host=args.lan_ip or "127.0.0.1"
    if args.lan_ip:
        ip=ipaddress.IPv4Address(args.lan_ip)
        assert any(ip in ipaddress.ip_network(net) for net in ("10.0.0.0/8","172.16.0.0/12","192.168.0.0/16"))
    output = args.output.resolve()
    output.relative_to((ROOT / "private").resolve())
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.umask(0o077)
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(output / "key.pem"), "-out", str(output / "cert.pem"),
                    "-days", "1", "-subj", "/CN=127.0.0.1",
                    "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True)
    (output / "secrets").mkdir(mode=0o700)
    control = output / "control.json"
    control.write_text(json.dumps({"delivery": "ok", "probe": "ok"}))
    calls = []

    def mode(name):
        return json.loads(control.read_text()).get(name, "ok")

    def record(provider, method):
        calls.append({"provider": provider, "method": method})
        (output / "provider-calls.json").write_text(json.dumps(calls))

    def transport(request, timeout=15):
        name = "qwen" if "dashscope" in request.full_url else "siyuan"
        record(name, request.get_method())
        if mode("probe") == "fail":
            raise urllib.error.URLError("synthetic TLS failure")
        if request.get_method() == "HEAD":
            return _Response(200, b"")
        if name == "qwen":
            # Exercise the actual provider JSON parser and synthetic-silence path.
            body = {"output": {"choices": [{"message": {"content": [{"text": ""}]}}]}}
        else:
            body = {"code": 0}
        return _Response(200, json.dumps(body).encode())

    runtime_module.probe_config = lambda *a, **kw: probe_config(*a, **kw, transport=transport)

    def factory(store, config, progress=None):
        service = ConfiguredService(store, config, progress=progress)

        def asr(_data, _duration):
            record("task-qwen", "POST")
            return "合成测试正文，仅用于隔离浏览器验收", "synthetic"

        def delivery(_title, _content):
            record("task-siyuan", "POST")
            value = mode("delivery")
            if value == "fail":
                raise RelayError(502, "siyuan_rejected", "synthetic rejection")
            if value == "uncertain":
                raise RelayError(503, "siyuan_uncertain", "synthetic lost acknowledgement")

        service._qwen_transcriber, service._siyuan_sender = asr, delivery
        return service

    broker = LocalBroker(output / "secrets")
    runtime = Runtime(output / "state.db", broker, output / "work", output / "streams", provider_factory=factory)
    with socket.socket() as probe:
        probe.bind((host, 0))
        port = probe.getsockname()[1]
    url = f"{'http' if args.lan_ip else 'https'}://{host}:{port}/memo"
    app = create_app(output / "state.db", public_url=url, broker=broker, runtime=runtime)
    code = app.state.admins.issue_bootstrap()

    @app.middleware("http")
    async def receipt(request, call_next):
        response = await call_next(request)
        with (output / "http.jsonl").open("a") as log:
            log.write(json.dumps({"method": request.method, "path": request.url.path, "status": response.status_code}) + "\n")
        return response

    (output / "fixture.json").write_text(json.dumps({"url": url + "/admin/", "code": code, "password": "synthetic-password-123"}))
    try:
        uvicorn.run(app, host=host, port=port, ssl_keyfile=None if args.lan_ip else str(output / "key.pem"),
                    ssl_certfile=None if args.lan_ip else str(output / "cert.pem"), access_log=False, log_level="warning", proxy_headers=False)
    finally:
        runtime.close()
        broker.close()


if __name__ == "__main__":
    main()
