"""Production cloud protocol over real local TLS/WS; ESP/RF are not emulated PASS.

Only IO is replaced. Its destination rewrite and private CA live in this test,
never in the firmware. All keys/audio/text below are synthetic.
"""
import base64
import ctypes as C
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
OK, EMPTY, INVALID, SOURCE, CANCELLED, TIMEOUT, TRANSPORT, TLS, CREDENTIALS, LIMITED, REJECTED, RESPONSE, TOO_LARGE, MEMORY, UNCERTAIN, BUSY = range(16)
class Result(C.Structure):
    _fields_ = [('code', C.c_int), ('status', C.c_int), ('retry', C.c_uint32), ('sent', C.c_bool), ('provider', C.c_char*48)]
PTR = C.c_void_p
NOW = C.CFUNCTYPE(C.c_int64, PTR)
BOOL = C.CFUNCTYPE(C.c_bool, PTR)
VOID = C.CFUNCTYPE(None, PTR)
OPEN = C.CFUNCTYPE(C.c_int, PTR, C.c_char_p, C.c_char_p, C.c_size_t, C.c_int64)
WRITE = C.CFUNCTYPE(C.c_int, PTR, PTR, C.c_size_t, C.c_int)
HEADERS = C.CFUNCTYPE(C.c_int, PTR, C.POINTER(C.c_uint32), C.c_int)
WSOPEN = C.CFUNCTYPE(C.c_int, PTR, C.c_char_p, C.c_char_p, C.c_int64)
FAIL = C.CFUNCTYPE(C.c_int, PTR)
READ = C.CFUNCTYPE(C.c_int, PTR, C.c_uint64, PTR, C.c_size_t)
APPEND = C.CFUNCTYPE(C.c_bool, PTR, PTR, C.c_size_t)
class IO(C.Structure):
    _fields_ = [('ctx', PTR), ('now', NOW), ('cancelled', BOOL), ('acquire', BOOL), ('release', VOID),
                ('http_open', OPEN), ('http_write', WRITE), ('http_headers', HEADERS), ('http_read', WRITE),
                ('http_complete', BOOL), ('http_sent', BOOL), ('http_close', VOID),
                ('ws_open', WSOPEN), ('ws_send', WRITE), ('ws_read', WRITE), ('ws_close', VOID), ('failure', FAIL)]
class Source(C.Structure):
    _fields_ = [('ctx', PTR), ('size', C.c_uint64), ('read', READ)]
class Sink(C.Structure):
    _fields_ = [('ctx', PTR), ('append', APPEND)]

def exact(stream, size):
    data = bytearray()
    while len(data) < size:
        chunk = stream.read(size-len(data))
        if not chunk:
            raise EOFError('incomplete frame')
        data.extend(chunk)
    return bytes(data)

def frame(data, *, opcode=1, fin=True, masked=False):
    header = bytes([(0x80 if fin else 0) | opcode])
    n = len(data)
    header += bytes([(0x80 if masked else 0) | (n if n < 126 else 126 if n < 65536 else 127)])
    if n >= 126:
        header += struct.pack('!H' if n < 65536 else '!Q', n)
    if masked:
        mask = b'abcd'
        header += mask
        data = bytes(v ^ mask[i % 4] for i, v in enumerate(data))
    return header + data

