# AI+题库智能教学平台 — AI 辅助开发计划

> 版本：v1.2 | 日期：2026-10-13 | 维护：戴儒骋、卢恒毅
> v1.1 变更：纳入 kv_store 存储层与前端三页等首轮实践成果；更新测试体系与投喂文件清单
> v1.2 变更：修正第四节角色分工——补入戴儒骋（文档/项目经理），卢恒毅加入后端组并补后端任务示例；认证模块归属修正（刘英哲已完成用户认证含注册，牛宇歌转向 P2 审核/通知）

---

## 一、概述

### 1.1 背景

本项目（C11 + libmicrohttpd + SQLite3）技术栈对多数团队成员较陌生，开发效率面临挑战。通过系统化使用 AI 辅助编码，可大幅降低学习曲线、提高代码质量与一致性。

### 1.2 目标

- 用 AI 辅助完成 80% 以上的初始代码生成
- 确保 AI 生成代码符合 C11 零警告、内存安全、API 统一等规范
- CI 自动验证 AI 输出质量，人工审查仅聚焦架构与安全
- 7 人团队中每人掌握 AI 辅助开发流程

### 1.3 首轮实践成果（截至 2026-10-12）

本计划的流程已在第一轮开发中完整跑通一次：

| 成果 | 说明 |
|------|------|
| kv_store 目录式存储层 | PR #5 合入：namespace → SQLite 表 `kv_{ns}`，含 18 断言单元测试与 `docs/kvstore.md` 接口文档 |
| 题目只读 API | `GET /api/v1/questions`（列表/详情），数据存 kv_store `qdata` namespace，5 道固定题 seed |
| 前端三页 | 登录页 / 题目列表页 / 对话页（SPA hash 路由，在线 + file:// 离线双模式，35 项端到端断言） |

流程复现：最小充分集投喂 → Prompt 模板约束 → AI 自查 → CI 编译与测试 → PR 人工审查 → 合入。

---

## 二、AI 辅助开发工作流

### 2.1 总体流程

```
需求分析 → 喂给 AI → 生成代码 → AI 自查 → 本地 review → commit + push → CI 验证 → PR Review → 合并
```

### 2.2 各阶段 AI 角色

| 阶段 | AI 做什么 | 人做什么 |
|------|----------|----------|
| 需求理解 | 解读 api.md 接口定义 | 确认需求正确性 |
| 代码生成 | 根据 AGENT.md + 头文件 + Makefile 生成 C 代码 | 粘贴 Prompt 模板 |
| AI 自查 | 逐条对照编码规范自检 | 确认自查通过 |
| 本地 review | — | 阅代码逻辑、安全风险 |
| CI 验证 | GitHub Actions 自动 build+test | 看 CI 结果，红了修 |
| PR Review | — | 代码审查（架构/安全/规范） |
| 测试 | 生成冒烟测试用例 | 确认覆盖场景 |

### 2.3 喂给 AI 的文件清单

#### 通用必喂（所有人）

| 文件 | 作用 |
|------|------|
| `docs/AGENT.md` | 项目手册：结构/构建/规范/模块/常见坑 |
| `CONTRIBUTING.md` | 分支命名、commit 规范、PR 流程 |
| 根 `Makefile` + `server/Makefile` | 编译约束：`-std=c11 -Wall -Wextra`、链接库 |
| 相关现有源码 1–2 个文件 | 风格模板，如 `server/src/routes.c` |

#### 按角色追加

| 角色 | 追加喂入 |
|------|----------|
| 后端（牛宇歌/李俊熠/刘英哲/卢恒毅） | 模块接口文档（`docs/auth.md`/`docs/questions.md`）、`docs/kvstore.md`（涉及存储时必喂）、`docs/architecture.md`、`server/include/*.h` 头文件（含 `kv_store.h`） |
| 卢恒毅（安全/测试/CI） | `.gitignore`、`server/.env.example`、现有测试（`server/tests/*/case_*.sh`） |
| 前端（曹渲东/王英杰） | `web/index.html`、`web/css/style.css`、`web/js/`（app/api/auth/questions/chat）、模块接口文档 |
| 文档（戴儒骋） | `AGENT.md` 现有内容、功能说明书、`docs/kvstore.md` |

---

## 三、AI Prompt 模板

### 3.1 后端代码生成模板

