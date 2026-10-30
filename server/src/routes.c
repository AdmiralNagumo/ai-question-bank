#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#include <microhttpd.h>

#include "ai_client.h"
#include "ai_access.h"
#include "auth.h"
#include "cJSON.h"
#include "config.h"
#include "db.h"
#include "kv_store.h"
#include "routes.h"

/* ── 密码哈希（与 db.c 保持一致的算法和 salt） ── */
#include <openssl/sha.h>

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

/* ── JSON 工具 ── */

/* 从 JSON 对象中安全提取字符串值，返回 malloc 副本或 NULL */
static char *json_get_string(const cJSON *root, const char *key) {
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == NULL || !cJSON_IsString(item)) return NULL;
    return strdup(item->valuestring);
}

/* ── 响应工具 ── */

static enum MHD_Result reply_json(struct MHD_Connection *conn, unsigned int code,
                                  const char *json) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(json), (void *)json, MHD_RESPMEM_MUST_COPY);
    if (resp == NULL) return MHD_NO;
    MHD_add_response_header(resp, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(resp, "Cache-Control", "no-store");
    enum MHD_Result ret = MHD_queue_response(conn, code, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result reply_error(struct MHD_Connection *conn, unsigned int code,
                                   const char *msg) {
    cJSON *err = cJSON_CreateObject();
    if (err == NULL)
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"oom\"}");
    cJSON_AddStringToObject(err, "error", msg);
    char *body = cJSON_PrintUnformatted(err);
    enum MHD_Result ret = reply_json(conn, code, body);
    cJSON_free(body);
    cJSON_Delete(err);
    return ret;
}

/* ── 路由处理器 ── */

/* 检查密码强度：至少 8 位，包含字母和数字 */
static int validate_password(const char *pw) {
    if (pw == NULL) return 0;
    size_t len = strlen(pw);
    if (len < 8) return 0;
    int has_letter = 0, has_digit = 0;
    for (size_t i = 0; i < len; i++) {
        if (isalpha((unsigned char)pw[i])) has_letter = 1;
        if (isdigit((unsigned char)pw[i])) has_digit = 1;
    }
    return has_letter && has_digit;
}

/* POST /api/v1/auth/register */
static enum MHD_Result handle_register(struct MHD_Connection *conn,
                                       const char *body, size_t body_len,
                                       const AppConfig *cfg) {
    (void)body_len;
    if (body == NULL || body[0] == '\0')
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "请求体为空");

    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "JSON 解析失败");

    char *username = json_get_string(root, "username");
    char *password = json_get_string(root, "password");
    char *confirm  = json_get_string(root, "confirm_password");
    char *req_role = json_get_string(root, "role");  /* 可选 */
    cJSON_Delete(root);

    if (username == NULL || password == NULL || confirm == NULL) {
        free(username);
        free(password);
        free(confirm);
        free(req_role);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST,
                           "缺少 username、password 或 confirm_password");
    }

    /* 检查用户名是否为空白 */
    if (strlen(username) == 0) {
        free(username);
        free(password);
        free(confirm);
        free(req_role);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "用户名不能为空");
    }

    /* 检查两次密码一致 */
    if (strcmp(password, confirm) != 0) {
        free(username);
        free(password);
        free(confirm);
        free(req_role);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "两次输入的密码不一致");
    }

    /* 检查密码强度 */
    if (!validate_password(password)) {
        free(username);
        free(password);
        free(confirm);
        free(req_role);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST,
                           "密码至少 8 位，且必须包含字母和数字");
    }

    /* 检查角色是否合法 */
    if (req_role != NULL &&
        strcmp(req_role, "student") != 0 &&
        strcmp(req_role, "teacher") != 0 &&
        strcmp(req_role, "admin") != 0) {
        free(username);
        free(password);
        free(confirm);
        free(req_role);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST,
                           "角色值无效，仅支持 student/teacher/admin");
    }

    /* 哈希密码（与 login 使用相同 salt） */
    const char *salt = "ai-question-bank-demo-salt-2026";
    char *hash = hash_password(password, salt);
    free(password);
    free(confirm);
    if (hash == NULL) {
        free(username);
        free(req_role);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "服务器内部错误");
    }

    int ret = db_create_user(cfg->db, username, hash, req_role);
    free(hash);
    free(req_role);

    if (ret == 1) {
        free(username);
        return reply_error(conn, MHD_HTTP_CONFLICT, "用户名已存在");
    }
    if (ret != 0) {
        free(username);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "注册失败，请稍后重试");
    }

    /* 注册成功后自动登录，为其创建 session */
    char *user_json = db_query_user_by_username(cfg->db, username);
    if (user_json == NULL) {
        free(username);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "注册成功但查询用户失败");
    }

    cJSON *u_root = cJSON_Parse(user_json);
    int    user_id = 0;
    char  *db_role = NULL;
    char  *db_name = NULL;
    if (u_root) {
        cJSON *id_item = cJSON_GetObjectItem(u_root, "id");
        if (id_item && cJSON_IsNumber(id_item)) user_id = id_item->valueint;
        db_role = json_get_string(u_root, "role");
        db_name = json_get_string(u_root, "name");
    }

    const char *token = session_create(user_id, db_role ? db_role : "student",
                                       db_name ? db_name : username);

    cJSON *resp = cJSON_CreateObject();
    cJSON *user_obj = cJSON_CreateObject();
    if (token && resp && user_obj) {
        cJSON_AddStringToObject(resp, "token", token);
        cJSON_AddNumberToObject(user_obj, "id", user_id);
        cJSON_AddStringToObject(user_obj, "username", username);
        cJSON_AddStringToObject(user_obj, "name", db_name ? db_name : username);
        cJSON_AddStringToObject(user_obj, "role", db_role ? db_role : "student");
        cJSON_AddItemToObject(resp, "user", user_obj);

        char *out = cJSON_PrintUnformatted(resp);
        enum MHD_Result result = reply_json(conn, MHD_HTTP_CREATED, out);
        cJSON_free(out);
        cJSON_Delete(resp);
        free(user_json);
        free(username);
        free(db_role);
        free(db_name);
        return result;
    }

    cJSON_Delete(resp);
    free(user_json);
    free(username);
    free(db_role);
    free(db_name);
    return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "创建会话失败");
}

