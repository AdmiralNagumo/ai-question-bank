# Pi Agent 前端接口档案

版本：1.2；日期：2026-10-20。适用于 C 主后端＋独立 Pi AI 服务。

前端只调用 **C 后端的 `/api/v1/*`**。本机默认地址为 `http://localhost:8000`；
其他机器联调时由后端负责人提供实际地址。Pi 内部端口、模型密钥和服务凭证不发送给前端。
页面由 C 后端托管时可使用相对路径；独立前端开发服务器应把 `/api` 代理到 C 后端。

配套文件：`pi-chat-openapi.json`（可导入支持 OpenAPI 3.1 的工具）与
`examples/pi-chat-client.mjs`（包含提交、轮询、取消和错误处理的客户端示例）。

## 1. 前端最短接入流程

1. 调现有 `POST /api/v1/auth/login`，保存返回的用户 `token`。
2. 调 `GET /api/v1/ai/access`；只有 allowed=true 才开放新任务发送。其他状态仍可浏览题目。
3. 调 `GET /api/v1/questions` 获取题目，使用返回的 id。
4. 调 `POST /api/v1/chat` 提交题目和问题，得到 job_id、conversation_id。
5. 每隔约 800 ms 调 `GET /api/v1/chat/{job_id}`，直到任务结束。
6. 成功时展示 reply；同一道题追问时传 conversation_id。
7. 用户取消时调用 `DELETE /api/v1/chat/{job_id}`。

请求头统一使用：

```http
Authorization: Bearer <用户登录 token>
Content-Type: application/json
```

题目详情沿用 `GET /api/v1/questions/{id}`。登录和题目接口的现有字段保持不变。
登录示例请求：`{"username":"student","password":"你设置的密码"}`；成功响应中读取 token。

## 2. 提交解答任务

```http
POST /api/v1/chat
```

首次提问：

```json
{
  "question_id": 1,
  "message": "请分步骤解释这道题"
}
```

同题追问：

```json
{
  "question_id": 1,
  "message": "为什么要两边同时除以系数？",
  "conversation_id": "22222222-2222-4222-8222-222222222222"
}
```

| 字段 | 类型 | 必填 | 规则 |
| --- | --- | --- | --- |
| question_id | integer | 是 | 1–2147483647，必须是已有且可用的题目 |
| message | string | 是 | 1–4000 个 Unicode 字符，不允许全空白或 NUL |
| conversation_id | string | 否 | 后端返回的 UUID；限定同用户、同题目、同教学模式 |
| mode | string | 否 | solve / hint / review；新对话默认 solve，追问省略时继承 |
| student_answer | string | review 每轮必填 | 1–4000 个 Unicode 字符，非空白，无 NUL；其他模式不接受 |

不发送 user_id、模型、API Key、系统提示、工具配置或自定义 history。
未知字段、重复 JSON 字段及非法字段类型返回 400；请求体最大 64 KiB。

成功响应 **HTTP 202**：

```json
{
  "job_id": "11111111-1111-4111-8111-111111111111",
  "conversation_id": "22222222-2222-4222-8222-222222222222",
  "question_id": 1,
  "mode": "solve",
  "status": "queued"
}
```

202 表示已接受任务。解答文本通过后续查询获取，界面此时显示排队/解答中。
同一个 conversation_id 同时只接受一个任务，等待前一个结束后再追问。
任务总时限默认 30 秒（包含排队），由后端 AI_TIMEOUT_MS 配置；首次上线返回完整文本，使用轮询获取状态。

## 3. 查询任务

```http
GET /api/v1/chat/{job_id}
```

HTTP 200 表示查询成功。**必须再检查响应中的 status**：

| status | 页面行为 |
| --- | --- |
| queued | 显示“排队中”，继续轮询 |
| running | 显示“解答中”，继续轮询 |
| succeeded | 停止轮询，展示 reply，保存 conversation_id |
| failed | 停止轮询，展示 error；根据 code 决定重试提示 |
| cancelled | 停止轮询，显示“已取消” |

成功结果示例：

