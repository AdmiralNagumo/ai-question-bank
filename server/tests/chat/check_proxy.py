import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

reply = {'job_id': '11111111-1111-4111-8111-111111111111',
         'conversation_id': '22222222-2222-4222-8222-222222222222', 'question_id': 1, 'status': 'queued'}
responses = [
    (200, json.dumps(reply).encode(), 200, None),
    (401, b'{"error":"bad service credential","code":"UNAUTHORIZED"}', 503, 'AI_SERVICE_UNAVAILABLE'),
    (503, b'{"error":"busy","code":"AI_BUSY"}', 503, 'AI_BUSY'),
    (200, b'not JSON', 502, 'AI_INVALID_RESPONSE'),
    (200, b'{}', 502, 'AI_INVALID_RESPONSE'),
    (302, json.dumps(reply).encode(), 502, 'AI_INVALID_RESPONSE'),
    (200, b'x' * 70000, 502, 'AI_INVALID_RESPONSE'),
]
fixture = {'index': 0}
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        assert self.headers['Authorization'] == 'Bearer test-only-internal-service-12345678'
        assert self.headers['X-User-Id'] == '42'
        status, body, _, _ = responses[fixture['index']]
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass
    def log_message(self, *args): pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
driver = os.path.join(os.environ['AQB_TEST_BIN_DIR'], 'chat_proxy')
try:
    for index, (_, _, expected, code) in enumerate(responses):
        fixture['index'] = index
        result = subprocess.run([driver, 'http://127.0.0.1:' + str(server.server_port)],
                                check=True, capture_output=True, text=True,
                                env={**os.environ, 'http_proxy': 'http://127.0.0.1:1'})
        status, body = result.stdout.split('\n', 1)
        assert int(status) == expected, result.stdout
        data = json.loads(body)
        if code is not None: assert data['code'] == code
    # 连接拒绝必须是可恢复的服务不可用，而不是用户鉴权错误。
    result = subprocess.run([driver, 'http://127.0.0.1:1'], check=True, capture_output=True, text=True)
    status, body = result.stdout.split('\n', 1)
    assert int(status) == 503 and json.loads(body)['code'] == 'AI_SERVICE_UNAVAILABLE'
finally:
    server.shutdown(); server.server_close(); thread.join()
print('PASS: C 内部服务凭证、用户标识、异常响应/超大响应、代理绕过与连接失败')
