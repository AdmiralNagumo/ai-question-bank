# AI 协作指南：开发位置 · 汇总 · 核验 · 喂 AI 的文件清单

> 适用于本项目（C 技术栈）7 人小组协作。核心事实：**代码在各自电脑写，GitHub main 是唯一权威汇总点，核验靠"CI 自动 + PR 人工 + 服务器演示"三层**；**组员用 AI 写代码时，喂给 AI 的文件 = AGENT.md + Makefile + 相关现有源码 + api.md，并用固定 prompt 模板约束输出**。

---

## 1. 各自在哪完成自己的部分

| 角色 | 完成位置 | 说明 |
| --- | --- | --- |
| 后端（牛宇歌/李俊熠/刘英哲/卢恒毅） | 各自电脑 clone 仓库 → 功能分支 | Windows 本机**无法编译 C**（gcc 过旧），代码写完靠 CI 编译验证；可选装 WSL2 或直接在课程服务器开发以获得本地反馈 |
| 前端（曹渲东、王英杰） | 各自电脑 clone 仓库 → 功能分支 | `web/` 是纯静态页，**本地浏览器直接打开 `web/index.html` 即可预览**，不依赖编译 |
| 文档（戴儒骋/卢恒毅） | 各自电脑 clone 仓库 → `docs/` 分支 | 纯 Markdown，本地编辑即可 |

**建议的本地环境**（二选一即可）：
- **方案 A（默认）**：Windows 上只写代码，push 后由 GitHub Actions CI 编译验证（每次 push 约 1–2 分钟出结果）；
- **方案 B（可选，反馈更快）**：装 WSL2 + Ubuntu，执行 `sudo apt install gcc make libmicrohttpd-dev libcurl4-openssl-dev libsqlite3-dev`，本地即可 `make build && make test`。

## 2. 完成之后如何汇总

**代码汇总 = 分支 + PR + 合并到 main（GitHub 是唯一权威汇总点）**：

```
git pull origin main                    # 拉最新
git switch -c feat/你的功能             # 建功能分支（命名见 CONTRIBUTING.md）
... 开发、本地自测（前端） ...
git add -A && git commit -m "feat(server): 增加登录接口"   # Conventional Commits
git push origin feat/你的功能
→ GitHub 上 New Pull Request（填模板）
→ CI 自动跑（安装依赖 → make build → 测试：接口回归 + kv_store 单元测试）
→ code owner 审查 + 至少 1 人 Approve
→ Squash merge 进 main
```

- **文档汇总**：AGENT.md / api.md 由戴儒骋统一维护，通过同样的 PR 流程合入；
- **禁止**：任何人直接 push main（分支保护已强制）。

## 3. 如何核验功能实现（三层核验）

| 层 | 谁做 | 核验内容 | 门槛 |
| --- | --- | --- | --- |
| ① 自动层 | GitHub Actions（每次 push/PR 自动） | 依赖安装 → `make build`（`-Wall -Wextra` 零警告）→ 测试（auth/health/questions 接口回归 + kv_store 单元测试） | CI 全绿，否则分支保护不允许合并 |
| ② 人工层 | 卢恒毅（审查）+ 各模块 code owner | 代码审查：安全（无明文 key/密码）、内存、无共享可变状态、错误处理；审查意见写入 PR | ≥1 人 Approve |
| ③ 集成层 | 全体（演示日） | 课程服务器部署 + 全链路手动走查 | D7 演示通过 |

**各人核验自己的部分**：
- 刘英哲：CI 冒烟里的登录用例 + 演示时用两个账号真实登录（认证已实现）；CI 冒烟（mock 路径）+ 演示时真实对话（切真实 API Key）；
- 李俊熠：CI 题目组用例（列表/详情/401/404/405/迁移）+ 前端列表页真实展示；
- 卢恒毅：`grep` 检查源码无明文 key、架构约束检查、测试四组（auth/health/questions/kv）全过；
- 曹渲东、王英杰：本地浏览器走查全流程 + 演示；
- 戴儒骋/卢恒毅：api.md 与实现逐接口核对。