/* GET /api/v1/health */
static enum MHD_Result handle_health(struct MHD_Connection *conn) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"oom\"}");
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "service", "ai-question-bank");
    char *body = cJSON_PrintUnformatted(root);
    enum MHD_Result ret = reply_json(conn, MHD_HTTP_OK, body);
    cJSON_free(body);
    cJSON_Delete(root);
    return ret;
}

/* POST /api/v1/auth/login */
static enum MHD_Result handle_login(struct MHD_Connection *conn,
                                    const char *body, size_t body_len,
                                    const AppConfig *cfg) {
    (void)body_len;
    if (body == NULL || body[0] == '\0')
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "请求体为空");

    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "JSON 解析失败");

    char *username = json_get_string(root, "username");
    char *password = json_get_string(root, "password");
    cJSON_Delete(root);

    if (username == NULL || password == NULL) {
        free(username);
        free(password);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST,
                           "缺少 username 或 password");
    }

    /* 查询数据库中存储的 password_hash */
    char *stored_hash = db_query_password_hash(cfg->db, username);
    if (stored_hash == NULL) {
        free(username);
        free(password);
        return reply_error(conn, MHD_HTTP_UNAUTHORIZED, "用户名或密码错误");
    }

    /* 对用户输入的密码做相同哈希计算后比较 */
    const char *salt = "ai-question-bank-demo-salt-2026";
    char *input_hash = hash_password(password, salt);

    int matched = (input_hash != NULL && strcmp(input_hash, stored_hash) == 0);

    free(stored_hash);
    free(input_hash);

    if (!matched) {
        free(username);
        free(password);
        return reply_error(conn, MHD_HTTP_UNAUTHORIZED, "用户名或密码错误");
    }

    /* 查询用户信息用于创建 session */
    char *user_json = db_query_user_by_username(cfg->db, username);
    if (user_json == NULL) {
        free(username);
        free(password);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "查询用户信息失败");
    }

    /* 解析 user_json 提取 user_id、role、name */
    cJSON *u_root = cJSON_Parse(user_json);
    int    user_id = 0;
    char  *role = NULL;
    char  *name = NULL;
    if (u_root) {
        cJSON *id_item = cJSON_GetObjectItem(u_root, "id");
        if (id_item && cJSON_IsNumber(id_item)) user_id = id_item->valueint;
        role = json_get_string(u_root, "role");
        name = json_get_string(u_root, "name");
    }

    const char *token = session_create(user_id, role ? role : "", name ? name : "");

    cJSON *resp = cJSON_CreateObject();
    cJSON *user_obj = cJSON_CreateObject();
    if (token && resp && user_obj) {
        cJSON_AddStringToObject(resp, "token", token);
        cJSON_AddNumberToObject(user_obj, "id", user_id);
        cJSON_AddStringToObject(user_obj, "username", username);
        cJSON_AddStringToObject(user_obj, "name", name ? name : "");
        cJSON_AddStringToObject(user_obj, "role", role ? role : "");
        cJSON_AddItemToObject(resp, "user", user_obj);

        char *out = cJSON_PrintUnformatted(resp);
        enum MHD_Result ret = reply_json(conn, MHD_HTTP_OK, out);
        cJSON_free(out);
        cJSON_Delete(resp);
        free(user_json);
        free(username);
        free(password);
        free(role);
        free(name);
        return ret;
    }

    cJSON_Delete(resp);
    free(user_json);
    free(username);
    free(password);
    free(role);
    free(name);
    return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "创建会话失败");
}

/* POST /api/v1/auth/logout */
static enum MHD_Result handle_logout(struct MHD_Connection *conn) {
    const char *auth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                     "Authorization");
    const char *token = extract_bearer_token(auth);
    if (token != NULL) {
        session_destroy(token);
    }
    return reply_json(conn, MHD_HTTP_OK, "{\"status\":\"ok\"}");
}

/* GET /api/v1/auth/me */
static enum MHD_Result handle_me(struct MHD_Connection *conn) {
    const char *auth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                     "Authorization");
    const char *token = extract_bearer_token(auth);
    Session *s = session_validate(token);
    if (s == NULL) {
        return reply_error(conn, MHD_HTTP_UNAUTHORIZED, "未登录或 token 已过期");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *user_obj = cJSON_CreateObject();
    if (resp == NULL || user_obj == NULL) {
        cJSON_Delete(resp);
        cJSON_Delete(user_obj);
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"oom\"}");
    }
    cJSON_AddNumberToObject(user_obj, "id", s->user_id);
    cJSON_AddStringToObject(user_obj, "name", s->name);
    cJSON_AddStringToObject(user_obj, "role", s->role);
    cJSON_AddItemToObject(resp, "user", user_obj);

    char *out = cJSON_PrintUnformatted(resp);
    enum MHD_Result ret = reply_json(conn, MHD_HTTP_OK, out);
    cJSON_free(out);
    cJSON_Delete(resp);
    return ret;
}


/* ── 固定题目接口（管理员与学生使用同一只读接口） ── */

