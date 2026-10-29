# 项目架构与扩展指南（C 技术栈）


## 教师 API v1.3（当前默认模式）

默认AI_ROUTING_MODE=teacher：教师注册/登录即有资格GET/PUT/PATCH自己的/api/v1/teacher/ai-api，一个教师一条配置。学生提交题目后由C从qdata.teacher_id解析配置并校验日额度（默认每学生/每教师/上海自然日20次）；不叠加旧CLI授权，不回退平台Key。

仓库根目录先执行python3 scripts/configure_pi_demo.py和python3 scripts/configure_teacher_api.py，后者只补充C的加密主密钥，绝不打印或覆盖已有值。两个服务的AI_ROUTING_MODE保持一致；教师模式Pi无需全局AI_API_KEY。保留已有Key只在显式platform兼容模式使用。

启动后由前端/接口工具登录教师并配置其API。现有演示题归属admin，不会自动转移，需题库负责人提供teacher归属的题目联调。不要把新限额/路由测试误判为旧演示题直接可调用。

前端接口：`docs/pi-chat-api.md`（1.3）、`docs/teacher-api.md`及OpenAPI；教师Key只进后端，GET只回脱敏信息。本次没有改web业务页面、题库CRUD或注册策略。


## 1. 总体架构

```
┌──────────┐   HTTP/JSON    ┌─────────────────────────────┐
│  web/     │ ─────────────▶ │ server（C11 后端）            │
│ 静态页面  │  /api/v1/*     │ libmicrohttpd + SQLite3      │
└──────────┘                │ + libcurl（AI 代理）          │
                             └─────────────┬───────────────┘
                                           │ 内部HTTP（独立服务凭证）
                                   ┌───────┴───────┐
                                   │ 内部Pi AI服务   │
                                   │ Node/TS → 模型  │
                                   └───────────────┘
```

- `web/` 为纯静态页面（HTML/CSS/JS），由 C 服务端直接托管，**无 Node/npm 工具链**。
- `server/` 为唯一公共业务入口：现有登录、题目读取、AI授权和任务代理。多租户业务隔离、试卷/解答树与用户额度属于后续目标，当前未完成。
- 所有大模型调用由后端统一代理，**API Key只存在于可信后端：教师配置加密入库，主密钥独立注入；platform兼容配置从Pi环境读取**。

## 2. 后端分层（server/）

