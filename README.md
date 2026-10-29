# AI+题库智能教学平台


## 教师 API v1.3（当前默认模式）

默认AI_ROUTING_MODE=teacher：教师注册/登录即有资格GET/PUT/PATCH自己的/api/v1/teacher/ai-api，一个教师一条配置。学生提交题目后由C从qdata.teacher_id解析配置并校验日额度（默认每学生/每教师/上海自然日20次）；不叠加旧CLI授权，不回退平台Key。

仓库根目录先执行python3 scripts/configure_pi_demo.py和python3 scripts/configure_teacher_api.py，后者只补充C的加密主密钥，绝不打印或覆盖已有值。两个服务的AI_ROUTING_MODE保持一致；教师模式Pi无需全局AI_API_KEY。保留已有Key只在显式platform兼容模式使用。

启动后由前端/接口工具登录教师并配置其API。现有演示题归属admin，不会自动转移，需题库负责人提供teacher归属的题目联调。不要把新限额/路由测试误判为旧演示题直接可调用。

前端接口：`docs/pi-chat-api.md`（1.3）、`docs/teacher-api.md`及OpenAPI；教师Key只进后端，GET只回脱敏信息。本次没有改web业务页面、题库CRUD或注册策略。


一个面向教育场景的 **多教师、多学生、多租户** 智能题库与试卷管理平台。教师维护题库/试卷并配置 AI 解答；学生提交题目、查看 AI 解答、在分支对话树中追问；管理员负责用户、全局 API、审计与运维。

功能预期详见 [`docs/AI题库项目整体预期功能说明书.md`](docs/AI题库项目整体预期功能说明书.md)。

## 技术栈（C 语言）

| 模块 | 技术 | 说明 |
| --- | --- | --- |
| server（后端） | **C11** · libmicrohttpd · SQLite3 · libcurl · cJSON | HTTP 服务 + 存储 + AI 代理（API Key 不出后端） |
| ai-service | Node.js >=22.19 · TypeScript · Pi 0.84.1 | 内部模型执行、教学技能与异步队列 |
| web（前端） | 纯 HTML / CSS / JS | 静态页面，由 C 服务端直接托管，无构建工具链 |
| 构建 | GNU Make + gcc | 根 `Makefile` 统一分派各模块 |
| 部署 | systemd（课程服务器 Linux） | 见 `docs/environment-setup.md` |

## 目录结构

```
.
├── Makefile               # 根构建入口（模块化分派，支持新增语言/模块）
├── server/                # C 后端
│   ├── Makefile           # 编译规则（lint / build / test / run / clean）
│   ├── include/           # 头文件（config/db/http/routes/ai）
│   ├── src/               # 源码（main/http_server/routes/db/ai_client/config）
│   ├── third_party/       # cJSON（单文件库，MIT 协议）
│   ├── tests/             # 冒烟测试脚本
│   └── .env.example       # 环境变量配置示例
├── web/                   # 静态前端（index.html / css / js）
├── docs/                  # 需求文档、架构说明、环境部署指南
├── scripts/               # 环境检查/安装、GitHub 初始化
├── deploy/                # 部署配置（systemd 服务示例）
└── .github/               # PR 模板、CODEOWNERS、CI workflow、分支保护
```

新增语言/功能时如何扩展：见 [`docs/architecture.md`](docs/architecture.md)。

## 快速开始

### 在课程服务器（Linux）上

```bash
# 1. 环境检查与系统依赖安装（gcc / make / libmicrohttpd / libcurl / sqlite3）
bash scripts/check_env.sh          # 先检查，缺什么看提示
sudo bash scripts/install_env.sh   # 安装系统依赖（可只装缺的）

# 2. 构建 + 测试 + 运行
make build && make test            # 编译 + 冒烟测试（健康检查 + 静态页）
make -C server run                 # 启动服务 http://<服务器IP>:8000
curl http://<服务器IP>:8000/api/v1/health   # 应返回 {"status":"ok"}
```

### 本地 Windows 开发

建议使用 **WSL（Ubuntu）** 或直接在课程服务器上开发（C 后端依赖 Linux 库）。
`scripts/check_env.ps1` 可检查 git / gcc / make 是否就绪。

### 配置

服务端通过**环境变量**读取配置（默认值见 `server/src/config.c`）：

```bash
PORT=8000                       # 监听端口
DATABASE_PATH=data/ai_question_bank.db
WEB_ROOT=../web                 # 静态页根目录（相对 server/ 工作目录）
AI_SERVICE_URL=http://127.0.0.1:8001   # C只访问内部Pi服务
AI_SERVICE_TOKEN=                   # C与Pi共享内部凭证，与用户token不同
AI_CONNECT_TIMEOUT_MS=500           # 内部HTTP连接超时，毫秒
AI_REQUEST_TIMEOUT_MS=2000          # 内部HTTP总超时（含连接），毫秒
```

模型的 `AI_API_BASE_URL / AI_MODEL / AI_API_KEY` 仅配置在 Pi 服务环境；C不再读取这些旧变量。

生产部署：复制 `server/.env.example` 为 `server/.env` 填写，由 systemd `EnvironmentFile` 加载。

固定题目列表与详情的字段约定、登录要求及回归用例见 [docs/questions.md](docs/questions.md)。

## Pi Agent 后端

C 主后端通过内部 HTTP 接口调用独立的 Node.js/TypeScript Pi 服务。
支持完整讲解、分层提示、错因分析，以及异步任务、同题追问和取消，默认可用 Mock 联调。
提供受限算术、一元一次方程求解/代入校验工具和30个解题评测案例。

- 统一 API 授权与边界：[docs/ai-access.md](docs/ai-access.md)（所有账号默认无AI权限，需运维CLI显式授权）
- 启动与模型配置：[ai-service/README.md](ai-service/README.md)
- V4.1-Flash真实模型阶段验证：[docs/pi-chat-evaluation.md](docs/pi-chat-evaluation.md)
- 前端接口档案：[docs/pi-chat-api.md](docs/pi-chat-api.md)
- OpenAPI：[docs/pi-chat-openapi.json](docs/pi-chat-openapi.json)
- 可用客户端示例：[docs/examples/pi-chat-client.mjs](docs/examples/pi-chat-client.mjs)

## 协作规范

- **分支模型**：`main` 为受保护分支，禁止直接推送；所有改动走 feature 分支 + Pull Request。
- **代码审查**：每个 PR 至少 1 人 Review、CI 全绿后才可合并（分支保护已配置）。
- **提交信息**：Conventional Commits，如 `feat: 支持题目批量导入`。
- 详细规则见 [`CONTRIBUTING.md`](CONTRIBUTING.md) 与 [`docs/architecture.md`](docs/architecture.md)。

## License

MIT
