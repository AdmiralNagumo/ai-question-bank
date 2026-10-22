#include <assert.h>
#include <stdio.h>
#include "ai_access.h"
#include "kv_store.h"

static void sql(sqlite3 *db, const char *value) { assert(sqlite3_exec(db, value, NULL, NULL, NULL) == SQLITE_OK); }
int main(void) {
    sqlite3 *db = NULL; AiAccess state;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(ai_access_init(db) == 0 && ai_access_init(db) == 0);
    assert(kv_init(db) == 0 && kv_set(db,"users","student","id","3") == 0);
    assert(ai_access_get(db,3,100,&state) == 0 && state.status == AI_ACCESS_NOT_GRANTED);
    sql(db,"INSERT INTO ai_access_grants VALUES(3,'student',1,NULL,100);");
    assert(ai_access_get(db,3,100,&state) == 0 && state.status == AI_ACCESS_ACTIVE && !state.has_expiry);
    sql(db,"UPDATE ai_access_grants SET expires_at=101;");
    assert(ai_access_get(db,3,100,&state) == 0 && state.status == AI_ACCESS_ACTIVE && state.expires_at == 101);
    assert(ai_access_get(db,3,101,&state) == 0 && state.status == AI_ACCESS_EXPIRED);
    sql(db,"UPDATE ai_access_grants SET enabled=0;");
    assert(ai_access_get(db,3,101,&state) == 0 && state.status == AI_ACCESS_REVOKED);
    sql(db,"UPDATE ai_access_grants SET enabled=1;");
    assert(kv_set(db,"users","other","id","3") == 0);
    assert(ai_access_get(db,3,100,&state) == 0 && state.status == AI_ACCESS_NOT_GRANTED);
    assert(kv_delete_key(db,"users","other") == 0);
    sql(db,"UPDATE ai_access_grants SET user_key='deleted';");
    assert(ai_access_get(db,3,100,&state) == 0 && state.status == AI_ACCESS_NOT_GRANTED);
    sql(db,"UPDATE ai_access_grants SET user_key='student'; PRAGMA ignore_check_constraints=ON; UPDATE ai_access_grants SET enabled=99;");
    assert(ai_access_get(db,3,100,&state) == -1 && state.status == AI_ACCESS_NOT_GRANTED);
    sql(db,"DROP TABLE ai_access_grants;");
    assert(ai_access_get(db,3,100,&state) == -1);
    assert(ai_access_get(NULL,3,100,&state) == -1 && ai_access_get(db,0,100,&state) == -1);
    assert(ai_access_get(db,3,100,NULL) == -1);
    sqlite3_close(db);
    puts("PASS: 默认拒绝、永久/到期边界、撤销优先、身份绑定与重复ID、损坏数据和存储故障");
    return 0;
}
