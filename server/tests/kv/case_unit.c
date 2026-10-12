/*
 * kv_store 单元测试 case
 * 编译：gcc -std=c11 -Iinclude -Ithird_party case_unit.c ../../src/kv_store.c ../../third_party/cJSON.c -lsqlite3 -lm -o /tmp/kv_test
 * 运行：/tmp/kv_test
 *
 * 独立测试，不依赖服务器进程，使用 :memory: SQLite 数据库。
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "kv_store.h"

/* ── 测试工具 ── */

static int total_pass = 0;
static int total_fail = 0;

#define TEST(name)                                  \
    do {                                            \
        printf("  [TEST] %s ... ", name);           \
        fflush(stdout);                             \
    } while (0)

#define PASS() do {                                 \
    printf("✓\n");                                  \
    total_pass++;                                   \
} while (0)

#define FAIL(...) do {                              \
    printf("✘  ");                                  \
    printf(__VA_ARGS__);                            \
    printf("\n");                                   \
    total_fail++;                                   \
} while (0)

#define ASSERT_EQ_STR(actual, expected, msg)        \
    do {                                            \
        if ((actual) == NULL ||                     \
            strcmp((actual), (expected)) != 0) {     \
            FAIL("%s (got: '%s', expected: '%s')",  \
                 msg,                               \
                 (actual) ? (actual) : "NULL",      \
                 (expected));                       \
            free(actual);                           \
            return;                                 \
        }                                           \
        free(actual);                               \
    } while (0)

#define ASSERT_NULL(ptr, msg)                       \
    do {                                            \
        if ((ptr) != NULL) {                        \
            FAIL("%s (expected NULL)", msg);        \
            free(ptr);                              \
            return;                                 \
        }                                           \
    } while (0)

#define ASSERT_TRUE(cond, msg)                      \
    do {                                            \
        if (!(cond)) { FAIL("%s", msg); return; }   \
    } while (0)

/* ── 各测试用例 ── */

static void test_init(sqlite3 *db) {
    TEST("kv_init 正常初始化");
    ASSERT_TRUE(kv_init(db) == 0, "kv_init 应返回 0");

    /* 重复调用应幂等 */
    ASSERT_TRUE(kv_init(db) == 0, "kv_init 重复调用应仍成功");

    PASS();
}

static void test_set_and_get(sqlite3 *db) {
    TEST("kv_set + kv_get 基本读写");

    ASSERT_TRUE(kv_set(db, "config", "server", "port", "8080") == 0,
                 "kv_set config/server/port");
    ASSERT_TRUE(kv_set(db, "config", "server", "host", "0.0.0.0") == 0,
                 "kv_set config/server/host");
    ASSERT_TRUE(kv_set(db, "config", "database", "url",
                       "sqlite:///data/db") == 0,
                 "kv_set config/database/url");

    char *v = kv_get(db, "config", "server", "port");
    ASSERT_EQ_STR(v, "8080", "kv_get config/server/port → 8080");

    v = kv_get(db, "config", "server", "host");
    ASSERT_EQ_STR(v, "0.0.0.0", "kv_get config/server/host → 0.0.0.0");

    v = kv_get(db, "config", "database", "url");
    ASSERT_EQ_STR(v, "sqlite:///data/db", "kv_get config/database/url");

    PASS();
}

static void test_get_nonexistent(sqlite3 *db) {
    TEST("kv_get 不存在的键/值名");

    char *v = kv_get(db, "config", "server", "nonexistent");
    ASSERT_NULL(v, "不存在的 value_name 应返回 NULL");

    v = kv_get(db, "nonexistent_ns", "key", "val");
    ASSERT_NULL(v, "不存在的 namespace 应返回 NULL");

    PASS();
}

static void test_overwrite(sqlite3 *db) {
    TEST("kv_set 覆盖已有值");

    ASSERT_TRUE(kv_set(db, "config", "server", "port", "9090") == 0,
                 "覆盖 port → 9090");

    char *v = kv_get(db, "config", "server", "port");
    ASSERT_EQ_STR(v, "9090", "port 应为 9090");

    /* 恢复 */
    ASSERT_TRUE(kv_set(db, "config", "server", "port", "8080") == 0, "");
    v = kv_get(db, "config", "server", "port");
    ASSERT_EQ_STR(v, "8080", "port 恢复为 8080");

    PASS();
}