| 目录/文件 | 职责 | 说明 |
| --- | --- | --- |
| `src/main.c` | 入口 | 加载配置 → 初始化数据库 → 启动 HTTP 服务 |
| `src/config.c` + `include/config.h` | 配置 | 环境变量读取（PORT/数据库/AI），默认值见 config.h |
| `src/db.c` + `include/db.h` | 数据库 | SQLite 初始化、建表（users/questions 骨架） |
| `src/http_server.c` + `include/http_server.h` | HTTP 服务 | libmicrohttpd 守护进程，信号优雅退出 |
| `src/routes.c` + `include/routes.h` | 路由 | /api/v1/* JSON API + 静态文件服务（防路径穿越） |
| `src/ai_access.c` + `include/ai_access.h` | AI授权 | 复用共享SQLite连接查询权限，默认拒绝；增量建表不授权旧用户 |
| `src/ai_client.c` + `include/ai_client.h` | AI 客户端 | libcurl 调用内部 Pi 任务接口（提交/查询/取消），不直接等待模型 |
| `third_party/cJSON.*` | JSON 库 | 单文件 MIT 协议，随仓库分发 |
| `tests/run_tests.sh` | 冒烟测试 | 启动 → 健康检查 → 静态页 → 关闭 |

**约定：**
- 路由层只做分发与参数处理，业务规则放独立模块（后续按功能拆 `src/services_*.c`）；
- 头文件统一放 `include/`，源文件放 `src/`；
- 新增 C 源文件必须登记到 `server/Makefile` 的 `SRCS`。

## 3. 前端（web/）

| 目录 | 职责 |
| --- | --- |
| `index.html` | 页面骨架（后续按模块拆分页面） |
| `css/` | 样式 |
| `js/` | 脚本（fetch 调用后端 /api/v1/*） |

## 4. 新增模块 / 新语言（扩展方式）

本仓库是 **monorepo**，采用「一个模块一个目录，各自自带 Makefile」的模式。新增模块步骤：

1. 新建目录，例如 `worker/`（语言自选：Go / Rust / C++…）；
2. 在该目录提供 `Makefile`，实现统一目标：`install lint test build run clean`；
3. 根 `Makefile` 的 `MODULES := server` 追加 `worker`；
4. 在 `.github/workflows/ci.yml` 中按需增加 job（复制 server job 模板改语言即可）；
5. 更新本文件目录说明与 README 技术栈表。

根 Makefile 通过 `$(addprefix <目标>-,$(MODULES))` 自动为每个模块生成
`build-xxx` / `test-xxx` 等分派目标，新增模块**不需要**改动分派逻辑本身。

## 5. 关键目录约定

- `data/`、`server/data/`、`*.db`：运行时数据，已被 `.gitignore` 排除，不入库；
- `docs/`：需求、架构、环境文档；重大技术决策在此留档；
- `deploy/`：部署配置示例（systemd）；
- `scripts/`：仓库级运维脚本（环境检查/安装、GitHub 初始化）。

## 6. 安全边界（框架层面已内置的约束）

- `.env` 不入库，密钥只进 `.env.example` 占位；
- 后端统一代理 AI 调用，前端永远拿不到 API Key；
- 静态文件服务已做**路径穿越防护**（拒绝含 `..` 的路径）；
- 多租户隔离：所有业务查询必须带当前用户/所属老师上下文（后续在路由层统一鉴权）；
- C 代码审查重点：缓冲区边界、指针生命周期、资源释放（见 CONTRIBUTING.md）。


## 7. Pi AI 服务与异步任务

`ai-service/` 为独立的 Node.js/TypeScript 模块，固定使用 Pi 0.84.1 的 pi-ai 与 pi-agent-core。
它提供模型协议适配、每任务独立 Agent、有界队列和用户/题目/教学模式隔离的对话历史。
`skills/` 下三个 SKILL.md 由 `src/teaching.ts` 加载，后端按 solve/hint/review 只注入选中的技能。
hint 层级按成功轮次递增到3，失败/取消不升级；同对话不能切换模式。
`src/math-tools.ts` 提供数值系数一元一次方程求解和候选解代入校验；算术工具保留，hint不开放方程工具。
工具只核验输入数值，不证明模型列式、定义域或教学反馈正确。质量验收使用 `eval/fixtures.json` 的30个案例。
前端契约见 [pi-chat-api.md](pi-chat-api.md)，部署和配置见 [../ai-service/README.md](../ai-service/README.md)。

公共流程：用户登录 → GET /api/v1/ai/access 查询独立授权 → C 读取题目 → POST /api/v1/chat 接受任务（202） →
GET /api/v1/chat/{job_id} 查询状态 → 展示 reply；DELETE 同一路径可取消。
C 校验用户 token 并设置内部 user_id；内部服务凭证与用户 token 分离，模型 Key 配置在 Pi 服务环境中。

C 对内部请求只等待最多 2 秒的任务提交/查询响应，模型执行发生在 Node 后台队列。
因此模型等待时间不占用 C 的 MHD 轮询线程。C 的数据库和会话操作仍遵循既有串行假设；
Node 的任务并行不会把共享 SQLite 连接交给其他线程或进程。

默认任务总预算 30 秒（包含排队）、最多 3 次模型调用和 6 次工具调用。
SDK 自动网络重试被关闭，以免一次任务产生不可预期的额外调用。
默认并行 4 个任务、等待 32 个任务；对话绑定用户与题目，一次只运行一个任务。
取消/失败不提交当前轮次到历史。状态与历史目前只在内存保存，闲置/完成状态默认 1 小时过期，服务重启清空。

## 8. 测试与配置隔离

Pi 单元测试使用 Faux 模型和本地兼容 HTTP 测试服务，验证实际 Agent 循环与工具调用。
server 测试运行器只在 chat 组运行时启动临时 Pi Mock 进程，并为其产生临时内部凭证。
测试使用独立数据库，跳过本地 .env，不启动外部付费模型调用，不终止占用端口的其他服务。
新增请求上下文在 MHD 完成回调统一释放，包含连接中断路径，最大请求体为 64 KiB。
内部 HTTP 客户端限制响应大小、不跟随跳转并绕过外部代理。

授权写入只开放可信服务器CLI，按用户绑定持久化并以事务追加审计；无HTTP写入口。每次POST重新检查，权限库故障默认拒绝；GET/DELETE本人既有任务不要求授权仍active。模型配置只由Pi进程读取，C仅持内部服务凭证。全部边界见 [ai-access.md](ai-access.md)。


## C → Pi 内部请求超时与诊断（2026-10-21）

C从进程环境或server/.env读取AI_CONNECT_TIMEOUT_MS（默认500）和AI_REQUEST_TIMEOUT_MS（默认2000），单位毫秒，允许纯十进制整数1–60000。未设置或空字符串使用默认；零、负数、带符号/空白、小数、非数字、溢出或超范围值回退到该项默认，并只记录变量名与默认值，不记录原始输入。进程环境优先，修改后重启C生效。

总请求超时包含连接阶段；两项独立校验，连接阶段受两者较小值约束。这些设置只控制C提交/查询/取消内部HTTP请求，与Pi后台模型任务的AI_TIMEOUT_MS不同，不能用延长内部HTTP超时来延长模型推理时间。C当前是单轮询线程；大幅提高内部超时会延长故障期间其他请求等待时间，应按部署延迟保守调整。

C标准错误日志以[ai-client]标识分类：connect_failed表示无法连接/解析内部服务；request_timeout表示内部HTTP超时；transport_failed表示其他传输故障；internal_auth_failed表示Pi返回HTTP401，应核对C/Pi两侧AI_SERVICE_TOKEN。仅记录固定分类、curl数字错误码或HTTP401，不记录URL、请求头、token、用户输入或上游响应。内部401即使响应非JSON也转为脱敏503 AI_SERVICE_UNAVAILABLE；连接失败仍503，超时仍504 AI_SERVICE_TIMEOUT。公共API字段及前端1.2契约不变。