static cJSON *kv_to_question(const char *key, sqlite3 *db) {
    char *json = kv_get_all(db, "qdata", key);
    if (json == NULL) return NULL;

    cJSON *arr = cJSON_Parse(json);
    cJSON_free(json);
    if (arr == NULL || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return NULL; }

    /* 无数据 = 题目不存在 */
    int size = cJSON_GetArraySize(arr);
    if (size == 0) { cJSON_Delete(arr); return NULL; }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) { cJSON_Delete(arr); return NULL; }
    {
        int id_val;
        if (kv_str_to_int(key, &id_val) == 0 && id_val > 0)
            cJSON_AddNumberToObject(obj, "id", id_val);
        else
            cJSON_AddNumberToObject(obj, "id", 0);
    }

    for (int i = 0; i < size; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *val  = cJSON_GetObjectItem(item, "value");
        if (name == NULL || val == NULL || !cJSON_IsString(name) || !cJSON_IsString(val))
            continue;
        const char *n = name->valuestring;
        const char *v = val->valuestring;
        /* 跳过内部字段 */
        if (strcmp(n, "seed_key") == 0 || strcmp(n, "teacher_id") == 0)
            continue;
        if (strcmp(n, "options") == 0 || strcmp(n, "knowledge_points") == 0) {
            cJSON *parsed = cJSON_Parse(v);
            cJSON_AddItemToObject(obj, n, parsed ? parsed : cJSON_CreateArray());
        } else {
            cJSON_AddStringToObject(obj, n, v);
        }
    }
    cJSON_Delete(arr);
    return obj;
}

/* ── 题目写操作辅助 ── */

/* 解析 /api/v1/questions/{id} 的题目 id。
 * 成功返回正数；失败返回 0（已向 conn 发送 404 响应）。 */
static long long parse_question_id(struct MHD_Connection *conn, const char *url) {
    const char *id_text = url + sizeof("/api/v1/questions/") - 1;
    if (*id_text == '\0') {
        reply_json(conn, MHD_HTTP_NOT_FOUND, "{\"error\":\"question not found\"}");
        return 0;
    }
    for (const char *p = id_text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            reply_json(conn, MHD_HTTP_NOT_FOUND, "{\"error\":\"question not found\"}");
            return 0;
        }
    }
    errno = 0;
    char *end = NULL;
    long long id = strtoll(id_text, &end, 10);
    if (errno == ERANGE || end == id_text || *end != '\0' || id <= 0) {
        reply_json(conn, MHD_HTTP_NOT_FOUND, "{\"error\":\"question not found\"}");
        return 0;
    }
    return id;
}

/* 计算下一个题目 id：扫描 qdata 现有 key 中的最大数字 id + 1。失败返回 -1。 */
static int next_question_id(sqlite3 *db) {
    int count = 0;
    char **keys = kv_list_keys(db, "qdata", &count);
    if (keys == NULL) return -1;

    int max_id = 0;
    for (int i = 0; i < count; i++) {
        int id = 0;
        if (kv_str_to_int(keys[i], &id) == 0 && id > max_id) max_id = id;
    }
    kv_free_str_array(keys);
    return max_id + 1;
}

/* 从已解析的请求体中校验并提取题目字段。
 * 规则对齐 docs/questions.md 与 tests 的 check_question：
 *   content 必填非空；type ∈ {single_choice, short_answer}；
 *   single_choice 的 options 为非空 {label,text} 数组且 answer 为某 label；
 *   short_answer 的 options 必须为空数组；knowledge_points 为字符串数组（可选）。
 * 成功返回 0；失败返回 -1（已向 conn 发送 400 响应）。
 * 输出参数均为 malloc 副本（analysis 可为 NULL），调用者负责释放。 */
