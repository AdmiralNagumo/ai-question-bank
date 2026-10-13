#!/usr/bin/env bash
# 注册测试 case：注册后自动登录验证（/me + 登出 + token 失效）
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}[FAIL]${NC} $1"; }

PORT="${TEST_PORT:-${PORT:-8099}}"

# 先注册一个用户
BODY=$(curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"autologin","password":"mypass123","confirm_password":"mypass123","role":"teacher"}')
TOKEN=$(echo "$BODY" | sed -n 's/.*"token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[ -n "$TOKEN" ] && ok "注册后拿到 token" || { fail "注册后未返回 token"; exit 1; }

# 检查点 1：注册后直接用返回的 token 访问 /me
BODY=$(curl -s --noproxy '*' "http://127.0.0.1:$PORT/api/v1/auth/me" \
    -H "Authorization: Bearer $TOKEN")
echo "$BODY" | grep -q '"name":"autologin"' && ok "注册后 /me 返回正确 username" || fail "/me username 不符（响应: $BODY）"
echo "$BODY" | grep -q '"role":"teacher"' && ok "注册后 /me 返回正确 role" || fail "/me role 不符（响应: $BODY）"

# 检查点 2：注册后能正常登出
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/logout" \
    -H "Authorization: Bearer $TOKEN")
[ "$HTTP_CODE" = "200" ] && ok "注册后登出返回 200" || fail "注册后登出期望 200 实际 $HTTP_CODE"

# 检查点 3：登出后 token 失效
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PORT/api/v1/auth/me" \
    -H "Authorization: Bearer $TOKEN")
[ "$HTTP_CODE" = "401" ] && ok "登出后 token 失效返回 401" || fail "登出后 token 仍有效，期望 401 实际 $HTTP_CODE"

[ "$FAIL" -eq 0 ] || exit 1