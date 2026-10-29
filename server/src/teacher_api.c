#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "teacher_api.h"

#define UNAVAILABLE "AI_API_UNAVAILABLE"
#define CRYPTO_UNAVAILABLE "AI_CREDENTIALS_UNAVAILABLE"
typedef struct {
    int exists, enabled, version;
    char owner[1024], model[129], mask[16];
    unsigned char blob[540];
    int bytes;
    sqlite3_int64 updated;
} Slot;

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **s) {
    return sqlite3_prepare_v2(db, sql, -1, s, NULL) == SQLITE_OK ? 0 : -1;
}
static int run(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1; }

int teacher_api_init(sqlite3 *db) {
    return run(db,
        "CREATE TABLE IF NOT EXISTS teacher_ai_credentials(teacher_id INTEGER PRIMARY KEY,"
        "owner_key TEXT NOT NULL,model TEXT NOT NULL,secret BLOB NOT NULL,masked_key TEXT NOT NULL,"
        "enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),version INTEGER NOT NULL CHECK(version>0),updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS teacher_ai_usage(student_id INTEGER NOT NULL,teacher_id INTEGER NOT NULL,"
        "day INTEGER NOT NULL,used INTEGER NOT NULL CHECK(used>=0),PRIMARY KEY(student_id,teacher_id,day));"
        "CREATE TABLE IF NOT EXISTS teacher_ai_charges(id INTEGER PRIMARY KEY AUTOINCREMENT,student_id INTEGER NOT NULL,"
        "teacher_id INTEGER NOT NULL,day INTEGER NOT NULL,released INTEGER NOT NULL DEFAULT 0 CHECK(released IN(0,1)),created_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS teacher_ai_audit(id INTEGER PRIMARY KEY AUTOINCREMENT,teacher_id INTEGER NOT NULL,"
        "version INTEGER NOT NULL,action TEXT NOT NULL,created_at INTEGER NOT NULL);");
}

