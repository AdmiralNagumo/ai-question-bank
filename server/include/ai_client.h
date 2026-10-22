#ifndef AIQB_AI_CLIENT_H
#define AIQB_AI_CLIENT_H

#include "config.h"

/* 调用独立 Pi 服务。path 仅由后端构造，user_id 来自已验证会话。
 * 返回公共 HTTP 状态码，out_json 由 malloc 分配，调用者 free()。
 * C 只提交/查询任务，不等待模型完成。 */
int ai_service_request(const AppConfig *cfg, const char *method, const char *path,
                       int user_id, const char *body, char **out_json);

#endif