```json
{
  "job_id": "11111111-1111-4111-8111-111111111111",
  "conversation_id": "22222222-2222-4222-8222-222222222222",
  "question_id": 1,
  "mode": "solve",
  "status": "succeeded",
  "reply": "等式两边减去 3 得到 2x = 8，再除以 2，得到 x = 4。",
  "model": "配置的模型名称",
  "usage": {
    "input_tokens": 100,
    "output_tokens": 40,
    "model_calls": 1,
    "tool_calls": 0
  }
}
```

reply 包含本轮工具调用前后的教学文字，不包含内部思考或往轮回复；它是需要安全渲染的文本，可以包含 Markdown 和公式。先用 textContent 展示，
如转换为 HTML，应使用安全的渲染与过滤流程。model 为 `mock` 时显示“后端 Mock 模式”提示。
usage 是整个任务中模型调用的累计 token 数和调用次数，不代表价格；Mock 模式 token 数可以为 0。
input_tokens 包含供应商返回的普通输入、缓存读取和缓存写入 token。

失败结果示例（查询本身仍为 **HTTP 200**）：

```json
{
  "job_id": "11111111-1111-4111-8111-111111111111",
  "conversation_id": "22222222-2222-4222-8222-222222222222",
  "question_id": 1,
  "mode": "solve",
  "status": "failed",
  "error": "解答超时，请稍后重试",
  "code": "AI_TIMEOUT",
  "http_status": 504
}
```

这里 http_status 描述任务失败类别，实际 GET 响应码为 200。
已取消或失败的任务不会提交这轮消息到对话历史。

## 4. 取消任务

```http
DELETE /api/v1/chat/{job_id}
```

HTTP 200 返回任务对象。排队/运行中的任务变为 cancelled；已完成的任务保留原状态和结果。
取消运行中的任务后，底层调用会收到中止信号；极短时间内再次向同一对话提交可能仍收到 409。
取消不能保证撤回模型供应商已经产生的计费。

## 5. HTTP 错误和任务错误

请求失败响应为 `{"error":"可显示的说明","code":"稳定的错误标识"}`。

| HTTP / 任务类别 | code | 前端处理 |
| --- | --- | --- |
| 400 | INVALID_REQUEST / INVALID_JSON | 检查字段和格式，保留用户输入 |
| 401 | UNAUTHORIZED | 清理当前登录状态并提示重新登录 |
| 403 | AI_ACCESS_REQUIRED / AI_ACCESS_REVOKED / AI_ACCESS_EXPIRED | 保留登录和输入，刷新权限，提示联系平台负责人；不自动重试 |
| 503 | AI_ACCESS_UNAVAILABLE | 权限存储暂不可用，禁用新发送，保留登录并允许手动刷新 |
| 404 | QUESTION_NOT_FOUND | 返回题目列表或提示题目不可用 |
| 404 | JOB_NOT_FOUND / CONVERSATION_NOT_FOUND | 状态过期或资源不可访问；提示用户重新开始对话 |
| 405 | METHOD_NOT_ALLOWED | 检查请求方法 |
| 409 | CONVERSATION_MODE_MISMATCH | 省略 conversation_id，以新模式新建对话 |
| 409 | CONVERSATION_BUSY | 等待当前任务结束 |
| 409 | CONVERSATION_LIMIT | 对话已达历史长度限制，省略 conversation_id 开始新对话 |
| 413 | REQUEST_TOO_LARGE | 缩短请求内容 |
| 415 | UNSUPPORTED_MEDIA_TYPE | 使用 application/json |
| 500 | INTERNAL_ERROR / AI_INTERNAL_ERROR | 提示服务异常 |
| 502 | AI_UPSTREAM_ERROR / AI_INVALID_RESPONSE / AI_OUTPUT_LIMIT / AI_CALL_LIMIT | 展示说明，让用户决定重试 |
| 503 | AI_SERVICE_UNAVAILABLE / AI_BUSY / AI_RATE_LIMITED | 服务未就绪或繁忙；稍后重试，不清理用户登录 |
| 504 | AI_SERVICE_TIMEOUT / AI_TIMEOUT | 提示超时；不要自动重复提交 POST |

模型供应商的鉴权失败会作为模型服务错误呈现，不转换成用户登录的 401。
HTTP 错误和 failed 任务都会停止当前客户端轮询；网络错误可以保留 job_id 供恢复查询。
不要将业务 404、405、409 或模型异常自动回退成“前端模拟解答”。