/* 唯一、规范的ID和现存角色，从可信账号表解析，不信任提交的归属字段。 */
static int identity(sqlite3 *db, int id, const char *role, char owner[1024]) {
    sqlite3_stmt *s = NULL;
    if (id <= 0 || prepare(db,"SELECT u.key_name FROM users u WHERE u.value_name='id' AND u.value=CAST(? AS TEXT) "
        "AND EXISTS(SELECT 1 FROM users r WHERE r.key_name=u.key_name AND r.value_name='role' AND r.value=?) "
        "AND (SELECT count(*) FROM users i WHERE i.value_name='id' AND i.value=u.value)=1",&s)) return -1;
    sqlite3_bind_int(s,1,id); sqlite3_bind_text(s,2,role,-1,SQLITE_STATIC);
    int rc=sqlite3_step(s), ok=0;
    if (rc==SQLITE_ROW) {
        const char *key=(const char *)sqlite3_column_text(s,0);
        if (key && strlen(key)>0 && strlen(key)<1024) { memcpy(owner,key,strlen(key)+1); ok=1; }
    }
    sqlite3_finalize(s); return ok ? 0 : -1;
}
static int copy_text(sqlite3_stmt *s,int column,char *out,size_t capacity) {
    if (sqlite3_column_type(s,column)!=SQLITE_TEXT) return -1;
    const char *value=(const char *)sqlite3_column_text(s,column);
    int length=sqlite3_column_bytes(s,column);
    if (length<0 || (size_t)length>=capacity || !value || strlen(value)!=(size_t)length) return -1;
    memcpy(out,value,(size_t)length+1); return 0;
}
static int load(sqlite3 *db,int id,Slot *slot) {
    memset(slot,0,sizeof(*slot));
    sqlite3_stmt *s=NULL;
    if (prepare(db,"SELECT owner_key,model,secret,masked_key,enabled,version,updated_at FROM teacher_ai_credentials WHERE teacher_id=?",&s)) return -1;
    sqlite3_bind_int(s,1,id);
    int rc=sqlite3_step(s), result=-1;
    if (rc==SQLITE_DONE) result=0;
    if (rc==SQLITE_ROW && !copy_text(s,0,slot->owner,sizeof(slot->owner)) &&
        !copy_text(s,1,slot->model,sizeof(slot->model)) && !copy_text(s,3,slot->mask,sizeof(slot->mask)) &&
        sqlite3_column_type(s,2)==SQLITE_BLOB && sqlite3_column_type(s,4)==SQLITE_INTEGER &&
        sqlite3_column_type(s,5)==SQLITE_INTEGER && sqlite3_column_type(s,6)==SQLITE_INTEGER) {
        slot->bytes=sqlite3_column_bytes(s,2);
        sqlite3_int64 version=sqlite3_column_int64(s,5), enabled=sqlite3_column_int64(s,4);
        if (slot->bytes>=44 && slot->bytes<=540 && (enabled==0 || enabled==1) && version>0 && version<INT_MAX) {
            memcpy(slot->blob,sqlite3_column_blob(s,2),(size_t)slot->bytes);
            slot->exists=1; slot->enabled=(int)enabled; slot->version=(int)version;
            slot->updated=sqlite3_column_int64(s,6); result=0;
        }
    }
    sqlite3_finalize(s); return result;
}
static int master_key(const AppConfig *cfg,unsigned char key[32]) {
    const char *hex=cfg->ai_key_encryption_key;
    if (!hex || strlen(hex)!=64) return -1;
    for (int i=0;i<32;i++) {
        unsigned int value=0;
        for (int j=0;j<2;j++) {
            unsigned char c=(unsigned char)hex[i*2+j];
            unsigned int digit;
            if (c>='0' && c<='9') digit=c-'0';
            else if (c>='a' && c<='f') digit=c-'a'+10;
            else if (c>='A' && c<='F') digit=c-'A'+10;
            else { OPENSSL_cleanse(key,32); return -1; }
            value=value*16+digit;
        }
        key[i]=(unsigned char)value;
    }
    return 0;
}
/* blob = 12字节nonce + ciphertext + 16字节tag，GCM认证AAD防止跨教师/版本移植。 */
static int crypt(const AppConfig *cfg,int id,Slot *slot,char plain[513],int encrypt) {
    unsigned char key[32]={0}; char aad[1400];
    int length=snprintf(aad,sizeof(aad),"teacher-api-v1:%d:%zu:%s:%d:%s",id,strlen(slot->owner),slot->owner,slot->version,slot->model);
    if (length<0 || (size_t)length>=sizeof(aad) || master_key(cfg,key)) return -1;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new(); int n=0,total=0,ok=0;
    if (!ctx) goto done;
    if (encrypt) {
        int bytes=(int)strlen(plain);
        if (bytes<16 || bytes>512 || RAND_bytes(slot->blob,12)!=1) goto done;
        if (EVP_EncryptInit_ex(ctx,EVP_aes_256_gcm(),NULL,key,slot->blob)!=1 ||
            EVP_EncryptUpdate(ctx,NULL,&n,(const unsigned char *)aad,length)!=1 ||
            EVP_EncryptUpdate(ctx,slot->blob+12,&n,(const unsigned char *)plain,bytes)!=1) goto done;
        total=n;
        if (EVP_EncryptFinal_ex(ctx,slot->blob+12+total,&n)!=1) goto done;
        total+=n;
        if (EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,16,slot->blob+12+total)!=1) goto done;
        slot->bytes=12+total+16; ok=1;
    } else {
        int bytes=slot->bytes-28;
        if (bytes<16 || bytes>512 || EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),NULL,key,slot->blob)!=1 ||
            EVP_DecryptUpdate(ctx,NULL,&n,(const unsigned char *)aad,length)!=1 ||
            EVP_DecryptUpdate(ctx,(unsigned char *)plain,&n,slot->blob+12,bytes)!=1) goto done;
        total=n;
        if (EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,16,slot->blob+12+bytes)!=1 ||
            EVP_DecryptFinal_ex(ctx,(unsigned char *)plain+total,&n)!=1) goto done;
        total+=n; plain[total]='\0';
        if (strlen(plain)!=(size_t)total) goto done;
        ok=1;
    }
