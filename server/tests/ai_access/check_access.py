import datetime as dt
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import time
from urllib.error import HTTPError
from urllib.request import build_opener,ProxyHandler,Request

client = build_opener(ProxyHandler({}))
base = 'http://127.0.0.1:' + os.environ['TEST_PORT']
database = Path(os.environ['DATABASE_PATH'])
command = [sys.executable,'../scripts/ai_access.py','--db',str(database)]

def request(path,method='GET',data=None,token=None,headers=None):
    head = {'Content-Type':'application/json',**(headers or {})}
    if token: head['Authorization'] = 'Bearer ' + token
    try: response = client.open(Request(base+path,data=json.dumps(data).encode() if data is not None else None,method=method,headers=head),timeout=4)
    except HTTPError as error: response = error
    with response:
        assert response.headers['Cache-Control'] == 'no-store'
        return response.status,json.loads(response.read())

def login(username):
    status,body=request('/api/v1/auth/login','POST',{'username':username,'password':os.environ['DEMO_'+username.upper()+'_PW']})
    assert status == 200
    return body['token']

def operator(action,name='student',extra=(),success=True):
    args=command+[action,'--username',name]
    if action != 'status': args+=['--reason','API authorization regression']
    result=subprocess.run(args+list(extra),capture_output=True,text=True)
    assert (result.returncode == 0) == success, result.stderr
    return json.loads(result.stdout) if success else None

student,admin,teacher=(login(name) for name in ['student','admin','teacher'])
payload={'question_id':1,'message':'测试AI授权'}
assert request('/api/v1/ai/access')[0] == 401
for token in [student,admin,teacher]:
    status,body=request('/api/v1/ai/access',token=token)
    assert status == 200 and body['ai_access'] == {'allowed':False,'status':'not_granted','expires_at':None}
    status,body=request('/api/v1/chat','POST',payload,token)
    assert status == 403 and body['code'] == 'AI_ACCESS_REQUIRED'
assert request('/api/v1/ai/access','POST',{'allowed':True},admin)[0] == 405
assert request('/api/v1/ai/access','PUT',{'allowed':True},admin)[0] == 405
assert request('/api/v1/chat','POST',{**payload,'user_id':3,'allowed':True},teacher,{'X-User-Id':'3'})[0] == 403
status,new=request('/api/v1/auth/register','POST',{'username':'access_self_admin','password':'access1234','confirm_password':'access1234','role':'admin','ai_access':True})
assert status == 201
assert request('/api/v1/chat','POST',payload,new['token'])[0] == 403

operator('grant')
assert operator('status')['ai_access']['allowed']
assert request('/api/v1/ai/access',token=student)[1]['ai_access']['allowed']
assert request('/api/v1/ai/access',token=teacher)[1]['ai_access']['status'] == 'not_granted'
assert request('/api/v1/chat','POST',{**payload,'allowed':True},student)[0] == 400
status,job=request('/api/v1/chat','POST',payload,student)
assert status == 202
operator('revoke')
assert request('/api/v1/chat','POST',payload,student)[1]['code'] == 'AI_ACCESS_REVOKED'
assert request('/api/v1/chat/'+job['job_id'],token=student)[0] == 200
assert request('/api/v1/chat/'+job['job_id'],'DELETE',token=student)[0] == 200
assert request('/api/v1/chat/'+job['job_id'],token=teacher)[0] == 404
assert request('/api/v1/chat/'+job['job_id'],'DELETE',token=teacher)[0] == 404

future=(dt.datetime.now(dt.timezone.utc)+dt.timedelta(days=1)).replace(microsecond=0).isoformat()
granted=operator('grant',extra=('--expires-at',future))
assert request('/api/v1/ai/access',token=student)[1]['ai_access'] == granted['ai_access']
with sqlite3.connect(database) as db:
    db.execute('UPDATE ai_access_grants SET expires_at=? WHERE user_id=3',(int(time.time())-1,))
assert request('/api/v1/ai/access',token=student)[1]['ai_access']['status'] == 'expired'
assert request('/api/v1/chat','POST',payload,student)[1]['code'] == 'AI_ACCESS_EXPIRED'
operator('grant',name='missing-account',success=False)
operator('grant',extra=('--expires-at','2020-01-01T00:00:00Z'),success=False)
operator('grant',extra=('--expires-at','2030-01-01T00:00:00'),success=False)
with sqlite3.connect(database) as db:
    count=db.execute('SELECT count(*) FROM ai_access_audit').fetchone()[0]
    assert count == 3
    db.execute("CREATE TRIGGER fail_audit BEFORE INSERT ON ai_access_audit BEGIN SELECT RAISE(ABORT,'test'); END")
operator('grant',success=False)
assert request('/api/v1/ai/access',token=student)[1]['ai_access']['status'] == 'expired'
with sqlite3.connect(database) as db:
    assert db.execute('SELECT count(*) FROM ai_access_audit').fetchone()[0] == count
    db.execute('DROP TRIGGER fail_audit')
    db.execute('ALTER TABLE ai_access_grants RENAME TO ai_access_grants_backup')
assert request('/api/v1/ai/access',token=student)[1]['code'] == 'AI_ACCESS_UNAVAILABLE'
assert request('/api/v1/chat','POST',payload,student)[1]['code'] == 'AI_ACCESS_UNAVAILABLE'
with sqlite3.connect(database) as db: db.execute('ALTER TABLE ai_access_grants_backup RENAME TO ai_access_grants')
missing=database.parent/'missing-access.db'
result=subprocess.run([sys.executable,'../scripts/ai_access.py','--db',str(missing),'status','--username','student'],capture_output=True)
assert result.returncode != 0 and not missing.exists()
operator('revoke')
print('PASS: 默认拒绝所有角色/自注册admin、可信CLI授予/撤销/到期、既有任务所有权、存储失败关闭和审计事务回滚')
