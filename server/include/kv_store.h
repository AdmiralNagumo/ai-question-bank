#ifndef AIQB_KV_STORE_H
#define AIQB_KV_STORE_H

#include <sqlite3.h>

/**
 * @file kv_store.h
 * @brief 目录式键值对存储层
 *
 * 设计概念：
 *   - 一级目录（namespace）对应数据库内一张独立表 kv_{namespace}
 *   - 一个 namespace 内，一个 key 可通过 value_name 对应多个值
 *
 * 命名约束：
 *   namespace 只允许字母、数字、下划线（a-z A-Z 0-9 _），
 *   首字符不能是数字。不符合的名称返回 -1（无效参数）。
 *
 * 线程安全：
 *   本模块未加锁。若在多线程环境下使用同一个 sqlite3* 连接，
 *   调用者需自行保证串行化（如 SQLite 的 sqlite3_busy_timeout）。
 *
 * 所有权约定（重要）：
 *   - @c char* 返回值均由 malloc 分配，调用者必须 free()。
 *   - @c char** 数组（末尾 NULL 哨兵）中每个字符串及数组本身
 *     均由 malloc 分配，调用者需依次 free 每个元素后再 free 数组。
 *   - 传入的字符串参数（namespace, key, value_name, value）由调用者
 *     保证在函数返回前有效；函数内部会复制或绑定 SQL 参数。
 */

/* ── 生命周期 ── */

/**
 * @brief 初始化 kv 子系统：创建 kv_tables 元信息表。
 *        可在 db_init 之后调用一次，或在首次使用任意 kv API 时自动触发。
 * @param db  已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @return int  0 成功；-1 失败（原因打印到 stderr）。
 * @note 重复调用幂等。
 */
int kv_init(sqlite3 *db);

/* ── 读写接口 ── */

/**
 * @brief 写入一个值（INSERT OR REPLACE）。
 *        namespace 不存在时自动创建对应表和数据表。
 * @param db          已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace   一级目录名（只允许 a-zA-Z0-9_，首字符不能是数字）。
 * @param key         键名。
 * @param value_name  值名，同一 key 下唯一标识一个值。
 * @param value       值的文本内容。
 * @return int  0 成功；-1 失败（原因打印到 stderr）。
 * @note 若 (namespace+key+value_name) 已存在，原有 value 被覆盖。
 * @note 所有字符串参数不可为 NULL，否则返回 -1。
 */
int kv_set(sqlite3 *db,
           const char *namespace,
           const char *key,
           const char *value_name,
           const char *value);

/**
 * @brief 读取指定 value_name 的值。
 * @param db          已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace   一级目录名。
 * @param key         键名。
 * @param value_name  值名。
 * @return char*  找到返回 malloc 分配的字符串（调用者 free()）；
 *                未找到或出错返回 NULL。
 * @note 返回值由 strdup 分配，调用者负责 free()。
 * @note 参数不可为 NULL，否则返回 NULL。
 */
char *kv_get(sqlite3 *db,
             const char *namespace,
             const char *key,
             const char *value_name);

/**
 * @brief 读取某个 key 下所有 (value_name, value) 对。
 * @param db          已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace   一级目录名。
 * @param key         键名。
 * @return char*  JSON 数组字符串，格式为：
 *                 [{"name":"...","value":"..."}, ...]
 *                 没有数据时返回 "[]"；失败返回 NULL。
 * @note 返回值由 cJSON_PrintUnformatted 分配（malloc），调用者 free()。
 * @note namespace 或 key 为 NULL 时返回 NULL。
 */
char *kv_get_all(sqlite3 *db,
                 const char *namespace,
                 const char *key);

/* ── 删除接口 ── */

/**
 * @brief 删除某个 key 下的指定 value_name。
 * @param db          已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace   一级目录名。
 * @param key         键名。
 * @param value_name  要删除的值名。
 * @return int  0 成功；-1 失败。
 * @note 要删除的条目不存在时，SQLite 仍返回 DONE（视为成功）。
 * @note 参数不可为 NULL，否则返回 -1。
 */
int kv_delete(sqlite3 *db,
              const char *namespace,
              const char *key,
              const char *value_name);

/**
 * @brief 删除某个 key 下的所有值。
 * @param db          已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace   一级目录名。
 * @param key         要删除的键名。
 * @return int  0 成功；-1 失败。
 * @note 仅删除数据，不删除 namespace 表本身；被删空的 key 从列举中消失。
 * @note 参数不可为 NULL，否则返回 -1。
 */
