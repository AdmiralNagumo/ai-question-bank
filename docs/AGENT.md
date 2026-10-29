# AGENT.md — AI Agent 项目手册


## 教师 API v1.3（当前默认模式）

默认AI_ROUTING_MODE=teacher：教师注册/登录即有资格GET/PUT/PATCH自己的/api/v1/teacher/ai-api，一个教师一条配置。学生提交题目后由C从qdata.teacher_id解析配置并校验日额度（默认每学生/每教师/上海自然日20次）；不叠加旧CLI授权，不回退平台Key。

仓库根目录先执行python3 scripts/configure_pi_demo.py和python3 scripts/configure_teacher_api.py，后者只补充C的加密主密钥，绝不打印或覆盖已有值。两个服务的AI_ROUTING_MODE保持一致；教师模式Pi无需全局AI_API_KEY。保留已有Key只在显式platform兼容模式使用。

启动后由前端/接口工具登录教师并配置其API。现有演示题归属admin，不会自动转移，需题库负责人提供teacher归属的题目联调。不要把新限额/路由测试误判为旧演示题直接可调用。

前端接口：`docs/pi-chat-api.md`（1.3）、`docs/teacher-api.md`及OpenAPI；教师Key只进后端，GET只回脱敏信息。本次没有改web业务页面、题库CRUD或注册策略。


> 维护者：戴儒骋、卢恒毅
> 用途：AI Agent 在本项目中生成/修改代码时的唯一权威参考。  
> 更新频率：项目结构、构建命令或编码规范变更时同步更新。

---

## 1. 项目概述

**AI+题库智能教学平台** 是一个面向教育场景的多教师、多学生、多租户智能题库与试卷管理系统。

- 教师维护题库/试卷并配置 AI 解答
- 学生提交题目、查看 AI 解答、在分支对话树中追问
- 管理员负责用户管理、全局 API、审计与运维

功能规格详见 [`AI题库项目整体预期功能说明书.md`](AI题库项目整体预期功能说明书.md)。

---

## 2. 技术栈

| 模块 | 技术 | 说明 |
|------|------|------|
| 后端 | **C11** · libmicrohttpd · SQLite3 · libcurl · cJSON | HTTP 服务 + 存储 + AI 代理（API Key 不出后端） |
| 存储层 | db.c（SQL 关系表）+ **kv_store**（目录式 KV） | 当前users/questions走KV；namespace实际映射同名表（users/questions），旧文档的kv_前缀不适用于现实现。AI授权/审计为专用关系表 |
| AI执行 | Node.js >=22.19 · TypeScript · Pi 0.84.1 | 内部教学技能/受限数学工具/异步队列，teacher模式Key由C加密托管并按任务交给Pi；platform模式Key仅Pi环境持有 |
| 前端 | 纯 HTML / CSS / JS | 静态页面，由 C 服务端直接托管，无构建工具链 |
| 构建 | GNU Make + gcc | `-std=c11 -Wall -Wextra`，零警告编译 |
| 部署 | systemd（Linux 服务器） | 见 `environment-setup.md` |
| 版本控制 | Git + GitHub | `main` 受保护，PR + CI + Code Review |

---

## 3. 目录结构

