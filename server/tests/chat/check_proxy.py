import json
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

reply = {'job_id': '11111111-1111-4111-8111-111111111111',
         'conversation_id': '22222222-2222-4222-8222-222222222222', 'question_id': 1, 'status': 'queued'}
responses = [
    (200, json.dumps(reply).encode(), 200, None),
    (401, b'{"error":"bad service credential","code":"UNAUTHORIZED"}', 503, 'AI_SERVICE_UNAVAILABLE'),
    (401, b'private-upstream-non-json-body', 503, 'AI_SERVICE_UNAVAILABLE'),
    (503, b'{"error":"busy","code":"AI_BUSY"}', 503, 'AI_BUSY'),
    (200, b'not JSON', 502, 'AI_INVALID_RESPONSE'),
    (200, b'{}', 502, 'AI_INVALID_RESPONSE'),
    (302, json.dumps(reply).encode(), 502, 'AI_INVALID_RESPONSE'),
    (200, b'x' * 70000, 502, 'AI_INVALID_RESPONSE'),
]
fixture = {'index': 0, 'delay': 0}
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        assert self.headers['Authorization'] == 'Bearer test-only-internal-service-12345678'
        assert self.headers['X-User-Id'] == '42'
        status, body, _, _ = responses[fixture['index']]
        time.sleep(fixture['delay'])
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
env = {**os.environ, 'AQB_SKIP_ENV_FILE': '1', 'http_proxy': 'http://127.0.0.1:1'}
env.pop('AI_CONNECT_TIMEOUT_MS', None)
env.pop('AI_REQUEST_TIMEOUT_MS', None)
def check_private_logs(result):
    for secret in ['test-only-internal-service-12345678', 'private-upstream-non-json-body',
                   'bad service credential', 'http://127.0.0.1']:
        assert secret not in result.stderr, result.stderr
try:
    # 真实config_load：默认、边界、合法覆盖及非法值回退，不能把0传给curl关闭超时。
    result = subprocess.run([driver, '--config'], env=env, check=True, capture_output=True, text=True)
    assert result.stdout.strip() == '500 2000' and not result.stderr
    for connect, request in [('1','60000'),('1200','3500'),('60000','1')]:
        result = subprocess.run([driver, '--config'], env={**env,'AI_CONNECT_TIMEOUT_MS':connect,'AI_REQUEST_TIMEOUT_MS':request},
                                check=True,capture_output=True,text=True)
        assert result.stdout.strip() == f'{connect} {request}' and not result.stderr
    for invalid in ['', '0', '-1', '+10', ' 10', '10 ', '1.5', '2000ms', '60001', '9'*100, 'private-invalid-value']:
        result = subprocess.run([driver, '--config'], env={**env,'AI_CONNECT_TIMEOUT_MS':invalid,'AI_REQUEST_TIMEOUT_MS':invalid},
                                check=True,capture_output=True,text=True)
        assert result.stdout.strip() == '500 2000'
        if invalid:
            assert 'invalid AI_CONNECT_TIMEOUT_MS' in result.stderr and 'invalid AI_REQUEST_TIMEOUT_MS' in result.stderr
        assert 'private-invalid-value' not in result.stderr
    for index, (_, _, expected, code) in enumerate(responses):
        fixture['index'] = index
        result = subprocess.run([driver, 'http://127.0.0.1:' + str(server.server_port)],
                                check=True, capture_output=True, text=True,
                                env=env)
        status, body = result.stdout.split('\n', 1)
        assert int(status) == expected, result.stdout
        data = json.loads(body)
        if code is not None: assert data['code'] == code
        check_private_logs(result)
        if responses[index][0] == 401:
            assert 'internal_auth_failed' in result.stderr and 'HTTP 401' in result.stderr
            assert 'connect_failed' not in result.stderr
    # 连接拒绝必须是可恢复的服务不可用，而不是用户鉴权错误。
    result = subprocess.run([driver, 'http://127.0.0.1:1'], env=env, check=True, capture_output=True, text=True)
    status, body = result.stdout.split('\n', 1)
    assert int(status) == 503 and json.loads(body)['code'] == 'AI_SERVICE_UNAVAILABLE'
    assert 'connect_failed' in result.stderr and 'internal_auth_failed' not in result.stderr
    check_private_logs(result)
    # 延迟相同，短请求预算必须超时，足够预算则成功；验证配置真的传入libcurl。
    fixture.update(index=0,delay=0.4)
    for budget, expected in [('100',504),('1500',200)]:
        result = subprocess.run([driver,'http://127.0.0.1:'+str(server.server_port)],
                                env={**env,'AI_REQUEST_TIMEOUT_MS':budget}, check=True,capture_output=True,text=True,timeout=5)
        status, body = result.stdout.split('\n',1)
        assert int(status) == expected, result.stdout
        if expected == 504:
            assert json.loads(body)['code'] == 'AI_SERVICE_TIMEOUT'
            assert 'request_timeout' in result.stderr and 'internal_auth_failed' not in result.stderr
        else: assert not result.stderr
        check_private_logs(result)
finally:
    server.shutdown(); server.server_close(); thread.join()
print('PASS: 超时配置/非法值回退与真实请求截止、脱敏故障分类日志、内部凭证/用户标识及异常响应/代理绕过')
