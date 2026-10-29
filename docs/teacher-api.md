# 教师 API 管理与调用契约 v1.3

实现范围：教师自行注册并以teacher登录即可管理自己的一个API，无额外审核或运维授权。学生调用从题目的teacher_id解析归属，不修改题库CRUD、数据或课程关系。

## 接口

- `GET /api/v1/teacher/ai-api`：教师查询自己的配置，返回`{"api":{"configured":false}}`或包含configured、enabled、provider、model、masked_key、version、updated_at的对象。绝不返回完整Key或密文。
- `PUT /api/v1/teacher/ai-api`：`{"api_key":"供应商密钥","model":"deepseek-flash"}`；model可省略。一个教师只有一条配置，重复提交替换并启用，version递增。只支持DeepSeek兼容协议，供应商地址由运维在Pi环境设置，教师不能指定地址。
- `PATCH /api/v1/teacher/ai-api`：`{"enabled":false}`或true；停用/启用，不回显Key，version递增。
- 上述接口需要有效Bearer登录和teacher角色；student/admin均403。归属从token获得，不接受teacher_id/user_id等未知字段。新增/替换/启停均200；未知配置启停404，非法字段400，非JSON415，方法405。
- `GET /api/v1/ai/access?question_id=1`：teacher路由模式下需question_id。200仍使用`ai_access.allowed/status/expires_at`；附加teacher_id、reason_code、quota（limit/used/remaining/reset_at/timezone/scope）。可用active，不可用not_granted；缺失question_id为400。故障503，不缓存、不放行。
- `POST /api/v1/chat`请求字段和异步202保持不变。题目归属来自存储的qdata.teacher_id，不接受客户端覆盖。学生可用当前题目所属教师的API，教师仅可调用自己的API；admin无教师API调用资格。不再叠加旧的CLI全局AI授权。
- `GET/DELETE /api/v1/chat/{job_id}`继续仅校验登录与任务本人；教师停用后仍可查询/取消已有任务。

## 配额与错误

学生每名、每位教师、每天默认20次，按Asia/Shanghai自然日重置，由C的AI_STUDENT_DAILY_LIMIT配置（1–10000）。教师调用自己的配置不扣学生额度。每次通过本地校验并向Pi提交的新任务预扣1次；追问是新任务，查询/取消不计新次数。

SQLite短写事务原子检查/扣减，不能靠前端计数。Pi明确拒绝（有效4xx、AI_BUSY等明确未接收错误）时释放；已接受的失败/取消不退次数。网络中断/超时/响应损坏时无法证明是否已接受，保守保留扣次，不自动重复POST。此版不提供幂等POST或供应商退款保证；次数不是token计费，一次任务仍受模型轮次/输出/超时预算控制。占用记录和额度跨C/Pi重启保留，任务内容仍是Pi内存TTL。

| HTTP | code | 含义 |
|---|---|---|
|401|UNAUTHORIZED|重新登录|
|403|TEACHER_REQUIRED / AI_TEACHER_ONLY|非教师管理配置 / 教师试图使用他人API|
|404|QUESTION_NOT_FOUND / AI_API_NOT_CONFIGURED|题目/配置不存在|
|403|AI_API_DISABLED|教师停用配置|
|429|AI_QUOTA_EXCEEDED|学生本周期用完，响应含quota和Retry-After|
|409|AI_CREDENTIAL_CHANGED|对话绑定的教师/配置版本改变，需新建对话|
|503|AI_CREDENTIALS_UNAVAILABLE / AI_API_UNAVAILABLE|主密钥、身份绑定、加密数据或存储不可用，拒绝调用|

## 保存与部署

C的AI_KEY_ENCRYPTION_KEY为32字节随机主密钥的64位十六进制。AES-256-GCM随机12字节nonce、16字节认证tag，AAD绑定教师ID/账号键/版本/模型。主密钥不进数据库或Git；丢失后旧密文无法恢复，需教师重新提交。使用scripts/configure_teacher_api.py仅补充缺失主密钥，不打印、不覆盖；备份数据库必须单独备份主密钥。禁止直接覆盖主密钥实现“轮换”。

教师凭证只在可信C/Pi后端为任务短暂解密，不加入模型提示词、工具、用户响应或日志；C清理临时密钥/请求缓冲，Pi终态清理执行配置引用。JS字符串无法保证内存立即擦除，不宣称抵御主机管理员或内存转储。数据库篡改或主密钥错误应认证失败，不降级。

默认AI_ROUTING_MODE=teacher（C/Pi保持一致）；旧平台统一Key演示可显式设置platform，使用旧CLI授权，不能将其视为教师路由回退。教师配置缺失/停用时绝不回退平台Key。teacher模式Pi启动不需要平台AI_API_KEY。AI_MOCK=1仍运行完整授权/配额流程但不调用供应商。

Pi应只监听127.0.0.1并使用独立AI_SERVICE_TOKEN；跨主机必须用TLS和访问控制。教师只能配置Key/模型，不能上传插件/自定义请求地址。凭证版本变更后已有接受任务使用其快照完成；同对话追问必须新建，避免跨教师/配置串历史。

## 题库对接与非目标

只读取已有qdata.teacher_id；归属必须指向现存且唯一ID的teacher账号，绑定账号键，重复ID/角色改变拒绝。演示seed归属admin，不会自动迁移或转给教师，需要题库负责人创建/归属教师的题目后联调。用户ID删除重用及完整认证体系由认证模块负责，当前不改其注册角色策略。

学生可读题目范围沿用题库现有规则，本模块不额外建立师生、班级、课程关系，不声称限制所有题目读取。未实现多个API、学生捐赠、供应商余额、计费、教师总额度、逐学生自定义额度和持久化对话。后续可将当前teacher_id唯一配置扩展成多记录，本阶段不开放。

设计依据：[OpenSSL AEAD接口](https://docs.openssl.org/3.0/man3/EVP_EncryptInit/)、[SQLite事务](https://www.sqlite.org/lang_transaction.html)。
