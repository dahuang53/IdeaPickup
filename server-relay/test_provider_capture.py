"""Actual local TLS capture; only the validated public dial address is mapped.

No public network, real credentials, system trust changes or provider claims.
HTTP framing, certificate/SNI verification and PublicHTTPS are the real code.
"""
import contextlib
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import urllib.request

from app import RelayError
from config_broker.core import DEFAULTS
from relay.providers import PublicHTTPS
import test_runtime_http as runtime_fixture


class ProviderCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1] / "private" / "provider-capture"
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=root)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        def openssl(*args):
            subprocess.run(["openssl", *args], cwd=cls.directory, check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj",
                "/CN=Isolated Provider Capture CA", "-keyout", "ca.key", "-out", "ca.pem")
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=synthetic-provider",
                "-keyout", "server.key", "-out", "server.csr")
        for filename, names in (("server.pem", "DNS:dashscope.aliyuncs.com,DNS:ld246.com"),
                                ("wrong.pem", "DNS:wrong.invalid")):
            (cls.directory / "extensions").write_text("subjectAltName=" + names + "\nextendedKeyUsage=serverAuth\n")
            openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                    "-CAcreateserial", "-days", "1", "-extfile", "extensions", "-out", filename)
        for path in cls.directory.iterdir():
            path.chmod(0o600)

    def setUp(self):
        self.records, self.sni, self.dials, self.lookups = [], [], [], []
        self.code, self.location, self.drop = 200, None, False
        self.http_version, self.close_response = "HTTP/1.0", False
        self.response_body, self.declared_length, self.send_length = b"{}", None, True
        self.context = ssl.create_default_context(cafile=str(self.directory / "ca.pem"))
        self.real_socket = socket.socket

    @contextlib.contextmanager
    def capture(self, certificate="server.pem"):
        owner = self
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = owner.http_version
            def log_message(self, *_):
                pass
            def do_POST(self):
                data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                owner.records.append({"path": self.path, "host": self.headers.get("Host"),
                                      "authorization": self.headers.get("Authorization"), "body": data})
                if owner.drop:
                    self.close_connection = True
                    return
                self.send_response(owner.code)
                if owner.location:
                    self.send_header("Location", owner.location)
                if owner.send_length:
                    self.send_header("Content-Length", str(len(owner.response_body) if owner.declared_length is None else owner.declared_length))
                if owner.close_response:
                    self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(owner.response_body)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.directory / certificate, self.directory / "server.key")
        context.set_servername_callback(lambda sock, name, ctx: self.sni.append(name))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield server.server_address
        finally:
            server.shutdown(); server.server_close(); worker.join(5)
            self.assertFalse(worker.is_alive())

    def resolve(self, hostname, port, **kwargs):
        self.lookups.append((hostname, port, kwargs))
        # No real lookup. These public answers are never dialled on a network.
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (ip, 443))
                for ip in ("93.184.216.34", "93.184.216.35")]

    @contextlib.contextmanager
    def map_dial(self, address):
        owner = self
        class LocalCaptureSocket(self.real_socket):
            def connect(self, target):
                owner.dials.append(target)
                if target not in {("93.184.216.34", 443), ("93.184.216.35", 443)}:
                    raise AssertionError("test attempted an unapproved network dial")
                return super().connect(address)
        with mock.patch("relay.providers.socket.socket", LocalCaptureSocket):
            yield

    def request(self, provider="qwen"):
        return urllib.request.Request(DEFAULTS[provider + "_url"], data=b"synthetic-capture-body",
                                      headers={"Authorization": "Bearer synthetic-capture-only"})

    def test_actual_tls_sni_http_and_key_ignore_environment_proxy(self):
        for version, closing in (("HTTP/1.0", False), ("HTTP/1.1", True), ("HTTP/1.1", False)):
            self.http_version, self.close_response = version, closing
            for provider, hostname in (("qwen", "dashscope.aliyuncs.com"), ("siyuan", "ld246.com")):
                with self.subTest(provider=provider, version=version, closing=closing), self.capture() as address, self.map_dial(address), \
                        mock.patch.dict(os.environ, {"HTTPS_PROXY": "http://127.0.0.1:1", "https_proxy": "http://127.0.0.1:1"}):
                    response = PublicHTTPS(resolver=self.resolve, context=self.context)(self.request(provider), timeout=3)
                    self.assertEqual(response.read(), b"{}")
                    self.assertEqual(self.records[-1]["authorization"], "Bearer synthetic-capture-only")
                    self.assertEqual(self.records[-1]["body"], b"synthetic-capture-body")
                    self.assertEqual(self.records[-1]["host"], hostname)
                    self.assertEqual(self.sni[-1], hostname)
        self.assertEqual(len(self.lookups), 6)
        self.assertEqual(self.dials, [("93.184.216.34", 443)] * 6)

    def test_premature_content_length_is_rejected_and_eof_delimited_body_is_valid(self):
        self.declared_length = 100
        with self.capture() as address, self.map_dial(address):
            with self.assertRaises(urllib.error.URLError) as caught:
                PublicHTTPS(resolver=self.resolve, context=self.context)(self.request(), timeout=3)
            self.assertEqual(caught.exception.reason, "IncompleteRead")
        self.send_length = False
        with self.capture() as address, self.map_dial(address):
            response = PublicHTTPS(resolver=self.resolve, context=self.context)(self.request(), timeout=3)
            self.assertEqual(response.read(), b"{}")
        self.assertEqual(len(self.records), 2)
        self.assertEqual(self.dials, [("93.184.216.34", 443)] * 2)

    def test_response_limit_is_still_enforced(self):
        self.response_body = b"x" * (2 * 1024 * 1024 + 1)
        with self.capture() as address, self.map_dial(address):
            with self.assertRaises(RelayError) as caught:
                PublicHTTPS(resolver=self.resolve, context=self.context)(self.request(), timeout=3)
            self.assertEqual(caught.exception.code, "provider_response_too_large")
        self.assertEqual(len(self.records), 1)

    def test_oversized_delivery_ack_blocks_retry_until_reconciliation(self):
        fixture = runtime_fixture.RuntimeHTTPTests()
        fixture.setUp()
        try:
            fixture.config()
            device = fixture.pair()
            original_factory = fixture.runtime._provider_factory
            def factory(store, config, progress=None):
                service = original_factory(store, config, progress=progress)
                service._http_open = PublicHTTPS(resolver=self.resolve, context=self.context)
                service._siyuan_sender = service._send_siyuan
                return service
            fixture.runtime._provider_factory = factory
            self.response_body = json.dumps({'code': 0, 'padding': 'x' * (2 * 1024 * 1024)}).encode()
            with self.capture() as address, self.map_dial(address):
                self.assertEqual(fixture.wav(device).status_code, 202)
                self.assertTrue(fixture.runtime.run_pending_once())
                item = fixture.runtime.list_jobs()['items'][0]
                self.assertEqual(item['status'], 'uncertain')
                self.assertFalse(item['can_retry'])
                self.assertEqual(len(self.records), 1)
                path = '/admin/api/jobs/' + device['device_id'] + '/1'
                self.assertEqual(fixture.client.post(path + '/retry', json={}, headers=fixture.admin_headers).status_code, 409)
                self.assertFalse(fixture.runtime.run_pending_once())
                fixture.restart()
                self.assertEqual(fixture.client.post(path + '/retry', json={}, headers=fixture.admin_headers).status_code, 409)
                self.assertFalse(fixture.runtime.run_pending_once())
                self.assertEqual(len(self.records), 1)
                self.assertEqual(fixture.client.post(path + '/reconcile', json={'present': True}, headers=fixture.admin_headers).status_code, 200)
                self.assertEqual(fixture.status(device).json()['status'], 'sent')
                self.assertIsNone(fixture.runtime.store.get(device['device_id'], 1)['cloud_content'])
                self.assertEqual(len(self.records), 1)
        finally:
            fixture.doCleanups()

    def test_pre_request_dns_rejection_is_not_uncertain_delivery(self):
        fixture = runtime_fixture.RuntimeHTTPTests()
        fixture.setUp()
        try:
            fixture.config()
            device = fixture.pair()
            original_factory = fixture.runtime._provider_factory
            def factory(store, config, progress=None):
                service = original_factory(store, config, progress=progress)
                service._http_open = PublicHTTPS(resolver=lambda *a, **k: [], context=self.context)
                service._siyuan_sender = service._send_siyuan
                return service
            fixture.runtime._provider_factory = factory
            self.assertEqual(fixture.wav(device).status_code, 202)
            self.assertTrue(fixture.runtime.run_pending_once())
            item = fixture.runtime.list_jobs()['items'][0]
            self.assertEqual(item['status'], 'delivery_failed')
            self.assertEqual(item['reason'], 'provider_dns')
            self.assertTrue(item['can_retry'])
            self.assertEqual(self.records, [])
        finally:
            fixture.doCleanups()

    def test_redirects_do_not_send_key_to_another_request(self):
        for code in (301, 302, 303, 307, 308):
            for destination in (DEFAULTS["qwen_url"], "https://wrong.invalid/steal"):
                with self.subTest(code=code, destination=destination), self.capture() as address, self.map_dial(address):
                    self.code, self.location = code, destination
                    before = len(self.records), len(self.lookups), len(self.dials)
                    with self.assertRaises(urllib.error.HTTPError) as caught:
                        PublicHTTPS(resolver=self.resolve, context=self.context)(self.request(), timeout=3)
                    self.assertEqual(caught.exception.code, code); caught.exception.close()
                    self.assertEqual((len(self.records), len(self.lookups), len(self.dials)), tuple(x + 1 for x in before))
                    self.assertEqual(self.records[-1]["authorization"], "Bearer synthetic-capture-only")

    def test_uncertain_response_does_not_retry_second_ip_or_resolve_again(self):
        self.drop = True
        with self.capture() as address, self.map_dial(address):
            with self.assertRaises(urllib.error.URLError):
                PublicHTTPS(resolver=self.resolve, context=self.context)(self.request(), timeout=3)
        self.assertEqual(len(self.records), 1)
        self.assertEqual(len(self.lookups), 1)
        self.assertEqual(self.dials, [("93.184.216.34", 443)])

    def test_untrusted_ca_or_wrong_san_never_receives_key(self):
        for certificate, context in (("server.pem", ssl.create_default_context()), ("wrong.pem", self.context)):
            with self.subTest(certificate=certificate), self.capture(certificate) as address, self.map_dial(address):
                with self.assertRaises(urllib.error.URLError) as caught:
                    PublicHTTPS(resolver=self.resolve, context=context)(self.request(), timeout=3)
                self.assertEqual(caught.exception.reason, "SSLCertVerificationError")
                self.assertEqual(self.records, [])

    def test_private_or_mixed_dns_is_rejected_before_any_dial(self):
        for addresses in (("127.0.0.1",), ("93.184.216.34", "10.0.0.1")):
            def resolver(*args, **kwargs):
                return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (ip, 443)) for ip in addresses]
            with self.subTest(addresses=addresses), self.map_dial(("127.0.0.1", 1)):
                with self.assertRaises(RelayError) as caught:
                    PublicHTTPS(resolver=resolver, context=self.context)(self.request(), timeout=3)
                self.assertEqual(caught.exception.code, "provider_dns")
                self.assertEqual(self.dials, [])


if __name__ == "__main__":
    unittest.main()