## 6. 对话和状态生命周期

- 对话绑定当前用户、question_id 和教学模式。跨用户或跨题目返回 404；同对话改 mode 返回 409。
- 服务端保存完整模型/工具消息；前端仅保存 conversation_id。
- 同一对话最多 12 轮成功提问；历史过长时返回 CONVERSATION_LIMIT。
- 默认成功/失败/取消任务与闲置对话保存 1 小时，过期后访问返回 404。
- 本版状态在 Pi 服务内存中，重启该服务后清空；C 后端重启后用户需要重新登录。
- 本版提供三个教学模式、受限算术与一元一次方程求解/代入校验工具，消息接口接收文本；图片/PDF 上传、fork 和持久化对话树不包含在此契约中。

## 7. 接入现有前端的修改点

现有 `web/js/api.js` 的 API.chat 直接返回 POST 结果，前端 `web/js/chat.js` 立即读取 data.reply。
请把 API.chat 改为“提交＋轮询”的封装，完成后仍返回带 reply 的最终任务对象。

可以复制 `examples/pi-chat-client.mjs` 为 `web/js/pi-chat-client.js`，通过动态 import 引入：

```javascript
const { sendPiChat, getPiAiAccess } = await import('./pi-chat-client.js');
const access = await getPiAiAccess({ token: API.getToken() });
setSendEnabled(access.allowed);
if (!access.allowed) return; // 页面保留题目、登录和用户输入
const result = await sendPiChat({
  questionId: currentQuestionId,
  message: text,
  conversationId: savedConversationId, // 首次为 undefined
  token: API.getToken(),
  signal: abortController.signal,
  onStatus: job => updateLoading(job.status)
});
savedConversationId = result.conversation_id;
addMessage('ai', result.reply);
```

- 发送期间禁用重复发送；切换题目、退出登录时清理/取消旧任务，防止回复出现在错误的题目页面。
- conversation_id 应按“用户＋题目＋教学模式”隔离保存；切换用户时清空。
- 取消按钮调用 abortController.abort()；示例客户端会在已知 job_id 时发送 DELETE。
- 示例客户端默认每 800 ms 查询一次，等待上限 45 秒；如后端延长总时限，请同步设置 timeoutMs。
- 保留明确标注的 file:// 离线演示，但在线业务失败应显示真实错误，不静默切换模拟数据。

## 8. 前端联调验收

- 默认未授权时禁用发送；授权后无需重新登录即可刷新为 active；自注册 admin 不获得 AI 权限。
- 撤销/过期返回 403 时保留登录和输入；已有任务仍能查询/取消。
- 权限查询 503 时不使用旧的 allowed=true 放行；接口返回 active 也要处理模型失败。
- 登录后选题、提交、查询并展示后端回复，model=mock 时标明 Mock。
- 同题追问复用 conversation_id，切换题目建立新对话。
- 401 能提示重新登录，400/409 能保留输入并给出处理提示。
- failed / cancelled 能结束 loading；模型异常不会显示为“无回复”。
- 能取消任务；切换题目后旧请求不会覆盖新页面。
- 404 不触发在线模拟回复回退；供应商错误不会清理用户登录。

## 9. 基础解题模式（1.1 新增）

| mode | 用途 | 界面建议 |
| --- | --- | --- |
| solve | 完整讲解、关键步骤和答案检查 | 默认“讲解” |
| hint | 分层提示，保留学生推导空间 | “给我提示” |
| review | 检查学生答案或过程，定位首个可确认错误 | “检查作答”，显示作答输入框 |

提示任务首次返回 `mode:"hint", hint_level:1`；每轮成功后，下一轮升至 2、3 级，此后保持 3。
失败/取消不会升级。一级侧重方向，二级给公式，三级给下一步操作。层级由后端决定，前端不传 hint_level。
提示内容由模型生成，真实模型评测前不能保证其完全不泄露答案；Mock 只验证接口，不代表教学质量。

切换模式时清理当前 conversation_id，建立新对话。后端拒绝在已有对话切换模式，避免完整答案历史混入提示模式。
提示模式下学生在 message 中要求“直接给答案”，服务仍选择提示技能；要查看完整讲解请由页面切换到 solve。