static void test_get_all(sqlite3 *db) {
    TEST("kv_get_all 获取 key 下所有值");

    char *json = kv_get_all(db, "config", "server");
    ASSERT_TRUE(json != NULL, "kv_get_all 应返回 JSON");

    ASSERT_TRUE(strstr(json, "\"name\":\"host\"") != NULL,
                 "JSON 应包含 host");
    ASSERT_TRUE(strstr(json, "\"value\":\"0.0.0.0\"") != NULL,
                 "JSON 应包含 0.0.0.0");
    ASSERT_TRUE(strstr(json, "\"name\":\"port\"") != NULL,
                 "JSON 应包含 port");
    ASSERT_TRUE(strstr(json, "\"value\":\"8080\"") != NULL,
                 "JSON 应包含 8080");
    free(json);

    /* 不存在的 key 应返回空数组 */
    json = kv_get_all(db, "config", "nonexistent_key");
    ASSERT_EQ_STR(json, "[]", "不存在的 key 应返回 []");

    PASS();
}

static void test_list_namespaces(sqlite3 *db) {
    TEST("kv_list_namespaces 列举所有 namespace");

    /* 确保已有 config，再添加 users */
    ASSERT_TRUE(kv_set(db, "users", "alice", "email",
                       "alice@example.com") == 0, "");
    ASSERT_TRUE(kv_set(db, "users", "bob", "role", "student") == 0, "");

    int count = 0;
    char **ns = kv_list_namespaces(db, &count);
    ASSERT_TRUE(ns != NULL, "list_namespaces 不应返回 NULL");

    int found_config = 0, found_users = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(ns[i], "config") == 0) found_config = 1;
        if (strcmp(ns[i], "users")  == 0) found_users  = 1;
        free(ns[i]);
    }
    free(ns);

    ASSERT_TRUE(found_config && found_users,
                 "应同时找到 config 和 users");

    PASS();
}

static void test_list_keys(sqlite3 *db) {
    TEST("kv_list_keys 列举 namespace 下所有 key");

    int count = 0;
    char **keys = kv_list_keys(db, "config", &count);
    ASSERT_TRUE(keys != NULL, "list_keys 不应返回 NULL");
    ASSERT_TRUE(count >= 2, "config 下至少应有 2 个 key");

    int found_server = 0, found_database = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(keys[i], "server")   == 0) found_server   = 1;
        if (strcmp(keys[i], "database") == 0) found_database  = 1;
        free(keys[i]);
    }
    free(keys);

    ASSERT_TRUE(found_server, "应找到 server");
    ASSERT_TRUE(found_database, "应找到 database");

    PASS();
}

static void test_list_value_names(sqlite3 *db) {
    TEST("kv_list_value_names 列举 key 下所有值名");

    int count = 0;
    char **vnames = kv_list_value_names(db, "users", "alice", &count);
    ASSERT_TRUE(vnames != NULL, "list_value_names 不应返回 NULL");
    ASSERT_TRUE(count == 1, "alice 应只有 1 个值名");

    ASSERT_TRUE(strcmp(vnames[0], "email") == 0,
                 "值名应为 email");
    free(vnames[0]);
    free(vnames);

    PASS();
}

static void test_delete(sqlite3 *db) {
    TEST("kv_delete 删除单个值名");

    ASSERT_TRUE(kv_delete(db, "users", "alice", "email") == 0,
                 "kv_delete email");

    char *v = kv_get(db, "users", "alice", "email");
    ASSERT_NULL(v, "删除后 email 应为 NULL");

    PASS();
}

