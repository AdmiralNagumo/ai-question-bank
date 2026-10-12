# C 后端（libmicrohttpd + SQLite + libcurl + cJSON）构建规则
# 目标（与根 Makefile 分派约定一致）：install lint test build run clean
# 系统级依赖（libmicrohttpd-dev/libcurl4-openssl-dev/libsqlite3-dev）
# 由 scripts/install_env.sh 安装，本 Makefile 只负责编译与测试。

SHELL := /bin/bash

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -Iinclude -Ithird_party
LDLIBS := -lmicrohttpd -lcurl -lsqlite3 -lpthread -lm -lcrypto

BIN := ai-question-bank-server
SRCS := src/main.c src/config.c src/db.c src/http_server.c src/routes.c src/ai_client.c src/auth.c src/kv_store.c third_party/cJSON.c

.PHONY: install lint test build run clean

install: ## 系统依赖通过 scripts/install_env.sh 安装（无 pip/npm 包管理器依赖）
	@echo "C 后端无需安装依赖；请先执行: bash scripts/install_env.sh"

lint: ## 语法 + 语义检查（-fsyntax-only，配合 -Wall -Wextra）
	$(CC) $(CFLAGS) -fsyntax-only $(SRCS)

build: ## 编译生成 $(BIN)
	$(CC) $(CFLAGS) -o $(BIN) $(SRCS) $(LDLIBS)

test: build ## 构建 + 运行全部测试（由 run_tests.sh 管理服务生命周期）
	bash tests/run_tests.sh --all

run: build ## 开发运行（http://localhost:8000，自动加载 .env）
	@if [ -f .env ]; then set -a; source .env; set +a; fi; ./$(BIN)

clean: ## 清理构建产物与测试数据
	rm -f $(BIN) data/*.db 2>/dev/null || true
