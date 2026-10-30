#ifndef AIQB_ROUTES_H
#define AIQB_ROUTES_H

#include <stddef.h>

#include <microhttpd.h>

#include "config.h"

/* 请求分发：/api/v1/ 路径为 JSON API；其余路径 → 静态文件（web/ 目录）。
 * /api/v1/questions 系列需有效 Bearer token：
 *   GET 列表/详情：任意登录角色；POST 新增 / PUT 修改 / DELETE 删除：仅 teacher/admin。
 * body / body_len 为 POST/PUT 请求体（GET/DELETE 请求时 body 为 NULL）。 */
enum MHD_Result route_dispatch(struct MHD_Connection *conn, const char *url,
                               const char *method, const char *body,
                               size_t body_len, const AppConfig *cfg);

#endif /* AIQB_ROUTES_H */