提示请求：

```json
{"question_id":1,"message":"给我一个方向","mode":"hint"}
```

错因分析请求：

```json
{"question_id":1,"message":"检查我的移项过程","mode":"review","student_answer":"2x+3=11，所以2x=14，x=7。"}
```

review 追问可以省略 mode，仍需传 student_answer（前端保留作答输入框内容即可）。仅有最终答案时后端会指导模型检查答案，不能声称找到了学生未提供的中间错误。

对于可识别的单个数值候选和显式一元一次方程，后端会使用确定性代入反馈，请求学生补充过程；
其他题型仍由模型处理。前端按同一reply文本字段展示，不需要另接工具接口。

所有任务状态都增加 mode；仅 hint 增加 hint_level。已授权用户的旧调用不传新字段仍按 solve 工作，异步状态流程保持一致。

客户端示例：

```javascript
const job = await sendPiChat({
  questionId: currentQuestionId, message: '检查我的作答',
  mode: 'review', studentAnswer: answerInput.value,
  token: API.getToken(), onStatus: updateLoading
});
```

新增验收：能选择三个模式；提示追问展示1→2→3级；切换模式建新对话；review缺失/过长作答能提示修正；取消后不升级提示。
目前自动数学工具仅支持 a*x+b=c，模型需正确整理系数；数值校验不等于原题、单位、定义域全部核验通过。

后端启动及真实模型配置见仓库 `ai-service/README.md`。此档案不含任何密钥。


## 10. 统一 API 与独立 AI 授权（1.2 必接）

**升级行为变化：所有账号默认没有 AI 使用权限。** 注册、登录、admin/teacher/student 角色，以及平台填好了 Key，都不等于有 AI 授权。后端负责人使用服务器 CLI 授予权限，前端无需接入授权写操作。

```http
GET /api/v1/ai/access
Authorization: Bearer <用户登录 token>
```

```json
{"ai_access":{"allowed":true,"status":"active","expires_at":"2026-12-31T23:59:59Z"}}
```

| status | allowed | 建议文案 |
| --- | --- | --- |
| not_granted | false | 账号尚未获得 AI 使用权限，请联系平台负责人 |
| active | true | 可发送新的 AI 提问 |
| revoked | false | AI 使用权限已撤销，请联系平台负责人 |
| expired | false | AI 使用权限已到期，请联系平台负责人 |

expires_at 为 UTC RFC3339 秒精度时间或 null；null 表示未设置到期时间，不等于已授权。revoked 可保留旧到期时间。以服务器状态为准，不用浏览器时间、角色或本地缓存计算授权。响应带 Cache-Control: no-store。200/active 只证明有权限，不能证明模型余额、Key、网络或模型当前可用。

登录后、进入解题页及用户主动刷新时调用 getPiAiAccess。权限未知/查询失败时先禁用新发送；刷新成功再恢复。每次 POST 仍由服务器重新检查，预检查无法消除授权被撤销的竞态。403 要保存草稿、关闭当前发送状态并刷新权限；不要退出登录，不自动重试 POST，不切换在线 Mock。只有用户认证 401 才走重新登录流程。

```javascript
try {
  const access = await getPiAiAccess({ token: API.getToken() });
  renderAiAccess(access);
  setSendEnabled(access.allowed);
} catch (error) {
  setSendEnabled(false);
  showError(error.message); // 401 另按现有登录流程处理
}
```

**撤销边界：** POST 新任务被阻止；已接受的排队/运行任务仍可完成；本人仍可 GET 查询和 DELETE 取消。不要因发送被禁用就清空 job_id 或停止已有任务的查询。跨用户始终不可访问。撤销不退款、不删除历史、不强制中断供应商调用；已通过授权检查的提交仍可能返回 202。

前端只持有用户登录 token。模型 Key、内部服务 token、用户 ID、额度或授权状态都不能作为提交字段。一个 Pi 实例使用平台统一模型配置，本版不提供用户 Key 上传、用户额度/计费、授权管理页面或教师授权接口。完整运维步骤、部署和数据边界见 [ai-access.md](ai-access.md)。现有 web 页面需要前端同学按本档案接入；本次交付的是可运行客户端、接口契约和后端实现。
