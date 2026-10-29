# Pi AI 服务


## 教师 API v1.3（当前默认模式）

默认AI_ROUTING_MODE=teacher：教师注册/登录即有资格GET/PUT/PATCH自己的/api/v1/teacher/ai-api，一个教师一条配置。学生提交题目后由C从qdata.teacher_id解析配置并校验日额度（默认每学生/每教师/上海自然日20次）；不叠加旧CLI授权，不回退平台Key。

仓库根目录先执行python3 scripts/configure_pi_demo.py和python3 scripts/configure_teacher_api.py，后者只补充C的加密主密钥，绝不打印或覆盖已有值。两个服务的AI_ROUTING_MODE保持一致；教师模式Pi无需全局AI_API_KEY。保留已有Key只在显式platform兼容模式使用。

启动后由前端/接口工具登录教师并配置其API。现有演示题归属admin，不会自动转移，需题库负责人提供teacher归属的题目联调。不要把新限额/路由测试误判为旧演示题直接可调用。

前端接口：`docs/pi-chat-api.md`（1.3）、`docs/teacher-api.md`及OpenAPI；教师Key只进后端，GET只回脱敏信息。本次没有改web业务页面、题库CRUD或注册策略。


使用本机已有的 Pi 系列版本 0.84.1：`pi-ai`＋`pi-agent-core`，固定依赖及 lockfile。
运行于独立 Node.js 进程，C 负责用户登录、题目读取和公共接口；模型调用通过后台任务完成。
服务加载本项目随代码发布的三个教学 Skill，不加载个人 Pi CLI 配置、扩展、会话或文件/命令工具。
工具为受限 arithmetic、一元一次方程求解与代入校验；提示模式仅开放 arithmetic。

## WSL 本地启动

需要 C 后端既有开发库、Python 3（测试及授权CLI）与 Node >=22.19。
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

仅在AI_ROUTING_MODE=platform旧演示模式下：C启动后，在仓库根目录显式给联调账号授权（确认db是C正在使用的DATABASE_PATH）：

```bash
python3 scripts/ai_access.py --db server/data/ai_question_bank.db status --username student
python3 scripts/ai_access.py --db server/data/ai_question_bank.db grant --username student --expires-at 2026-12-31T23:59:59Z --reason "受控联调"
```

所有角色默认无AI授权；配置模型Key、登录成功均不会自动授权。CLI需要可信服务器权限，不能交由前端调用。撤销/到期只阻止新提交，已有本人任务仍可查询/取消。授权保存在SQLite，详细边界及回退要求见 [../docs/ai-access.md](../docs/ai-access.md)。本版不含用户额度、收费和授权管理UI。

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
teacher模式Key由C加密保存、执行时交给Pi；platform模式由Pi环境提供。服务凭证与用户登录 token 分别校验；C 内部请求绕过代理且不跟随跳转。
自动回归使用Mock与本地兼容HTTP模型，不消耗真实模型额度。先前V4.1-Flash真实验证记录见 `docs/pi-chat-evaluation.md`；该记录不能替代上线后的人工质量检查。一个Pi实例使用一套平台模型配置，不接受客户端或用户个人Key。

## 模块和限制

- `src/agent.ts`：每个任务创建自己的 Pi Agent，恢复当前对话历史，运行有 Schema 的数学工具。
- `src/teaching.ts`、`skills/*/SKILL.md`：加载受信任的教学技能，只注入当前模式的内容。修改技能文件后重启服务，无效或缺失技能会阻止启动。
- `src/math-tools.ts`：只处理数值系数 a*x+b=c，区分唯一解、无解、任意实数；代入校验返回残差与浮点容差。
- `src/jobs.ts`：有界队列、用户/题目/教学模式隔离、单对话锁定、提示层级、取消、超时和状态清理。
- `src/server.ts`：内部 HTTP 接口；公共接口和前端契约见 `docs/pi-chat-api.md`。
- 单请求最大 64 KiB，提问最大 4000 个 Unicode 字符。
- 默认任务总预算 30 秒（含排队），最多 3 次模型调用、6 次工具调用，输出最多 2048 tokens。
- 默认 4 个并行任务、32 个等待位置、1000 个对话；闲置/完成状态保存 1 小时。
- 未启用持久化，重启 Pi 服务会清空任务和历史。未来需要持久化时应独立设计，不能把所有学生复用同一 Agent。
- 模型生成的文本不保证题目解答正确；质量验收需要真实题目集与人工核对。

环境变量清单见 `.env.example`。服务关闭会向正在运行的 Agent 发送取消信号。

## 基础解题模式

- `solve`（新对话默认）：完整讲解、关键步骤、条件和答案校验。
- `hint`：按成功轮次给1→2→3级提示，此后保持3级；失败或取消不升级。不给完整方程求解工具。
- `review`：每轮提供 `student_answer`；优先找首个可确认的错误，仅有最终答案时不能编造中间过程。

追问省略 mode 时继承原模式。切换模式需新建对话；已有完整答案不会混入提示对话历史。
Skill 是模型的教学指导，提示不泄露答案、错误定位准确等质量要求仍需真实模型评测。