将以下约束块粘贴在任务描述前：

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

### 3.2 前端代码生成模板

```
你是本项目的前端开发者。项目使用纯 HTML/CSS/JS，无构建工具链。
请完成：<具体任务>

输出必须满足：
1. 不引入 npm/webpack/vite 等工具链，直接修改 web/ 下静态文件；
2. fetch 调用后端 /api/v1/* 接口，携带 Authorization: Bearer <token>；
3. 错误处理：后端返回 {"error":"..."} 时展示用户可读提示；
4. 与 docs/api.md 定义的接口字段完全一致；
5. 保持与现有 style.css / app.js 风格一致。
```

### 3.3 文档生成模板

```
你是本项目的文档维护者。项目背景见附带的 AGENT.md 与功能说明书。
请完成：<具体任务>

输出必须满足：
1. Markdown 格式，与现有 docs/ 风格一致；
2. 引用的文件路径使用相对路径（如 docs/architecture.md）；
3. 事实信息以 AGENT.md 和功能说明书为准，不编造。
```

---

## 四、各角色 AI 使用指南

### 4.1 牛宇歌 — 后端审核/通知模块

```
任务示例：实现内容审核功能 / 通知中心

输入：AGENT.md + server/Makefile + server/include/*.h（现有头文件签名参考）
      + docs/api.md（接口定义参考）

注意：
- 认证模块已由刘英哲完成（auth.c/h，登录/注册/鉴权均已实现）
- 牛宇歌转向 P2 任务：内容审核（敏感词过滤）、通知中心（系统通知推送）
- 审核模块需兼容学生提交内容与 AI 生成内容的审核
- 通知中心支持审核结果通知、额度告警等类型
```

### 4.2 李俊熠 — 后端题库模块

```
任务示例：实现题目写接口（创建/编辑/归档，POST/PUT/DELETE /api/v1/questions）

输入：AGENT.md + server/Makefile + server/include/kv_store.h + routes.h
      + docs/questions.md（现有只读契约）+ docs/kvstore.md

现状（2026-10-12）：只读接口已上线——题目数据存 kv_store 的 qdata namespace
（content/options/answer/analysis/type/knowledge_points/status），启动 seed 5 道固定题。

注意：
- 写接口沿用 kv_store 存储，保持与只读接口字段一致
- 多租户隔离：所有查询以当前登录用户所属教师为 scope
- 后续支持分页 ?page=1&limit=20 与筛选 ?type=&difficulty=&tag=
```

### 4.3 刘英哲 — 后端认证+AI 模块

```
已完成：用户认证系统（auth.c/h，登录/注册/鉴权/Token/me 接口全部实现，session 定期清理机制，8 个测试用例 case_1~8）

任务示例（AI 模块）：实现 AI 解答调用 ai_client.c

输入：AGENT.md + server/Makefile + server/include/ai_client.h + config.h

安全约束：
- API Key 从环境变量 AI_API_KEY 读取，绝对不硬编码
- libcurl 请求构造重试机制（3 次）
- 超时 30s 返回 {"error":"AI 服务不可用"}
```

### 4.4 卢恒毅 — 后端架构/安全/测试/CI

```
任务示例一（后端）：实现 HTTP 服务框架 / CI 流水线配置

输入：AGENT.md + server/Makefile + server/include/http_server.h
      + server/src/http_server.c + .github/workflows/ci.yml

约束：
- libmicrohttpd 守护进程，支持静态文件服务与 JSON API 路由分发
- CI 流水线自动编译（make build）+ 全量回归测试（make test）
- 分支保护规则同步 scripts/branch_protection.json

任务示例二（测试）：新增 API 冒烟测试 case

输入：AGENT.md + server/tests/run_tests.sh + 现有 case_*.sh

约束：
- 新 case 自给自足（自动登录获取 token）
- curl 加 --noproxy '*' 绕过系统代理
- 使用 sed 而非 grep/cut 解析 token
```

### 4.5 曹渲东、王英杰 — 前端

```
任务示例：实现题库列表页（已完成，见 web/js/questions.js）→ 下一步：试卷管理页

输入：web/index.html + web/css/style.css + web/js/（app/api/auth/questions/chat）
      + docs/questions.md（接口契约）

约束：
- 纯静态页，fetch 调后端，统一经 web/js/api.js（含 file:// 离线降级）
- Bearer Token 存储在 localStorage
- 未登录重定向到登录页
- 后端内容插入 innerHTML 前必须 HTML 转义；不硬编码题目 id/数量
```

