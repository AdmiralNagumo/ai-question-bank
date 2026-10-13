#!/usr/bin/env bash
# 注册测试 case：注册后二次登录验证
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}[FAIL]${NC} $1"; }

PORT="${TEST_PORT:-${PORT:-8099}}"

# 注册一个用户（不使用自动登录 token，专门测二次登录）
curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"relogin","password":"mypass123","confirm_password":"mypass123","role":"student"}' >/dev/null 2>&1 || true

# 检查点 1：注册后能用相同凭证正常登录（二次登录）
BODY=$(curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"relogin","password":"mypass123"}')
echo "$BODY" | grep -q '"token"' && ok "注册后二次登录返回 token" || fail "二次登录未返回 token（响应: $BODY）"
echo "$BODY" | grep -q '"role":"student"' && ok "二次登录 role 正确" || fail "二次登录 role 不符（响应: $BODY）"
echo "$BODY" | grep -q '"username":"relogin"' && ok "二次登录 username 正确" || fail "二次登录 username 不符（响应: $BODY）"
TOKEN=$(echo "$BODY" | sed -n 's/.*"token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

# 检查点 2：二次登录的 token 能访问 /me
BODY=$(curl -s --noproxy '*' "http://127.0.0.1:$PORT/api/v1/auth/me" \
    -H "Authorization: Bearer $TOKEN")
echo "$BODY" | grep -q '"name":"relogin"' && ok "二次登录 token 可访问 /me" || fail "二次登录 token 访问 /me 失败（响应: $BODY）"

# 检查点 3：错误密码不能登录注册的账号
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"relogin","password":"wrongpass"}')
[ "$HTTP_CODE" = "401" ] && ok "注册用户错误密码返回 401" || fail "错误密码期望 401 实际 $HTTP_CODE"

# 检查点 4：不存在的用户登录返回 401（防枚举一致性）
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"nobody","password":"pass1234"}')
[ "$HTTP_CODE" = "401" ] && ok "不存在用户返回 401（防枚举）" || fail "不存在用户期望 401 实际 $HTTP_CODE"

[ "$FAIL" -eq 0 ] || exit 1