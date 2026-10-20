#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"
#include "ai_client.h"

#define MAX_AI_RESPONSE 65536U
typedef struct { char *data; size_t len; int too_large; int allocation_failed; } ResponseBuffer;

static size_t receive_data(char *data, size_t size, size_t count, void *context) {
    ResponseBuffer *buffer = context;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes > MAX_AI_RESPONSE - buffer->len) { buffer->too_large = 1; return 0; }
    char *next = realloc(buffer->data, buffer->len + bytes + 1);
    if (next == NULL) { buffer->allocation_failed = 1; return 0; }
    buffer->data = next;
    memcpy(buffer->data + buffer->len, data, bytes);
    buffer->len += bytes;
    buffer->data[buffer->len] = '\0';
    return bytes;
}

static int failure(char **out, int code, const char *json) {
    *out = strdup(json);
    return *out != NULL ? code : 500;
}

static int token_valid(const char *token) {
    if (token == NULL) return 0;
    size_t length = strlen(token);
    if (length < 32 || length > 128) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int ai_service_request(const AppConfig *cfg, const char *method, const char *path,
                       int user_id, const char *body, char **out_json) {
    if (out_json == NULL) return 500;
    *out_json = NULL;
    if (!token_valid(cfg->ai_service_token) || cfg->ai_service_url == NULL) {
        return failure(out_json, 503,
            "{\"error\":\"AI 服务尚未配置\",\"code\":\"AI_SERVICE_UNAVAILABLE\"}");
    }
    char url[2048];
    size_t base_len = strlen(cfg->ai_service_url);
    while (base_len > 0 && cfg->ai_service_url[base_len - 1] == '/') base_len--;
    if (base_len + strlen(path) >= sizeof(url)) {
        return failure(out_json, 503,
            "{\"error\":\"AI 服务配置不正确\",\"code\":\"AI_SERVICE_UNAVAILABLE\"}");
    }
    memcpy(url, cfg->ai_service_url, base_len);
    strcpy(url + base_len, path);
    char authorization[160];
    char owner[64];
    snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", cfg->ai_service_token);
    snprintf(owner, sizeof(owner), "X-User-Id: %d", user_id);

    CURL *curl = curl_easy_init();
    if (curl == NULL) return failure(out_json, 500, "{\"error\":\"服务器内部错误\",\"code\":\"INTERNAL_ERROR\"}");
    struct curl_slist *headers = NULL;
    const char *values[] = { authorization, owner, "Content-Type: application/json", "Accept: application/json" };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        struct curl_slist *next = curl_slist_append(headers, values[i]);
        if (next == NULL) {
            curl_slist_free_all(headers); curl_easy_cleanup(curl);
            return failure(out_json, 500, "{\"error\":\"服务器内部错误\",\"code\":\"INTERNAL_ERROR\"}");
        }
        headers = next;
    }
    ResponseBuffer response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, cfg->ai_connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg->ai_request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, ""); /* 内部请求绕过系统代理，避免服务凭证外泄。 */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        free(response.data);
        if (response.allocation_failed) return failure(out_json, 500,
            "{\"error\":\"服务器内部错误\",\"code\":\"INTERNAL_ERROR\"}");
        if (response.too_large) return failure(out_json, 502,
            "{\"error\":\"AI 服务返回内容过长\",\"code\":\"AI_INVALID_RESPONSE\"}");
        const char *reason = result == CURLE_OPERATION_TIMEDOUT ? "request_timeout" :
            (result == CURLE_COULDNT_CONNECT || result == CURLE_COULDNT_RESOLVE_HOST) ? "connect_failed" : "transport_failed";
        /* 只记固定分类和数字，不记录URL、请求头、凭证或供应商响应。 */
        fprintf(stderr, "[ai-client] %s (curl_code=%d)\n", reason, (int)result);
        if (result == CURLE_OPERATION_TIMEDOUT) return failure(out_json, 504,
            "{\"error\":\"AI 服务连接超时\",\"code\":\"AI_SERVICE_TIMEOUT\"}");
        return failure(out_json, 503,
            "{\"error\":\"AI 服务暂时不可用\",\"code\":\"AI_SERVICE_UNAVAILABLE\"}");
    }
    if (status == 401) {
        fprintf(stderr, "[ai-client] internal_auth_failed: Pi returned HTTP 401; check AI_SERVICE_TOKEN on both services\n");
        free(response.data);
        return failure(out_json, 503,
            "{\"error\":\"AI 服务凭证配置不正确\",\"code\":\"AI_SERVICE_UNAVAILABLE\"}");
    }
    cJSON *json = response.data != NULL ? cJSON_ParseWithOpts(response.data, NULL, 1) : NULL;
    int valid = cJSON_IsObject(json) && ((status >= 400 && status <= 599) || status == 200 || status == 202);
    if (valid && status >= 400) {
        valid = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "error")) &&
                cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "code"));
    } else if (valid) {
        cJSON *job = cJSON_GetObjectItemCaseSensitive(json, "job_id");
        cJSON *conversation = cJSON_GetObjectItemCaseSensitive(json, "conversation_id");
        cJSON *state = cJSON_GetObjectItemCaseSensitive(json, "status");
        cJSON *question = cJSON_GetObjectItemCaseSensitive(json, "question_id");
        valid = cJSON_IsString(job) && strlen(job->valuestring) == 36 &&
                cJSON_IsString(conversation) && strlen(conversation->valuestring) == 36 &&
                cJSON_IsString(state) && cJSON_IsNumber(question) && question->valuedouble > 0;
        if (valid && strcmp(state->valuestring, "succeeded") == 0) {
            valid = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "reply")) &&
                    cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "model")) &&
                    cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(json, "usage"));
        } else if (valid && strcmp(state->valuestring, "failed") == 0) {
            valid = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "error")) &&
                    cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "code")) &&
                    cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(json, "http_status"));
        } else if (valid) {
            valid = strcmp(state->valuestring, "queued") == 0 || strcmp(state->valuestring, "running") == 0 ||
                    strcmp(state->valuestring, "cancelled") == 0;
        }
    }
    if (!valid) {
        cJSON_Delete(json); free(response.data);
        return failure(out_json, 502,
            "{\"error\":\"AI 服务返回格式不正确\",\"code\":\"AI_INVALID_RESPONSE\"}");
    }
    cJSON_Delete(json);
    *out_json = response.data;
    return (int)status;
}