done:
    EVP_CIPHER_CTX_free(ctx); OPENSSL_cleanse(key,sizeof(key));
    if (!ok) OPENSSL_cleanse(plain,513);
    return ok ? 0 : -1;
}
static int valid_key(const char *key) {
    if (!key || strlen(key)<16 || strlen(key)>512) return 0;
    for (const unsigned char *p=(const unsigned char *)key;*p;p++) if (*p<33 || *p>126) return 0;
    return 1;
}
static int valid_model(const char *model) {
    if (!model || !*model || strlen(model)>128) return 0;
    for (const char *p=model;*p;p++) if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
        (*p>='0' && *p<='9') || strchr("-_.:/",*p))) return 0;
    return 1;
}
static cJSON *metadata(const Slot *slot) {
    cJSON *object=cJSON_CreateObject();
    int ok=object && cJSON_AddBoolToObject(object,"configured",slot->exists);
    if (ok && slot->exists) ok=cJSON_AddBoolToObject(object,"enabled",slot->enabled) &&
        cJSON_AddStringToObject(object,"provider","deepseek") && cJSON_AddStringToObject(object,"model",slot->model) &&
        cJSON_AddStringToObject(object,"masked_key",slot->mask) && cJSON_AddNumberToObject(object,"version",slot->version) &&
        cJSON_AddNumberToObject(object,"updated_at",(double)slot->updated);
    if (!ok) { cJSON_Delete(object); return NULL; }
    return object;
}
const char *teacher_api_metadata(const AppConfig *cfg,int teacher,cJSON **out) {
    char owner[1024]; Slot slot; *out=NULL;
    if (identity(cfg->db,teacher,"teacher",owner) || load(cfg->db,teacher,&slot) ||
        (slot.exists && strcmp(slot.owner,owner))) return UNAVAILABLE;
    *out=metadata(&slot); return *out ? NULL : UNAVAILABLE;
}
const char *teacher_api_save(const AppConfig *cfg,int teacher,const char *key,const char *model,int enabled) {
    if (!valid_key(key) || !valid_model(model) || (enabled!=0 && enabled!=1)) return "INVALID_REQUEST";
    Slot slot; char owner[1024],plain[513]={0}; const char *error=UNAVAILABLE;
    if (run(cfg->db,"BEGIN IMMEDIATE")) return error;
    sqlite3_stmt *s=NULL;
    if (identity(cfg->db,teacher,"teacher",owner) || load(cfg->db,teacher,&slot) ||
        (slot.exists && strcmp(slot.owner,owner))) goto done;
    memcpy(slot.owner,owner,strlen(owner)+1); slot.version++; slot.enabled=enabled;
    memcpy(slot.model,model,strlen(model)+1); memcpy(plain,key,strlen(key)+1);
    snprintf(slot.mask,sizeof(slot.mask),"****%s",key+strlen(key)-4);
    if (crypt(cfg,teacher,&slot,plain,1)) { error=CRYPTO_UNAVAILABLE; goto done; }
    if (prepare(cfg->db,"INSERT INTO teacher_ai_credentials(teacher_id,owner_key,model,secret,masked_key,enabled,version,updated_at) "
        "VALUES(?,?,?,?,?,?,?,unixepoch()) ON CONFLICT(teacher_id) DO UPDATE SET owner_key=excluded.owner_key,model=excluded.model,"
        "secret=excluded.secret,masked_key=excluded.masked_key,enabled=excluded.enabled,version=excluded.version,updated_at=excluded.updated_at",&s)) goto done;
    sqlite3_bind_int(s,1,teacher); sqlite3_bind_text(s,2,slot.owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,3,slot.model,-1,SQLITE_TRANSIENT); sqlite3_bind_blob(s,4,slot.blob,slot.bytes,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,5,slot.mask,-1,SQLITE_TRANSIENT); sqlite3_bind_int(s,6,enabled); sqlite3_bind_int(s,7,slot.version);
    if (sqlite3_step(s)!=SQLITE_DONE) goto done;
    sqlite3_finalize(s); s=NULL;
    if (prepare(cfg->db,"INSERT INTO teacher_ai_audit(teacher_id,version,action,created_at) VALUES(?,?,?,unixepoch())",&s)) goto done;
    sqlite3_bind_int(s,1,teacher); sqlite3_bind_int(s,2,slot.version);
    sqlite3_bind_text(s,3,enabled ? "configure_or_enable" : "disable",-1,SQLITE_STATIC);
    if (sqlite3_step(s)!=SQLITE_DONE) goto done;
    sqlite3_finalize(s); s=NULL;
    if (run(cfg->db,"COMMIT")) goto done;
    error=NULL;
done:
    sqlite3_finalize(s); if (error) run(cfg->db,"ROLLBACK");
    OPENSSL_cleanse(plain,sizeof(plain)); OPENSSL_cleanse(&slot,sizeof(slot)); return error;
}
const char *teacher_api_toggle(const AppConfig *cfg,int teacher,int enabled) {
    char owner[1024],plain[513]={0}; Slot slot;
    if (identity(cfg->db,teacher,"teacher",owner) || load(cfg->db,teacher,&slot) ||
        (slot.exists && strcmp(slot.owner,owner))) return UNAVAILABLE;
    if (!slot.exists) return "AI_API_NOT_CONFIGURED";
    if (crypt(cfg,teacher,&slot,plain,0)) return CRYPTO_UNAVAILABLE;
    const char *error=teacher_api_save(cfg,teacher,plain,slot.model,enabled);
    OPENSSL_cleanse(plain,sizeof(plain)); return error;
}