```
ai-question-bank/
├── .editorconfig / .gitattributes  # 编辑器与行尾配置
├── Makefile                    # 根构建入口（模块化分派）
├── README.md / CONTRIBUTING.md / LICENSE
├── .github/
│   ├── CODEOWNERS              # 模块审查人分配
│   ├── PULL_REQUEST_TEMPLATE.md
│   └── workflows/
│       └── ci.yml              # CI：每次 push/PR 自动 build + test
│
├── server/                     # C 后端（唯一业务入口）
│   ├── Makefile                # lint / build / test / run / clean
│   ├── .env.example            # 环境变量配置示例
│   ├── include/                # 头文件
│   │   ├── ai_client.h         # AI 代理接口（libcurl）
│   │   ├── auth.h              # 认证鉴权接口
│   │   ├── config.h            # 环境变量配置（含共享 sqlite3 连接）
│   │   ├── db.h                # SQLite 数据库接口
│   │   ├── debug.h             # 调试日志宏（编译时 -DDEBUG 开关，零开销）
│   │   ├── http_server.h       # HTTP 守护进程
│   │   ├── kv_store.h          # 目录式键值存储接口
│   │   └── routes.h            # 路由分发
│   ├── src/                    # 源码
│   │   ├── main.c              # 入口：加载配置 → db_init → kv_init → seed → 启动 HTTP
│   │   ├── config.c            # 环境变量读取（PORT/数据库/AI）
│   │   ├── db.c                # SQLite 建表、demo 账号 seed、固定题目 seed（经 kv_store 写入）
│   │   ├── http_server.c       # libmicrohttpd 守护进程
│   │   ├── routes.c            # /api/v1/* JSON API + 静态文件服务
│   │   ├── ai_client.c         # AI API 代理调用（libcurl）
│   │   ├── auth.c              # 认证鉴权实现
│   │   └── kv_store.c          # 目录式键值存储实现（namespace → 同名表）
│   ├── third_party/            # cJSON（MIT 协议，单文件库：cJSON.c + cJSON.h）
│   └── tests/                  # 测试（run_tests.sh 自动发现测试组）
│       ├── run_tests.sh        # 测试入口：自动遍历 auth/health/questions/kv 四组
│       ├── TEST.md             # 测试说明文档
│       ├── auth/               # 认证接口回归（case_1~4.sh）
│       ├── health/             # 健康检查回归（case_1.sh）
│       ├── kv/                 # kv_store 单元测试（case_unit.c + case_unit.sh，18 断言）
│       └── questions/          # 题目接口回归（case_1~5.sh）+ 辅助（check_api.py / db_test.c）
│
├── web/                        # 静态前端（SPA，hash 路由）
│   ├── index.html
│   ├── css/style.css
│   └── js/
│       ├── app.js              # 页面骨架与路由切换
│       ├── api.js              # API 客户端（在线模式 + file:// 离线演示降级）
│       ├── auth.js             # 登录页
│       ├── questions.js        # 题目列表页
│       └── chat.js             # 对话页（题目详情 + AI 对话）
│
├── docs/                       # 项目文档
│   ├── AGENT.md                # 本文件
│   ├── AI协作指南.md / AI辅助开发计划.md
│   ├── AI题库项目整体预期功能说明书.md
│   ├── architecture.md / auth.md / environment-setup.md
│   ├── kvstore.md              # kv_store 存储层接口文档（涉及存储时必读）
│   ├── questions.md            # 题目模块契约（数据初始化/接口/测试）
│   ├── 软件需求说明书.md / 软件项目开发计划.md / 项目开发时间轴.md
│   ├── 项目开发计划甘特图.xlsx / 系统UI设计稿.pptx
│   └── 2026秋季软件工程课程实践说明20260910.pdf
│
├── scripts/                    # 运维脚本
│   ├── check_env.sh / check_env.ps1    # 环境检查
│   ├── install_env.sh                  # 依赖安装
│   ├── github_setup.sh / github_setup.ps1  # GitHub 仓库配置
│   ├── branch_protection.json          # 分支保护规则
│   └── githooks/               # Git hooks（commit-msg / pre-commit）
│
└── deploy/systemd/             # 生产部署配置（ai-question-bank.service）
```

---

## 4. 构建与运行

### 4.1 有 Linux 环境（课程服务器 / WSL2）

```bash
# 环境检查与依赖安装
bash scripts/check_env.sh
sudo bash scripts/install_env.sh   # gcc make libmicrohttpd-dev libcurl4-openssl-dev libsqlite3-dev

# 构建
make build          # 编译 server/ 和 ai-service/；需先make -C ai-service install
make test           # 测试（run_tests.sh 自动发现 auth/health/questions/kv 测试组）
make lint           # 静态检查

# 单独运行某一测试组
bash server/tests/run_tests.sh kv         # 仅 kv_store 单元测试
bash server/tests/run_tests.sh questions  # 仅题目接口回归

# 运行（开发模式）
make -C server run  # 启动 http://localhost:8000
curl http://localhost:8000/api/v1/health   # → {"status":"ok"}
```

### 4.2 Windows 本地开发

- **C 后端无法在 Windows 直接编译**（gcc 过旧），以 GitHub Actions CI 编译结果为准
- 前端可直接用浏览器打开 `web/index.html` 预览
- 可选方案：WSL2 + Ubuntu 获得本地编译能力

### 4.3 配置

