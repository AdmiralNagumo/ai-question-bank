#ifndef AIQB_CONFIG_H
#define AIQB_CONFIG_H

#include <sqlite3.h>

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
    const char *ai_base_url; /* 大模型 API 地址，OpenAI 兼容协议（AI_API_BASE_URL） */
    const char *ai_api_key;  /* 后端统一持有的 API Key（AI_API_KEY），永不暴露前端 */
    const char *ai_model;    /* 默认模型名（AI_MODEL） */
} AppConfig;

AppConfig config_load(void);

#endif /* AIQB_CONFIG_H */
