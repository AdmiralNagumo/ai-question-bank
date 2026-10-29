#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <openssl/crypto.h>
#include "teacher_api_routes.h"
#include "teacher_api.h"
#include "auth.h"

static enum MHD_Result send_json(struct MHD_Connection *conn,int status,cJSON *root) {
    char *json=root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!json) { status=500; json=strdup("{\"error\":\"服务器内部错误\",\"code\":\"INTERNAL_ERROR\"}"); }
    if (!json) return MHD_NO;
    struct MHD_Response *response=MHD_create_response_from_buffer(strlen(json),json,MHD_RESPMEM_MUST_COPY);
    cJSON_free(json);
    if (!response) return MHD_NO;
    MHD_add_response_header(response,"Content-Type","application/json; charset=utf-8");
    MHD_add_response_header(response,"Cache-Control","no-store");
    if (status==429) {
        char seconds[32]; time_t now=time(NULL);
        snprintf(seconds,sizeof(seconds),"%lld",(long long)(86400-((now+28800)%86400)));
        MHD_add_response_header(response,"Retry-After",seconds);
    }
    if (status==405) MHD_add_response_header(response,"Allow","GET, PUT, PATCH");
    enum MHD_Result result=MHD_queue_response(conn,(unsigned int)status,response);
    MHD_destroy_response(response); return result;
}
static enum MHD_Result error_json(struct MHD_Connection *conn,int status,const char *code) {
    cJSON *root=cJSON_CreateObject();
    if (root) { cJSON_AddStringToObject(root,"error","请求未完成，请检查权限、配置或额度"); cJSON_AddStringToObject(root,"code",code); }
    return send_json(conn,status,root);
}
/* 和chat一样拒绝重复字段与NUL转义；未知字段不允许覆盖所有者/供应商地址。 */
static cJSON *parse(const char *body,size_t length,const char *first,const char *second) {
    if (!body || strlen(body)!=length) return NULL;
    for (size_t i=0;i+1<length;i++) {
        if (body[i]!='\\') continue;
        if (i+5<length && !memcmp(body+i+1,"u0000",5)) return NULL;
        i++;
    }
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(body,length+1,&end,1);
    if (!cJSON_IsObject(root) || end!=body+length) { cJSON_Delete(root); return NULL; }
    for (cJSON *item=root->child;item;item=item->next) {
        if (strcmp(item->string,first) && (!second || strcmp(item->string,second))) { cJSON_Delete(root); return NULL; }
        for (cJSON *previous=root->child;previous!=item;previous=previous->next) {
            if (!strcmp(previous->string,item->string)) { cJSON_Delete(root); return NULL; }
        }
    }
    return root;
}
enum MHD_Result teacher_api_route(struct MHD_Connection *conn,const char *url,const char *method,
                                  const char *body,size_t length,const AppConfig *cfg) {
    Session *session=session_validate(extract_bearer_token(MHD_lookup_connection_value(conn,MHD_HEADER_KIND,"Authorization")));
    if (!session) return error_json(conn,401,"UNAUTHORIZED");
    if (!strcmp(url,"/api/v1/ai/access")) {
        if (strcmp(method,"GET")) {
            /* 独立Allow头以保持权限查询只读。 */
            const char *json="{\"error\":\"请求方法不支持\",\"code\":\"METHOD_NOT_ALLOWED\"}";
            struct MHD_Response *response=MHD_create_response_from_buffer(strlen(json),(void *)json,MHD_RESPMEM_PERSISTENT);
            if (!response) return MHD_NO;
            MHD_add_response_header(response,"Allow","GET");
            MHD_add_response_header(response,"Content-Type","application/json; charset=utf-8");
            MHD_add_response_header(response,"Cache-Control","no-store");
            enum MHD_Result result=MHD_queue_response(conn,405,response); MHD_destroy_response(response); return result;
        }
        const char *id=MHD_lookup_connection_value(conn,MHD_GET_ARGUMENT_KIND,"question_id");
        long number=0;
        if (!id || !*id || strlen(id)>10) return error_json(conn,400,"QUESTION_REQUIRED");
        for (const char *p=id;*p;p++) {
            if (*p<'0' || *p>'9') return error_json(conn,400,"INVALID_REQUEST");
            number=number*10+(*p-'0');
            if (number>INT_MAX) return error_json(conn,400,"INVALID_REQUEST");
        }
        if (!number) return error_json(conn,400,"INVALID_REQUEST");
        TeacherExecution execution; cJSON *access=NULL;
        const char *code=teacher_api_resolve(cfg,session->user_id,session->role,(int)number,0,&execution,&access);
        teacher_api_clear(&execution);
        if (code && !access) return error_json(conn,teacher_api_error_status(code),code);
        return send_json(conn,200,access);
    }
    if (strcmp(session->role,"teacher")) return error_json(conn,403,"TEACHER_REQUIRED");
    cJSON *value=NULL; const char *code=NULL;
    if (!strcmp(method,"GET")) code=teacher_api_metadata(cfg,session->user_id,&value);
    else if (!strcmp(method,"PUT") || !strcmp(method,"PATCH")) {
        const char *media=MHD_lookup_connection_value(conn,MHD_HEADER_KIND,"Content-Type");
        if (!media || strncmp(media,"application/json",16) || (media[16] && media[16]!=';')) return error_json(conn,415,"UNSUPPORTED_MEDIA_TYPE");
        int put=!strcmp(method,"PUT");
        cJSON *root=parse(body,length,put ? "api_key" : "enabled",put ? "model" : NULL);
        if (!root) return error_json(conn,400,"INVALID_REQUEST");
        if (put) {
            cJSON *key=cJSON_GetObjectItemCaseSensitive(root,"api_key"),*model=cJSON_GetObjectItemCaseSensitive(root,"model");
            if (!cJSON_IsString(key) || (model && !cJSON_IsString(model))) code="INVALID_REQUEST";
            else code=teacher_api_save(cfg,session->user_id,key->valuestring,model ? model->valuestring : "deepseek-flash",1);
            if (cJSON_IsString(key)) OPENSSL_cleanse(key->valuestring,strlen(key->valuestring));
        } else {
            cJSON *enabled=cJSON_GetObjectItemCaseSensitive(root,"enabled");
            if (!cJSON_IsBool(enabled)) code="INVALID_REQUEST";
            else code=teacher_api_toggle(cfg,session->user_id,cJSON_IsTrue(enabled));
        }
        cJSON_Delete(root);
        if (!code) code=teacher_api_metadata(cfg,session->user_id,&value);
    } else return error_json(conn,405,"METHOD_NOT_ALLOWED");
    if (code) { cJSON_Delete(value); return error_json(conn,teacher_api_error_status(code),code); }
    cJSON *root=cJSON_CreateObject();
    if (!root || !cJSON_AddItemToObject(root,"api",value)) { cJSON_Delete(root); cJSON_Delete(value); return error_json(conn,500,"INTERNAL_ERROR"); }
    return send_json(conn,200,root);
}