static int extract_question_fields(struct MHD_Connection *conn,
                                   const cJSON *root,
                                   char **out_content,
                                   char **out_type,
                                   char **out_answer,
                                   char **out_analysis,
                                   char **out_options_json,
                                   char **out_kp_json) {
    *out_content = *out_type = *out_answer = *out_analysis = NULL;
    *out_options_json = *out_kp_json = NULL;

    char *content = json_get_string(root, "content");
    if (content == NULL || content[0] == '\0') {
        free(content);
        reply_error(conn, MHD_HTTP_BAD_REQUEST, "content 为必填非空字段");
        return -1;
    }

    char *type = json_get_string(root, "type");
    if (type == NULL || (strcmp(type, "single_choice") != 0 &&
                         strcmp(type, "short_answer") != 0)) {
        free(content);
        free(type);
        reply_error(conn, MHD_HTTP_BAD_REQUEST,
                    "type 仅支持 single_choice 或 short_answer");
        return -1;
    }

    char *answer = json_get_string(root, "answer");
    if (answer == NULL || answer[0] == '\0') {
        free(content);
        free(type);
        free(answer);
        reply_error(conn, MHD_HTTP_BAD_REQUEST, "answer 为必填非空字段");
        return -1;
    }

    char *analysis = json_get_string(root, "analysis");  /* 可选 */

    /* options：single_choice 必须为非空 {label,text} 数组；short_answer 必须为空数组 */
    cJSON *options = cJSON_GetObjectItem(root, "options");
    char *options_json = NULL;
    if (options != NULL) {
        if (!cJSON_IsArray(options)) {
            free(content); free(type); free(answer); free(analysis);
            reply_error(conn, MHD_HTTP_BAD_REQUEST, "options 必须为数组");
            return -1;
        }
        int n = cJSON_GetArraySize(options);
        if (strcmp(type, "single_choice") == 0) {
            if (n == 0) {
                free(content); free(type); free(answer); free(analysis);
                reply_error(conn, MHD_HTTP_BAD_REQUEST, "单选题至少需要 1 个选项");
                return -1;
            }
            int answer_in_options = 0;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(options, i);
                cJSON *label = item ? cJSON_GetObjectItem(item, "label") : NULL;
                cJSON *text  = item ? cJSON_GetObjectItem(item, "text") : NULL;
                if (!cJSON_IsObject(item) || label == NULL || !cJSON_IsString(label) ||
                    label->valuestring[0] == '\0' || text == NULL || !cJSON_IsString(text) ||
                    text->valuestring[0] == '\0') {
                    free(content); free(type); free(answer); free(analysis);
                    reply_error(conn, MHD_HTTP_BAD_REQUEST,
                                "每个选项需包含非空的 label 与 text");
                    return -1;
                }
                if (strcmp(label->valuestring, answer) == 0) answer_in_options = 1;
            }
            if (!answer_in_options) {
                free(content); free(type); free(answer); free(analysis);
                reply_error(conn, MHD_HTTP_BAD_REQUEST,
                            "answer 必须是某个选项的 label");
                return -1;
            }
        } else {
            if (n != 0) {
                free(content); free(type); free(answer); free(analysis);
                reply_error(conn, MHD_HTTP_BAD_REQUEST, "简答题 options 必须为空数组");
                return -1;
            }
        }
        options_json = cJSON_PrintUnformatted(options);
    } else if (strcmp(type, "single_choice") == 0) {
        free(content); free(type); free(answer); free(analysis);
        reply_error(conn, MHD_HTTP_BAD_REQUEST, "单选题必须提供 options");
        return -1;
    } else {
        options_json = strdup("[]");
    }

    /* knowledge_points：可选字符串数组 */
    cJSON *kp = cJSON_GetObjectItem(root, "knowledge_points");
    char *kp_json = NULL;
    if (kp != NULL) {
        if (!cJSON_IsArray(kp)) {
            free(content); free(type); free(answer); free(analysis);
            cJSON_free(options_json);
            reply_error(conn, MHD_HTTP_BAD_REQUEST, "knowledge_points 必须为数组");
            return -1;
        }
        for (int i = 0; i < cJSON_GetArraySize(kp); i++) {
            cJSON *item = cJSON_GetArrayItem(kp, i);
            if (!cJSON_IsString(item)) {
                free(content); free(type); free(answer); free(analysis);
                cJSON_free(options_json);
                reply_error(conn, MHD_HTTP_BAD_REQUEST,
                            "knowledge_points 必须为字符串数组");
                return -1;
            }
        }
        kp_json = cJSON_PrintUnformatted(kp);
    } else {
        kp_json = strdup("[]");
    }

    *out_content = content;
    *out_type = type;
    *out_answer = answer;
    *out_analysis = analysis;
    *out_options_json = options_json;
    *out_kp_json = kp_json;
    return 0;
}

static void free_question_fields(char *content, char *type, char *answer,
                                 char *analysis, char *options_json,
                                 char *kp_json) {
    free(content);
    free(type);
    free(answer);
    free(analysis);
    cJSON_free(options_json);
    cJSON_free(kp_json);
}

/* POST /api/v1/questions：新增题目（teacher/admin） */
static enum MHD_Result handle_question_create(struct MHD_Connection *conn,
                                              const char *body, size_t body_len,
                                              const AppConfig *cfg,
                                              const Session *s) {
    (void)body_len;
    if (body == NULL || body[0] == '\0')
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "请求体为空");

    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "JSON 解析失败");

    char *content, *type, *answer, *analysis, *options_json, *kp_json;
    if (extract_question_fields(conn, root, &content, &type, &answer,
                                &analysis, &options_json, &kp_json) != 0) {
        cJSON_Delete(root);
        return MHD_YES;
    }
    cJSON_Delete(root);

    int id = next_question_id(cfg->db);
    if (id <= 0) {
        free_question_fields(content, type, answer, analysis, options_json, kp_json);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "create question failed");
    }

    char key[32];
    snprintf(key, sizeof(key), "%d", id);
    char tid[32];
    snprintf(tid, sizeof(tid), "%d", s->user_id);

    if (kv_set(cfg->db, "qdata", key, "content", content) != 0 ||
        kv_set(cfg->db, "qdata", key, "options", options_json) != 0 ||
        kv_set(cfg->db, "qdata", key, "answer", answer) != 0 ||
        kv_set(cfg->db, "qdata", key, "analysis",
               analysis ? analysis : "") != 0 ||
        kv_set(cfg->db, "qdata", key, "type", type) != 0 ||
        kv_set(cfg->db, "qdata", key, "knowledge_points", kp_json) != 0 ||
        kv_set(cfg->db, "qdata", key, "teacher_id", tid) != 0 ||
        kv_set(cfg->db, "qdata", key, "status", "active") != 0) {
        free_question_fields(content, type, answer, analysis, options_json, kp_json);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "create question failed");
    }
    free_question_fields(content, type, answer, analysis, options_json, kp_json);

    cJSON *question = kv_to_question(key, cfg->db);
    if (question == NULL) {
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "create question failed");
    }
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        cJSON_Delete(question);
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"oom\"}");
    }
    cJSON_AddItemToObject(resp, "question", question);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    enum MHD_Result ret = reply_json(conn, MHD_HTTP_CREATED, json);
    cJSON_free(json);
    return ret;
}

