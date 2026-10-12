#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <sqlite3.h>

/* OpenSSL SHA-256（通过 -lcrypto 链接；libcurl-dev 已间接依赖 libssl-dev） */
#include <openssl/sha.h>

#include "cJSON.h"
#include "db.h"
#include "kv_store.h"

/* ── SHA-256 工具 ── */

/* 计算 SHA-256(salt || password) 的十六进制字符串。
 * 返回值由 malloc 分配，调用者 free()。 */
static char *hash_password(const char *password, const char *salt) {
    if (password == NULL) password = "";
    if (salt == NULL) salt = "";

    size_t plen = strlen(password);
    size_t slen = strlen(salt);
    size_t total = plen + slen;
    char *input = malloc(total + 1);
    if (input == NULL) return NULL;

    memcpy(input, salt, slen);
    memcpy(input + slen, password, plen);
    input[total] = '\0';

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)input, total, digest);
    free(input);

    char *hex = malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    if (hex == NULL) return NULL;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    return hex;
}

/* ── 确保目录存在 ── */

static int ensure_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    const char *sep = (slash != NULL && bs != NULL) ? (slash > bs ? slash : bs)
                    : (slash != NULL ? slash : bs);
    if (sep == NULL || sep == path) return 0;

    size_t len = (size_t)(sep - path);
    if (len >= 512) return -1;
    char dir[512];
    memcpy(dir, path, len);
    dir[len] = '\0';

    struct stat st;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (errno != ENOENT) return -1;
    return mkdir(dir, 0755);
}

/* ── 插入 demo 账号（密码来自环境变量，有默认值） ── */

static int seed_demo_accounts(sqlite3 *db) {
    /* 密码从环境变量读取，不在源码中硬编码明文敏感值 */
    const char *admin_pw   = getenv("DEMO_ADMIN_PW");
    const char *teacher_pw = getenv("DEMO_TEACHER_PW");
    const char *student_pw = getenv("DEMO_STUDENT_PW");
    if (admin_pw == NULL)   admin_pw   = "admin123";
    if (teacher_pw == NULL) teacher_pw = "teacher123";
    if (student_pw == NULL) student_pw = "student123";

    /* 固定 salt 用于 demo（对抗彩虹表，salt 不要求保密） */
    const char *salt = "ai-question-bank-demo-salt-2026";

    struct {
        const char *username;
        const char *name;
        const char *role;
        const char *pw;
    } demos[] = {
        {"admin",   "系统管理员", "admin",   admin_pw},
        {"teacher", "张老师",     "teacher", teacher_pw},
        {"student", "李同学",     "student", student_pw},
    };
    int count = sizeof(demos) / sizeof(demos[0]);

    const char *sql = "INSERT OR IGNORE INTO users "
                      "(username, name, role, password_hash) "
                      "VALUES (?, ?, ?, ?)";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✘ 预编译 demo 插入语句失败: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    for (int i = 0; i < count; i++) {
        char *hash = hash_password(demos[i].pw, salt);
        if (hash == NULL) {
            fprintf(stderr, "✘ 密码哈希分配失败\n");
            sqlite3_finalize(stmt);
            return -1;
        }

        sqlite3_bind_text(stmt, 1, demos[i].username, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, demos[i].name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, demos[i].role, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, hash, -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "⚠ 插入 demo 账号 '%s' 失败: %s\n",
                    demos[i].username, sqlite3_errmsg(db));
        }

        sqlite3_reset(stmt);
        free(hash);
    }

    sqlite3_finalize(stmt);
    return 0;
}

/* ── 固定题目 seed（使用 kv_store） ── */

