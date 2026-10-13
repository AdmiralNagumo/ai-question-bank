# Pi AI 服务

使用本机已有的 Pi 系列版本 0.84.1：`pi-ai`＋`pi-agent-core`，固定依赖及 lockfile。
运行于独立 Node.js 进程，C 负责用户登录、题目读取和公共接口；模型调用通过后台任务完成。
服务不加载个人 Pi CLI 配置、扩展、会话或文件/命令工具。唯一工具为受限 arithmetic。

## WSL 本地启动

需要 C 后端既有开发库、Python 3（测试用）与 Node >=22.19。
本项目提供用户目录安装脚本，不替换系统 Node：

```bash
cd ~/projects/ai-question-bank
bash scripts/install_ai_node.sh
make -C ai-service install
python3 scripts/configure_pi_demo.py
```

随后分别在两个 Ubuntu 终端启动：

```bash
make -C ai-service run
```

```bash
make -C server run
```

C 默认 `http://localhost:8000`，Pi 默认只监听 `127.0.0.1:8001`。
演示配置脚本产生随机的内部服务凭证，保存到被 Git 忽略的 .env 中，不打印凭证。
它保留已有配置；若两个 .env 中凭证不一致会报错，避免覆盖用户配置。

`scripts/find_ai_node.sh` 优先使用 AI_NODE_BIN、符合版本要求的 PATH node，
然后使用安装脚本的用户目录运行时。也可以自行安装 Node 并设置 AI_NODE_BIN。

## 切换真实模型

在 `ai-service/.env` 配置：

```dotenv
AI_MOCK=0
AI_API_BASE_URL=https://api.deepseek.com
AI_MODEL=你的模型名称
AI_API_KEY=你的 API Key
```

当前实现使用兼容 Chat Completions 的流式模型协议。AI_API_BASE_URL 是基础路径，
SDK 会调用其 `/chat/completions`；供应商要求 `/v1` 时在 base 中包含 `/v1`。
模型 Key 仅由 Pi 服务持有。服务凭证与用户登录 token 分别校验；C 内部请求绕过代理且不跟随跳转。
本次验证使用离线 Faux 模型与本地兼容模型 HTTP 测试服务；真实外部供应商需你提供 API Key 后联调。

## 模块和限制

- `src/agent.ts`：每个任务创建自己的 Pi Agent，恢复当前对话历史，运行有 Schema 的算术工具。
- `src/jobs.ts`：有界队列、用户/题目隔离、单对话锁定、取消、超时和状态清理。
- `src/server.ts`：内部 HTTP 接口；公共接口和前端契约见 `docs/pi-chat-api.md`。
- 单请求最大 64 KiB，提问最大 4000 个 Unicode 字符。
- 默认任务总预算 30 秒（含排队），最多 3 次模型调用、6 次工具调用，输出最多 2048 tokens。
- 默认 4 个并行任务、32 个等待位置、1000 个对话；闲置/完成状态保存 1 小时。
- 未启用持久化，重启 Pi 服务会清空任务和历史。未来需要持久化时应独立设计，不能把所有学生复用同一 Agent。
- 模型生成的文本不保证题目解答正确；质量验收需要真实题目集与人工核对。

环境变量清单见 `.env.example`。服务关闭会向正在运行的 Agent 发送取消信号。

## 验证

```bash
make lint
make build
make test
```

Pi 用例包含 Mock 会话、参数与服务鉴权、队列、取消、超时、用户隔离、真实 SDK HTTP 协议、
工具调用与模型 401/429/500 错误脱敏；C 回归还验证公共接口和代理层异常处理。
CI 安装固定 Node 和依赖，运行两套测试，不要求真实模型密钥，不产生外部模型费用。
