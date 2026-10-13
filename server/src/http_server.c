/* 启用 POSIX 接口（sigaction 等），配合 -std=c11 使用 */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <microhttpd.h>

#include "http_server.h"
#include "routes.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ── POST 请求体累积上下文 ── */

typedef struct {
    char *body;
    size_t len;
    size_t cap;
    int too_large;
    int responded;
} RequestCtx;

static RequestCtx *ctx_create(void) {
    RequestCtx *ctx = calloc(1, sizeof(RequestCtx));
    if (ctx == NULL) return NULL;
    ctx->cap = 1024;
    ctx->body = malloc(ctx->cap);
    if (ctx->body == NULL) {
        free(ctx);
        return NULL;
    }
    ctx->body[0] = '\0';
    ctx->len = 0;
    return ctx;
}

static void ctx_free(void *cls) {
    RequestCtx *ctx = (RequestCtx *)cls;
    if (ctx == NULL) return;
    free(ctx->body);
    free(ctx);
}

static int ctx_append(RequestCtx *ctx, const char *data, size_t data_len) {
    if (ctx->too_large || data_len > 65536U - ctx->len) {
        ctx->too_large = 1; return 0;
    }
    size_t needed = ctx->len + data_len + 1;
    if (needed > ctx->cap) {
        size_t new_cap = ctx->cap * 2;
        while (new_cap < needed) new_cap *= 2;
        char *new_body = realloc(ctx->body, new_cap);
        if (new_body == NULL) return -1;
        ctx->body = new_body;
        ctx->cap = new_cap;
    }
    memcpy(ctx->body + ctx->len, data, data_len);
    ctx->len += data_len;
    ctx->body[ctx->len] = '\0';
    return 0;
}

static void request_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                               enum MHD_RequestTerminationCode termination) {
    (void)cls; (void)conn; (void)termination;
    ctx_free(*con_cls); *con_cls = NULL;
}

/* ── libmicrohttpd 请求回调 ── */

static enum MHD_Result request_handler(void *cls, struct MHD_Connection *conn,
                                       const char *url, const char *method,
                                       const char *version, const char *upload_data,
                                       size_t *upload_data_size, void **con_cls) {
    (void)version;

    /* 首次调用：分配上下文 */
    if (*con_cls == NULL) {
        RequestCtx *ctx = ctx_create();
        if (ctx == NULL) return MHD_NO;
        *con_cls = ctx;

        /* 某些 MHD 版本会在首次回调时就携带 upload_data，此时直接处理 */
        if (*upload_data_size > 0) {
            if (ctx_append(ctx, upload_data, *upload_data_size) != 0) {
                *upload_data_size = 0;
                return MHD_NO;
            }
            *upload_data_size = 0;
        }
        return MHD_YES;
    }

    RequestCtx *ctx = (RequestCtx *)*con_cls;

    /* 还在接收请求体数据 */
    if (*upload_data_size > 0) {
        if (ctx_append(ctx, upload_data, *upload_data_size) != 0) {
            *upload_data_size = 0;
            return MHD_NO;
        }
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (ctx->responded) return MHD_YES;
    ctx->responded = 1;
    if (ctx->too_large) {
        const char *json = "{\"error\":\"请求内容过长\",\"code\":\"REQUEST_TOO_LARGE\"}";
        struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(json), (void *)json, MHD_RESPMEM_PERSISTENT);
        if (resp == NULL) return MHD_NO;
        MHD_add_response_header(resp, "Content-Type", "application/json; charset=utf-8");
        enum MHD_Result ret = MHD_queue_response(conn, 413, resp);
        MHD_destroy_response(resp); return ret;
    }
    /* 请求体接收完毕，分发一次；包括断开连接在内的所有清理由完成回调处理。 */
    enum MHD_Result ret = route_dispatch(conn, url, method,
                                         ctx->body, ctx->len,
                                         (const AppConfig *)cls);
    return ret;
}

/* ── 启动服务 ── */

int http_server_start(const AppConfig *cfg) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        (uint16_t)cfg->port, NULL, NULL,
        &request_handler, (void *)cfg,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
        MHD_OPTION_END);
    if (daemon == NULL) {
        fprintf(stderr, "✘ HTTP 服务启动失败（端口 %d 可能被占用）\n", cfg->port);
        return -1;
    }

    fprintf(stderr, "✔ HTTP 服务运行中: http://0.0.0.0:%d （Ctrl+C 停止）\n", cfg->port);
    while (!g_stop) sleep(1);

    MHD_stop_daemon(daemon);
    fprintf(stderr, "✔ 服务已停止\n");
    return 0;
}