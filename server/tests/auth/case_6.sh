#!/usr/bin/env bash
# 注册测试 case：注册校验失败场景
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}[FAIL]${NC} $1"; }

PORT="${TEST_PORT:-${PORT:-8099}}"

# 检查点 1：密码太短（< 8 位）
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r1","password":"ab1","confirm_password":"ab1"}')
[ "$HTTP_CODE" = "400" ] && ok "密码太短返回 400" || fail "密码太短期望 400 实际 $HTTP_CODE"

BODY=$(curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r2","password":"ab1","confirm_password":"ab1"}')
echo "$BODY" | grep -q '"密码至少 8 位' && ok "密码太短提示正确" || fail "密码太短提示不符（响应: $BODY）"

# 检查点 2：密码仅字母
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r3","password":"abcdefgh","confirm_password":"abcdefgh"}')
[ "$HTTP_CODE" = "400" ] && ok "密码仅字母返回 400" || fail "密码仅字母期望 400 实际 $HTTP_CODE"

# 检查点 3：密码仅数字
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r4","password":"12345678","confirm_password":"12345678"}')
[ "$HTTP_CODE" = "400" ] && ok "密码仅数字返回 400" || fail "密码仅数字期望 400 实际 $HTTP_CODE"

# 检查点 4：两次密码不一致
BODY=$(curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r5","password":"pass1234","confirm_password":"pass5678"}')
echo "$BODY" | grep -q '"两次输入的密码不一致"' && ok "密码不一致提示正确" || fail "密码不一致提示不符（响应: $BODY）"

# 检查点 5：缺少字段（无 confirm_password）
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r6","password":"pass1234"}')
[ "$HTTP_CODE" = "400" ] && ok "缺少字段返回 400" || fail "缺少字段期望 400 实际 $HTTP_CODE"

# 检查点 6：用户名已存在
# 先注册一个
curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"dupuser","password":"mypass123","confirm_password":"mypass123"}' >/dev/null 2>&1 || true
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"dupuser","password":"mypass123","confirm_password":"mypass123"}')
[ "$HTTP_CODE" = "409" ] && ok "用户名已存在返回 409" || fail "用户名已存在期望 409 实际 $HTTP_CODE"

# 检查点 7：无效 role
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r7","password":"mypass123","confirm_password":"mypass123","role":"superadmin"}')
[ "$HTTP_CODE" = "400" ] && ok "无效 role 返回 400" || fail "无效 role 期望 400 实际 $HTTP_CODE"

BODY=$(curl -s --noproxy '*' -X POST "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"r8","password":"mypass123","confirm_password":"mypass123","role":"superadmin"}')
echo "$BODY" | grep -q '仅支持 student/teacher/admin' && ok "无效 role 提示正确" || fail "无效 role 提示不符（响应: $BODY）"

# 检查点 8：用户名为空
HTTP_CODE=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"","password":"pass1234","confirm_password":"pass1234"}')
[ "$HTTP_CODE" = "400" ] && ok "用户名为空返回 400" || fail "用户名为空期望 400 实际 $HTTP_CODE"

[ "$FAIL" -eq 0 ] || exit 1