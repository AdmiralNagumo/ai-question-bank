/*
 * kv_store 题目 seed 回归测试
 * 验证 db_seed_questions 幂等、数据完整、KV 存储可读。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#include "kv_store.h"
#include "db.h"

#define CHECK(expr) do {                                        \
    if (!(expr)) {                                              \
        fprintf(stderr, "[FAIL] %s:%d: %s\n",                   \
                __FILE__, __LINE__, #expr);                     \
        exit(1);                                                \
    }                                                           \
} while (0)

int main(int argc, char **argv) {
    (void)argc;
    const char *path = argv[1];

    CHECK(db_init(path) == 0);

    sqlite3 *db;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(kv_init(db) == 0);

    CHECK(db_seed_questions(db) == 0);

    int count;
    char **keys = kv_list_keys(db, "qdata", &count);
    CHECK(keys != NULL && count == 5);
    kv_free_str_array(keys);

    char *content = kv_get(db, "qdata", "1", "content");
    CHECK(content != NULL && strstr(content, "2x + 3 = 11") != NULL);
    free(content);

    char *answer = kv_get(db, "qdata", "1", "answer");
    CHECK(answer != NULL && strcmp(answer, "C") == 0);
    free(answer);

    CHECK(db_seed_questions(db) == 0);
    keys = kv_list_keys(db, "qdata", &count);
    CHECK(count == 5);
    kv_free_str_array(keys);

    char *kp = kv_get(db, "qdata", "1", "knowledge_points");
    CHECK(kp != NULL && kp[0] == '[');
    free(kp);

    char *tid = kv_get(db, "qdata", "1", "teacher_id");
    CHECK(tid != NULL && strcmp(tid, "1") == 0);
    free(tid);

    printf("PASS: 5 道题 seed 成功，幂等正确，字段完整可读\n");
    sqlite3_close(db);
    return 0;
}
