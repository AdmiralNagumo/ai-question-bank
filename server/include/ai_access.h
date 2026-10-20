#ifndef AIQB_AI_ACCESS_H
#define AIQB_AI_ACCESS_H
#include <sqlite3.h>

typedef enum { AI_ACCESS_NOT_GRANTED, AI_ACCESS_ACTIVE, AI_ACCESS_REVOKED, AI_ACCESS_EXPIRED } AiAccessStatus;
typedef struct {
    AiAccessStatus status;
    int has_expiry;
    sqlite3_int64 expires_at;
} AiAccess;

/* 在main持有的共享连接初始化；不授予任何账号权限。0成功，-1失败。 */
int ai_access_init(sqlite3 *db);
/* 只读检查，now为UTC Unix秒。错误返回-1，调用者必须拒绝；不缓存、不读取模型凭证。 */
int ai_access_get(sqlite3 *db, int user_id, sqlite3_int64 now, AiAccess *out);
const char *ai_access_status_name(AiAccessStatus status);
const char *ai_access_error_code(AiAccessStatus status);
#endif
