#include <stdio.h>
#include <sqlite3.h>

#include "config.h"
#include "db.h"
#include "http_server.h"
#include "kv_store.h"

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

    printf("✔ 数据库就绪: %s\n", cfg.db_path);

    int ret = http_server_start(&cfg);

    sqlite3_close(cfg.db);
    return ret;
}
