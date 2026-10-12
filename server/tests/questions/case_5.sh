#!/usr/bin/env bash
# kv_store 题目 seed 回归，使用独立数据库测试幂等性。
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Iinclude -Ithird_party \
    tests/questions/db_test.c src/db.c src/kv_store.c third_party/cJSON.c \
    -lsqlite3 -lcrypto -o "$test_dir/db_test"
"$test_dir/db_test" "$test_dir/fresh.db"
