# =============================================================================
# AI+题库智能教学平台 —— 根构建文件（GNU Make）
#
# 设计原则：模块化分派。
#   每个子模块（server / 未来新增模块）自带自己的 Makefile，
#   根 Makefile 只负责把目标分派到对应目录。
#
# 新增模块（例如新增一个 Go / Rust 微服务）只需：
#   1. 在 MODULES 中加入目录名；
#   2. 在该目录下提供自己的 Makefile（install/lint/test/build/run/clean）。
#   无需修改根 Makefile 的其他内容。
# =============================================================================

SHELL := /bin/bash

# 所有需要纳入统一构建的模块目录（按需增删）
MODULES := ai-service server

.PHONY: help setup env-check install lint test build run clean $(MODULES)

help: ## 显示所有可用目标
	@echo "AI+题库智能教学平台 构建入口"
	@echo ""
	@echo "用法（在仓库根目录执行）："
	@echo "  make help            显示本帮助"
	@echo "  make env-check       检查本机/服务器环境依赖"
	@echo "  make setup           首次初始化：环境检查 + 安装系统依赖"
	@echo "  make lint            运行所有模块静态检查"
	@echo "  make test            运行所有模块测试（含冒烟测试）"
	@echo "  make build           构建所有模块产物"
	@echo "  make run             开发模式启动（提示各模块启动命令）"
	@echo "  make clean           清理各模块构建产物与缓存"
	@echo ""
	@echo "也支持模块级目标，如：make build-server  make test-server"
	@echo "当前模块：$(MODULES)"

setup: env-check install ## 首次初始化
	@echo "✔ 环境准备完成，可执行 make build && make test"

env-check: ## 检查环境依赖（Linux 服务器）
	@bash scripts/check_env.sh

# -----------------------------------------------------------------------------
# 模块目标分派规则
# 对 MODULES 中每个目录 X，生成 build-X / test-X 等目标，
# 统一执行 `make -C X <target>`；目录缺少 Makefile 时跳过并提示。
# -----------------------------------------------------------------------------

install: $(addprefix install-,$(MODULES))
lint:    $(addprefix lint-,$(MODULES))
test:    $(addprefix test-,$(MODULES))
build:   $(addprefix build-,$(MODULES))
clean:   $(addprefix clean-,$(MODULES))

install-%:
	@if [ -f "$*/Makefile" ]; then $(MAKE) -C $* install; \
	else echo "⚠ 跳过 $*（目录下没有 Makefile）"; fi

lint-%:
	@if [ -f "$*/Makefile" ]; then $(MAKE) -C $* lint; \
	else echo "⚠ 跳过 $*（目录下没有 Makefile）"; fi

test-%:
	@if [ -f "$*/Makefile" ]; then $(MAKE) -C $* test; \
	else echo "⚠ 跳过 $*（目录下没有 Makefile）"; fi

build-%:
	@if [ -f "$*/Makefile" ]; then $(MAKE) -C $* build; \
	else echo "⚠ 跳过 $*（目录下没有 Makefile）"; fi

clean-%:
	@if [ -f "$*/Makefile" ]; then $(MAKE) -C $* clean; \
	else echo "⚠ 跳过 $*（目录下没有 Makefile）"; fi

run: ## 开发模式启动
	@echo "请执行："
	@echo "  make -C server run    # 后端服务（http://localhost:8000）"
	@echo "  web/ 为静态资源，由服务端直接托管，无需独立前端服务"