int db_seed_questions(sqlite3 *db) {
    static const struct {
        const char *key;         /* 数字 ID */
        const char *seed_key;    /* 原始 seed_key（幂等检测用） */
        const char *content;
        const char *options;
        const char *answer;
        const char *analysis;
        const char *knowledge_points;
        const char *type;
    } questions[] = {
        {"1", "demo-linear-equation", "解方程：2x + 3 = 11，x 的值是多少？",
         "[{\"label\":\"A\",\"text\":\"2\"},{\"label\":\"B\",\"text\":\"3\"},{\"label\":\"C\",\"text\":\"4\"},{\"label\":\"D\",\"text\":\"5\"}]",
         "C", "两边减去 3 得到 2x = 8，再除以 2，得到 x = 4。",
         "[\"一元一次方程\",\"等式性质\"]", "single_choice"},
        {"2", "demo-derivative", "求函数 f(x) = x^2 的导数。", "[]",
         "f'(x) = 2x", "由幂函数求导公式 (x^n)' = n*x^(n-1)，得到 f'(x) = 2x。",
         "[\"导数\",\"幂函数求导\"]", "short_answer"},
        {"3", "demo-limit", "当 x 趋近于 0 时，sin(x)/x 的极限是多少？（x 使用弧度制）",
         "[{\"label\":\"A\",\"text\":\"0\"},{\"label\":\"B\",\"text\":\"1\"},{\"label\":\"C\",\"text\":\"无穷大\"},{\"label\":\"D\",\"text\":\"不存在\"}]",
         "B", "这是重要极限：在弧度制下，lim(x→0) sin(x)/x = 1。",
         "[\"极限\",\"三角函数\"]", "single_choice"},
        {"4", "demo-newton-law", "质量为 2 kg 的物体，加速度为 3 m/s^2，所受合力是多少？",
         "[{\"label\":\"A\",\"text\":\"1.5 N\"},{\"label\":\"B\",\"text\":\"5 N\"},{\"label\":\"C\",\"text\":\"6 N\"},{\"label\":\"D\",\"text\":\"9 N\"}]",
         "C", "根据牛顿第二定律 F = ma，合力为 2 × 3 = 6 N。",
         "[\"牛顿第二定律\",\"力与运动\"]", "single_choice"},
        {"5", "demo-c-sizeof-char", "在 C 语言中，sizeof(char) 的值是多少？",
         "[{\"label\":\"A\",\"text\":\"1\"},{\"label\":\"B\",\"text\":\"2\"},{\"label\":\"C\",\"text\":\"4\"},{\"label\":\"D\",\"text\":\"由指针大小决定\"}]",
         "A", "C 标准规定 sizeof(char) 为 1；一个字节的位数由 CHAR_BIT 决定。",
         "[\"C 语言\",\"sizeof 运算符\",\"数据类型\"]", "single_choice"},
    };
    int count = sizeof(questions) / sizeof(questions[0]);

    /* 使用 "qdata" namespace 避免与 db_init 创建的 questions 表冲突 */
    char *check = kv_get(db, "qdata", "1", "content");
    if (check != NULL) { free(check); return 0; }

    for (int i = 0; i < count; i++) {
        if (kv_set(db, "qdata", questions[i].key, "content", questions[i].content) != 0 ||
            kv_set(db, "qdata", questions[i].key, "options", questions[i].options) != 0 ||
            kv_set(db, "qdata", questions[i].key, "answer", questions[i].answer) != 0 ||
            kv_set(db, "qdata", questions[i].key, "analysis", questions[i].analysis) != 0 ||
            kv_set(db, "qdata", questions[i].key, "type", questions[i].type) != 0 ||
            kv_set(db, "qdata", questions[i].key, "knowledge_points", questions[i].knowledge_points) != 0 ||
            kv_set(db, "qdata", questions[i].key, "seed_key", questions[i].seed_key) != 0 ||
            kv_set(db, "qdata", questions[i].key, "teacher_id", "1") != 0 ||
            kv_set(db, "qdata", questions[i].key, "status", "active") != 0) {
            fprintf(stderr, "✘ 写入题目 '%s' 失败\n", questions[i].key);
            return -1;
        }
    }
    return 0;
}

/* ── 公开 API ── */

int db_init(const char *path) {
    if (ensure_parent_dir(path) != 0) {
        fprintf(stderr, "✘ 无法创建数据库目录（%s）\n", path);
        return -1;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✘ 打开数据库失败: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return rc;
    }

    /* 框架阶段基础表 */
    static const char *schema =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  role TEXT NOT NULL DEFAULT 'student',"
        "  name TEXT NOT NULL,"
        "  username TEXT NOT NULL UNIQUE,"
        "  password_hash TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'active',"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");"
        "CREATE TABLE IF NOT EXISTS questions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  teacher_id INTEGER NOT NULL,"
        "  content TEXT NOT NULL,"
        "  answer TEXT DEFAULT '',"
        "  analysis TEXT DEFAULT '',"
        "  tags TEXT DEFAULT '',"
        "  status TEXT NOT NULL DEFAULT 'active',"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");";

    char *err = NULL;
    rc = sqlite3_exec(db, schema, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "✘ 建表失败: %s\n", err != NULL ? err : "unknown");
        sqlite3_free(err);
        sqlite3_close(db);
        return rc;
    }

    /* 插入演示账号 */
    rc = seed_demo_accounts(db);
    if (rc != 0) {
        fprintf(stderr, "✘ 插入演示账号失败\n");
        sqlite3_close(db);
        return rc;
    }

    sqlite3_close(db);
    return 0;
}

char *db_query_user_by_username(const char *username) {
    if (username == NULL) return NULL;

    sqlite3 *db = NULL;
    const char *path = getenv("DATABASE_PATH");
    if (path == NULL) path = "data/ai_question_bank.db";

    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "✘ 打开数据库失败（查询用户）: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    const char *sql = "SELECT id, username, name, role FROM users "
                      "WHERE username = ? AND status = 'active' LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✘ 预编译查询用户语句失败\n");
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id          = sqlite3_column_int(stmt, 0);
        const char *u   = (const char *)sqlite3_column_text(stmt, 1);
        const char *n   = (const char *)sqlite3_column_text(stmt, 2);
        const char *r   = (const char *)sqlite3_column_text(stmt, 3);

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", id);
        cJSON_AddStringToObject(obj, "username", u ? u : "");
        cJSON_AddStringToObject(obj, "name", n ? n : "");
        cJSON_AddStringToObject(obj, "role", r ? r : "");
        result = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

char *db_query_password_hash(const char *username) {
    if (username == NULL) return NULL;

    sqlite3 *db = NULL;
    const char *path = getenv("DATABASE_PATH");
    if (path == NULL) path = "data/ai_question_bank.db";

    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "✘ 打开数据库失败（查询密码）: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    const char *sql = "SELECT password_hash FROM users "
                      "WHERE username = ? AND status = 'active' LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "✘ 预编译查询密码语句失败\n");
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        if (hash) result = strdup(hash);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