所有配置通过**环境变量**传入（默认值见 `server/src/config.c`）：

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `PORT` | 监听端口 | `8000` |
| `DATABASE_PATH` | SQLite 数据库路径 | `data/ai_question_bank.db` |
| `WEB_ROOT` | 静态文件根目录 | `../web`（相对 server/ 工作目录） |
| `AI_SERVICE_URL` | C访问内部Pi的地址 | `http://127.0.0.1:8001` |
| `AI_SERVICE_TOKEN` | C/Pi共享服务凭证 | 空（启动Pi联调前配置） |
| `AI_CONNECT_TIMEOUT_MS` | 内部HTTP连接超时，整数1–60000毫秒 | `500` |
| `AI_REQUEST_TIMEOUT_MS` | 内部HTTP总超时（含连接），整数1–60000毫秒 | `2000` |

C内部HTTP超时仅接受1–60000毫秒整数，非法值回退默认并脱敏记录；修改需重启C，总请求预算包含连接，不能代替Pi模型任务AI_TIMEOUT_MS。诊断日志分类和单线程等待边界见architecture.md的“C → Pi 内部请求超时与诊断”。

模型变量AI_API_BASE_URL/AI_MODEL/AI_API_KEY仅由ai-service读取，完整配置见其.env.example。所有账号默认无AI授权；scripts/ai_access.py是可信运维写入口，不可根据用户role=admin开放HTTP授权写入。接口和边界见pi-chat-api.md（1.2）、ai-access.md。

生产部署：复制 `server/.env.example` → `server/.env`，systemd 通过 `EnvironmentFile` 加载。

---

## 5. 编码规范（C11）

### 5.1 编译约束（必须满足）

```
gcc -std=c11 -Wall -Wextra  → 零警告
```

- 所有源文件必须在 `server/Makefile` 的 `SRCS` 变量中登记
- 新增头文件放 `server/include/`，源文件放 `server/src/`
- 链接库：libmicrohttpd / libcurl / libsqlite3 / libpthread / libm / libcrypto（会话使用RAND_bytes，随机源失败拒绝登录）

### 5.2 命名约定

| 类型 | 风格 | 示例 |
|------|------|------|
| 函数 | `snake_case` | `handle_question_upload()` |
| 结构体 | `snake_case` | `struct app_config` |
| 宏 / 常量 | `UPPER_SNAKE_CASE` | `MAX_PATH_LEN` |
| 文件名 | `snake_case` | `ai_client.c`, `http_server.h` |

### 5.3 内存管理（红线）

- **每次 `malloc` / `calloc` 必须有对应的 `free`**，禁止泄漏
- 禁止越界访问、悬垂指针（use-after-free / double-free）
- 函数返回指针时必须明确其生命周期（调用方释放 or 内部静态）
- 字符串操作优先 `snprintf` / `strncpy`，禁止裸 `sprintf` / `strcpy`
- **kv_store 所有权**：`kv_get` / `kv_get_all` 返回的 `char*` 由调用者 `free()`；`kv_list_*` 返回的 `char**` 数组必须用 `kv_free_str_array()` 释放

### 5.4 错误处理

- HTTP 错误统一返回 JSON：`{"error": "描述信息"}`
- HTTP 状态码语义必须正确：

| 场景 | 状态码 |
|------|--------|
| 未认证 | `401` |
| 无权限 | `403` |
| 资源不存在 | `404` |
| 参数错误 | `400` |
| 服务器内部错误 | `500` |
| 成功 | `200` / `201` |

- 每个返回值必须检查，stderr 打印有用诊断信息

---

## 6. 架构约束

### 6.1 分层原则

```
路由层（routes.c）    →  只做分发与参数处理
业务层（services_*.c）→  业务规则
数据层（db.c）        →  关系型数据 SQL 读写（用户数据经 kv_store `users` namespace 存取）
存储层（kv_store.c）  →  目录式键值数据（namespace / key / value_name / value）
AI 层（ai_client.c）  →  大模型代理调用
```

- 路由层不直接拼接 SQL，通过 db.h/c 或 kv_store.h 接口
- 业务规则放独立模块，不散落在路由处理函数中
- **存储选型**：强关系数据（用户等）走 db.c 关系表；目录式/弱 schema 数据（题目等）走 kv_store，接口见 `docs/kvstore.md`
- **共享数据库连接**：`AppConfig.db` 在 main.c 打开一次，kv_store 与路由层复用同一 `sqlite3*`，禁止各自另开连接
- **namespace 命名**：仅允许 `a-zA-Z0-9_` 且首字符非数字，否则 kv_* 返回 -1

### 6.2 安全红线

