# Pi Agent 前端接口档案

版本：1.0；日期：2026-10-13。适用于 C 主后端＋独立 Pi AI 服务。

前端只调用 **C 后端的 `/api/v1/*`**。本机默认地址为 `http://localhost:8000`；
其他机器联调时由后端负责人提供实际地址。Pi 内部端口、模型密钥和服务凭证不发送给前端。
页面由 C 后端托管时可使用相对路径；独立前端开发服务器应把 `/api` 代理到 C 后端。

配套文件：`pi-chat-openapi.json`（可导入支持 OpenAPI 3.1 的工具）与
`examples/pi-chat-client.mjs`（包含提交、轮询、取消和错误处理的客户端示例）。

## 1. 前端最短接入流程

1. 调现有 `POST /api/v1/auth/login`，保存返回的用户 `token`。
2. 调 `GET /api/v1/questions` 获取题目，使用返回的 id。
3. 调 `POST /api/v1/chat` 提交题目和问题，得到 job_id、conversation_id。
4. 每隔约 800 ms 调 `GET /api/v1/chat/{job_id}`，直到任务结束。
5. 成功时展示 reply；同一道题追问时传 conversation_id。
6. 用户取消时调用 `DELETE /api/v1/chat/{job_id}`。

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
| conversation_id | string | 否 | 后端返回的 UUID；限定同用户、同题目 |

不发送 user_id、模型、API Key、系统提示、工具配置或自定义 history。
未知字段、重复 JSON 字段及非法字段类型返回 400；请求体最大 64 KiB。

成功响应 **HTTP 202**：

```json
{
  "job_id": "11111111-1111-4111-8111-111111111111",
  "conversation_id": "22222222-2222-4222-8222-222222222222",
  "question_id": 1,
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

reply 是需要安全渲染的文本，可以包含 Markdown 和公式。先用 textContent 展示，
如转换为 HTML，应使用安全的渲染与过滤流程。model 为 `mock` 时显示“后端 Mock 模式”提示。
usage 是整个任务中模型调用的累计 token 数和调用次数，不代表价格；Mock 模式 token 数可以为 0。
input_tokens 包含供应商返回的普通输入、缓存读取和缓存写入 token。

失败结果示例（查询本身仍为 **HTTP 200**）：

```json
{
  "job_id": "11111111-1111-4111-8111-111111111111",
  "conversation_id": "22222222-2222-4222-8222-222222222222",
  "question_id": 1,
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
| 404 | QUESTION_NOT_FOUND | 返回题目列表或提示题目不可用 |
| 404 | JOB_NOT_FOUND / CONVERSATION_NOT_FOUND | 状态过期或资源不可访问；提示用户重新开始对话 |
| 405 | METHOD_NOT_ALLOWED | 检查请求方法 |
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

- 对话绑定当前用户和 question_id，跨用户或跨题目访问返回 404。
- 服务端保存完整模型/工具消息；前端仅保存 conversation_id。
- 同一对话最多 12 轮成功提问；历史过长时返回 CONVERSATION_LIMIT。
- 默认成功/失败/取消任务与闲置对话保存 1 小时，过期后访问返回 404。
- 本版状态在 Pi 服务内存中，重启该服务后清空；C 后端重启后用户需要重新登录。
- 本版提供文本解答和受限算术工具，消息接口接收文本；图片/PDF 上传、fork 和持久化对话树不包含在此契约中。

## 7. 接入现有前端的修改点

现有 `web/js/api.js` 的 API.chat 直接返回 POST 结果，前端 `web/js/chat.js` 立即读取 data.reply。
请把 API.chat 改为“提交＋轮询”的封装，完成后仍返回带 reply 的最终任务对象。

可以复制 `examples/pi-chat-client.mjs` 为 `web/js/pi-chat-client.js`，通过动态 import 引入：

```javascript
const { sendPiChat } = await import('./pi-chat-client.js');
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
- conversation_id 应按“用户＋题目”隔离保存；切换用户时清空。
- 取消按钮调用 abortController.abort()；示例客户端会在已知 job_id 时发送 DELETE。
- 示例客户端默认每 800 ms 查询一次，等待上限 45 秒；如后端延长总时限，请同步设置 timeoutMs。
- 保留明确标注的 file:// 离线演示，但在线业务失败应显示真实错误，不静默切换模拟数据。

## 8. 前端联调验收

- 登录后选题、提交、查询并展示后端回复，model=mock 时标明 Mock。
- 同题追问复用 conversation_id，切换题目建立新对话。
- 401 能提示重新登录，400/409 能保留输入并给出处理提示。
- failed / cancelled 能结束 loading；模型异常不会显示为“无回复”。
- 能取消任务；切换题目后旧请求不会覆盖新页面。
- 404 不触发在线模拟回复回退；供应商错误不会清理用户登录。

后端启动及真实模型配置见仓库 `ai-service/README.md`。此档案不含任何密钥。