int kv_delete_key(sqlite3 *db,
                  const char *namespace,
                  const char *key);

/**
 * @brief 删除整个 namespace（删除对应数据表和元信息记录）。
 * @param db          已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace   要删除的一级目录名。
 * @return int  0 成功；-1 失败。
 * @note DROP TABLE 后无法恢复，请谨慎使用。
 * @note namespace 不可为 NULL，否则返回 -1。
 */
int kv_drop_namespace(sqlite3 *db, const char *namespace);

/* ── 列举接口 ── */

/**
 * @brief 返回所有 namespace 名称的 NULL 结尾数组。
 * @param db     已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param count  输出参数，填入元素个数（不含末尾 NULL 哨兵）。
 *               可为 NULL（放弃计数）。
 * @return char**  NULL 结尾的字符串数组；
 *                  失败返回 NULL 并将 *count 置为 0。
 * @note 所有权：返回的数组及其中每个字符串均由 malloc 分配，
 *       调用者依次 free 每个元素后再 free 数组。
 */
char **kv_list_namespaces(sqlite3 *db, int *count);

/**
 * @brief 返回某个 namespace 下所有 key 名称的 NULL 结尾数组。
 * @param db         已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace  一级目录名。
 * @param count      输出参数，填入元素个数（不含 NULL 哨兵）。
 * @return char**  NULL 结尾的字符串数组；失败返回 NULL，*count = 0。
 * @note 所有权同 kv_list_namespaces。
 * @note namespace 不可为 NULL。
 */
char **kv_list_keys(sqlite3 *db, const char *namespace, int *count);

/**
 * @brief 返回某个 namespace 下某个 key 的所有 value_name 的 NULL 结尾数组。
 * @param db         已打开的 SQLite 数据库句柄（不可为 NULL）。
 * @param namespace  一级目录名。
 * @param key        键名。
 * @param count      输出参数，填入元素个数（不含 NULL 哨兵）。
 * @return char**  NULL 结尾的字符串数组；失败返回 NULL，*count = 0。
 * @note 所有权同 kv_list_namespaces。
 * @note namespace 和 key 均不可为 NULL。
 */
char **kv_list_value_names(sqlite3 *db,
                           const char *namespace,
                           const char *key,
                           int *count);

/* ── 内存管理工具 ── */

/**
 * @brief 释放 NULL 结尾的字符串数组（由 kv_list_* 返回）。
 * @param arr  kv_list_* 系列函数返回的数组，或 NULL（安全）。
 * @note 依次 free 每个元素后再 free 数组本身。传入 NULL 无操作。
 */
void kv_free_str_array(char **arr);

/* ── 类型转换工具 ── */

/**
 * @brief 将字符串解析为 int。
 * @param str  输入字符串。
 * @param out  输出整数。
 * @return int  0 成功；-1 失败（str 为 NULL 或非数字）。
 */
int kv_str_to_int(const char *str, int *out);


/* ═══════════════════════════════════════════════════════════════════════════
 * 以下类型转换函数当前未被任何调用方使用（死代码），保留注释以供参考。
 * 如需启用，取消注释并在 kv_store.c 中恢复对应的实现。
 * ═══════════════════════════════════════════════════════════════════════════ */

#if 0

/**
 * @brief 将字符串解析为 double。
 * @param str  输入字符串。
 * @param out  输出浮点数。
 * @return int  0 成功；-1 失败（str 为 NULL 或非数字）。
 */
int kv_str_to_double(const char *str, double *out);

/**
 * @brief 将 NULL 结尾的字符串数组逐个转换为 int 数组。
 * @param strs   NULL 结尾的字符串数组（如 kv_list_keys 返回值）。
 * @param out    输出参数，指向 malloc 分配的 int 数组（调用者 free）。
 * @param count  输出参数，转换的元素个数。
 * @return int  0 全部成功；-1 存在非数字项（此时 *out = NULL, *count = 0）。
 */
int kv_str_array_to_int(char **strs, int **out, int *count);

/**
 * @brief 将 NULL 结尾的字符串数组逐个转换为 double 数组。
 * @param strs   NULL 结尾的字符串数组。
 * @param out    输出参数，指向 malloc 分配的 double 数组（调用者 free）。
 * @param count  输出参数，转换的元素个数。
 * @return int  0 全部成功；-1 存在非数字项（此时 *out = NULL, *count = 0）。
 */
int kv_str_array_to_double(char **strs, double **out, int *count);

#endif /* 死代码 */

#endif /* AIQB_KV_STORE_H */