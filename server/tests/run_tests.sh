#!/usr/bin/env bash
# ============================================================================
# 测试运行器（三阶段）：
#   Phase 0 – 预编译所有 C 测试二进制（一次性编译，复用执行）
#   Phase 1 – 独立单元测试（case_unit*，无需 server）
#   Phase 2 – API 集成测试（其余 case_*，启动 server 后执行）
#
# 用法：
#   bash tests/run_tests.sh --all              # 运行全部测试目录（两阶段）
#   bash tests/run_tests.sh kv                 # 运行指定目录（自动判断阶段）
#   bash tests/run_tests.sh auth health        # 运行多个目录
#
# 目录约定：
#   每个测试目录下含 case_*.sh。
#   case_unit*.sh → 单元测试（Phase 1，无需 server）
#   其他 case_*.sh → API 测试（Phase 2，需要 server）
#   case_*.c      → 预编译为二进制供对应 case 脚本使用
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

# ── 颜色 ──
GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

# ── 全局统计 ──
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_CASES=0

# ── 配置 ──
TEST_PORT="${TEST_PORT:-8099}"
export TEST_PORT
export PORT="$TEST_PORT"
TEST_TMP_DIR=$(mktemp -d)
DB_PATH="${DATABASE_PATH:-$TEST_TMP_DIR/test.db}"
export DATABASE_PATH="$DB_PATH"
SERVER_LOG="$TEST_TMP_DIR/server.log"
AQB_TEST_BIN_DIR="/tmp/aqb_test_bin_$$"
export AQB_TEST_BIN_DIR
if [ -e "$DB_PATH" ]; then
    echo -e "${RED}拒绝覆盖已有数据库：$DB_PATH；请使用临时测试库${NC}"
    exit 1
fi