1. **API Key 永远不出后端**：所有 AI 调用由 server 代理，前端永远拿不到 Key
2. **`.env` 不入库**：已加入 `.gitignore`，敏感信息只能进 `.env.example`（占位符）
3. **路径穿越防护**：静态文件服务拒绝含 `..` 的路径
4. **多租户隔离**：所有业务查询必须带当前用户/所属教师上下文
5. **无硬编码密钥/密码**：敏感信息一律走环境变量
6. **SQL 注入防护**：使用参数化查询，禁止拼接 SQL 字符串

### 6.3 并发预留

- 保持「无共享可变状态」（no shared mutable state）
- 路由处理函数应为无副作用的纯函数，状态通过 db / kv_store 持久化
- kv_store **未加锁**：当前单线程 demo 模式安全；若引入工作线程共享同一 `sqlite3*`，调用方必须自行串行化
- 考虑后续引入工作线程时的数据竞争风险

---

## 7. API 设计规范

### 7.1 URL 约定

```
/api/v1/health          → 健康检查（GET，无需认证）【已实现】
/api/v1/auth/*          → 认证相关（login/logout/me）【已实现】
/api/v1/questions       → 题目列表/详情（GET，只读）【已实现】
/api/v1/chat            → AI 对话（规划中；前端已预留降级演示数据）
/api/v1/exams/*         → 试卷相关（规划中）
/api/v1/conversations/* → 对话树相关
/api/v1/admin/*        → 管理功能（规划中）
```

### 7.2 请求/响应格式

- Content-Type: `application/json`
- 鉴权方式：Bearer Token（Authorization header）
- 所有 POST/PUT 请求体为 JSON
- 响应统一 JSON，错误格式 `{"error": "..."}`

### 7.3 API 文档

统一 `docs/api.md` 尚在整合中；当前接口契约以模块文档为准：`docs/auth.md`（认证）、`docs/questions.md`（题目）、`docs/kvstore.md`（存储层）。后端开发必须严格按对应模块文档的接口签名实现，新接口先落模块文档再写代码。

---

## 8. 添加新功能的流程

### 8.1 C 后端新增源文件

1. 头文件放 `server/include/`
2. 源文件放 `server/src/`
3. 在 `server/Makefile` 的 `SRCS` 中追加新 `.c` 文件
4. 根 `Makefile` 无需修改（自动分派）

### 8.2 新增路由

1. 在 `server/src/routes.c` 的 URL 派遣函数中添加新路径匹配
2. 如需新业务模块，创建 `server/src/services_xxx.c`
3. 在 `server/tests/<组名>/` 下新增 `case_N.sh`（run_tests.sh 自动发现）；需要编译的 C 测试在 `run_tests.sh` 中登记
4. 更新对应模块文档（questions.md / auth.md 等）的接口定义，统一 api.md 整合后迁移

### 8.3 新增模块/语言（monorepo 扩展）

1. 创建新目录（如 `worker/`）
2. 提供自有 `Makefile`（实现 `install lint test build run clean` 目标）
3. 根 `Makefile` 的 `MODULES := server` 追加新目录名
4. `.github/workflows/ci.yml` 中按需增加 job
5. 更新 `docs/architecture.md` 和本文件

---

## 9. 测试

### 9.1 测试体系（run_tests.sh）

- 运行器：`server/tests/run_tests.sh`，自动发现测试组并管理服务生命周期（启动 → 跑用例 → 关闭），默认使用独立临时数据库
- 测试组：
  - `auth/`：登录成功/失败、token 校验、登出（case_1~4.sh）
  - `health/`：健康检查（case_1.sh）
  - `questions/`：题目列表/详情、401/404/405/500、旧库迁移（case_1~5.sh + check_api.py + db_test.c）
  - `kv/`：kv_store 单元测试（case_unit.c，18 断言，不依赖 server 进程）
- 单独运行：`bash server/tests/run_tests.sh kv`（或 questions / auth / health）
- CI 必须全绿，否则分支保护不允许合并

### 9.2 测试风格

```bash
#!/usr/bin/env bash
set -euo pipefail
BASE_URL="http://localhost:${PORT:-8000}"

# 1. 健康检查
HEALTH=$(curl -s "$BASE_URL/api/v1/health")
echo "$HEALTH" | grep -q '"status":"ok"' || { echo "FAIL: health"; exit 1; }
echo "PASS: health"

# ... 更多用例 ...
```

---

## 10. 常见坑与解决方案