static int usage(sqlite3 *db,int user,int teacher,sqlite3_int64 day,int *used) {
    sqlite3_stmt *s=NULL; *used=0;
    if (prepare(db,"SELECT used FROM teacher_ai_usage WHERE student_id=? AND teacher_id=? AND day=?",&s)) return -1;
    sqlite3_bind_int(s,1,user); sqlite3_bind_int(s,2,teacher); sqlite3_bind_int64(s,3,day);
    int rc=sqlite3_step(s),ok=rc==SQLITE_DONE;
    if (rc==SQLITE_ROW && sqlite3_column_type(s,0)==SQLITE_INTEGER) {
        sqlite3_int64 value=sqlite3_column_int64(s,0);
        if (value>=0 && value<=INT_MAX) { *used=(int)value; ok=1; }
    }
    sqlite3_finalize(s); return ok ? 0 : -1;
}
static cJSON *access_json(const AppConfig *cfg,int teacher,int student,int used,sqlite3_int64 day,const char *error) {
    cJSON *root=cJSON_CreateObject(),*access=root ? cJSON_AddObjectToObject(root,"ai_access") : NULL;
    int ok=access && cJSON_AddBoolToObject(access,"allowed",!error) &&
        cJSON_AddStringToObject(access,"status",error ? "not_granted" : "active") && cJSON_AddNullToObject(access,"expires_at") &&
        cJSON_AddNumberToObject(root,"teacher_id",teacher);
    if (ok && error) ok=cJSON_AddStringToObject(root,"reason_code",error)!=NULL;
    cJSON *quota=ok ? cJSON_AddObjectToObject(root,"quota") : NULL;
    char timestamp[32]; time_t reset=(time_t)((day+1)*86400-28800); struct tm utc;
    if (!gmtime_r(&reset,&utc) || !strftime(timestamp,sizeof(timestamp),"%Y-%m-%dT%H:%M:%SZ",&utc)) ok=0;
    ok=ok && quota && cJSON_AddBoolToObject(quota,"applies",student) &&
        cJSON_AddNumberToObject(quota,"limit",cfg->ai_student_daily_limit) && cJSON_AddNumberToObject(quota,"used",used) &&
        cJSON_AddNumberToObject(quota,"remaining",student ? (used<cfg->ai_student_daily_limit ? cfg->ai_student_daily_limit-used : 0) : cfg->ai_student_daily_limit) &&
        cJSON_AddStringToObject(quota,"reset_at",timestamp) && cJSON_AddStringToObject(quota,"timezone","Asia/Shanghai") &&
        cJSON_AddStringToObject(quota,"scope","student_teacher_day");
    if (!ok) { cJSON_Delete(root); return NULL; }
    return root;
}
const char *teacher_api_resolve(const AppConfig *cfg,int user,const char *role,int question,int charge,
                               TeacherExecution *execution,cJSON **access) {
    memset(execution,0,sizeof(*execution)); *access=NULL;
    if (cfg->ai_student_daily_limit<1 || cfg->ai_student_daily_limit>10000) return UNAVAILABLE;
    sqlite3_stmt *s=NULL; const char *error=UNAVAILABLE;
    char actor[1024],owner[1024]; Slot slot; int teacher=0,used=0;
    time_t now=time(NULL); if (now<0) return UNAVAILABLE;
    sqlite3_int64 day=((sqlite3_int64)now+28800)/86400;
    if (run(cfg->db,"BEGIN IMMEDIATE")) return UNAVAILABLE;
    if (identity(cfg->db,user,role,actor)) goto done;
    if (prepare(cfg->db,"SELECT value FROM qdata WHERE key_name=CAST(? AS TEXT) AND value_name='teacher_id'",&s)) goto done;
    sqlite3_bind_int(s,1,question);
    int rc=sqlite3_step(s);
    if (rc==SQLITE_DONE) { error="QUESTION_NOT_FOUND"; goto done; }
    if (rc!=SQLITE_ROW || sqlite3_column_type(s,0)!=SQLITE_TEXT) goto done;
    const char *value=(const char *)sqlite3_column_text(s,0); char *end=NULL;
    long parsed=value ? strtol(value,&end,10) : 0;
    char canonical[32]; snprintf(canonical,sizeof(canonical),"%ld",parsed);
    if (!value || !end || *end || parsed<=0 || parsed>INT_MAX || strcmp(value,canonical)) goto done;
    teacher=(int)parsed; sqlite3_finalize(s); s=NULL;
    if (strcmp(role,"student") && (strcmp(role,"teacher") || teacher!=user)) { error="AI_TEACHER_ONLY"; goto done; }
    if (identity(cfg->db,teacher,"teacher",owner) || load(cfg->db,teacher,&slot)) goto done;
    if (slot.exists && strcmp(slot.owner,owner)) goto done;
    if (usage(cfg->db,user,teacher,day,&used)) goto done;
    error=!slot.exists ? "AI_API_NOT_CONFIGURED" : !slot.enabled ? "AI_API_DISABLED" : NULL;
    if (!error && !strcmp(role,"student") && used>=cfg->ai_student_daily_limit) error="AI_QUOTA_EXCEEDED";
    if (error) { *access=access_json(cfg,teacher,!strcmp(role,"student"),used,day,error); goto done; }
    if (crypt(cfg,teacher,&slot,execution->api_key,0)) { error=CRYPTO_UNAVAILABLE; goto done; }
    execution->teacher_id=teacher; execution->version=slot.version;
    memcpy(execution->model,slot.model,strlen(slot.model)+1);
    if (charge && !strcmp(role,"student")) {
        if (prepare(cfg->db,"INSERT INTO teacher_ai_usage(student_id,teacher_id,day,used) VALUES(?,?,?,1) "
            "ON CONFLICT(student_id,teacher_id,day) DO UPDATE SET used=used+1 WHERE used<?",&s)) { error=UNAVAILABLE; goto done; }
        sqlite3_bind_int(s,1,user); sqlite3_bind_int(s,2,teacher); sqlite3_bind_int64(s,3,day);
        sqlite3_bind_int(s,4,cfg->ai_student_daily_limit);
        if (sqlite3_step(s)!=SQLITE_DONE || sqlite3_changes(cfg->db)!=1) { error=UNAVAILABLE; goto done; }
        sqlite3_finalize(s); s=NULL;
        if (prepare(cfg->db,"INSERT INTO teacher_ai_charges(student_id,teacher_id,day,created_at) VALUES(?,?,?,?)",&s)) { error=UNAVAILABLE; goto done; }
        sqlite3_bind_int(s,1,user); sqlite3_bind_int(s,2,teacher); sqlite3_bind_int64(s,3,day); sqlite3_bind_int64(s,4,(sqlite3_int64)now);
        if (sqlite3_step(s)!=SQLITE_DONE) { error=UNAVAILABLE; goto done; }
        execution->charge_id=sqlite3_last_insert_rowid(cfg->db); used++;
        sqlite3_finalize(s); s=NULL;
    }
    *access=access_json(cfg,teacher,!strcmp(role,"student"),used,day,NULL);
    if (!*access) { error=UNAVAILABLE; goto done; }
    if (run(cfg->db,"COMMIT")) { error=UNAVAILABLE; goto done; }
done:
    sqlite3_finalize(s);
    if (error) {
        run(cfg->db,"ROLLBACK"); teacher_api_clear(execution);
        /* 存储/加密故障不得带出此前构造的allowed=true快照。 */
        if (teacher_api_error_status(error)>=500) { cJSON_Delete(*access); *access=NULL; }
    }
    return error;
}
int teacher_api_refund(sqlite3 *db,sqlite3_int64 id) {
    if (id<=0) return 0;
    if (run(db,"BEGIN IMMEDIATE")) return -1;
    sqlite3_stmt *s=NULL; int ok=0;
    if (prepare(db,"UPDATE teacher_ai_usage SET used=used-1 WHERE used>0 AND (student_id,teacher_id,day) IN "
        "(SELECT student_id,teacher_id,day FROM teacher_ai_charges WHERE id=? AND released=0)",&s)) goto done;
    sqlite3_bind_int64(s,1,id); if (sqlite3_step(s)!=SQLITE_DONE) goto done;
    sqlite3_finalize(s); s=NULL;
    if (prepare(db,"UPDATE teacher_ai_charges SET released=1 WHERE id=? AND released=0",&s)) goto done;
    sqlite3_bind_int64(s,1,id); if (sqlite3_step(s)!=SQLITE_DONE) goto done;
    sqlite3_finalize(s); s=NULL;
    if (run(db,"COMMIT")) goto done;
    ok=1;
done:
    sqlite3_finalize(s); if (!ok) run(db,"ROLLBACK"); return ok ? 0 : -1;
}
void teacher_api_clear(TeacherExecution *execution) { OPENSSL_cleanse(execution,sizeof(*execution)); }
void teacher_api_clear_request(cJSON *request) {
    cJSON *execution=cJSON_GetObjectItemCaseSensitive(request,"execution");
    cJSON *key=cJSON_GetObjectItemCaseSensitive(execution,"api_key");
    if (cJSON_IsString(key)) OPENSSL_cleanse(key->valuestring,strlen(key->valuestring));
}
int teacher_api_error_status(const char *code) {
    if (!strcmp(code,"INVALID_REQUEST") || !strcmp(code,"QUESTION_REQUIRED")) return 400;
    if (!strcmp(code,"QUESTION_NOT_FOUND") || !strcmp(code,"AI_API_NOT_CONFIGURED")) return 404;
    if (!strcmp(code,"AI_API_DISABLED") || !strcmp(code,"AI_TEACHER_ONLY")) return 403;
    if (!strcmp(code,"AI_QUOTA_EXCEEDED")) return 429;
    return 503;
}
