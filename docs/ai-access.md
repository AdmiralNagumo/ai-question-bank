# 统一 AI 接口与用户授权边界

> v1.3新增默认teacher路由；本文以下全局CLI规则仅适用显式AI_ROUTING_MODE=platform。新契约见[teacher-api.md](teacher-api.md)。

版本1.2，2026-10-20。C是唯一业务入口，Pi是内部模型执行服务。

## 接口契约（实现依据）

- `GET /api/v1/ai/access`：Bearer登录；200返回`{"ai_access":{"allowed":false,"status":"not_granted","expires_at":null}}`。
- status为`not_granted | active | revoked | expired`；active时allowed=true。expires_at为UTC RFC3339秒精度字符串或null；等于当前服务器时间时已过期。
- 此接口只查权限，不探测模型、余额或网络；200/allowed=true不保证模型调用成功。错误时不返回允许使用的缓存结果。
- 未登录401；其他方法405，Allow: GET；数据库故障503 `AI_ACCESS_UNAVAILABLE`，默认拒绝新AI提交。
- `POST /api/v1/chat`：登录、方法检查后先查权限，再校验题目/请求。无授权403 `AI_ACCESS_REQUIRED`，撤销403 `AI_ACCESS_REVOKED`，到期403 `AI_ACCESS_EXPIRED`。
- 403保留登录token和用户输入；前端刷新权限并提示联系平台负责人。不得切换为在线Mock，不自动重试POST。
- 请求字段、三种mode、202异步任务及轮询不变。前端不能传user_id、权限、模型或Key来覆盖后端配置。
- `GET/DELETE /api/v1/chat/{job_id}`沿用登录及本人任务校验；未获得/已撤销AI权限仍可查询/取消自己的既有任务。跨用户始终404。

## 授权与撤销

- 所有账号初始无AI授权，包括demo admin/teacher/student及自选admin注册账号。角色、模型Key是否存在不产生授权。
- 本阶段没有HTTP授权写入接口；拥有服务器数据库写入权限的运维人员通过`scripts/ai_access.py`授权/撤销。
- 可设置到期时间；未设置表示不自动到期。变更直接存库，下次提交读取，无需重新登录或重启C/Pi。
- 撤销只阻止之后通过权限检查的新任务，不自动中止已接收的排队/运行任务，不删除已有结果，不退还供应商费用。
- 并发撤销与提交以C读取授权时的快照为界；已通过检查的请求仍可能返回202。
- 持久化授权独立于内存会话、任务；C重启需重新登录，Pi重启清空任务，授权本身保留。

## 模型凭证和部署

- 当前一个Pi实例统一使用一套`AI_API_BASE_URL / AI_MODEL / AI_API_KEY`，所有获授权用户共用平台模型能力；不接收个人Key。
- Key从Pi进程环境注入，本地`.env`是开发便利入口；生产可通过服务管理器或密钥管理系统注入同名变量，无需改变公共API。
- Key不写用户/授权表、接口响应、前端配置、Git或交付包。内部AI_SERVICE_TOKEN与登录token、模型Key是三种不同凭证。
- Pi默认只监听127.0.0.1；不可向学生直接开放8001端口。反向代理只转发C的公共API，并配置HTTPS。
- 本阶段保留提供商兼容协议及受限工具；无服务端任意脚本/文件工具，不自动加载个人Pi配置。

## 明确未实现

用户额度、计费、按用户速率限制、租户授权继承、教师自行授权、授权管理页面、用户自行上传Key、多提供商按请求路由、任务持久化、撤销后强制杀任务均未实现。
当前全局队列/并发限制只控制Pi进程负载；不能当作用户额度。公共API本阶段不会因“个人额度”返回429。

当前登录仍是项目已有的demo会话实现，用户ID分配与demo账号seed策略也沿用现有模块。授权要求用户ID唯一且稳定；删除/重用ID及完整生产身份体系需要另行迁移并撤销相关会话/授权。
会话token已改为OpenSSL密码学随机数，格式仍为64位十六进制，随机源失败时拒绝创建会话。
有服务器/数据库写权限的人能改变授权，SQLite审计不提供防篡改或不可抵赖保证。本方案不宣称已完成整站生产安全加固。

题目列表和详情继续使用原有登录规则，当前响应包含answer/analysis；AI授权不限制查看标准答案，也不新增题库/考试租户隔离。这不是考试防作弊功能。
正式部署前必须处置已知demo账号/默认密码、HTTPS、数据库备份和主机权限；不得直接把开发演示账号授权后公开到互联网。

## 运维操作与数据边界

在仓库根目录，先启动新版C一次以创建两张表，再指定同一个实际DATABASE_PATH：

```bash
python3 scripts/ai_access.py --db server/data/ai_question_bank.db status --username student
python3 scripts/ai_access.py --db server/data/ai_question_bank.db grant --username student --expires-at 2026-12-31T23:59:59Z --reason "受控前端联调"
python3 scripts/ai_access.py --db server/data/ai_question_bank.db revoke --username student --reason "联调结束"
```

这些是运维命令，不能放到前端执行。grant不传expires-at表示无限期，并清除旧到期时间；重授予可恢复撤销或过期账号。revoke保留旧到期时间，状态以revoked优先。
授权状态更新与审计插入是同一SQLite事务，失败全部回滚；重复操作状态幂等，但每次写入都会追加审计。
CLI拒绝未知账号、重复ID、无时区/过去的到期时间、未初始化或不存在的数据库，不会因路径拼错创建新库。

`ai_access_grants`存用户ID、账号键、enabled、到期/更新时间；`ai_access_audit`存操作、操作人、原因和时间。两表不存模型Key。
当前存储实现将users这个KV namespace映射为实际users表（旧文档称kv_users，不能据此直接写SQL）。C复用main的共享连接；只在单HTTP轮询线程下使用，不新增业务线程。
部署是增量建表且不自动授权旧账号；回退到不含授权检查的旧代码会重新开放登录用户的AI提交，安全回退需先由代理禁用chat提交入口。
公共请求体超过64KiB时接收层可能先返回413；正常大小请求按登录→方法→AI授权→字段/题目检查。当前取消、重试和多次提交没有费用退款或幂等计费保证。

前端详细流程与示例以`pi-chat-api.md`及`pi-chat-openapi.json`的1.2契约为准。
