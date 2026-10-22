#include "ai_access.h"
#include <stddef.h>

int ai_access_init(sqlite3 *db) {
    if (db == NULL) return -1;
    const char *schema =
        "CREATE TABLE IF NOT EXISTS ai_access_grants("
        " user_id INTEGER PRIMARY KEY CHECK(user_id BETWEEN 1 AND 2147483647),"
        " user_key TEXT NOT NULL,"
        " enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),"
        " expires_at INTEGER CHECK(expires_at IS NULL OR (typeof(expires_at)='integer' AND expires_at BETWEEN 1 AND 253402300799)),"
        " updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS ai_access_audit("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, user_key TEXT NOT NULL,"
        " action TEXT NOT NULL CHECK(action IN('grant','revoke')), expires_at INTEGER,"
        " actor TEXT NOT NULL, reason TEXT NOT NULL, created_at INTEGER NOT NULL);";
    return sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int ai_access_get(sqlite3 *db, int user_id, sqlite3_int64 now, AiAccess *out) {
    if (out == NULL) return -1;
    *out = (AiAccess){ .status = AI_ACCESS_NOT_GRANTED };
    if (db == NULL || user_id <= 0 || now < 0) return -1;
    /* 授权关联现存账号，拒绝当前库中的重复ID；SQL均使用绑定参数。 */
    const char *sql =
        "SELECT g.enabled,g.expires_at FROM ai_access_grants g WHERE g.user_id=?1 "
        "AND EXISTS(SELECT 1 FROM users u WHERE u.key_name=g.user_key AND u.value_name='id' AND u.value=CAST(?1 AS TEXT)) "
        "AND (SELECT count(*) FROM users u WHERE u.value_name='id' AND u.value=CAST(?1 AS TEXT))=1";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
    int result = -1;
    if (sqlite3_bind_int(statement, 1, user_id) != SQLITE_OK) goto done;
    int rc = sqlite3_step(statement);
    if (rc == SQLITE_DONE) { result = 0; goto done; }
    if (rc != SQLITE_ROW || sqlite3_column_type(statement, 0) != SQLITE_INTEGER) goto done;
    int enabled = sqlite3_column_int(statement, 0);
    if (enabled != 0 && enabled != 1) goto done;
    out->has_expiry = sqlite3_column_type(statement, 1) != SQLITE_NULL;
    if (out->has_expiry) {
        if (sqlite3_column_type(statement, 1) != SQLITE_INTEGER) goto done;
        out->expires_at = sqlite3_column_int64(statement, 1);
        if (out->expires_at <= 0 || out->expires_at > 253402300799LL) goto done;
    }
    out->status = !enabled ? AI_ACCESS_REVOKED : out->has_expiry && out->expires_at <= now ? AI_ACCESS_EXPIRED : AI_ACCESS_ACTIVE;
    result = 0;
done:
    sqlite3_finalize(statement);
    if (result != 0) *out = (AiAccess){ .status = AI_ACCESS_NOT_GRANTED };
    return result;
}

const char *ai_access_status_name(AiAccessStatus status) {
    switch (status) {
        case AI_ACCESS_ACTIVE: return "active";
        case AI_ACCESS_REVOKED: return "revoked";
        case AI_ACCESS_EXPIRED: return "expired";
        default: return "not_granted";
    }
}
const char *ai_access_error_code(AiAccessStatus status) {
    switch (status) {
        case AI_ACCESS_REVOKED: return "AI_ACCESS_REVOKED";
        case AI_ACCESS_EXPIRED: return "AI_ACCESS_EXPIRED";
        default: return "AI_ACCESS_REQUIRED";
    }
}
