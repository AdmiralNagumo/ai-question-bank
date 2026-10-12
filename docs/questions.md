# 第一阶段固定题目模块（李俊熠）

实现范围来自《第一阶段开发计划与 AI 协作指南》第一部分 1.2：固定题 seed、列表、详情和回归。

## 数据初始化

- 启动时插入 5 道题：一元一次方程、导数、极限、牛顿第二定律、C 的 sizeof。
- 包含 `content`、`options`、`answer`、`analysis`、`knowledge_points`、`type`。
- 数据库存储沿用 `content/answer/analysis`，新增 `options`、`knowledge_points`（JSON 数组文本）、
  `question_type` 与内部 `seed_key`。新增字段兼容旧的 questions 表。
- `seed_key` 唯一索引保证重复启动不会重复插入，也不会覆盖已有题目的内容、答案或状态。
- 旧库只增加字段与缺失的 seed，保留原有数据和 id。新库默认题目 id 为 1–5；
  **前端必须使用列表返回的 id，不可硬编码 id 或题目数量。**
- 本阶段 Demo 题目由管理员持有，teacher_id 从现有 admin 账号查询；不引入多租户功能。
- `teacher_id`、`tags`、`status`、`created_at` 等已有字段继续保留，以便后续扩展。

## 接口契约（供题目列表页、对话页与 api.md 维护者使用）

两个接口均需登录取得 token，然后携带 `Authorization: Bearer <token>`。
管理员、学生及现有有效会话使用相同的只读接口。未登录、无效或已登出 token 返回 401。

### GET /api/v1/questions

200 响应为 `{"questions":[题目对象,...]}`，按 id 升序返回全部 active 题目。
本阶段无检索与分页，查询参数不会过滤列表；空列表返回 `{"questions":[]}`。

### GET /api/v1/questions/{id}

200 响应示例（新数据库第一道题）：

```json
{
  "question": {
    "id": 1,
    "content": "解方程：2x + 3 = 11，x 的值是多少？",
    "options": [
      {"label": "A", "text": "2"},
      {"label": "B", "text": "3"},
      {"label": "C", "text": "4"},
      {"label": "D", "text": "5"}
    ],
    "answer": "C",
    "analysis": "两边减去 3 得到 2x = 8，再除以 2，得到 x = 4。",
    "knowledge_points": ["一元一次方程", "等式性质"],
    "type": "single_choice"
  }
}
```

列表中的题目对象与详情中的 `question` 对象字段和内容相同。
`type` 为 `single_choice` 或 `short_answer`；单选题 answer 是选项 label，
简答题 answer 是文本且 options 为 `[]`；知识点始终为字符串数组。
旧表迁移的记录缺少选项/知识点时以空数组返回，不捏造内容。

| 状态码 | 响应 | 场景 |
| --- | --- | --- |
| 401 | `{"error":"未登录或 token 已过期"}` | 无有效会话，优先于题目参数校验 |
| 404 | `{"error":"question not found"}` | 不存在/非 active 题目，或不是正整数的 id、溢出、额外路径段 |
| 405 | `{"error":"method not allowed"}` | 登录后对题目接口使用非 GET 方法；响应含 `Allow: GET` |
| 500 | `{"error":"query questions failed"}` | 数据库不可读、查询/JSON 处理失败 |

id 必须是正的十进制整数。客户端先登录，再 GET 列表，点击后用该条 id GET 详情；
将 id 传给对话接口的实现者。客户端收到 401 应提示重新登录。

## 代码位置与集成点

- `server/src/kv_store.c`：键值对存储层，namespace "qdata" 存储题目数据（content, options, answer, analysis, type, knowledge_points, status 等）。
- `server/include/kv_store.h`：kv_store 查询接口和返回约定。
- `server/src/routes.c`：严格匹配路由、有效会话校验、id 校验和 HTTP 状态映射，通过 `handle_questions` 调用 `kv_list_keys` / `kv_get_all` 组装列表与详情。
- `server/include/routes.h`：更新路由说明，沿用现有 route_dispatch 签名。
- `server/src/db.c`：启动时通过 `db_seed_questions` 将 5 道固定题 seed 到 kv_store。
- 现有 auth 模块负责会话；本模块没有重写认证、前端或 AI 模块。

## 本地验证

在 WSL 的仓库根目录运行：

```bash
make lint
make build
make test
# 仅跑题目组（先 build）
bash server/tests/run_tests.sh questions
# 开发运行（另一个终端用已有登录接口获取 token）
make -C server run
```

当前 main 已将 run_smoke.sh 替换为 `run_tests.sh`，新增用例放进自动发现的 questions 目录。
HTTP 回归使用 Python 3 标准库（Ubuntu 与 ubuntu-latest 已提供），不增加 pip 依赖。
测试运行器默认使用独立临时数据库，不读写开发数据库，也不会杀死占用测试端口的其他进程。
如果指定 DATABASE_PATH，目标文件必须尚不存在。

| 用例 | 验证内容 |
| --- | --- |
| questions/case_1.sh | admin/student 全量列表、5 题、字段与选项结构、中文、无分页 |
| questions/case_2.sh | 所有详情与列表一致；不存在/非法/溢出 id 的 JSON 404 |
| questions/case_3.sh | 缺失/无效/登出 token 的 401，非 GET 的 405，路径前缀不误匹配 |
| questions/case_4.sh | 数据库缺失返回 500，不创建空库，恢复后正常 |
| questions/case_5.sh | 旧库迁移保留数据，重复 seed 不重复且不覆盖修改，空列表和归档 |

## 联调交接与剩余人工验收

后端契约和自动回归已提供。曹渲东的列表页可使用 questions 数组，点击后传递题目 id；
王英杰的对话页可使用详情 question 对象；戴儒骋可将本节契约整合进统一 api.md。
PDF 中任务 D 的实际前端联调、演示和队友 PR 审查，需要相关模块就绪后由团队完成。
合入 main 仍需按照 CONTRIBUTING.md 通过 PR、CI 与至少一名队友审查。