| 问题 | 原因 | 解决 |
|------|------|------|
| 编译报 `undeclared` | 新源文件未登记 | 在 `server/Makefile` 的 `SRCS` 中追加 |
| `microhttpd.h` 找不到 | 缺少开发库 | `sudo apt install libmicrohttpd-dev` |
| 前端页面 404 | WEB_ROOT 路径不对 | 必须在 `server/` 目录下运行，WEB_ROOT 默认 `../web` |
| 服务器端口无法访问 | 防火墙/安全组 | 检查放行规则 |
| CI 检查名与分支保护不一致 | 配置不同步 | `gh pr checks <PR号>` 查看后更新 `scripts/branch_protection.json` |
| Windows 无法编译 | gcc 版本过旧 | 以 CI 编译结果为准；或装 WSL2 |
| `make test` 失败但服务正常 | 端口冲突/服务未完全启动 | 冒烟脚本中增加 `sleep 1` |
| kv_* 返回 -1（无效参数） | namespace 命名不合法 | namespace 仅允许 `a-zA-Z0-9_`，首字符非数字 |
| kv_* 返回的内存泄漏 | 忘记释放 | `char*` 直接 `free()`；`char**` 数组用 `kv_free_str_array()` |
| 题目数据不在 questions 表 | 题目已改存 kv_store | 查 `qdata` namespace（`kv_get` / `kv_list_keys`），见 `docs/kvstore.md` |

---

## 11. Git 工作流摘要

```
main（受保护） ← PR（Squash merge） ← feat/fix/docs/... 分支
```

- **禁止**直接 push main
- 分支命名：`feat/xxx` / `fix/xxx` / `refactor/xxx` / `docs/xxx` / `chore/xxx`
- 提交信息：Conventional Commits（`feat(server): 描述`）
- PR 至少 1 人 Approve + CI 全绿才可合并
- 作者不合并自己的 PR，统一 Squash and merge
- 详细规则见 `CONTRIBUTING.md`

---

## 12. 团队角色与模块归属

| 角色 | 成员 | 负责模块 |
|------|------|----------|
| 后端-认证+AI | 刘英哲 | `auth.c/h`（登录/注册/鉴权，已完成）、`ai_client.c/h`、对话树 |
| 后端-题库 | 李俊熠 | 题目 CRUD 路由（只读接口已接入 kv_store）, `db.c` 扩展 |
| 后端-审核/通知 | 牛宇歌 | 内容审核、通知中心（P2），配合其他后端模块 |
| 后端-安全/测试/架构 | 卢恒毅 | 代码审查、测试体系、CI、安全审计 |
| AI执行 | Node.js >=22.19 · TypeScript · Pi 0.84.1 | 内部教学技能/受限数学工具/异步队列，teacher模式Key由C加密托管并按任务交给Pi；platform模式Key仅Pi环境持有 |
| 前端 | 曹渲东、王英杰 | `web/` 全部 |
| 文档/接口 | 戴儒骋 | `AGENT.md`、`api.md`、功能说明书 |

---

> kv_store 公共存储层（`kv_store.c/h` + `debug.h`，经 PR #5 合入）是后端各模块共用基础设施，接口文档见 `docs/kvstore.md`。

---

> **AI Agent 使用提醒**：生成代码前，请完整阅读本文件 + `server/Makefile` + 相关头文件（`server/include/*.h`）；涉及数据存储时必读 `docs/kvstore.md`。确保输出满足 C11 零警告、内存安全（含 kv_* 返回值释放）、API 签名一致、安全红线不触碰。

## Pi与AI授权新增文件（2026-10-20）

- ai-service/：独立Node/TS模块、固定Pi依赖，src/、skills/、eval/，提供Make lint/build/test/run。
- server/src/ai_access.c、include/ai_access.h：授权读与建表，不在routes中写SQL，不开额外C数据库连接。
- scripts/ai_access.py：可信运维授权/撤销/状态，事务审计。
- server/tests/ai_access/：C权限单元、会话随机源失败和HTTP+CLI集成；chat测试显式授权自己的临时库，禁止依赖真实.env。
- docs/pi-chat-api.md、pi-chat-openapi.json、examples/pi-chat-client.mjs、ai-access.md：前端1.2契约与边界。
- 根make lint/build/test同时覆盖C与Pi；单跑C可make -C server lint build test。

新增teacher_api.c/h：独立API配置/加密/配额数据层，复用main共享连接；teacher_api_routes.c/h仅HTTP校验/序列化；routes.c仅改AI路由与提交桥接，不改题库handler。教师管理资格只看已登录teacher（用户已明确无需额外审核）。Key不加入prompt、历史或公开任务。teacher_api测试使用独立临时题库，不依赖真实.env。