/* PUT /api/v1/questions/{id}：修改题目（teacher/admin；部分更新，未提供字段保留） */
static enum MHD_Result handle_question_update(struct MHD_Connection *conn,
                                              const char *url, const char *body,
                                              size_t body_len, const AppConfig *cfg) {
    (void)body_len;
    long long id = parse_question_id(conn, url);
    if (id == 0) return MHD_YES;

    char key[32];
    snprintf(key, sizeof(key), "%lld", id);

    /* 目标题目必须存在 */
    cJSON *old = kv_to_question(key, cfg->db);
    if (old == NULL) {
        return reply_error(conn, MHD_HTTP_NOT_FOUND, "question not found");
    }

    if (body == NULL || body[0] == '\0') {
        cJSON_Delete(old);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "请求体为空");
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        cJSON_Delete(old);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST, "JSON 解析失败");
    }

    /* 合并：以旧题为基，请求体中出现的字段覆盖（id 不可修改） */
    cJSON *merged = cJSON_Duplicate(old, 1);
    cJSON_Delete(old);
    if (merged == NULL) {
        cJSON_Delete(root);
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"oom\"}");
    }
    cJSON_DeleteItemFromObject(merged, "id");

    static const char *editable[] = {
        "content", "answer", "analysis", "type",
        "options", "knowledge_points"
    };
    for (size_t i = 0; i < sizeof(editable) / sizeof(editable[0]); i++) {
        cJSON *item = cJSON_GetObjectItem(root, editable[i]);
        if (item != NULL) {
            cJSON_DeleteItemFromObject(merged, editable[i]);
            cJSON_AddItemToObject(merged, editable[i], cJSON_Duplicate(item, 1));
        }
    }
    /* 请求体必须包含至少一个可更新字段（空对象视为非法） */
    int has_field = 0;
    for (size_t i = 0; i < sizeof(editable) / sizeof(editable[0]); i++) {
        if (cJSON_GetObjectItem(root, editable[i]) != NULL) {
            has_field = 1;
            break;
        }
    }
    if (!has_field) {
        cJSON_Delete(root);
        cJSON_Delete(merged);
        return reply_error(conn, MHD_HTTP_BAD_REQUEST,
                           "请求体未包含任何可更新字段");
    }
    cJSON_Delete(root);

    /* 用合并后的完整负载做统一校验，保证 answer/options/type 一致 */
    char *content, *type, *answer, *analysis, *options_json, *kp_json;
    if (extract_question_fields(conn, merged, &content, &type, &answer,
                                &analysis, &options_json, &kp_json) != 0) {
        cJSON_Delete(merged);
        return MHD_YES;
    }
    cJSON_Delete(merged);

    if (kv_set(cfg->db, "qdata", key, "content", content) != 0 ||
        kv_set(cfg->db, "qdata", key, "options", options_json) != 0 ||
        kv_set(cfg->db, "qdata", key, "answer", answer) != 0 ||
        kv_set(cfg->db, "qdata", key, "analysis",
               analysis ? analysis : "") != 0 ||
        kv_set(cfg->db, "qdata", key, "type", type) != 0 ||
        kv_set(cfg->db, "qdata", key, "knowledge_points", kp_json) != 0) {
        free_question_fields(content, type, answer, analysis, options_json, kp_json);
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "update question failed");
    }
    free_question_fields(content, type, answer, analysis, options_json, kp_json);

    cJSON *question = kv_to_question(key, cfg->db);
    if (question == NULL) {
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "update question failed");
    }
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        cJSON_Delete(question);
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"oom\"}");
    }
    cJSON_AddItemToObject(resp, "question", question);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    enum MHD_Result ret = reply_json(conn, MHD_HTTP_OK, json);
    cJSON_free(json);
    return ret;
}

/* DELETE /api/v1/questions/{id}：删除题目（teacher/admin） */
static enum MHD_Result handle_question_delete(struct MHD_Connection *conn,
                                              const char *url,
                                              const AppConfig *cfg) {
    long long id = parse_question_id(conn, url);
    if (id == 0) return MHD_YES;

    char key[32];
    snprintf(key, sizeof(key), "%lld", id);

    cJSON *question = kv_to_question(key, cfg->db);
    if (question == NULL) {
        return reply_error(conn, MHD_HTTP_NOT_FOUND, "question not found");
    }
    cJSON_Delete(question);

    if (kv_delete_key(cfg->db, "qdata", key) != 0) {
        return reply_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "delete question failed");
    }
    return reply_json(conn, MHD_HTTP_OK, "{\"status\":\"ok\"}");
}