static void test_delete_key(sqlite3 *db) {
    TEST("kv_delete_key 删除整个 key");

    ASSERT_TRUE(kv_delete_key(db, "users", "bob") == 0,
                 "kv_delete_key bob");

    char *v = kv_get(db, "users", "bob", "role");
    ASSERT_NULL(v, "删除后 bob/role 应为 NULL");

    int count = 0;
    char **keys = kv_list_keys(db, "users", &count);
    ASSERT_TRUE(keys != NULL, "list_keys 应成功");
    ASSERT_TRUE(count == 0, "users 下应无 key");
    free(keys);

    PASS();
}

static void test_drop_namespace(sqlite3 *db) {
    TEST("kv_drop_namespace 删除整个 namespace");

    ASSERT_TRUE(kv_drop_namespace(db, "users") == 0,
                 "kv_drop_namespace users");

    int count = 0;
    char **ns = kv_list_namespaces(db, &count);
    ASSERT_TRUE(ns != NULL, "list_namespaces 应成功");

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(ns[i], "users") == 0) found = 1;
        free(ns[i]);
    }
    free(ns);
    ASSERT_TRUE(!found, "users 应已被移除");
    ASSERT_TRUE(count == 1, "应只剩 1 个 namespace (config)");

    PASS();
}

static void test_invalid_namespace(sqlite3 *db) {
    TEST("无效 namespace 名拒绝写入");

    /* 含特殊字符 */
    int rc = kv_set(db, "my config", "k", "vn", "v");
    ASSERT_TRUE(rc != 0, "含空格的 namespace 应拒绝");

    /* 数字开头 */
    rc = kv_set(db, "1config", "k", "vn", "v");
    ASSERT_TRUE(rc != 0, "数字开头的 namespace 应拒绝");

    /* NULL 参数 */
    rc = kv_set(db, NULL, "k", "vn", "v");
    ASSERT_TRUE(rc != 0, "NULL namespace 应拒绝");

    PASS();
}

static void test_multiple_namespaces_independence(sqlite3 *db) {
    TEST("多 namespace 间数据隔离");

    ASSERT_TRUE(kv_set(db, "app1", "key_x", "color", "red") == 0, "");
    ASSERT_TRUE(kv_set(db, "app2", "key_x", "color", "blue") == 0, "");

    char *v1 = kv_get(db, "app1", "key_x", "color");
    ASSERT_EQ_STR(v1, "red", "app1 的值应为 red");

    char *v2 = kv_get(db, "app2", "key_x", "color");
    ASSERT_EQ_STR(v2, "blue", "app2 的值应为 blue");

    /* 清理 */
    kv_drop_namespace(db, "app1");
    kv_drop_namespace(db, "app2");

    PASS();
}

/* ── 主入口 ── */

static void test_free_str_array(sqlite3 *db) {
    (void)db;
    TEST("kv_free_str_array 释放字符串数组");

    /* 构造一个数组 */
    char **arr = calloc(4, sizeof(char *));
    arr[0] = strdup("alpha");
    arr[1] = strdup("beta");
    arr[2] = strdup("gamma");
    arr[3] = NULL;

    /* 释放（不应崩溃） */
    kv_free_str_array(arr);

    /* NULL 安全 */
    kv_free_str_array(NULL);

    PASS();
}

