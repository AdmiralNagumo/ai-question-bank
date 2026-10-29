"""隔离临时库/临时端口；不读取.env、不修改真实题库、不调用付费模型。"""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import secrets
import socket
import sqlite3
import subprocess
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

repo=Path(__file__).resolve().parents[3]
node=subprocess.check_output(['bash',str(repo/'scripts/find_ai_node.sh')],text=True).strip()
children=[]
client=build_opener(ProxyHandler({}))
secret_a='test-teacher-a-key-'+secrets.token_hex(16)
secret_b='test-teacher-b-key-'+secrets.token_hex(16)
def request(base,path,method='GET',value=None,token=None,raw=None):
    headers={'Content-Type':'application/json'}
    if token: headers['Authorization']='Bearer '+token
    data=raw if raw is not None else json.dumps(value,ensure_ascii=False).encode() if value is not None else None
    try: response=client.open(Request(base+path,data=data,headers=headers,method=method),timeout=5)
    except HTTPError as error: response=error
    with response:
        text=response.read().decode()
        assert secret_a not in text and secret_b not in text, 'secret in public response'
        if path.startswith('/api/'): assert response.headers.get('Cache-Control')=='no-store'
        return response.status,json.loads(text),response.headers
def expect(base,path,status,method='GET',value=None,token=None,code=None,raw=None):
    actual,body,headers=request(base,path,method,value,token,raw)
    assert actual==status,(path,actual,body)
    if code: assert body.get('code')==code,body
    return body,headers
def wait_service(base):
    until=time.monotonic()+10
    while time.monotonic()<until:
        try:
            if request(base,'/api/v1/health')[0]==200: return
        except OSError: pass
        time.sleep(.05)
    raise RuntimeError('service did not become ready')
