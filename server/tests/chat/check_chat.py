#!/usr/bin/env python3
import json
import os
import sys
import time
import subprocess
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

base = 'http://127.0.0.1:' + os.getenv('TEST_PORT', '8099')
client = build_opener(ProxyHandler({}))
def request(path, method='GET', value=None, token=None, raw=None, content_type='application/json'):
    headers = {}
    if token is not None:
        headers['Authorization'] = 'Bearer ' + token
    data = raw if raw is not None else json.dumps(value).encode() if value is not None else None
    if data is not None: headers['Content-Type'] = content_type
    try:
        response = client.open(Request(base + path, data=data, headers=headers, method=method), timeout=4)
    except HTTPError as error:
        response = error
    with response:
        assert response.headers.get_content_type() == 'application/json'
        return response.status, json.loads(response.read())

def login(name):
    password = os.getenv('DEMO_' + name.upper() + '_PW', name + '123')
    status, result = request('/api/v1/auth/login', 'POST', {'username': name, 'password': password})
    assert status == 200
    return result['token']

def complete(job, token):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        status, result = request('/api/v1/chat/' + job, token=token)
        assert status == 200
        if result['status'] not in ('queued', 'running'): return result
        time.sleep(0.02)
    raise AssertionError('job did not finish')

token = login('student')
subprocess.run([sys.executable,'../scripts/ai_access.py','--db',os.environ['DATABASE_PATH'],'grant',
                '--username','student','--reason','existing chat regression fixture'],check=True,stdout=subprocess.DEVNULL)
status, result = request('/api/v1/questions', token=token)
assert status == 200 and result['questions']
qid = result['questions'][0]['id']
message = {'question_id': qid, 'message': '请解释这道题'}
mode = sys.argv[1]
if mode == 'flow':
    started = time.monotonic()
    status, result = request('/api/v1/chat', 'POST', message, token)
    assert status == 202 and result['status'] in ('queued', 'running')
    assert time.monotonic() - started < 1, '提交不应等待模型完成'
    assert request('/api/v1/health')[0] == 200
    first = complete(result['job_id'], token)
    assert first['status'] == 'succeeded' and '第 1 轮' in first['reply']
    status, second = request('/api/v1/chat', 'POST', {**message,
        'message': '再详细说明', 'conversation_id': first['conversation_id']}, token)
    assert status == 202
    finished = complete(second['job_id'], token)
    assert finished['status'] == 'succeeded' and '第 2 轮' in finished['reply']
    assert finished['conversation_id'] == first['conversation_id']
    print('PASS: C 登录/选题 -> Pi 任务 -> 查询解答 -> 同题追问；服务等待期间仍可访问健康接口')
elif mode == 'validation':
    assert request('/api/v1/chat', 'POST', message)[0] == 401
    for value in [{'question_id': 0, 'message': 'a'}, {'question_id': 1.5, 'message': 'a'},
                  {**message, 'message': ''}, {**message, 'message': 'x' * 4001},
                  {**message, 'api_key': 'not-allowed'}, {**message, 'user_id': 1},
                  {**message, 'conversation_id': 'invalid'}]:
        assert request('/api/v1/chat', 'POST', value, token)[0] == 400
    assert request('/api/v1/chat', 'POST', {**message, 'question_id': 99999}, token)[0] == 404
    assert request('/api/v1/chat', 'POST', token=token, raw=b'{bad')[0] == 400
    assert request('/api/v1/chat', 'POST', token=token,
                   raw=b'{"question_id":1,"question_id":2,"message":"a"}')[0] == 400
    assert request('/api/v1/chat', 'POST', {**message, 'message': 'a\0b'}, token)[0] == 400
    assert request('/api/v1/chat', 'POST', message, token, content_type='text/plain')[0] == 415
    assert request('/api/v1/chat', token=token)[0] == 405
    assert request('/api/v1/chat/bad-id', token=token)[0] == 404
    assert request('/api/v1/chat', 'POST', token=token, raw=b'x' * 65537)[0] == 413
    print('PASS: 鉴权、非法字段/ID、重复字段、媒体类型和请求大小')
