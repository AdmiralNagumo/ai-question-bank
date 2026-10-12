#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "cJSON.h"
#include "debug.h"
#include "kv_store.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部工具函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 将 namespace 转化为安全的 SQL 表名。
 *        只保留字母、数字和下划线；若原始字符串含有不允许的字符，
 *        或首字符是数字，或结果为空，返回空串 ""。
 * @param ns  namespace 字符串。
 * @return const char*  指向静态缓冲区的指针（每次调用覆盖，不可保留跨调用引用）。
 *                      返回 "" 表示名称非法。
 * @note  返回值不可 free()，不可跨两次调用保存。
 */
/* 最大合法 namespace 长度（字符数，不含前缀） */
#define MAX_NS_LEN 128

static const char *safe_table_name(const char *ns) {
    static char buf[256];
    int j = 0;
    int has_invalid = 0;
    int overlong = 0;
    for (int i = 0; ns[i] != '\0'; i++) {
        if (i >= MAX_NS_LEN) { overlong = 1; break; }
        if (j >= (int)sizeof(buf) - 1) { overlong = 1; break; }
        char c = ns[i];
        if (isalnum((unsigned char)c) || c == '_') {
            buf[j++] = c;
        } else {
            has_invalid = 1;
        }
    }
    buf[j] = '\0';

    /* 如果含有无效字符，或表名为空，或首字符是数字，或超长，返回无效标记 */
    if (has_invalid || j == 0 || (buf[0] >= '0' && buf[0] <= '9') || overlong) {
        buf[0] = '\0';
    }
    return buf;
}

/**
 * @brief 构建 INSERT OR REPLACE SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_upsert_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "INSERT OR REPLACE INTO %s (key_name, value_name, value) VALUES (?1, ?2, ?3)",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 构建单值查询 SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_get_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "SELECT value FROM %s WHERE key_name=?1 AND value_name=?2 LIMIT 1",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 构建全量查询 SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_get_all_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "SELECT value_name, value FROM %s WHERE key_name=?1 ORDER BY value_name",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 构建删除单值 SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_delete_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "DELETE FROM %s WHERE key_name=?1 AND value_name=?2",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 构建删除整条 key SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_delete_key_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "DELETE FROM %s WHERE key_name=?1",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 构建列举 key SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_list_keys_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "SELECT DISTINCT key_name FROM %s ORDER BY CAST(key_name AS INTEGER)",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 构建列举 value_name SQL 语句。
 * @param table  表名（已 sanitize）。
 * @param sql    输出缓冲区。
 * @param sql_sz 缓冲区大小。
 * @return int  0 成功；-1 缓冲区不足。
 */
static int build_list_values_sql(const char *table, char *sql, size_t sql_sz) {
    int n = snprintf(sql, sql_sz,
        "SELECT value_name FROM %s WHERE key_name=?1 ORDER BY value_name",
        table);
    return (n < 0 || (size_t)n >= sql_sz) ? -1 : 0;
}

/**
 * @brief 判断 namespace 是否已在 kv_tables 元信息表中注册。
 * @param db  SQLite 数据库句柄。
 * @param ns  namespace 名称。
 * @return int  1 已注册；0 未注册或出错。
 */
static int namespace_exists(sqlite3 *db, const char *ns) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT 1 FROM kv_tables WHERE namespace=?1 LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, ns, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

/**
 * @brief 将 namespace 注册到 kv_tables 元信息表。
 * @param db  SQLite 数据库句柄。
 * @param ns  namespace 名称。
 * @return int  0 成功；-1 失败（原因打印到 stderr）。
 * @note INSERT OR IGNORE，重复注册静默成功。
 */
static int register_namespace(sqlite3 *db, const char *ns) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO kv_tables (namespace) VALUES (?1)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✘ kv: 预编译注册 namespace 失败: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, ns, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "✘ kv: 注册 namespace 失败: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

/**
 * @brief 确保 namespace 对应的数据表存在；不存在则创建并注册元信息。
 * @param db  SQLite 数据库句柄。
 * @param ns  namespace 名称。
 * @return int  0 成功；-1 失败。
 */
