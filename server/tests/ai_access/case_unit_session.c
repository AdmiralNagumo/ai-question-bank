#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "auth.h"

static int fail_source = 1;
static unsigned char nonce = 0x11;
static int calls;
int __wrap_RAND_bytes(unsigned char *buffer, int count) {
    calls++;
    if (fail_source) return 0;
    memset(buffer,nonce,(size_t)count); return 1;
}
int main(void) {
    assert(session_create(1,"student","first") == NULL);
    fail_source=0;
    const char *first=session_create(1,"student","first");
    assert(first != NULL && strlen(first) == SESSION_TOKEN_LEN);
    char saved[SESSION_TOKEN_LEN+1]; memcpy(saved,first,sizeof(saved));
    int before=calls;
    assert(session_create(2,"student","collision") == NULL && calls-before == 5);
    assert(session_validate(saved)->user_id == 1);
    nonce=0x22;
    const char *second=session_create(2,"student","second");
    assert(second != NULL && strcmp(saved,second) != 0 && session_validate(second)->user_id == 2);
    session_destroy(saved);
    assert(session_validate(saved) == NULL);
    puts("PASS: 随机源失败和碰撞耗尽时拒绝创建，会话格式/身份及登出保持兼容");
    return 0;
}
