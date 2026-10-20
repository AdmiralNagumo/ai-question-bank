#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "ai_client.h"

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    AppConfig cfg = config_load();
    if (strcmp(argv[1], "--config") == 0) {
        printf("%ld %ld\n", cfg.ai_connect_timeout_ms, cfg.ai_request_timeout_ms);
        return 0;
    }
    cfg.ai_service_url = argv[1];
    cfg.ai_service_token = "test-only-internal-service-12345678";
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    char *body = NULL;
    int status = ai_service_request(&cfg, "GET", "/internal/v1/jobs/11111111-1111-4111-8111-111111111111", 42, NULL, &body);
    if (body == NULL) { curl_global_cleanup(); return 1; }
    printf("%d\n%s\n", status, body);
    free(body);
    curl_global_cleanup();
    return 0;
}
