#ifndef AIQB_CONFIG_H
#define AIQB_CONFIG_H

#include <sqlite3.h>

#define AI_CONNECT_TIMEOUT_MS_DEFAULT 500L
#define AI_REQUEST_TIMEOUT_MS_DEFAULT 2000L
#define AI_SERVICE_TIMEOUT_MS_MAX 60000L

/* 应用配置：从环境变量加载（未设置时使用默认值）。
 * systemd 部署时由 EnvironmentFile 加载 server/.env 注入。 */
typedef struct {
    int port;                /* HTTP 监听端口（PORT） */
    const char *db_path;     /* SQLite 数据库路径（DATABASE_PATH） */
    sqlite3 *db;             /* 共享数据库连接（db_init 后打开，http_server_start 期间持有）。
                                     * 注意：依赖 MHD 单线程串行假设
                                     * (MHD_USE_INTERNAL_POLLING_THREAD)，
                                     * 未加锁；若启用线程池需加 sqlite3_config(SERIALIZED) 保护。 */
    const char *web_root;    /* 静态前端根目录，相对 server/ 工作目录（WEB_ROOT） */
    const char *ai_service_url;   /* AI_SERVICE_URL：内部 Pi 服务地址 */
    const char *ai_service_token; /* AI_SERVICE_TOKEN：内部服务凭证 */
    const char *ai_routing_mode; /* teacher默认；platform仅兼容旧演示 */
    const char *ai_key_encryption_key; /* 64位hex，不写DB或日志 */
    int ai_student_daily_limit; /* 学生/教师/上海自然日 */
    long ai_connect_timeout_ms;  /* AI_CONNECT_TIMEOUT_MS：1–60000，默认500 */
    long ai_request_timeout_ms;  /* AI_REQUEST_TIMEOUT_MS：1–60000，默认2000；包含连接 */
} AppConfig;

AppConfig config_load(void);

#endif /* AIQB_CONFIG_H */