static enum MHD_Result handle_questions(struct MHD_Connection *conn,
                                        const char *url, const char *method,
                                        const char *body, size_t body_len,
                                        const AppConfig *cfg) {
    const char *auth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization");
    Session *s = session_validate(extract_bearer_token(auth));
    if (s == NULL) {
        return reply_json(conn, MHD_HTTP_UNAUTHORIZED,
                          "{\"error\":\"未登录或 token 已过期\"}");
    }

    /* 写操作仅教师/管理员可执行 */
    if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
        strcmp(method, "DELETE") == 0) {
        if (strcmp(s->role, "teacher") != 0 && strcmp(s->role, "admin") != 0) {
            return reply_error(conn, MHD_HTTP_FORBIDDEN,
                               "仅教师或管理员可执行此操作");
        }
    }

    if (strcmp(method, "GET") == 0) {
        /* 读接口不创建空数据库；数据库丢失应是 500，不能伪装成空列表或 404。 */
        if (access(cfg->db_path, R_OK) != 0) {
            return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                              "{\"error\":\"query questions failed\"}");
        }

        if (strcmp(url, "/api/v1/questions") == 0) {
            /* 列表：遍历 kv_store 中的 questions namespace */
            int count;
            char **keys = kv_list_keys(cfg->db, "qdata", &count);
            if (keys == NULL) {
                return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  "{\"error\":\"query questions failed\"}");
            }

            cJSON *root = cJSON_CreateObject();
            cJSON *arr  = cJSON_AddArrayToObject(root, "questions");
            for (int i = 0; i < count; i++) {
                cJSON *q = kv_to_question(keys[i], cfg->db);
                if (q) cJSON_AddItemToArray(arr, q);
            }
            kv_free_str_array(keys);

            char *json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            enum MHD_Result ret = reply_json(conn, MHD_HTTP_OK, json);
            cJSON_free(json);
            return ret;
        } else {
            /* 单题查询 */
            long long id = parse_question_id(conn, url);
            if (id == 0) return MHD_YES;

            char key[32];
            snprintf(key, sizeof(key), "%lld", id);

            cJSON *question = kv_to_question(key, cfg->db);
            if (question == NULL) {
                return reply_json(conn, MHD_HTTP_NOT_FOUND,
                                  "{\"error\":\"question not found\"}");
            }

            cJSON *root = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "question", question);
            char *json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            enum MHD_Result ret = reply_json(conn, MHD_HTTP_OK, json);
            cJSON_free(json);
            return ret;
        }
    }

    if (strcmp(method, "POST") == 0 &&
        strcmp(url, "/api/v1/questions") == 0) {
        return handle_question_create(conn, body, body_len, cfg, s);
    }

    if (strcmp(method, "PUT") == 0 &&
        strncmp(url, "/api/v1/questions/", sizeof("/api/v1/questions/") - 1) == 0) {
        return handle_question_update(conn, url, body, body_len, cfg);
    }

    if (strcmp(method, "DELETE") == 0 &&
        strncmp(url, "/api/v1/questions/", sizeof("/api/v1/questions/") - 1) == 0) {
        return handle_question_delete(conn, url, cfg);
    }

    /* 其余方法 → 405 */
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        sizeof("{\"error\":\"method not allowed\"}") - 1,
        (void *)"{\"error\":\"method not allowed\"}", MHD_RESPMEM_PERSISTENT);
    if (resp == NULL) return MHD_NO;
    MHD_add_response_header(resp, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(resp, "Allow", "GET, POST, PUT, DELETE");
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_METHOD_NOT_ALLOWED, resp);
    MHD_destroy_response(resp);
    return ret;
}


/* ── Pi 异步解答接口：由 C 验证身份和读取题目，Node 执行模型任务 ── */
static enum MHD_Result chat_error(struct MHD_Connection *conn, unsigned int status,
                                  const char *code, const char *message) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "error", message) == NULL ||
        cJSON_AddStringToObject(root, "code", code) == NULL) {
        cJSON_Delete(root);
        return reply_json(conn, 500, "{\"error\":\"服务器内部错误\",\"code\":\"INTERNAL_ERROR\"}");
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return reply_json(conn, 500, "{\"error\":\"服务器内部错误\",\"code\":\"INTERNAL_ERROR\"}");
    enum MHD_Result ret = reply_json(conn, status, json);
    cJSON_free(json);
    return ret;
}

static int chat_uuid_valid(const char *value) {
    if (value == NULL || strlen(value) != 36) return 0;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return 0;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    }
    return value[14] == '4' && strchr("89ab", value[19]) != NULL;
}

static enum MHD_Result chat_method_error(struct MHD_Connection *conn, const char *allowed) {
    const char *json = "{\"error\":\"请求方法不支持\",\"code\":\"METHOD_NOT_ALLOWED\"}";
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(json), (void *)json, MHD_RESPMEM_PERSISTENT);
    if (response == NULL) return MHD_NO;
    MHD_add_response_header(response, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(response, "Allow", allowed);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    enum MHD_Result ret = MHD_queue_response(conn, 405, response);
    MHD_destroy_response(response);
    return ret;
}

static enum MHD_Result handle_ai_access(struct MHD_Connection *conn, const char *method, const AppConfig *cfg) {
    const char *authorization = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization");
    Session *session = session_validate(extract_bearer_token(authorization));
    if (session == NULL) return chat_error(conn, 401, "UNAUTHORIZED", "未登录或 token 已过期");
    if (strcmp(method, "GET") != 0) return chat_method_error(conn, "GET");
    AiAccess access;
    time_t now = time(NULL);
    if (now < 0 || ai_access_get(cfg->db, session->user_id, (sqlite3_int64)now, &access) != 0)
        return chat_error(conn, 503, "AI_ACCESS_UNAVAILABLE", "AI 授权暂时无法核验，请稍后重试");
    cJSON *root = cJSON_CreateObject();
    cJSON *value = root != NULL ? cJSON_AddObjectToObject(root, "ai_access") : NULL;
    int valid = value != NULL && cJSON_AddBoolToObject(value, "allowed", access.status == AI_ACCESS_ACTIVE) != NULL &&
        cJSON_AddStringToObject(value, "status", ai_access_status_name(access.status)) != NULL;
    if (valid && access.has_expiry) {
        char timestamp[32]; struct tm utc;
        time_t expiry = (time_t)access.expires_at;
        valid = (sqlite3_int64)expiry == access.expires_at && gmtime_r(&expiry, &utc) != NULL &&
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) > 0;
        if (valid) valid = cJSON_AddStringToObject(value, "expires_at", timestamp) != NULL;
    } else if (valid) valid = cJSON_AddNullToObject(value, "expires_at") != NULL;
    char *json = valid ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) return chat_error(conn, 500, "INTERNAL_ERROR", "服务器内部错误");
    enum MHD_Result result = reply_json(conn, 200, json);
    cJSON_free(json); return result;
}

static int chat_contains_null_escape(const char *body, size_t length) {
    for (size_t i = 0; i + 1 < length; i++) {
        if (body[i] != '\\') continue;
        if (i + 5 < length && memcmp(body + i + 1, "u0000", 5) == 0) return 1;
        i++; /* 跳过完整转义；\\\\u0000 是普通文字，不是 NUL。 */
    }
    return 0;
}

