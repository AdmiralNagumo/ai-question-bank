#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <openssl/rand.h>

#include "auth.h"

/* ── 内存 Session 存储（固定数组，单线程 demo 够用） ── */
static Session g_sessions[MAX_SESSIONS];
static int g_session_count = 0;
static int g_create_counter = 0;       /* 用于控制定期清理的频率 */

/* 每调用 session_create 多少次后触发一次定期清理 */
#define SESSION_CLEANUP_INTERVAL 16

/* 扫描并压缩 session 数组：移除所有失效条目（active=false 或已过期），
 * 将有效条目连续排列在数组头部，更新 g_session_count。
 * 单线程环境，不加锁。 */
static void session_cleanup(void) {
    time_t now = time(NULL);
    int write_idx = 0;

    for (int read_idx = 0; read_idx < g_session_count; read_idx++) {
        Session *s = &g_sessions[read_idx];

        /* 检查是否过期（过期也视为失效） */
        if (s->active && s->expires_at <= now) {
            s->active = false;
        }

        if (s->active) {
            if (write_idx != read_idx) {
                g_sessions[write_idx] = g_sessions[read_idx];
            }
            write_idx++;
        }
    }

    /* 将新释放的槽位清零（可选，防残留数据干扰） */
    for (int i = write_idx; i < g_session_count; i++) {
        memset(&g_sessions[i], 0, sizeof(Session));
    }

    g_session_count = write_idx;
}

/* AI授权依赖会话身份，使用密码学随机数；随机源失败时拒绝创建。 */
static int generate_token(char *buf, size_t len) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[SESSION_TOKEN_LEN / 2];
    if (len != SESSION_TOKEN_LEN + 1 || RAND_bytes(bytes, sizeof(bytes)) != 1) return -1;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        buf[i * 2] = hex[bytes[i] >> 4];
        buf[i * 2 + 1] = hex[bytes[i] & 15];
    }
    buf[len - 1] = '\0';
    return 0;
}

const char *session_create(int user_id, const char *role, const char *name) {
    /* 定期清理：每 SESSION_CLEANUP_INTERVAL 次创建触发一次压缩，避免槽位被僵尸 session 耗尽 */
    g_create_counter++;
    if (g_create_counter % SESSION_CLEANUP_INTERVAL == 0) {
        session_cleanup();
    }

    /* 槽位仍不够则强制清理一次；若清理后仍不足才真正失败 */
    if (g_session_count >= MAX_SESSIONS) {
        session_cleanup();
        if (g_session_count >= MAX_SESSIONS) return NULL;
    }

    Session *s = &g_sessions[g_session_count];
    /* 确保唯一性；耗尽重试或随机源失败不得返回可预测的token。 */
    bool unique = false;
    for (int retry = 0; retry < 5; retry++) {
        if (generate_token(s->token, sizeof(s->token)) != 0) return NULL;
        bool collision = false;
        for (int i = 0; i < g_session_count; i++) {
            if (g_sessions[i].active &&
                strcmp(g_sessions[i].token, s->token) == 0) {
                collision = true;
                break;
            }
        }
        if (!collision) { unique = true; break; }
    }
    if (!unique) return NULL;

    s->user_id    = user_id;
    s->expires_at = time(NULL) + 86400;  /* 24 小时后过期 */
    s->active     = true;
    snprintf(s->role, sizeof(s->role), "%s", role);
    snprintf(s->name, sizeof(s->name), "%s", name);

    g_session_count++;
    return s->token;
}

Session *session_validate(const char *token) {
    if (token == NULL || token[0] == '\0') return NULL;
    time_t now = time(NULL);

    for (int i = 0; i < g_session_count; i++) {
        Session *s = &g_sessions[i];
        if (s->active && strcmp(s->token, token) == 0) {
            if (s->expires_at > now) return s;
            /* 过期 → 标记失效 */
            s->active = false;
            return NULL;
        }
    }
    return NULL;
}

void session_destroy(const char *token) {
    if (token == NULL) return;
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i].active && strcmp(g_sessions[i].token, token) == 0) {
            g_sessions[i].active = false;
            return;
        }
    }
}

const char *extract_bearer_token(const char *auth_header) {
    if (auth_header == NULL) return NULL;

    /* 不区分大小写匹配 "Bearer " 前缀 */
    const char *prefix = "Bearer ";
    size_t plen = strlen(prefix);

    if (strncasecmp(auth_header, prefix, plen) != 0) return NULL;
    const char *token = auth_header + plen;
    if (token[0] == '\0') return NULL;
    return token;
}