计算输入与候选解必须有限且绝对值≤1e12；超范围报工具错误。
求解基于 IEEE-754 数值运算；代入校验容差为 `8*EPSILON*max(1, |a*x|, |b|, |c|)`。
零系数单独检查常数等式。模型负责从题干整理系数，工具不核验题干、定义域、单位或复杂符号表达式。
API 的 succeeded 只表示任务成功结束，不能理解为答案已被证明正确。

对于student_answer仅含数值候选（如x=7）且题干可识别为单个显式ax+b=c的情况，
`src/answer-review.ts`按题干系数确定性代入，最终反馈只说明等式是否成立并请求补充过程。
这类结果覆盖模型中无依据的错因猜测；对应文字也替换当前轮次的助手历史，不影响完整工具调用对。
该处理不读取题库标准答案，不保证识别复杂表达式，也不把满足等式当成满足全部定义域条件。

## 解题评测

`eval/fixtures.json` 包含30个案例（每模式10个），覆盖负数/小数方程、无解/多解、定义域、应用题、
缺少条件、移项错误、只提交最终答案、单位错误和提示模式中的越权请求。
标准答案在 rubric 中，仅写入评测报告用于人工核对，不进入模型提示。

只检查评测集，不调用模型：

```bash
cd ai-service
make eval
```

真实模型评测会串行提交30个案例并产生调用费用；将模型地址、名称和 Key 填入本地 `.env` 后显式运行：

```bash
make eval-real
```

本次使用官方 DeepSeek V4.1-Flash：`AI_API_BASE_URL=https://api.deepseek.com`、`AI_MODEL=deepseek-flash`。
`eval-real` 会读取 `.env`，普通 `make eval` 默认仅检查题集。Key 不会进入报告或提交。

也可完全通过环境变量配置（普通 eval 不自动加载 .env）：

```bash
export AI_API_BASE_URL=https://api.deepseek.com
export AI_MODEL=你的模型名称
read -rsp '模型 Key: ' AI_API_KEY; echo
export AI_API_KEY
AI_EVAL_REAL=1 make eval
unset AI_API_KEY
```

报告保存在被 Git 忽略的 `eval/reports/`，每题完成后立即保存。每题包含回复、单调时钟耗时、累计使用量和待填写的 human_review。
报告记录模型、地址以及题集/Agent/技能/工具/队列代码的哈希。恢复评测必须使用相同代码和模型，避免混合不同版本结果。

只评测指定案例，或恢复未完成报告（文件名从命令输出获得）：

```bash
npm run eval:real -- --cases solve-01,review-24
npm run eval:real -- --resume evaluation-时间戳.json
```

恢复时跳过已记录案例，不自动重复付费调用。认证、供应商异常或限流会保存进度并停止后续题目；
修改配置/代码后应新建报告重新验证。完成30题只表示请求完成，assessed仍需逐题核对。
将正确性、教学清晰度、模式遵循分别记0/1，失败任务记0；同一评测集比较模型或技能修改前后表现。
报告默认标注 `assessed:false`；评分时注明实际评审人或工具，Codex核对不能标成真人评分。
默认检查、CI与Mock均不能证明真实解题准确率；阶段二报告另见 `docs/pi-chat-evaluation.md`。

真实C接口学习流程检查（会启动临时服务、使用临时数据库并调用真实模型）：

```bash
python3 ai-service/eval/verify_learning_flow.py --real
```

在仓库根目录执行。覆盖三级提示与模式越权请求、仅答案反馈、修正过程追问和新建讲解对话。
测试结束后关闭自己启动的服务；默认不带--real只显示说明，不调用模型。

## 验证

```bash
make lint
make build
make test
```

Pi 用例包含 Mock 会话、参数与服务鉴权、队列、取消、超时、用户隔离、真实 SDK HTTP 协议、
工具调用与模型 401/429/500 错误脱敏；C 回归还验证公共接口和代理层异常处理。
CI 安装固定 Node 和依赖，运行两套测试，不要求真实模型密钥，不产生外部模型费用。


## C → Pi 内部请求超时与诊断（2026-10-21）

C从进程环境或server/.env读取AI_CONNECT_TIMEOUT_MS（默认500）和AI_REQUEST_TIMEOUT_MS（默认2000），单位毫秒，允许纯十进制整数1–60000。未设置或空字符串使用默认；零、负数、带符号/空白、小数、非数字、溢出或超范围值回退到该项默认，并只记录变量名与默认值，不记录原始输入。进程环境优先，修改后重启C生效。

总请求超时包含连接阶段；两项独立校验，连接阶段受两者较小值约束。这些设置只控制C提交/查询/取消内部HTTP请求，与Pi后台模型任务的AI_TIMEOUT_MS不同，不能用延长内部HTTP超时来延长模型推理时间。C当前是单轮询线程；大幅提高内部超时会延长故障期间其他请求等待时间，应按部署延迟保守调整。

C标准错误日志以[ai-client]标识分类：connect_failed表示无法连接/解析内部服务；request_timeout表示内部HTTP超时；transport_failed表示其他传输故障；internal_auth_failed表示Pi返回HTTP401，应核对C/Pi两侧AI_SERVICE_TOKEN。仅记录固定分类、curl数字错误码或HTTP401，不记录URL、请求头、token、用户输入或上游响应。内部401即使响应非JSON也转为脱敏503 AI_SERVICE_UNAVAILABLE；连接失败仍503，超时仍504 AI_SERVICE_TIMEOUT。公共API字段及前端1.2契约不变。
