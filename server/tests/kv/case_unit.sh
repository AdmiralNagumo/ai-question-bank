#!/usr/bin/env bash
# kv_store 单元测试 case
# 运行预编译的 C 单元测试二进制，无需启动服务器进程
set -euo pipefail

cd "$(dirname "$0")/../.."

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

BIN="${AQB_TEST_BIN_DIR:?缺少 AQB_TEST_BIN_DIR（请通过 run_tests.sh 执行）}/kv_unit"

if [ ! -x "$BIN" ]; then
    echo -e "  ${RED}[FAIL] 测试二进制不存在: $BIN${NC}"
    exit 1
fi

if "$BIN"; then
    echo -e "  ${GREEN}[PASS] kv_store 单元测试全部通过${NC}"
else
    echo -e "  ${RED}[FAIL] kv_store 单元测试存在失败${NC}"
    exit 1
fi