**Windows 无法编译时的应对**：以 CI 结果为准——PR 红了就看 CI 日志修（日志会精确到文件和行号），绿了才允许合并。

## 4. 组员用 AI 写代码：喂哪些文件

> 原则：**给 AI "最小充分集"**——给太少 AI 自由发挥不合规范；给太多 AI 迷失。以下按角色给出清单。

### 4.1 通用必喂（所有人）

| 文件 | 作用 |
| --- | --- |
| `AGENT.md` | 给 agent 的项目手册：结构/构建命令/代码规范/模块说明/常见坑（戴儒骋、卢恒毅维护） |
| `CONTRIBUTING.md` | 分支命名、commit 规范、PR 流程 |
| 根 `Makefile` + 自己模块的 `server/Makefile` | 让 AI 知道编译约束：`-std=c11 -Wall -Wextra`、链接库 LDLIBS，生成的代码必须满足零警告 |
| 自己任务相关的**现有源码 1–2 个文件** | 作为风格模板，让 AI 模仿既有命名/结构（如 `server/src/routes.c`） |

### 4.2 按角色追加

| 角色 | 追加喂入 | 原因 |
| --- | --- | --- |
| 后端（牛宇歌/李俊熠/刘英哲） | 模块接口文档（`docs/auth.md`/`docs/questions.md`）、`docs/kvstore.md`（存储层接口与所有权约定，涉及数据存取时必喂）、`docs/architecture.md`（并发预留约束）、`server/include/*.h`（函数签名，含 `kv_store.h`） | 路由必须严格符合接口约定；存储按选型走 db.c 或 kv_store；架构约束无共享可变状态 |
| 卢恒毅（安全/测试） | `.gitignore`、`server/.env.example`、`server/tests/*/case_*.sh`（现有测试风格） | 生成安全存储/测试代码符合既有模式 |
| 前端（曹渲东、王英杰） | `web/index.html`、`web/css/style.css`、`web/js/`（app/api/auth/questions/chat，现有风格）+ 模块接口文档（`docs/auth.md`、`docs/questions.md`：接口、token 鉴权方式、错误格式） | 前端必须与后端接口一一对应、字段一致；不可硬编码题目 id/数量 |
| 文档（戴儒骋） | `AGENT.md` 现有内容 + `docs/AI题库项目整体预期功能说明书.md`（内容来源） | 生成文档保持风格与事实一致 |

### 4.3 喂 AI 的固定 prompt 模板（重要）

让 AI 生成代码时，在任务描述前**先粘贴以下约束块**（可随角色删减）：

```
你是本项目的 C 技术栈开发者。项目约束见附带的 AGENT.md、Makefile 与相关源码。
请完成：<具体任务>

输出必须满足：
1. C11 标准，gcc -Wall -Wextra 编译零警告（注意 include 路径与 LDLIBS 匹配 server/Makefile）；
2. HTTP 错误统一返回 JSON {"error":"..."}，状态码语义正确（401/403/404/500）；
3. 动态内存必须 malloc/free 配对，禁止泄漏、越界、悬垂指针；kv_* 返回的 char* 用 free()，char** 数组用 kv_free_str_array()；
4. 禁止硬编码 API Key / 密码，敏感信息一律走环境变量（参考 server/.env.example）；
5. 路由与数据结构签名与现有头文件（server/include/*.h）保持一致；存储选型按 AGENT.md：关系数据走 db.c，目录式数据走 kv_store（docs/kvstore.md）；
6. 不破坏现有功能，新增路由不影响既有 /api/v1/health 与静态文件服务；
7. 完成后，给出需要在 server/tests/<组名>/ 中补充的测试用例。
```

**使用流程**：粘贴约束块 + 粘贴喂入文件内容 + 描述任务 → 得到代码 → **先让 AI 自查**（对照约束 1/2/3/4 逐条确认）→ 本地 commit + push → 以 CI 结果为准修正 → 合并。

**提醒**：AI 生成的代码质量以 **CI 红绿为准**，不要只看 AI 说"没问题"；文档类输出由戴儒骋/卢恒毅核对与实现一致后再合入。