# ── 解析参数 ──
if [ $# -eq 0 ]; then
    echo -e "${RED}用法: $0 [--all | dir_name1 dir_name2 ...]${NC}"
    exit 1
fi

if [ "$1" = "--all" ]; then
    DIRS=()
    for d in tests/*/; do
        dirname=$(basename "$d")
        [ "$dirname" = "run_tests" ] && continue
        if ls "$d"case_*.sh 2>/dev/null >/dev/null; then
            DIRS+=("$dirname")
        fi
    done
else
    DIRS=("$@")
fi

if [ ${#DIRS[@]} -eq 0 ]; then
    echo -e "${RED}未找到任何测试目录${NC}"
    exit 1
fi

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  测试运行器${NC}"
echo -e "${CYAN}  端口: $TEST_PORT  数据库: $DB_PATH${NC}"
echo -e "${CYAN}  测试目录: ${DIRS[*]}${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

# ── Phase 0：预编译所有 C 测试二进制 ──
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  Phase 0: 预编译 C 测试二进制${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

compile_ok=0
compile_fail=0

mkdir -p "$AQB_TEST_BIN_DIR"

# 定义各 C 测试的编译规则
# 格式：test_name|编译命令
# 每次新增 C 测试源文件时，在此添加一行即可。
COMPILE_RULES=(
    "kv_unit|gcc -std=c11 -Iinclude -Ithird_party -O0 -Wall -Wextra tests/kv/case_unit.c src/kv_store.c third_party/cJSON.c -lsqlite3 -lm -o ${AQB_TEST_BIN_DIR}/kv_unit"
)

for rule in "${COMPILE_RULES[@]}"; do
    name="${rule%%|*}"
    cmd="${rule#*|}"
    echo -e "  ${CYAN}[编译] ${name} ...${NC}"
    if eval "$cmd" 2>&1; then
        echo -e "  ${GREEN}[PASS] ${name} 编译成功${NC}"
        compile_ok=$((compile_ok + 1))
    else
        echo -e "  ${RED}[FAIL] ${name} 编译失败${NC}"
        compile_fail=$((compile_fail + 1))
    fi
done

if [ "$compile_fail" -gt 0 ]; then
    echo -e "${RED}⚠ 预编译阶段存在失败，终止。${NC}"
    rm -rf "$AQB_TEST_BIN_DIR"
    exit 1
fi
echo ""

# ── 执行单个 case 脚本 ──
# 参数：$1 = dir_name, $2 = case_script 路径, $3 = case_name
run_one_case() {
    local dir_name="$1"
    local case_script="$2"
    local case_name="$3"

    echo -e "${CYAN}  --- ${dir_name}/${case_name} ---${NC}"

    set +e
    timeout 60 bash "$case_script"
    rc=$?
    set -euo pipefail

    if [ "$rc" -eq 0 ]; then
        echo -e "  ${GREEN}[PASS] ${dir_name}/${case_name}${NC}"
        TOTAL_PASS=$((TOTAL_PASS + 1))
    else
        echo -e "  ${RED}[FAIL] ${dir_name}/${case_name}${NC}"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
    TOTAL_CASES=$((TOTAL_CASES + 1))
    echo ""
}

# ── 遍历指定目录，按模式筛选并执行 case ──
# 参数：$1 = 模式（glob pattern, 如 "case_unit*" 或 "case_*" 排除 unit）
#       $2 = 标签文字
run_case_group() {
    local pattern="$1"
    local label="$2"
    local group_pass=0
    local group_fail=0

    for dir_name in "${DIRS[@]}"; do
        dir_path="tests/${dir_name}"
        [ ! -d "$dir_path" ] && continue

        # 收集匹配的脚本
        local scripts=()
        if [ "$pattern" = "case_unit*" ]; then
            # Phase 1: 只匹配 case_unit*.sh
            for f in "$dir_path"/case_unit*.sh; do
                [ -f "$f" ] && scripts+=("$f")
            done
        else
            # Phase 2: 匹配 case_*.sh 但排除 case_unit*.sh
            for f in "$dir_path"/case_*.sh; do
                [ -f "$f" ] || continue
                local base; base=$(basename "$f" .sh)
                if [[ "$base" != case_unit* ]]; then
                    scripts+=("$f")
                fi
            done
        fi

        [ ${#scripts[@]} -eq 0 ] && continue

        for f in "${scripts[@]}"; do
            local case_name; case_name=$(basename "$f" .sh)
            run_one_case "$dir_name" "$f" "$case_name"
        done
    done
}

# ── 服务管理 ──
SERVER_PID=""

start_server() {
    >"$SERVER_LOG"  # 先清空日志

    # 用力杀掉占用端口的进程（多尝试几种方式）
    local port_pid
    port_pid="$(lsof -t -i ":$TEST_PORT" 2>/dev/null)" || true
    if [ -n "$port_pid" ]; then
        kill "$port_pid" 2>/dev/null || true
        sleep 0.5
        # 如果还没死，强杀
        if kill -0 "$port_pid" 2>/dev/null; then
            kill -9 "$port_pid" 2>/dev/null || true
            sleep 0.3
        fi
    fi
    # 备用方案：fuser（某些系统可用）
    fuser -k "${TEST_PORT}/tcp" 2>/dev/null || true
    sleep 0.3

    # 确认端口已释放
    if lsof -i ":$TEST_PORT" >/dev/null 2>&1; then
        echo -e "${RED}⚠ 端口 $TEST_PORT 仍被占用，无法启动测试服务${NC}"
        exit 1
    fi

    # 清空日志后启动
    : > "$SERVER_LOG"
    ./ai-question-bank-server >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 0.5  # 给进程一点初始化时间

    local _printed_diag=0
    for _ in $(seq 1 20); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo -e "${RED}⚠ 服务进程启动失败，日志:${NC}"
            cat "$SERVER_LOG"
            exit 1
        fi
        if curl -s --noproxy '*' "http://127.0.0.1:$TEST_PORT/api/v1/health" >/dev/null 2>&1; then
            return 0
        fi
        # 首轮失败时打印诊断信息
        if [ "$_printed_diag" -eq 0 ]; then
            _printed_diag=1
            echo -e "  ${CYAN}[诊断] server PID=$SERVER_PID 存活=$(kill -0 "$SERVER_PID" 2>&1 && echo '是' || echo '否')${NC}"
            echo -e "  ${CYAN}[诊断] curl HTTP 状态码: $(curl -s -o /dev/null -w '%{http_code}' --noproxy '*' "http://127.0.0.1:$TEST_PORT/api/v1/health" 2>&1 || echo 'curl失败')${NC}"
            echo -e "  ${CYAN}[诊断] ss 监听: $(ss -tlnp "src :$TEST_PORT" 2>/dev/null | tail -1)${NC}"
            echo -e "  ${CYAN}[诊断] http_proxy='${http_proxy:-}' https_proxy='${https_proxy:-}'${NC}"
            echo -e "  ${CYAN}[诊断] 日志前 5 行:${NC}"
            head -5 "$SERVER_LOG" 2>/dev/null | sed 's/^/        /'
        fi
        sleep 0.3
    done

    echo -e "${RED}⚠ 服务未在 6 秒内就绪，日志:${NC}"
    cat "$SERVER_LOG"
    exit 1
}

stop_server() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
}

cleanup() {
    stop_server
    rm -rf "$AQB_TEST_BIN_DIR" "$TEST_TMP_DIR"
}
trap cleanup EXIT

# ════════════════════════════════════════════════════════════════════════════
# Phase 1：独立单元测试（无需 server）
# ════════════════════════════════════════════════════════════════════════════
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  Phase 1: 独立单元测试（无需 server）${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

run_case_group "case_unit*" "单元测试"

# ════════════════════════════════════════════════════════════════════════════
# Phase 2：API 集成测试（启动 server）
# ════════════════════════════════════════════════════════════════════════════
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  Phase 2: API 集成测试${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

start_server

run_case_group "case_*" "API 测试"

stop_server

# ── 汇总 ──
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  测试汇总${NC}"
echo -e "${CYAN}========================================${NC}"
echo -e "  case 通过: ${GREEN}${TOTAL_PASS}${NC}"
echo -e "  case 失败: ${RED}${TOTAL_FAIL}${NC}"
echo -e "  case 总数: ${TOTAL_CASES}${NC}"

if [ "$TOTAL_FAIL" -eq 0 ]; then
    echo -e "${GREEN}✔ 全部测试通过${NC}"
    rm -rf "$AQB_TEST_BIN_DIR" "$TEST_TMP_DIR"
    exit 0
else
    echo -e "${RED}✘ 存在失败的测试${NC}"
    exit 1
fi