with tempfile.TemporaryDirectory(prefix='teacher-api-test-') as tmp:
    temp=Path(tmp); db=temp/'test.db'
    env={**os.environ,'AQB_SKIP_ENV_FILE':'1','AI_ROUTING_MODE':'teacher','AI_MOCK':'1','AI_API_KEY':'',
         'AI_MOCK_DELAY_MS':'100','AI_SERVICE_PORT':'0','AI_SERVICE_TOKEN':secrets.token_hex(32),
         'AI_KEY_ENCRYPTION_KEY':secrets.token_hex(32),'AI_STUDENT_DAILY_LIMIT':'2',
         'DEMO_ADMIN_PW':'admin123','DEMO_TEACHER_PW':'teacher123','DEMO_STUDENT_PW':'student123',
         'DATABASE_PATH':str(db),'WEB_ROOT':str(repo/'web')}
    try:
        with (temp/'pi.log').open('w') as pi_log,(temp/'c.log').open('w') as c_log:
            pi=subprocess.Popen([node,str(repo/'ai-service/dist/src/server.js')],env=env,stdout=subprocess.PIPE,stderr=pi_log,text=True)
            children.append(pi)
            ready=json.loads(pi.stdout.readline()); assert ready['event']=='listening'
            with socket.socket() as sock: sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
            env.update(PORT=str(port),AI_SERVICE_URL='http://127.0.0.1:'+str(ready['port']))
            def start_c():
                process=subprocess.Popen([str(repo/'server/ai-question-bank-server')],env=env,cwd=repo/'server',stdout=c_log,stderr=c_log)
                children.append(process); wait_service(base); return process
            base='http://127.0.0.1:'+str(port)
            server=start_c()
            def login(username,password): return expect(base,'/api/v1/auth/login',200,'POST',{'username':username,'password':password})[0]['token']
            teacher=login('teacher','teacher123'); student=login('student','student123'); admin=login('admin','admin123')
            registered=expect(base,'/api/v1/auth/register',201,'POST',{'username':'teacher-b','password':'TeacherB123','confirm_password':'TeacherB123','role':'teacher'})[0]
            second=registered['token']; second_id=registered['user']['id']
            student_b=expect(base,'/api/v1/auth/register',201,'POST',{'username':'student-b','password':'StudentB123','confirm_password':'StudentB123'})[0]['token']
            path='/api/v1/teacher/ai-api'
            expect(base,path,401)
            for token in [student,admin]:
                for method,value in [('GET',None),('PUT',{'api_key':secret_a}),('PATCH',{'enabled':False})]:
                    expect(base,path,403,method,value,token,'TEACHER_REQUIRED')
            assert expect(base,path,200,token=teacher)[0]['api']['configured'] is False
            for value in [{'api_key':secret_a,'teacher_id':second_id},{'api_key':secret_a,'base_url':'http://127.0.0.1'},
                          {'api_key':'short'},{'api_key':secret_a,'model':'bad\nmodel'}]:
                expect(base,path,400,'PUT',value,teacher,'INVALID_REQUEST')
            expect(base,path,400,'PUT',token=teacher,raw=b'{"api_key":"aaaaaaaaaaaaaaaa","api_key":"bbbbbbbbbbbbbbbb"}')
            expect(base,path,400,'PUT',token=teacher,raw=b'{"api_key":"aaaaaaaa\\u0000bbbbbbbb"}')
            metadata=expect(base,path,200,'PUT',{'api_key':secret_a},teacher)[0]['api']
            assert metadata['configured'] and metadata['enabled'] and metadata['masked_key'].endswith(secret_a[-4:])
            assert expect(base,path,200,token=second)[0]['api']['configured'] is False
            expect(base,path,200,'PUT',{'api_key':secret_b,'model':'deepseek-chat'},second)
            expect(base,path,200,'PUT',{'api_key':secret_a},teacher)
            # 仅构造临时题库fixture，不修改题库源代码或真实开发数据。
            def connection(): return sqlite3.connect(db)
            with connection() as c:
                for qid,owner in [(101,2),(102,second_id),(103,2)]:
                    for name,value in {'content':'解方程：2x+3=11','options':'[]','knowledge_points':'[]','type':'short_answer',
                                       'teacher_id':str(owner),'status':'active','answer':'4','analysis':'test fixture'}.items():
                        c.execute('INSERT INTO qdata(key_name,value_name,value) VALUES(?,?,?)',(str(qid),name,value))
                assert c.execute('SELECT count(*) FROM teacher_ai_credentials WHERE teacher_id=2').fetchone()[0]==1
                for row in c.execute('SELECT secret FROM teacher_ai_credentials'):
                    assert secret_a.encode() not in row[0] and secret_b.encode() not in row[0]
                c.execute('DELETE FROM teacher_ai_credentials WHERE teacher_id=?',(second_id,))
            expect(base,'/api/v1/chat',404,'POST',{'question_id':102,'message':'test'},student,'AI_API_NOT_CONFIGURED')
            missing=expect(base,'/api/v1/ai/access?question_id=102',200,token=student)[0]
            assert not missing['ai_access']['allowed'] and missing['reason_code']=='AI_API_NOT_CONFIGURED'
            expect(base,path,200,'PUT',{'api_key':secret_b,'model':'deepseek-chat'},second)
            # 前端交付客户端也走真实C/Pi接口，避免文档示例与实现脱节。
            script="""
import assert from 'node:assert/strict';
import { getTeacherApi, replaceTeacherApi, setTeacherApiEnabled, getQuestionAiAccess } from './docs/examples/teacher-api-client.mjs';
const options={token:process.env.TEST_TEACHER_TOKEN,baseUrl:process.env.TEST_BASE};
assert.equal((await getTeacherApi(options)).api.configured,true);
await replaceTeacherApi({...options,apiKey:process.env.TEST_API_KEY});
assert.equal((await setTeacherApiEnabled({...options,enabled:false})).api.enabled,false);
await setTeacherApiEnabled({...options,enabled:true});
const access=await getQuestionAiAccess({token:process.env.TEST_STUDENT_TOKEN,baseUrl:options.baseUrl,questionId:101});
assert.equal(access.ai_access.allowed,true); assert.equal(access.quota.remaining,2);
await assert.rejects(getTeacherApi({...options,token:process.env.TEST_STUDENT_TOKEN}),error=>error.status===403);
console.log('frontend teacher API client passed');
"""
            result=subprocess.run([node,'--input-type=module'],input=script,text=True,check=True,capture_output=True,cwd=repo,
                                  env={**env,'TEST_BASE':base,'TEST_TEACHER_TOKEN':teacher,'TEST_STUDENT_TOKEN':student,'TEST_API_KEY':secret_a})
            assert secret_a not in result.stdout+result.stderr
            expect(base,'/api/v1/ai/access',400,token=student,code='QUESTION_REQUIRED')
            access=expect(base,'/api/v1/ai/access?question_id=101',200,token=student)[0]
            assert access['ai_access']['allowed'] and access['teacher_id']==2 and access['quota']['remaining']==2
            expect(base,'/api/v1/chat',403,'POST',{'question_id':102,'message':'test'},teacher,'AI_TEACHER_ONLY')
            expect(base,'/api/v1/chat',400,'POST',{'question_id':101,'message':'test','teacher_id':second_id},student)
            expect(base,'/api/v1/chat',400,'POST',{'question_id':101,'message':'test','execution':{'api_key':secret_b}},student)
            def submit(qid=101,token=student,**fields):
                return request(base,'/api/v1/chat','POST',{'question_id':qid,'message':'请解释',**fields},token)
            def finish(job,token=student):
                for _ in range(100):
                    state=expect(base,'/api/v1/chat/'+job['job_id'],200,token=token)[0]
                    if state['status'] not in ['queued','running']: return state
                    time.sleep(.03)
                raise AssertionError('job timeout')
            first=submit(); assert first[0]==202,first
            assert finish(first[1])['status']=='succeeded'
            # 重复同对话模式错误由Pi明确拒绝，扣次必须释放。
            failed=submit(conversation_id=first[1]['conversation_id'],mode='hint'); assert failed[0]==409,failed
            assert expect(base,'/api/v1/ai/access?question_id=101',200,token=student)[0]['quota']['used']==1
            # 两道同教师题目共享同一个配额；并发只剩一个名额。
            with ThreadPoolExecutor(max_workers=4) as pool: results=list(pool.map(lambda _:submit(qid=103),range(4)))
            assert sorted(result[0] for result in results)==[202,429,429,429],results
            accepted=next(result[1] for result in results if result[0]==202)
            denied=next(result for result in results if result[0]==429)
            assert denied[1]['code']=='AI_QUOTA_EXCEEDED' and denied[2].get('Retry-After')
            assert finish(accepted)['status']=='succeeded'
            # 换教师不消耗原教师额度；另一个学生也有独立额度。
            other=submit(qid=102); assert other[0]==202; assert finish(other[1])['status']=='succeeded'
            assert submit(token=student_b)[0]==202
            expect(base,path,200,'PATCH',{'enabled':False},teacher)
            expect(base,'/api/v1/chat',403,'POST',{'question_id':101,'message':'test'},student,'AI_API_DISABLED')
            expect(base,'/api/v1/chat/'+first[1]['job_id'],200,token=student)
            expect(base,'/api/v1/chat/'+first[1]['job_id'],200,'DELETE',token=student)
            expect(base,'/api/v1/chat/'+first[1]['job_id'],404,token=second)
            expect(base,path,200,'PATCH',{'enabled':True},teacher)
            # 教师调用自己的API不扣学生限额；停用/重配后老对话409。
            own=submit(token=teacher); assert own[0]==202; assert finish(own[1],teacher)['status']=='succeeded'
            expect(base,path,200,'PUT',{'api_key':secret_a},teacher)
            changed=submit(token=teacher,conversation_id=own[1]['conversation_id']); assert changed[0]==409 and changed[1]['code']=='AI_CREDENTIAL_CHANGED'
            # 审计失败时配置更新必须回滚。
            with connection() as c:
                version=c.execute('SELECT version FROM teacher_ai_credentials WHERE teacher_id=2').fetchone()[0]
                c.execute("CREATE TRIGGER deny_audit BEFORE INSERT ON teacher_ai_audit BEGIN SELECT RAISE(ABORT,'test'); END")
            expect(base,path,503,'PUT',{'api_key':secret_a},teacher)
            with connection() as c:
                assert c.execute('SELECT version FROM teacher_ai_credentials WHERE teacher_id=2').fetchone()[0]==version
                c.execute('DROP TRIGGER deny_audit')
            # C重启保留密文/配额，但需要重新登录。
            server.terminate(); server.wait(timeout=5); server=start_c()
            student=login('student','student123'); teacher=login('teacher','teacher123')
            student_b=login('student-b','StudentB123')
            assert expect(base,path,200,token=teacher)[0]['api']['configured']
            assert expect(base,'/api/v1/ai/access?question_id=101',200,token=student)[0]['quota']['remaining']==0
            expect(base,'/api/v1/chat',429,'POST',{'question_id':101,'message':'test'},student,'AI_QUOTA_EXCEEDED')
            # 另一教师尚有额度，篡改密文必须拒绝且不退回平台Key。
            with connection() as c: c.execute('UPDATE teacher_ai_credentials SET secret=randomblob(length(secret)) WHERE teacher_id=?',(second_id,))
            expect(base,'/api/v1/chat',503,'POST',{'question_id':102,'message':'test'},student,'AI_CREDENTIALS_UNAVAILABLE')
            pi.terminate(); pi.wait(timeout=5)
            expect(base,'/api/v1/chat',503,'POST',{'question_id':101,'message':'test'},student_b,'AI_SERVICE_UNAVAILABLE')
            assert expect(base,'/api/v1/ai/access?question_id=101',200,token=student_b)[0]['quota']['remaining']==0
            server.terminate(); server.wait(timeout=5)
        for log in [temp/'pi.log',temp/'c.log']:
            text=log.read_text(); assert secret_a not in text and secret_b not in text,'secret in logs'
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired: child.kill(); child.wait()
print('PASS: teacher注册即管理、学生/admin拒绝、单配置替换/启停/隔离、密文存储、题目归属路由、并发额度/明确拒绝退次/重启保留、跨用户任务与日志不泄密')