elif mode == 'isolation':
    status, job = request('/api/v1/chat', 'POST', message, token)
    assert status == 202
    other = login('admin')
    subprocess.run([sys.executable,'../scripts/ai_access.py','--db',os.environ['DATABASE_PATH'],'grant',
                   '--username','admin','--reason','ownership regression fixture'],check=True,stdout=subprocess.DEVNULL)
    assert request('/api/v1/chat/' + job['job_id'], token=other)[0] == 404
    assert request('/api/v1/chat/' + job['job_id'], 'DELETE', token=other)[0] == 404
    assert request('/api/v1/chat', 'POST', {**message,
                   'conversation_id': job['conversation_id']}, other)[0] == 404
    assert request('/api/v1/chat', 'POST', {**message,
                   'conversation_id': job['conversation_id']}, token)[0] == 409
    status, cancelled = request('/api/v1/chat/' + job['job_id'], 'DELETE', token=token)
    assert status == 200 and cancelled['status'] == 'cancelled'
    assert complete(job['job_id'], token)['status'] == 'cancelled'
    request('/api/v1/auth/logout', 'POST', token=token)
    assert request('/api/v1/chat/' + job['job_id'], token=token)[0] == 401
    print('PASS: 用户隔离、对话锁定、取消任务与登出后的权限检查')
else:
    if mode != 'teaching': raise ValueError(mode)
    for value in [{**message, 'mode': 'bad'}, {**message, 'mode': ['solve']},
                  {**message, 'mode': None}, {**message, 'mode': 'review'},
                  {**message, 'mode': 'review', 'student_answer': ''},
                  {**message, 'mode': 'review', 'student_answer': 4},
                  {**message, 'mode': 'review', 'student_answer': 'x' * 4001},
                  {**message, 'mode': 'review', 'student_answer': 'x\0y'},
                  {**message, 'mode': 'hint', 'student_answer': 'x=7'},
                  {**message, 'student_answer': 'x=7'}]:
        assert request('/api/v1/chat', 'POST', value, token)[0] == 400
    assert request('/api/v1/chat', 'POST', token=token,
                   raw=b'{"question_id":1,"message":"a","mode":"solve","mode":"hint"}')[0] == 400
    conversation = None
    for expected in [1, 2, 3, 3]:
        payload = {**message, 'mode': 'hint'} if conversation is None else {**message, 'conversation_id': conversation}
        status, job = request('/api/v1/chat', 'POST', payload, token)
        assert status == 202 and job['mode'] == 'hint' and job['hint_level'] == expected
        finished = complete(job['job_id'], token)
        assert finished['status'] == 'succeeded' and finished['hint_level'] == expected
        assert f'提示 {expected} 级' in finished['reply']
        conversation = finished['conversation_id']
    status, mismatch = request('/api/v1/chat', 'POST', {**message, 'mode': 'solve', 'conversation_id': conversation}, token)
    assert status == 409 and mismatch['code'] == 'CONVERSATION_MODE_MISMATCH'
    status, job = request('/api/v1/chat', 'POST', {**message, 'mode': 'review', 'student_answer': 'x=7'}, token)
    assert status == 202 and job['mode'] == 'review' and 'hint_level' not in job
    first = complete(job['job_id'], token)
    assert first['status'] == 'succeeded'
    assert request('/api/v1/chat', 'POST', {**message, 'conversation_id': first['conversation_id']}, token)[0] == 400
    status, job = request('/api/v1/chat', 'POST', {**message, 'conversation_id': first['conversation_id'], 'student_answer': 'x=4'}, token)
    assert status == 202 and job['mode'] == 'review'
    assert complete(job['job_id'], token)['status'] == 'succeeded'
    print('PASS: C/Pi 教学模式、提示1→2→3、模式隔离、作答校验及追问继承')