def read_frame(stream):
    first, second = exact(stream, 2)
    n = second & 127
    if n == 126:
        n = struct.unpack('!H', exact(stream, 2))[0]
    elif n == 127:
        n = struct.unpack('!Q', exact(stream, 8))[0]
    if n > 100000:
        raise ValueError('test frame too large')
    mask = exact(stream, 4) if second & 128 else None
    data = exact(stream, n)
    if mask:
        data = bytes(v ^ mask[i % 4] for i, v in enumerate(data))
    return first, data

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    def handle_error(self, *args):
        pass  # Expected disconnect/certificate/timeout faults.

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args):
        pass
    def do_POST(self):
        peer = self.server
        body = exact(self.rfile, int(self.headers['Content-Length']))
        peer.requests.append((self.path, dict(self.headers), body))
        mode = peer.mode
        if mode == 'lost':
            self.close_connection = True
            return
        status = int(mode[4:]) if mode.startswith('http') else 400 if mode.startswith('err_') else 200
        if self.path.endswith('chat/completions'):
            value = {'choices': [{'finish_reason': 'stop', 'message': {'content': '合成转写😀'}}]}
        else:
            value = {'code': 0}
        if mode == 'empty':
            value['choices'][0]['message']['content'] = ' \n\t'
        if mode == 'content_missing':
            del value['choices'][0]['message']['content']
        if mode == 'finish_length':
            value['choices'][0]['finish_reason'] = 'length'
        if mode == 'business':
            value = {'code': 12}
        if mode == 'code_string':
            value = {'code': '0'}
        if mode.startswith('err_'):
            value = {'code': {'err_short': 'InvalidParameter.Audio', 'err_long': 'a'*48, 'err_wrongtype': 123, 'err_secret': 'synthetic-key'}.get(mode, None)}
        data = json.dumps(value, ensure_ascii=False).encode()
        if mode == 'err_badjson':
            data = b'{bad'
        if mode == 'leadingzero':
            data = b'{"code":00}'
        if mode == 'trailingdot':
            data = b'{"code":0.}'
        if mode == 'rawcontrol':
            data = b'{"code":0,"text":"bad\nline"}'
        if mode == 'invalid':
            data = b'{"code":0} trailing'
        if mode == 'duplicate':
            data = b'{"code":0,"code":9}'
        if mode == 'nul':
            data = b'{"choices":[{"finish_reason":"stop","message":{"content":"a\\u0000b"}}]}'
        if mode == 'oversize':
            data = b' ' * 65537
        if mode == 'nodes':
            data = b'{"code":0,"junk":[' + b'0,' * 12000 + b'0]}'
        self.send_response(status)
        self.send_header('Content-Length', str(len(data) + (10 if mode == 'truncated' else 0)))
        self.send_header('Retry-After', '301')
        self.send_header('Connection', 'close')
        self.end_headers()
        # Writes deliberately split UTF-8 and JSON tokens.
        for i in range(0, len(data), 127):
            self.wfile.write(data[i:i+127])
        self.close_connection = True
    def do_GET(self):
        peer = self.server
        accept = base64.b64encode(hashlib.sha1((self.headers['Sec-WebSocket-Key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        peer.ws_headers.append(dict(self.headers))
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        def send(value, fragmented=False):
            data = json.dumps(value, ensure_ascii=False).encode()
            if fragmented:
                self.wfile.write(frame(data[:len(data)//2], fin=False) + frame(data[len(data)//2:], opcode=0))
            else:
                self.wfile.write(frame(data))
            self.wfile.flush()
        if peer.mode in ('ws_credentials', 'ws_limited'):
            send({'type': 'error', 'error': {'code': 'invalid_api_key' if peer.mode == 'ws_credentials' else 'rate_limit_exceeded'}})
            return
        if peer.mode == 'ws_order':
            send({'type': 'session.updated'})
            return
        if peer.mode == 'ws_utf8':
            self.wfile.write(frame(b'{"type":"\xff"}'));self.wfile.flush()
            return
        if peer.mode == 'ws_oversize':
            self.wfile.write(frame(b' '*65537));self.wfile.flush();return
        send({'type': 'session.created'}, True)
        _, data = read_frame(self.rfile)
        peer.ws_messages.append(json.loads(data))
        send({'type': 'session.updated'})
        while True:
            _, data = read_frame(self.rfile)
            value = json.loads(data)
            peer.ws_messages.append(value)
            if value['type'] == 'session.finish':
                break
        if peer.mode == 'ws_timeout':
            time.sleep(1)
            return
        send({'type': 'input_audio_buffer.committed', 'item_id': 'one', 'previous_item_id': None})
        if peer.mode == 'ws_missing':
            send({'type': 'session.finished'})
            return
        send({'type': 'conversation.item.input_audio_transcription.text', 'item_id': 'one', 'text': '旧草稿', 'stash': '不会投递'})
        # Final #2 arrives before final #1; duplicate final #1 is ignored.
        send({'type': 'input_audio_buffer.committed', 'item_id': 'two', 'previous_item_id': 'one'})
        send({'type': 'conversation.item.input_audio_transcription.completed', 'item_id': 'two', 'transcript': '世界😀'}, True)
        final = {'type': 'conversation.item.input_audio_transcription.completed', 'item_id': 'one', 'transcript': '你好'}
        send(final);send(final)
        send({'type': 'session.finished'})

class Platform:
    def __init__(self, test, *, trusted=True):
        self.test, self.peer, self.trusted = test, test.server, trusted
        self.lease = self.cancel = self.sent = False
        self.closes = self.acquires = self.opens = 0
        self.connection = self.response = self.ws = self.stream = None
        self.error = TRANSPORT
        self.clock_shift = 0
        self.max_source = self.max_write = 0
        self.output = bytearray()
        self.refs = []
        def bind(kind, method):
            def safe(*args):
                try:
                    return method(*args)
                except ssl.SSLError:
                    self.error = TLS;return -1
                except (OSError, EOFError, http.client.HTTPException, ValueError):
                    self.error = TRANSPORT;return -1
            cb = kind(safe);self.refs.append(cb);return cb
        self.io = IO(None, NOW(lambda _: int(time.monotonic()*1000)+self.clock_shift), BOOL(lambda _: self.cancel),
            BOOL(self.acquire), VOID(self.release), bind(OPEN, self.open), bind(WRITE, self.write), bind(HEADERS, self.headers),
            bind(WRITE, self.read), BOOL(lambda _: self.response is not None and self.response.length == 0),
            BOOL(lambda _: self.sent), VOID(self.close), bind(WSOPEN, self.ws_open), bind(WRITE, self.ws_send),
            bind(WRITE, self.ws_read), VOID(self.ws_close), FAIL(lambda _: self.error))
    def context(self):
        return ssl.create_default_context(cafile=str(self.test.directory/'cert.pem')) if self.trusted else ssl.create_default_context()
    def acquire(self, _):
        if self.lease:
            return False
        self.lease = True;self.acquires += 1;return True
    def release(self, _):
        self.lease = False
    def source(self, data, *, short=False):
        def read(_, offset, out, capacity):
            self.max_source = max(self.max_source, capacity)
            n = min(capacity, len(data)-offset, 31 if short else capacity)
            if n <= 0:
                return 0
            C.memmove(out, data[offset:offset+n], n)
            return n
        callback = READ(read);self.refs.append(callback)
        return Source(None, len(data), callback)
    def sink(self):
        def put(_, p, n):
            self.output.extend(C.string_at(p, n));return True
        callback = APPEND(put);self.refs.append(callback)
        return Sink(None, callback)
    def open(self, _, url, auth, length, deadline):
        self.opens += 1;self.sent = False
        if self.peer.mode == 'connect_fail':
            return -1
        self.connection = http.client.HTTPSConnection('localhost', self.peer.server_port, context=self.context(), timeout=2)
        self.connection.connect()
        path = url.decode().split('.com', 1)[-1] if b'.com' in url else '/apis/siyuan/inbox/addCloudShorthand'
        self.connection.putrequest('POST', path)
        self.connection.putheader('Authorization', auth.decode())
        self.connection.putheader('Content-Type', 'application/json')
        self.connection.putheader('Content-Length', str(length))
        self.sent = True;self.connection.endheaders()
        return 0
    def write(self, _, data, n, ms):
        n = min(n, 997);self.max_write = max(self.max_write, n)
        self.connection.send(C.string_at(data, n))
        if self.peer.mode == 'cancel':
            self.cancel = True
        return n
    def headers(self, _, retry, ms):
        self.response = self.connection.getresponse()
        retry[0] = int(self.response.getheader('Retry-After', '0'))
        return self.response.status
    def read(self, _, target, capacity, ms):
        data = self.response.read(min(capacity, 7))
        C.memmove(target, data, len(data))
        if self.peer.mode == 'success_cancel' and self.response.length == 0:
            self.cancel = True
        if self.peer.mode == 'timeout':
            self.clock_shift += 240001
        return len(data)
    def close(self, _):
        if self.response:
            self.response.close();self.response = None
        if self.connection:
            self.connection.close();self.connection = None
        self.closes += 1
    def ws_open(self, _, url, auth, deadline):
        self.opens += 1
        raw = socket.create_connection(('localhost', self.peer.server_port), timeout=2)
        try:
            self.ws = self.context().wrap_socket(raw, server_hostname='localhost')
        except Exception:
            raw.close();raise
        self.stream = self.ws.makefile('rb', buffering=0)
        self.ws.sendall(b'GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJjZGVmZ2hpamtsbW5vcA==\r\n'+auth+b'\r\n')
        status = self.stream.readline()
        if b'101' not in status:
            return -1
        while self.stream.readline() != b'\r\n':
            pass
        return 0
    def ws_send(self, _, data, n, ms):
        self.ws.sendall(frame(C.string_at(data, n), masked=True))
        if self.peer.mode == 'ws_timeout' and b'session.finish' in C.string_at(data, n):
            self.clock_shift += 30001
        return n
    def ws_read(self, _, target, capacity, ms):
        # select avoids leaving a timed-out buffered reader in an unusable state.
        import select
        if not self.ws.pending() and not select.select([self.ws], [], [], max(ms/1000, 0.01))[0]:
            return 0
        data = bytearray()
        while True:
            first, chunk = read_frame(self.stream)
            data.extend(chunk)
            if len(data) > capacity:
                self.error = TOO_LARGE;return -1
            if first & 128:
                break
        C.memmove(target, bytes(data), len(data));return len(data)
    def ws_close(self, _):
        if self.stream:
            self.stream.close();self.stream = None
        if self.ws:
            self.ws.close();self.ws = None
        self.closes += 1

class CloudTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT/'private/host-tests').mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='direct-cloud-', dir=ROOT/'private/host-tests')
        cls.directory = Path(cls.temp.name)
        sdk = ROOT/'.tools/esp-idf'
        include = sdk/'components/json/cJSON'
        subprocess.run(['cc', '-c', str(include/'cJSON.c'), '-I'+str(include), '-fPIC', '-o', str(cls.directory/'json.o')], check=True)
        subprocess.run(['cc','-fsanitize=address,undefined','-c',str(ROOT/'main/memo_cloud_json.c'),'-I'+str(include),'-o',str(cls.directory/'private-json-asan.o')],check=True)
        subprocess.run(['cc','-fPIC','-c',str(ROOT/'main/memo_cloud_json.c'),'-I'+str(include),'-o',str(cls.directory/'private-json.o')],check=True)
        sources = [str(ROOT/'main/memo_cloud.cpp'), str(ROOT/'tests/cloud_native.cpp'), str(cls.directory/'json.o')]
        common = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DMEMO_CLOUD_TEST','-I'+str(ROOT/'main'), '-I'+str(include)]
        subprocess.run([*common, '-fPIC', '-shared', *sources,str(cls.directory/'private-json.o'), '-o', str(cls.directory/'cloud.so')], check=True)
        subprocess.run([*common, '-DCLOUD_UNIT', '-fsanitize=address,undefined', *sources,str(cls.directory/'private-json-asan.o'), '-o', str(cls.directory/'unit')], check=True)
        subprocess.run([str(cls.directory/'unit')], check=True)
        cls.lib = C.CDLL(str(cls.directory/'cloud.so'));cls.lib.cloud_setup()
        cls.lib.cloud_file.argtypes = [C.POINTER(IO), C.c_char_p, C.c_char_p, C.POINTER(Source), C.POINTER(Sink)]
        cls.lib.cloud_deliver.argtypes = [C.POINTER(IO), C.c_char_p, C.c_char_p, C.POINTER(Source)]
        for name in ('cloud_file', 'cloud_deliver', 'cloud_rt_begin', 'cloud_rt_audio', 'cloud_rt_poll', 'cloud_rt_finish'):
            getattr(cls.lib, name).restype = Result
        cls.lib.cloud_rt_new.argtypes = [C.POINTER(IO)];cls.lib.cloud_rt_new.restype = PTR
        cls.lib.cloud_rt_begin.argtypes = [PTR, C.c_char_p, C.c_char_p, C.POINTER(Sink)]
        cls.lib.cloud_rt_audio.argtypes = [PTR, PTR, C.c_size_t]
        for name in ('cloud_rt_delete', 'cloud_rt_poll', 'cloud_rt_finish', 'cloud_rt_cancel'):
            getattr(cls.lib, name).argtypes = [PTR]
        cls.lib.cloud_live.restype = C.c_size_t;cls.lib.cloud_peak.restype = C.c_size_t
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost', '-keyout', str(cls.directory/'key.pem'), '-out', str(cls.directory/'cert.pem')], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.server = Server(('127.0.0.1', 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cls.directory/'cert.pem', cls.directory/'key.pem')
        cls.server.socket = ctx.wrap_socket(cls.server.socket, server_side=True)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True);cls.thread.start()
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown();cls.server.server_close();cls.thread.join();cls.temp.cleanup()
    def setUp(self):
        self.server.mode = 'ok';self.server.requests = [];self.server.ws_messages = [];self.server.ws_headers = []
    def tearDown(self):
        self.assertEqual(self.lib.cloud_live(), 0)
        # All protocol allocations now counted, including the fixed 192 KiB
        # parser arena plus response/WS buffers, not only cJSON node payloads.
        self.assertLessEqual(self.lib.cloud_peak(), 320*1024)
    def file(self, mode='ok', data=None, trusted=True, short=False):
        self.server.mode = mode;p = Platform(self, trusted=trusted)
        data = data if data is not None else b'RIFF'+bytes(range(46))
        source = p.source(data, short=short);sink = p.sink()
        result = self.lib.cloud_file(C.byref(p.io), b'llm-test', b'synthetic-key', C.byref(source), C.byref(sink))
        self.assertFalse(p.lease);self.assertEqual(p.opens, 1);self.assertEqual(p.closes, 1)
        return p, result
    def deliver(self, mode='ok', data='合成"\\\n😀'.encode(), trusted=True):
        self.server.mode = mode;p = Platform(self, trusted=trusted);source = p.source(data, short=True)
        result = self.lib.cloud_deliver(C.byref(p.io), b'synthetic-token', '标题'.encode(), C.byref(source))
        self.assertFalse(p.lease);self.assertEqual(p.opens, 1 if result.code != INVALID else 0)
        return p, result
    def realtime(self, mode='ok', trusted=True, cancel=False, key=b'synthetic-key'):
        self.server.mode = mode;p = Platform(self, trusted=trusted);sink = p.sink()
        rt = self.lib.cloud_rt_new(C.byref(p.io))
        try:
            result = self.lib.cloud_rt_begin(rt, b'llm-test', key, C.byref(sink))
            if result.code == OK:
                self.assertEqual(self.lib.cloud_rt_audio(rt, b'\0'*3200, 3200).code, OK)
                self.assertEqual(self.lib.cloud_rt_audio(rt, b'\1\0'*7, 14).code, OK)
                if cancel:
                    p.cancel = True;result = self.lib.cloud_rt_poll(rt)
                else:
                    result = self.lib.cloud_rt_finish(rt)
        finally:
            self.lib.cloud_rt_delete(rt)
        self.assertFalse(p.lease);self.assertEqual(p.closes, 1)
        return p, result
    def test_file_streaming_partial_writes_and_reads(self):
        data = b'RIFF'+bytes(range(256))*100
        p, r = self.file(data=data, short=True)
        self.assertEqual(r.code, OK);self.assertEqual(p.output.decode(), '合成转写😀')
        payload = json.loads(self.server.requests[-1][2]);audio = payload['messages'][0]['content'][0]['input_audio']['data']
        self.assertEqual(base64.b64decode(audio.split(',', 1)[1]), data)
        self.assertLessEqual(p.max_source, 3072);self.assertLessEqual(p.max_write, 4096)
    def test_maximum_segment_streams_without_full_request_buffer(self):
        data = b'R'*5760044
        p, r = self.file(data=data)
        self.assertEqual(r.code, OK);self.assertLessEqual(p.max_source, 3072)
        self.assertEqual(len(self.server.requests[-1][2]), int(self.server.requests[-1][1]['Content-Length']))
    def test_file_error_classification(self):
        cases = {'http401': CREDENTIALS, 'http403': CREDENTIALS, 'http429': LIMITED, 'http500': REJECTED,
                 'http302': REJECTED, 'empty': EMPTY, 'content_missing': RESPONSE, 'finish_length': RESPONSE,
                 'truncated': RESPONSE, 'invalid': RESPONSE, 'oversize': TOO_LARGE, 'nodes': RESPONSE, 'nul': RESPONSE,
                 'cancel': CANCELLED, 'timeout': TIMEOUT, 'leadingzero': RESPONSE, 'trailingdot': RESPONSE, 'rawcontrol': RESPONSE}
        for mode, expected in cases.items():
            with self.subTest(mode=mode):
                p, r = self.file(mode);self.assertEqual(r.code, expected)
                if mode == 'http429':
                    self.assertEqual(r.retry, 301)
    def test_delivery_escape_and_error_side_effect_boundary(self):
        text = ('合成"\\\n😀' * 1000).encode()
        p, r = self.deliver(data=text)
        self.assertEqual(r.code, OK);self.assertTrue(r.sent)
        self.assertEqual(json.loads(self.server.requests[-1][2])['content'], text.decode())
        for mode, expected in {'http401': CREDENTIALS, 'http429': LIMITED, 'http500': UNCERTAIN,
                               'lost': UNCERTAIN, 'truncated': UNCERTAIN, 'invalid': UNCERTAIN,
                               'business': REJECTED, 'code_string': UNCERTAIN, 'duplicate': UNCERTAIN,
                               'cancel': UNCERTAIN, 'connect_fail': TRANSPORT, 'leadingzero': UNCERTAIN, 'trailingdot': UNCERTAIN, 'rawcontrol': UNCERTAIN}.items():
            with self.subTest(mode=mode):
                p, r = self.deliver(mode);self.assertEqual(r.code, expected);self.assertEqual(p.opens, 1)
    def test_bad_certificate_fails_before_http_bytes(self):
        p, r = self.file(trusted=False);self.assertEqual(r.code, TLS)
        p, r = self.deliver(trusted=False);self.assertEqual(r.code, TLS);self.assertFalse(r.sent)
        p, r = self.realtime(trusted=False);self.assertEqual(r.code, TLS)
    def test_realtime_final_order_duplicates_and_tail(self):
        p, r = self.realtime();self.assertEqual(r.code, OK);self.assertEqual(p.output.decode(), '你好世界😀')
        messages = self.server.ws_messages
        self.assertEqual(messages[0]['session']['input_audio_format'], 'pcm')
        self.assertEqual(messages[-1]['type'], 'session.finish')
        audio = [base64.b64decode(m['audio']) for m in messages if m['type'] == 'input_audio_buffer.append']
        self.assertEqual(list(map(len, audio)), [3200, 14])
    def test_realtime_faults(self):
        for mode, expected in {'ws_credentials': CREDENTIALS, 'ws_oversize': TOO_LARGE,
                               'ws_missing': RESPONSE, 'ws_timeout': TIMEOUT, 'ws_limited': LIMITED,
                               'ws_order': RESPONSE, 'ws_utf8': RESPONSE}.items():
            with self.subTest(mode=mode):
                p, r = self.realtime(mode);self.assertEqual(r.code, expected)
        p, r = self.realtime(cancel=True);self.assertEqual(r.code, CANCELLED)
    def test_admission_cancellation_and_unreadable_sources(self):
        for workspace, key in [(b'-bad', b'key'), (b'a'*64, b'key'), (b'test', b'key\r\nInjected: bad')]:
            p = Platform(self);source = p.source(b'x'*50);sink = p.sink()
            r = self.lib.cloud_file(C.byref(p.io), workspace, key, C.byref(source), C.byref(sink))
            self.assertEqual(r.code, INVALID);self.assertEqual(p.opens, 0);self.assertFalse(p.lease)
        p = Platform(self);p.lease = True;source = p.source(b'x'*50);sink = p.sink()
        r = self.lib.cloud_file(C.byref(p.io), b'test', b'key', C.byref(source), C.byref(sink))
        self.assertEqual(r.code, BUSY);self.assertTrue(p.lease);self.assertEqual(p.opens, 0)
        for raw in [b'\xed\xa0\x80', b'\xf0\x80\x80\x80', b'\xe7\x8c']:
            p, r = self.deliver(data=raw);self.assertEqual(r.code, INVALID);self.assertEqual(p.opens, 0)
        p = Platform(self);source = p.source(b'');source.size = 50;sink = p.sink()
        r = self.lib.cloud_file(C.byref(p.io), b'test', b'key', C.byref(source), C.byref(sink))
        self.assertEqual(r.code, SOURCE);self.assertFalse(p.lease);self.assertEqual(p.closes, 1)
        p = Platform(self);p.cancel = True;sink = p.sink();rt = self.lib.cloud_rt_new(C.byref(p.io))
        try:
            r = self.lib.cloud_rt_begin(rt, b'test', b'key', C.byref(sink))
            self.assertEqual(r.code, CANCELLED);self.assertEqual(p.opens, 0);self.assertFalse(p.lease)
        finally:
            self.lib.cloud_rt_delete(rt)
    def test_long_key_short_error_code_and_success_cancel_race(self):
        p, r = self.realtime(key=b'a'*256)
        self.assertEqual(r.code, OK)
        self.assertEqual(self.server.ws_headers[-1]['Authorization'], 'Bearer '+'a'*256)
        for mode in ['err_short', 'err_long', 'err_wrongtype', 'err_secret', 'err_missing', 'err_badjson']:
            p, r = self.file(mode)
            self.assertEqual(r.code, REJECTED)
            self.assertEqual(r.provider, b'InvalidParameter.Audio' if mode == 'err_short' else b'')
        p, r = self.deliver('success_cancel')
        self.assertTrue(p.cancel);self.assertEqual(r.code, OK)
    def test_repeated_success_failure_cancel_lifecycles(self):
        for _ in range(20):
            self.assertEqual(self.file()[1].code, OK)
            self.assertEqual(self.file('invalid')[1].code, RESPONSE)
            self.assertEqual(self.deliver('lost')[1].code, UNCERTAIN)
            self.assertEqual(self.realtime()[1].code, OK)
            self.assertEqual(self.realtime(cancel=True)[1].code, CANCELLED)
            self.assertEqual(self.lib.cloud_live(), 0)

if __name__ == '__main__':
    unittest.main(verbosity=2)