static int ensure_table(sqlite3 *db, const char *ns) {
    const char *safe = safe_table_name(ns);
    if (safe[0] == '\0') {
        fprintf(stderr, "✘ kv: 无效 namespace 名称: '%s'\n", ns);
        return -1;
    }

    /* 尝试创建表（IF NOT EXISTS 是幂等的） */
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
        "CREATE TABLE IF NOT EXISTS %s ("
        "  key_name TEXT NOT NULL,"
        "  value_name TEXT NOT NULL,"
        "  value TEXT DEFAULT '',"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  updated_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  PRIMARY KEY (key_name, value_name)"
        ")",
        safe);
    if (n < 0 || (size_t)n >= sizeof(sql)) {
        fprintf(stderr, "✘ kv: 表名过长\n");
        return -1;
    }

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "✘ kv: 创建表 %s 失败: %s\n", safe, err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }

    KV_DEBUG("表 %s 已就绪 (namespace='%s')", safe, ns);

    /* 如果尚未注册，写入元信息表 */
    if (!namespace_exists(db, ns)) {
        return register_namespace(db, ns);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════════════════════════════ */

int kv_init(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS kv_tables ("
        "  namespace TEXT PRIMARY KEY,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ")";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "✘ kv_init: 创建 kv_tables 失败: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int kv_set(sqlite3 *db, const char *namespace,
           const char *key, const char *value_name, const char *value) {
    if (db == NULL || namespace == NULL || key == NULL ||
        value_name == NULL || value == NULL) {
        fprintf(stderr, "✘ kv_set: 参数为空\n");
        return -1;
    }

    if (ensure_table(db, namespace) != 0) return -1;

    const char *safe = safe_table_name(namespace);
    char sql[512];
    if (build_upsert_sql(safe, sql, sizeof(sql)) != 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✘ kv_set: 预编译失败: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "✘ kv_set: 写入失败: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    KV_DEBUG("SET %s/%s/%s = '%s'", namespace, key, value_name, value);
    return 0;
}

char *kv_get(sqlite3 *db, const char *namespace,
             const char *key, const char *value_name) {
    if (db == NULL || namespace == NULL || key == NULL || value_name == NULL)
        return NULL;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return NULL;

    char sql[512];
    if (build_get_sql(safe, sql, sizeof(sql)) != 0) return NULL;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value_name, -1, SQLITE_STATIC);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }

    sqlite3_finalize(stmt);
    return result;
}

char *kv_get_all(sqlite3 *db, const char *namespace, const char *key) {
    if (db == NULL || namespace == NULL || key == NULL) return NULL;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return NULL;

    char sql[512];
    if (build_get_all_sql(safe, sql, sizeof(sql)) != 0) return NULL;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *vn = (const char *)sqlite3_column_text(stmt, 0);
        const char *vl = (const char *)sqlite3_column_text(stmt, 1);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name",  vn ? vn : "");
        cJSON_AddStringToObject(item, "value", vl ? vl : "");
        cJSON_AddItemToArray(arr, item);
    }

    sqlite3_finalize(stmt);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int kv_delete(sqlite3 *db, const char *namespace,
              const char *key, const char *value_name) {
    if (db == NULL || namespace == NULL || key == NULL || value_name == NULL)
        return -1;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return -1;

    char sql[512];
    if (build_delete_sql(safe, sql, sizeof(sql)) != 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value_name, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int kv_delete_key(sqlite3 *db, const char *namespace, const char *key) {
    if (db == NULL || namespace == NULL || key == NULL) return -1;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return -1;

    char sql[512];
    if (build_delete_key_sql(safe, sql, sizeof(sql)) != 0) return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int kv_drop_namespace(sqlite3 *db, const char *namespace) {
    if (db == NULL || namespace == NULL) return -1;

    /* 只允许删除已在 kv_tables 注册的 namespace，防止误删普通表 */
    if (!namespace_exists(db, namespace)) return -1;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return -1;

    /* 删除数据表 */
    char sql[512];
    int n = snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s", safe);
    if (n < 0 || (size_t)n >= sizeof(sql)) return -1;

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "✘ kv_drop: 删除表 %s 失败: %s\n", safe, err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }

    /* 从元信息表删除 */
    sqlite3_stmt *stmt = NULL;
    const char *del_sql = "DELETE FROM kv_tables WHERE namespace=?1";
    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, namespace, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

char **kv_list_namespaces(sqlite3 *db, int *count) {
    if (count) *count = 0;
    if (db == NULL) return NULL;

    const char *sql = "SELECT namespace FROM kv_tables ORDER BY namespace";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    /* 先统计行数 */
    int cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cap++;
    sqlite3_reset(stmt);

    char **arr = calloc((size_t)(cap + 1), sizeof(char *));
    if (arr == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < cap) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        char *dup = val ? strdup(val) : strdup("");
        if (dup == NULL) break;
        arr[idx++] = dup;
    }
    arr[idx] = NULL;
    sqlite3_finalize(stmt);

    if (count) *count = idx;
    return arr;
}

char **kv_list_keys(sqlite3 *db, const char *namespace, int *count) {
    if (count) *count = 0;
    if (db == NULL || namespace == NULL) return NULL;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return NULL;

    char sql[512];
    if (build_list_keys_sql(safe, sql, sizeof(sql)) != 0) return NULL;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    int cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cap++;
    sqlite3_reset(stmt);

    char **arr = calloc((size_t)(cap + 1), sizeof(char *));
    if (arr == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < cap) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        char *dup = val ? strdup(val) : strdup("");
        if (dup == NULL) break;
        arr[idx++] = dup;
    }
    arr[idx] = NULL;
    sqlite3_finalize(stmt);

    if (count) *count = idx;
    return arr;
}

char **kv_list_value_names(sqlite3 *db, const char *namespace,
                           const char *key, int *count) {
    if (count) *count = 0;
    if (db == NULL || namespace == NULL || key == NULL) return NULL;

    const char *safe = safe_table_name(namespace);
    if (safe[0] == '\0') return NULL;

    char sql[512];
    if (build_list_values_sql(safe, sql, sizeof(sql)) != 0) return NULL;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    int cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) cap++;
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    char **arr = calloc((size_t)(cap + 1), sizeof(char *));
    if (arr == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < cap) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        char *dup = val ? strdup(val) : strdup("");
        if (dup == NULL) break;
        arr[idx++] = dup;
    }
    arr[idx] = NULL;
    sqlite3_finalize(stmt);

    if (count) *count = idx;
    return arr;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存管理工具
 * ═══════════════════════════════════════════════════════════════════════════ */

void kv_free_str_array(char **arr) {
    if (arr == NULL) return;
    for (int i = 0; arr[i] != NULL; i++) {
        free(arr[i]);
    }
    free(arr);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 类型转换工具
 * ═══════════════════════════════════════════════════════════════════════════ */

int kv_str_to_int(const char *str, int *out) {
    if (str == NULL || out == NULL) return -1;
    char *end = NULL;
    long val = strtol(str, &end, 10);
    if (end == str || *end != '\0' || val < INT_MIN || val > INT_MAX)
        return -1;
    *out = (int)val;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 以下类型转换函数当前未被任何调用方使用（死代码），保留注释以供参考。
 * 如需启用，取消注释并在 kv_store.h 中恢复对应的声明。
 * ═══════════════════════════════════════════════════════════════════════════ */
#if 0
int kv_str_to_double(const char *str, double *out) {
    if (str == NULL || out == NULL) return -1;
    char *end = NULL;
    double val = strtod(str, &end);
    if (end == str || *end != '\0')
        return -1;
    *out = val;
    return 0;
}

int kv_str_array_to_int(char **strs, int **out, int *count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    if (strs == NULL || out == NULL || count == NULL) return -1;

    /* 先统计元素数 */
    int n = 0;
    while (strs[n] != NULL) n++;

    /* 逐元素转换 */
    int *arr = calloc((size_t)n, sizeof(int));
    if (arr == NULL) return -1;

    for (int i = 0; i < n; i++) {
        if (kv_str_to_int(strs[i], &arr[i]) != 0) {
            free(arr);
            return -1;
        }
    }

    *out = arr;
    *count = n;
    return 0;
}

int kv_str_array_to_double(char **strs, double **out, int *count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    if (strs == NULL || out == NULL || count == NULL) return -1;

    int n = 0;
    while (strs[n] != NULL) n++;

    double *arr = calloc((size_t)n, sizeof(double));
    if (arr == NULL) return -1;

    for (int i = 0; i < n; i++) {
        if (kv_str_to_double(strs[i], &arr[i]) != 0) {
            free(arr);
            return -1;
        }
    }

    *out = arr;
    *count = n;
    return 0;
}
#endif /* 死代码结束 */