static int chat_text_valid(cJSON *value) {
    if (!cJSON_IsString(value)) return 0;
    size_t characters = 0;
    int nonspace = 0;
    for (const unsigned char *p = (const unsigned char *)value->valuestring; *p; p++) {
        if ((*p & 0xc0) != 0x80) characters++;
        if (!isspace(*p)) nonspace = 1;
    }
    return nonspace && characters <= 4000;
}

static int chat_fields_valid(cJSON *root) {
    if (!cJSON_IsObject(root)) return 0;
    for (cJSON *field = root->child; field != NULL; field = field->next) {
        if (strcmp(field->string, "question_id") != 0 && strcmp(field->string, "message") != 0 &&
            strcmp(field->string, "conversation_id") != 0 && strcmp(field->string, "mode") != 0 &&
            strcmp(field->string, "student_answer") != 0) return 0;
        for (cJSON *previous = root->child; previous != field; previous = previous->next) {
            if (strcmp(previous->string, field->string) == 0) return 0;
        }
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "question_id");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    cJSON *conversation = cJSON_GetObjectItemCaseSensitive(root, "conversation_id");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    cJSON *answer = cJSON_GetObjectItemCaseSensitive(root, "student_answer");
    if (!cJSON_IsNumber(id) || id->valueint <= 0 || (double)id->valueint != id->valuedouble ||
        !chat_text_valid(message)) return 0;
    if (mode != NULL && (!cJSON_IsString(mode) ||
        (strcmp(mode->valuestring, "solve") != 0 && strcmp(mode->valuestring, "hint") != 0 &&
         strcmp(mode->valuestring, "review") != 0))) return 0;
    if (answer != NULL && !chat_text_valid(answer)) return 0;
    if (mode != NULL && ((strcmp(mode->valuestring, "review") == 0) != (answer != NULL))) return 0;
    return conversation == NULL || (cJSON_IsString(conversation) && chat_uuid_valid(conversation->valuestring));
}

static enum MHD_Result handle_chat(struct MHD_Connection *conn, const char *url,
                                    const char *method, const char *body,
                                    size_t body_len, const AppConfig *cfg) {
    const char *authorization = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Authorization");
    Session *session = session_validate(extract_bearer_token(authorization));
    if (session == NULL) return chat_error(conn, 401, "UNAUTHORIZED", "未登录或 token 已过期");
    const int submit = strcmp(url, "/api/v1/chat") == 0;
    if ((submit && strcmp(method, "POST") != 0) ||
        (!submit && strcmp(method, "GET") != 0 && strcmp(method, "DELETE") != 0)) {
        return chat_method_error(conn, submit ? "POST" : "GET, DELETE");
    }
    char path[96] = "/internal/v1/jobs";
    char *payload = NULL;
    if (submit) {
        AiAccess entitlement;
        time_t now = time(NULL);
        if (now < 0 || ai_access_get(cfg->db, session->user_id, (sqlite3_int64)now, &entitlement) != 0)
            return chat_error(conn, 503, "AI_ACCESS_UNAVAILABLE", "AI 授权暂时无法核验，请稍后重试");
        if (entitlement.status != AI_ACCESS_ACTIVE)
            return chat_error(conn, 403, ai_access_error_code(entitlement.status), "当前账号未获有效 AI 授权，请联系平台负责人");
        const char *media_type = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Content-Type");
        if (media_type == NULL || strncasecmp(media_type, "application/json", 16) != 0 ||
            (media_type[16] != '\0' && media_type[16] != ';')) {
            return chat_error(conn, 415, "UNSUPPORTED_MEDIA_TYPE", "请使用 application/json");
        }
        const char *parse_end = NULL;
        cJSON *input = body != NULL ? cJSON_ParseWithLengthOpts(body, body_len + 1, &parse_end, 1) : NULL;
        if (!chat_fields_valid(input) || parse_end != body + body_len ||
            chat_contains_null_escape(body, body_len)) {
            cJSON_Delete(input);
            return chat_error(conn, 400, "INVALID_REQUEST", "请求字段或格式不正确");
        }
        int question_id = cJSON_GetObjectItemCaseSensitive(input, "question_id")->valueint;
        char key[32]; snprintf(key, sizeof(key), "%d", question_id);
        if (cfg->db == NULL || access(cfg->db_path, R_OK) != 0) {
            cJSON_Delete(input); return chat_error(conn, 500, "INTERNAL_ERROR", "题目读取失败");
        }
        cJSON *source = kv_to_question(key, cfg->db);
        cJSON *status = source != NULL ? cJSON_GetObjectItemCaseSensitive(source, "status") : NULL;
        if (source == NULL || (cJSON_IsString(status) && strcmp(status->valuestring, "active") != 0)) {
            cJSON_Delete(source); cJSON_Delete(input);
            return chat_error(conn, 404, "QUESTION_NOT_FOUND", "题目不存在或不可用");
        }
        cJSON *request = cJSON_CreateObject();
        cJSON *question = request != NULL ? cJSON_AddObjectToObject(request, "question") : NULL;
        const char *fields[] = { "id", "content", "options", "knowledge_points", "type" };
        int valid = question != NULL && cJSON_AddNumberToObject(request, "user_id", session->user_id) != NULL;
        for (size_t i = 0; valid && i < sizeof(fields) / sizeof(fields[0]); i++) {
            cJSON *value = cJSON_GetObjectItemCaseSensitive(source, fields[i]);
            cJSON *copy = value != NULL ? cJSON_Duplicate(value, 1) : NULL;
            if (copy == NULL || !cJSON_AddItemToObject(question, fields[i], copy)) {
                cJSON_Delete(copy); valid = 0;
            }
        }
        cJSON *message = cJSON_GetObjectItemCaseSensitive(input, "message");
        if (valid && cJSON_AddStringToObject(request, "message", message->valuestring) == NULL) valid = 0;
        cJSON *conversation = cJSON_GetObjectItemCaseSensitive(input, "conversation_id");
        if (valid && conversation != NULL && cJSON_AddStringToObject(request, "conversation_id", conversation->valuestring) == NULL) valid = 0;
        const char *teaching_fields[] = { "mode", "student_answer" };
        for (size_t i = 0; valid && i < sizeof(teaching_fields) / sizeof(teaching_fields[0]); i++) {
            cJSON *value = cJSON_GetObjectItemCaseSensitive(input, teaching_fields[i]);
            if (value != NULL && cJSON_AddStringToObject(request, teaching_fields[i], value->valuestring) == NULL) valid = 0;
        }
        if (valid) payload = cJSON_PrintUnformatted(request);
        cJSON_Delete(request); cJSON_Delete(source); cJSON_Delete(input);
        if (payload == NULL) return chat_error(conn, 500, "INTERNAL_ERROR", "题目读取失败");
    } else {
        const char *job_id = url + sizeof("/api/v1/chat/") - 1;
        if (!chat_uuid_valid(job_id)) return chat_error(conn, 404, "JOB_NOT_FOUND", "任务不存在或已过期");
        snprintf(path, sizeof(path), "/internal/v1/jobs/%s", job_id);
    }
    char *response = NULL;
    int http_status = ai_service_request(cfg, method, path, session->user_id, payload, &response);
    cJSON_free(payload);
    if (response == NULL) return chat_error(conn, 500, "INTERNAL_ERROR", "服务器内部错误");
    enum MHD_Result ret = reply_json(conn, (unsigned int)http_status, response);
    free(response);
    return ret;
}

