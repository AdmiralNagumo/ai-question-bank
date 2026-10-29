#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "teacher_api.h"
#include "kv_store.h"

static time_t clock_now=1793330000;
time_t __wrap_time(time_t *out) { if (out) *out=clock_now; return clock_now; }
static int deny_commit(void *unused) { (void)unused; return 1; }
static void sql(sqlite3 *db,const char *text) { assert(sqlite3_exec(db,text,NULL,NULL,NULL)==SQLITE_OK); }
static void user(sqlite3 *db,const char *name,const char *id,const char *role) {
    assert(kv_set(db,"users",name,"id",id)==0); assert(kv_set(db,"users",name,"role",role)==0);
}
int main(void) {
    sqlite3 *db=NULL; assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
    assert(kv_init(db)==0 && teacher_api_init(db)==0);
    user(db,"teacher-a","2","teacher"); user(db,"student","3","student"); user(db,"teacher-b","4","teacher");
    assert(kv_set(db,"qdata","10","teacher_id","2")==0);
    AppConfig cfg={.db=db,.ai_key_encryption_key="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",.ai_student_daily_limit=2};
    const char *key="test-only-teacher-key-abcdefghijkl";
    cJSON *view=NULL;
    assert(teacher_api_metadata(&cfg,2,&view)==NULL && !cJSON_IsTrue(cJSON_GetObjectItem(view,"configured"))); cJSON_Delete(view);
    assert(!strcmp(teacher_api_save(&cfg,2,"short","deepseek-flash",1),"INVALID_REQUEST"));
    assert(teacher_api_save(&cfg,2,key,"deepseek-flash",1)==NULL);
    assert(teacher_api_save(&cfg,2,key,"deepseek-flash",1)==NULL);
    sqlite3_stmt *s=NULL;
    assert(sqlite3_prepare_v2(db,"SELECT count(*),hex(secret),version FROM teacher_ai_credentials",-1,&s,NULL)==SQLITE_OK);
    assert(sqlite3_step(s)==SQLITE_ROW && sqlite3_column_int(s,0)==1 && sqlite3_column_int(s,2)==2);
    assert(!strstr((const char *)sqlite3_column_text(s,1),key)); sqlite3_finalize(s);
    assert(sqlite3_prepare_v2(db,"SELECT secret FROM teacher_ai_credentials WHERE teacher_id=2",-1,&s,NULL)==SQLITE_OK);
    assert(sqlite3_step(s)==SQLITE_ROW);
    const unsigned char *blob=sqlite3_column_blob(s,0); int bytes=sqlite3_column_bytes(s,0);
    for (int i=0;i+(int)strlen(key)<=bytes;i++) assert(memcmp(blob+i,key,strlen(key))!=0);
    sqlite3_finalize(s);
    TeacherExecution execution; cJSON *access=NULL;
    assert(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access)==NULL);
    assert(!strcmp(execution.api_key,key) && execution.teacher_id==2 && execution.version==2 && execution.charge_id>0);
    sqlite3_int64 receipt=execution.charge_id; teacher_api_clear(&execution); cJSON_Delete(access);
    assert(teacher_api_refund(db,receipt)==0 && teacher_api_refund(db,receipt)==0);
    sql(db,"CREATE TRIGGER deny_charge BEFORE INSERT ON teacher_ai_charges BEGIN SELECT RAISE(ABORT,'test'); END;");
    assert(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access)!=NULL); cJSON_Delete(access);
    sql(db,"DROP TRIGGER deny_charge;");
    assert(teacher_api_resolve(&cfg,3,"student",10,0,&execution,&access)==NULL);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(access,"quota"),"used")->valueint==0); cJSON_Delete(access); teacher_api_clear(&execution);
    sqlite3_commit_hook(db,deny_commit,NULL);
    assert(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access)!=NULL);
    assert(access==NULL && execution.api_key[0]=='\0'); // COMMIT失败不能返回允许使用的快照。
    sqlite3_commit_hook(db,NULL,NULL);
    for (int i=0;i<2;i++) { assert(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access)==NULL); cJSON_Delete(access); teacher_api_clear(&execution); }
    assert(!strcmp(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access),"AI_QUOTA_EXCEEDED"));
    assert(execution.api_key[0]=='\0'); cJSON_Delete(access);
    clock_now=((clock_now+28800)/86400+1)*86400-28800; // 上海午夜，前一天额度不能污染新周期。
    assert(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access)==NULL);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(access,"quota"),"used")->valueint==1); cJSON_Delete(access); teacher_api_clear(&execution);
    assert(!strcmp(teacher_api_resolve(&cfg,4,"teacher",10,0,&execution,&access),"AI_TEACHER_ONLY")); cJSON_Delete(access);
    assert(teacher_api_toggle(&cfg,2,0)==NULL);
    assert(!strcmp(teacher_api_resolve(&cfg,3,"student",10,1,&execution,&access),"AI_API_DISABLED")); cJSON_Delete(access);
    assert(teacher_api_toggle(&cfg,2,1)==NULL);
    const char *correct=cfg.ai_key_encryption_key;
    cfg.ai_key_encryption_key="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    assert(!strcmp(teacher_api_resolve(&cfg,3,"student",10,0,&execution,&access),"AI_CREDENTIALS_UNAVAILABLE")); cJSON_Delete(access);
    cfg.ai_key_encryption_key=correct;
    sql(db,"UPDATE teacher_ai_credentials SET model='deepseek-chat' WHERE teacher_id=2;");
    assert(!strcmp(teacher_api_resolve(&cfg,3,"student",10,0,&execution,&access),"AI_CREDENTIALS_UNAVAILABLE")); cJSON_Delete(access);
    assert(teacher_api_save(&cfg,2,key,"deepseek-flash",1)==NULL);
    user(db,"duplicate","2","teacher");
    assert(teacher_api_resolve(&cfg,3,"student",10,0,&execution,&access)!=NULL); cJSON_Delete(access);
    sql(db,"DELETE FROM users WHERE key_name='duplicate'; UPDATE teacher_ai_credentials SET owner_key='other-account';");
    assert(teacher_api_resolve(&cfg,3,"student",10,0,&execution,&access)!=NULL); cJSON_Delete(access);
    sqlite3_close(db);
    puts("PASS: AES-GCM加解密/错误主密钥/AAD篡改、单教师单配置、身份绑定、配额事务回滚/幂等释放/上海午夜重置");
    return 0;
}