### 4.6 戴儒骋 — 文档/项目经理

```
任务示例：维护项目文档体系与进度跟踪

输入：AGENT.md 现有内容 + @功能说明书 + docs/ 下已有文档
      + docs/项目开发计划甘特图.xlsx（进度基准）

约束：
- 文档格式与现有 docs/ 风格一致，Markdown 优先
- 引用的文件路径使用相对路径（如 docs/architecture.md）
- 事实信息以 AGENT.md 和功能说明书为准，不编造
- 跟踪各里程碑进度，同步更新甘特图与周报
- 维护 docs/api.md 接口定义，与后端实现保持同步
```

---

## 五、AI 输出验证机制

### 5.1 自动化验证（CI）

| 检查项 | 工具 | 阈值 |
|--------|------|------|
| 编译 | `make build` | 零警告通过 |
| 接口回归 + 单元测试 | `make test`（run_tests.sh：auth/health/questions/kv） | 全部 case 通过 |
| 代码行数 | git diff | PR < 500 行（推荐） |

### 5.2 AI 自查清单

代码生成后，AI 必须逐条回答：

1. ☐ `gcc -std=c11 -Wall -Wextra` 是否零警告？
2. ☐ 所有 `malloc` 是否有对应 `free`？
3. ☐ HTTP 错误是否返回 JSON `{"error":"..."}`？
4. ☐ 状态码是否语义正确（401/403/404/500）？
5. ☐ 是否有硬编码的密钥/密码？
6. ☐ 新增源文件是否登记到 `server/Makefile` 的 SRCS？
7. ☐ 新增路由是否不影响 `/api/v1/health`？
8. ☐ kv_* 返回的字符串是否全部释放（char** 用 kv_free_str_array）？namespace 命名是否合法（a-zA-Z0-9_，首字符非数字）？
9. ☐ 是否给出了补充测试用例（server/tests/<组名>/）？

### 5.3 人工验证（PR Review）

- 对照 AGENT.md 第 5–6 章逐条审查
- 特别关注：缓冲区边界、指针生命周期、资源释放、多租户隔离

---

## 六、效率提升预估

| 场景 | 无 AI | 有 AI | 提升 |
|------|-------|-------|------|
| 新接口开发（C 后端） | 4–6 小时 | 1–2 小时 | 60–70% |
| 前端页面开发 | 3–4 小时 | 0.5–1 小时 | 70–80% |
| 冒烟测试编写 | 1–2 小时 | 15–30 分钟 | 75% |
| Bug 修复 | 1–2 小时 | 20–40 分钟 | 60% |
| 文档编写 | 2–3 小时 | 30–60 分钟 | 70% |

---

## 七、注意事项与红线

### 7.1 安全红线（AI 也不能违反）

1. **绝对不把 API Key 写入代码**：哪怕 AI 建议了也要拒绝
2. **`.env` 永远不入库**：AI 生成的配置示例只能进 `.env.example`
3. **密码必须哈希**：AI 生成代码若用明文存储密码，直接打回

### 7.2 常见陷阱

| 陷阱 | 应对 |
|------|------|
| AI 生成代码看起来对但编译不过 | 以 CI `make build` 结果为准 |
| AI 说不依赖某个库但实际需要 | 核对 `server/Makefile` 的 LDLIBS |
| AI 生成了 Windows-only 代码 | C11 跨平台，CI 会暴露 |
| AI 生成了不存在的函数 | 对照 `server/include/*.h` 头文件检查 |

### 7.3 质量底线

- AI 生成的代码 **必须** 经过 CI 验证
- 不要只看 AI 说"没问题"就合入
- 如果 AI 自查和 CI 结果矛盾，以 CI 为准

---

## 八、总结

本计划确保 7 人团队在 C 技术栈项目中系统化使用 AI 辅助开发。核心原则：

> **喂对文件 → 约束到位 → AI 自查 → CI 兜底 → 人工审安全**

通过 AGENT.md 统一规范、Prompt 模板标准化、CI 自动验证三层保障，实现 AI 辅助下的高质量快速开发。
