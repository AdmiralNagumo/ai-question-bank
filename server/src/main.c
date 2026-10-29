#include <stdio.h>
#include <curl/curl.h>
#include <sqlite3.h>

#include "config.h"
#include "db.h"
#include "http_server.h"
#include "kv_store.h"
#include "ai_access.h"
#include "teacher_api.h"

int main(void) {
    AppConfig cfg = config_load();

    int rc = db_init(cfg.db_path);
    if (rc != 0) {
        fprintf(stderr, "✘ 数据库初始化失败（%s）\n", cfg.db_path);
        return 1;
    }

    /* 打开共享数据库连接，供 kv_store 和 routes 使用 */
    rc = sqlite3_open(cfg.db_path, &cfg.db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✘ 打开数据库连接失败: %s\n", sqlite3_errmsg(cfg.db));
        sqlite3_close(cfg.db);
        return 1;
    }

    rc = kv_init(cfg.db);
    if (rc != 0) {
        fprintf(stderr, "✘ kv_store 初始化失败\n");
        sqlite3_close(cfg.db);
        return 1;
    }

    /* 将固定题目 seed 写入 kv_store */
    rc = db_seed_questions(cfg.db);
    if (rc != 0) {
        fprintf(stderr, "✘ 固定题目 seed 失败\n");
        sqlite3_close(cfg.db);
        return 1;
    }

    /* seed demo 账号（通过 kv_store） */
    rc = db_seed_users(cfg.db);
    if (rc != 0) {
        fprintf(stderr, "✘ demo 账号 seed 失败\n");
        sqlite3_close(cfg.db);
        return 1;
    }

    printf("✔ 数据库就绪: %s\n", cfg.db_path);
    if (ai_access_init(cfg.db) != 0) {
        fprintf(stderr, "✘ AI 授权存储初始化失败\n");
        sqlite3_close(cfg.db); return 1;
    }

    if (teacher_api_init(cfg.db) != 0) {
        fprintf(stderr,"教师API存储初始化失败\n"); sqlite3_close(cfg.db); return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        sqlite3_close(cfg.db); return 1;
    }
    int ret = http_server_start(&cfg);
    curl_global_cleanup();

    sqlite3_close(cfg.db);
    return ret;
}