static void test_str_to_int(sqlite3 *db) {
    (void)db;
    TEST("kv_str_to_int");

    int v;
    ASSERT_TRUE(kv_str_to_int("42", &v) == 0 && v == 42,
                 "42 → 42");
    ASSERT_TRUE(kv_str_to_int("-7", &v) == 0 && v == -7,
                 "-7 → -7");
    ASSERT_TRUE(kv_str_to_int("0", &v) == 0 && v == 0,
                 "0 → 0");
    ASSERT_TRUE(kv_str_to_int("abc", &v) != 0,
                 "abc 应失败");
    ASSERT_TRUE(kv_str_to_int("12.5", &v) != 0,
                 "12.5 应失败");
    ASSERT_TRUE(kv_str_to_int("", &v) != 0,
                 "空串应失败");
    ASSERT_TRUE(kv_str_to_int(NULL, &v) != 0,
                 "NULL 应失败");

    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 以下测试函数对应 kv_store.c 中已注释的死代码函数。
 * 若恢复那些函数，取消下方注释并取消 main() 中对应的调用注释。
 * ═══════════════════════════════════════════════════════════════════════════ */
#if 0
static void test_str_to_double(sqlite3 *db) {
    (void)db;
    TEST("kv_str_to_double");

    double v;
    ASSERT_TRUE(kv_str_to_double("3.14", &v) == 0 && v > 3.13 && v < 3.15,
                 "3.14 → ~3.14");
    ASSERT_TRUE(kv_str_to_double("-2.5", &v) == 0 && v > -2.6 && v < -2.4,
                 "-2.5 → ~-2.5");
    ASSERT_TRUE(kv_str_to_double("0", &v) == 0 && v == 0.0,
                 "0 → 0.0");
    ASSERT_TRUE(kv_str_to_double("abc", &v) != 0,
                 "abc 应失败");
    ASSERT_TRUE(kv_str_to_double(NULL, &v) != 0,
                 "NULL 应失败");

    PASS();
}

static void test_str_array_to_int(sqlite3 *db) {
    (void)db;
    TEST("kv_str_array_to_int");

    char *arr[] = {"10", "20", "30", NULL};
    int *out = NULL;
    int cnt = 0;

    ASSERT_TRUE(kv_str_array_to_int(arr, &out, &cnt) == 0,
                 "全部合法应成功");
    ASSERT_TRUE(cnt == 3, "应转换 3 个元素");
    ASSERT_TRUE(out[0] == 10 && out[1] == 20 && out[2] == 30,
                 "值正确");
    free(out);

    /* 含非法元素 */
    char *bad[] = {"10", "NaN", "30", NULL};
    ASSERT_TRUE(kv_str_array_to_int(bad, &out, &cnt) != 0,
                 "含非法元素应失败");
    ASSERT_TRUE(out == NULL, "失败时 out 应为 NULL");

    /* 空数组 */
    char *empty[] = {NULL};
    ASSERT_TRUE(kv_str_array_to_int(empty, &out, &cnt) == 0,
                 "空数组应成功");
    ASSERT_TRUE(cnt == 0, "空数组 count = 0");
    ASSERT_TRUE(out != NULL, "空数组 out 不应为 NULL");
    free(out);

    PASS();
}

static void test_str_array_to_double(sqlite3 *db) {
    (void)db;
    TEST("kv_str_array_to_double");

    char *arr[] = {"1.5", "2.5", "3.5", NULL};
    double *out = NULL;
    int cnt = 0;

    ASSERT_TRUE(kv_str_array_to_double(arr, &out, &cnt) == 0,
                 "全部合法应成功");
    ASSERT_TRUE(cnt == 3, "应转换 3 个元素");
    ASSERT_TRUE(out[0] > 1.4 && out[0] < 1.6, "out[0] ≈ 1.5");
    ASSERT_TRUE(out[2] > 3.4 && out[2] < 3.6, "out[2] ≈ 3.5");
    free(out);

    /* 含非法元素 */
    char *bad[] = {"1.0", "xyz", NULL};
    ASSERT_TRUE(kv_str_array_to_double(bad, &out, &cnt) != 0,
                 "含非法元素应失败");

    PASS();
}
#endif

int main(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "无法打开 :memory: 数据库\n");
        return 1;
    }

    printf("=== kv_store 单元测试 ===\n\n");

    test_init(db);
    test_set_and_get(db);
    test_get_nonexistent(db);
    test_overwrite(db);
    test_get_all(db);
    test_list_namespaces(db);
    test_list_keys(db);
    test_list_value_names(db);
    test_delete(db);
    test_delete_key(db);
    test_drop_namespace(db);
    test_invalid_namespace(db);
    test_multiple_namespaces_independence(db);
    test_free_str_array(db);
    test_str_to_int(db);
/*
    test_str_to_double(db);
    test_str_array_to_int(db);
    test_str_array_to_double(db);
*/

    sqlite3_close(db);

    printf("\n=======================\n");
    printf("  通过: %d   失败: %d\n", total_pass, total_fail);
    printf("=======================\n");

    return total_fail > 0 ? 1 : 0;
}