/* ── 静态文件服务 ── */

static const char *content_type_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (ext == NULL) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(ext, ".js") == 0)   return "application/javascript; charset=utf-8";
    if (strcmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(ext, ".png") == 0)  return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    return "application/octet-stream";
}

static enum MHD_Result serve_static(struct MHD_Connection *conn, const char *url,
                                    const AppConfig *cfg) {
    if (strstr(url, "..") != NULL) {
        return reply_json(conn, MHD_HTTP_FORBIDDEN, "{\"error\":\"forbidden\"}");
    }

    char rel[512];
    const char *p = (*url == '/') ? url + 1 : url;
    if (p[0] == '\0') p = "index.html";
    snprintf(rel, sizeof(rel), "%s", p);
    /* 去掉可能存在的 query 参数 */
    char *q = strchr(rel, '?');
    if (q != NULL) *q = '\0';

    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", cfg->web_root, rel);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return reply_json(conn, MHD_HTTP_NOT_FOUND, "{\"error\":\"not found\"}");
    }
    long sz;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"read failed\"}");
    }
    rewind(f);
    char *data = malloc((size_t)sz + 1);
    if (data == NULL) {
        fclose(f);
        return reply_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                          "{\"error\":\"oom\"}");
    }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[rd] = '\0';

    struct MHD_Response *resp = MHD_create_response_from_buffer(rd, data,
                                         MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", content_type_for(path));
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    free(data);
    return ret;
}

/* ── 路由分发入口 ── */

enum MHD_Result route_dispatch(struct MHD_Connection *conn, const char *url,
                               const char *method, const char *body,
                               size_t body_len, const AppConfig *cfg) {
    /* GET /api/v1/health */
    if (strcmp(method, "GET") == 0 &&
        strncmp(url, "/api/v1/health", 14) == 0 &&
        (url[14] == '\0' || url[14] == '?')) {
        return handle_health(conn);
    }

    /* POST /api/v1/auth/login */
    if (strcmp(method, "POST") == 0 &&
        strcmp(url, "/api/v1/auth/login") == 0) {
        return handle_login(conn, body, body_len, cfg);
    }

    /* POST /api/v1/auth/register */
    if (strcmp(method, "POST") == 0 &&
        strcmp(url, "/api/v1/auth/register") == 0) {
        return handle_register(conn, body, body_len, cfg);
    }

    /* POST /api/v1/auth/logout */
    if (strcmp(method, "POST") == 0 &&
        strcmp(url, "/api/v1/auth/logout") == 0) {
        return handle_logout(conn);
    }

    /* GET /api/v1/auth/me */
    if (strcmp(method, "GET") == 0 &&
        strcmp(url, "/api/v1/auth/me") == 0) {
        return handle_me(conn);
    }

    /* GET /api/v1/ai/access：统一 AI 授权查询（来自 PR #14） */
    if (strcmp(url, "/api/v1/ai/access") == 0) return handle_ai_access(conn, method, cfg);

    /* 题目接口：GET 只读；POST/PUT/DELETE 写操作（teacher/admin）。 */
    if (strcmp(url, "/api/v1/questions") == 0 ||
        strncmp(url, "/api/v1/questions/", sizeof("/api/v1/questions/") - 1) == 0) {
        return handle_questions(conn, url, method, body, body_len, cfg);
    }

    if (strcmp(url, "/api/v1/chat") == 0 ||
        strncmp(url, "/api/v1/chat/", sizeof("/api/v1/chat/") - 1) == 0) {
        return handle_chat(conn, url, method, body, body_len, cfg);
    }

    /* 其他 /api/ 路径 → 404 */
    if (strncmp(url, "/api/", 5) == 0) {
        return reply_json(conn, MHD_HTTP_NOT_FOUND,
                          "{\"error\":\"api not found\"}");
    }

    return serve_static(conn, url, cfg);
}
