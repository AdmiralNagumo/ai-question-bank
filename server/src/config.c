#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "config.h"

/* 从 server/ 工作目录自动加载 .env 文件（若存在），
 * 不覆盖已有环境变量（setenv overwrite=0）。
 * 使 make run / 直接运行二进制都能自动读取配置。 */
static void load_env_file(void) {
    FILE *f = fopen(".env", "r");
    if (f == NULL) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* 跳过注释和空行 */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char *eq = strchr(line, '=');
        if (eq == NULL) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* 去掉 value 尾部换行符 */
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) {
            val[--vlen] = '\0';
        }

        /* 去掉 key 尾部空白 */
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
            key[--klen] = '\0';
        }

        if (key[0] == '\0') continue;

        setenv(key, val, 0); /* 不覆盖已存在的环境变量 */
    }
    fclose(f);
}

static const char *env_or(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* 禁止0（libcurl表示不限时）、负数、溢出和尾随垃圾；日志不输出原始值。 */
static long timeout_ms(const char *key, long fallback) {
    const char *value = getenv(key);
    if (value == NULL || value[0] == '\0') return fallback;
    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') goto invalid;
    }
    errno = 0;
    char *end;
    long parsed = strtol(value, &end, 10);
    if (errno == 0 && *end == '\0' && parsed >= 1 && parsed <= AI_SERVICE_TIMEOUT_MS_MAX) return parsed;
invalid:
    fprintf(stderr, "[config] invalid %s; using default %ld ms (allowed 1-%ld)\n",
            key, fallback, AI_SERVICE_TIMEOUT_MS_MAX);
    return fallback;
}

AppConfig config_load(void) {
    const char *skip_env = getenv("AQB_SKIP_ENV_FILE");
    if (skip_env == NULL || strcmp(skip_env, "1") != 0) load_env_file();
    AppConfig cfg = {0};
    cfg.port = atoi(env_or("PORT", "8000"));
    if (cfg.port <= 0 || cfg.port > 65535) cfg.port = 8000;
    cfg.db_path = env_or("DATABASE_PATH", "data/ai_question_bank.db");
    cfg.web_root = env_or("WEB_ROOT", "../web");
    cfg.ai_service_url = env_or("AI_SERVICE_URL", "http://127.0.0.1:8001");
    cfg.ai_service_token = env_or("AI_SERVICE_TOKEN", "");
    cfg.ai_connect_timeout_ms = timeout_ms("AI_CONNECT_TIMEOUT_MS", AI_CONNECT_TIMEOUT_MS_DEFAULT);
    cfg.ai_request_timeout_ms = timeout_ms("AI_REQUEST_TIMEOUT_MS", AI_REQUEST_TIMEOUT_MS_DEFAULT);
    return cfg;
}
