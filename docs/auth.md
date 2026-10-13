# 用户认证模块文档

> 对应 PR：feat/auth（用户注册接口 + 用户存储迁移至 kv_store + session 清理机制）
> 最后更新：2026-10-13

---

## 1. 概述

本模块实现基于 Token 的用户认证系统，包括：

- **注册**（用户名/密码校验 + 密码强度检查 + 创建用户 + 自动登录）
- **登录**（密码校验 + 创建 Session）
- **登出**（销毁 Session）
- **当前用户信息查询**（Token 校验）
- **预设 Demo 账号**（三角色：admin / teacher / student）

认证方式为 **Bearer Token**（`Authorization: Bearer <token>`），Session 存储在服务端内存中（单线程 demo 模式）。用户数据通过 **kv_store** 目录式存储层持久化（namespace：`users`，每个用户为一个 key，id/username/name/role/password_hash 为 value_name）。

---

## 2. API 接口

### 2.1 注册

```
POST /api/v1/auth/register
Content-Type: application/json

{"username": "newuser", "password": "mypass123", "confirm_password": "mypass123", "role": "student"}
```

**参数说明：**

| 字段 | 必填 | 说明 |
|------|------|------|
| `username` | 是 | 用户名，不能为空 |
| `password` | 是 | 密码（≥8 位，必须同时包含字母和数字） |
| `confirm_password` | 是 | 确认密码，必须与 password 一致 |
| `role` | 否 | 角色：`student`（默认）/ `teacher` / `admin` |

**成功响应（201 Created）：**

```json
{
  "token": "a1b2c3d4e5f6...64位十六进制",
  "user": {
    "id": 4,
    "username": "newuser",
    "name": "newuser",
    "role": "student"
  }
}
```

注册成功后自动创建 Session 并返回 token，客户端可直接进入已登录状态。

**失败响应：**

| 状态码 | 说明 |
| --- | --- |
| 400 | 请求体为空 / JSON 解析失败 / 缺少必填字段 / 用户名不能为空 |
| 400 | 两次密码不一致 |
| 400 | 密码不符合强度要求（≥8 位 + 字母 + 数字） |
| 400 | 角色值无效（仅支持 student/teacher/admin） |
| 409 | 用户名已存在 |
| 500 | 服务器内部错误（创建用户失败 / 创建会话失败） |

### 2.2 登录

```
POST /api/v1/auth/login
Content-Type: application/json

{"username": "admin", "password": "admin123"}
```

**成功响应（200）：**

```json
{
  "token": "a1b2c3d4e5f6...64位十六进制",
  "user": {
    "id": 1,
    "username": "admin",
    "name": "系统管理员",
    "role": "admin"
  }
}
```

**失败响应：**

| 状态码 | 说明 |
| --- | --- |
| 400 | 请求体为空 / JSON 解析失败 / 缺少 username 或 password |
| 401 | 用户名或密码错误（不区分是用户不存在还是密码错误，防枚举） |
| 500 | 查询用户信息失败 / 创建会话失败（OOM 等） |

### 2.3 登出

```
POST /api/v1/auth/logout
Authorization: Bearer <token>
```

**成功响应（200）：** `{"status":"ok"}`

说明：无论 token 是否有效，均返回 200。仅销毁服务端内存中对应的 Session。

### 2.4 当前用户信息

```
GET /api/v1/auth/me
Authorization: Bearer <token>
```

**成功响应（200）：**

```json
{
  "user": {
    "id": 1,
    "name": "系统管理员",
    "role": "admin"
  }
}
```

**失败响应：**

| 状态码 | 说明 |
| --- | --- |
| 401 | 未登录或 token 已过期 |

---

## 3. Session 管理

### 3.1 存储方式

- 内存固定数组（`Session g_sessions[MAX_SESSIONS]`），最多 **128** 个并发 Session。
- 仅适用于单线程 demo 模式。

### 3.2 Token 生成

- 64 位随机十六进制字符串（`rand() % 16`，demo 够用）。
- 有碰撞重试机制（最多 5 次）。

### 3.3 Token 有效期

- 创建时设置 `expires_at = now + 86400`（24 小时）。
- 过期后自动标记失效，需重新登录。

### 3.4 Token 提取

- 从 `Authorization` 请求头中提取 `Bearer <token>`。
- 不区分大小写匹配 `Bearer ` 前缀。

### 3.5 Session 清理机制

为避免僵尸 session（已过期或已登出但未被压缩的条目）耗尽 128 个槽位：

- 每调用 `session_create()` **16 次**触发一次 `session_cleanup()`，扫描全部 session 条目：
  - 标记已过期的 session 为 inactive
  - 将活跃条目紧凑排列到数组头部
  - 清零尾部释放的槽位
- 若压缩后槽位仍不足，**强制再执行一次清理**；清理后仍不足才返回失败（NULL）
- 单线程环境，无需加锁

---

## 4. Demo 账号

系统启动时通过 `db_seed_users()` → `kv_store` 写入三个演示账号：

| 用户名 | 角色 | 默认密码 | 姓名 |
| --- | --- | --- | --- |
| `admin` | admin | admin123 | 系统管理员 |
| `teacher` | teacher | teacher123 | 张老师 |
| `student` | student | student123 | 李同学 |

密码可在 `server/.env` 中通过环境变量覆盖：

```
DEMO_ADMIN_PW=admin123
DEMO_TEACHER_PW=teacher123
DEMO_STUDENT_PW=student123
```

### 4.1 密码哈希

- 算法：`SHA-256(salt || password)`
- 固定 salt：`ai-question-bank-demo-salt-2026`（demo 阶段使用固定 salt，对抗彩虹表但不要求保密）
- 依赖：OpenSSL `libcrypto`（通过 `-lcrypto` 链接）
- 注册接口使用相同的 salt 和算法进行哈希

---

## 5. 数据存储

### 5.1 存储方案

用户数据不再使用独立 `users` 表，而是通过 **kv_store** 目录式存储层持久化：

| 层级 | 值 | 说明 |
|------|-----|------|
| namespace | `users` | 所有用户数据所在的命名空间 |
| key | `<username>` | 以用户名作为主键 |
| value_name / value | `id` / `"1"` | 用户 ID（自增） |
| | `username` / `"admin"` | 用户名 |
| | `name` / `"系统管理员"` | 显示名称 |
| | `role` / `"admin"` | 角色 |
| | `password_hash` / `"<SHA-256>"` | 密码哈希 |

底层映射为 SQLite 表 `kv_users`（由 kv_store 自动创建），无需手动管理 schema。详见 `docs/kvstore.md`。

### 5.2 用户查询与操作函数

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `db_query_user_by_username` | `char *(sqlite3 *db, const char *username)` | 通过 kv_store 查询用户完整信息（id/username/name/role），返回 JSON 字符串 |
| `db_query_password_hash` | `char *(sqlite3 *db, const char *username)` | 通过 kv_store 查询密码哈希，用于登录校验 |
| `db_create_user` | `int(sqlite3 *db, const char *username, const char *password_hash, const char *role)` | 创建新用户；role 为 NULL 时默认 `student`；返回 0 成功 / 1 用户名已存在 / -1 错误 |
| `db_seed_users` | `int(sqlite3 *db)` | 写入 demo 三账号（仅在 `db_init` + `kv_init` 之后调用） |

所有函数共享 `AppConfig.db` 连接，不另行打开数据库。函数签名中的 `sqlite3 *db` 从 `main.c` → `AppConfig` → `route_dispatch` 逐层传入。

### 5.3 用户 ID 生成

新用户 ID 通过 `kv_list_keys(db, "users", &count)` 统计当前已有用户数，取 `count + 1`。demo 账号固定为 1/2/3。

---

## 6. 测试

### 6.1 测试用例

| 文件 | 用例说明 |
| --- | --- |
| `server/tests/auth/case_1.sh` | 三个预设账号登录成功（admin/teacher/student） |
| `server/tests/auth/case_2.sh` | 登录失败场景（错误密码 / 不存在用户 / 缺少字段） |
| `server/tests/auth/case_3.sh` | 当前用户信息（有效 token / 无 token / 无效 token） |
| `server/tests/auth/case_4.sh` | 登出（登出返回 200 / 登出后 token 失效） |
| `server/tests/auth/case_5.sh` | 注册成功（admin/teacher/student 三种角色） |
| `server/tests/auth/case_6.sh` | 注册校验失败（密码过短/纯字母/纯数字/不一致/缺字段/用户名重复/无效 role/空用户名，共 10 条） |
| `server/tests/auth/case_7.sh` | 注册后自动登录（`/me` 验证 → 登出 → token 失效） |
| `server/tests/auth/case_8.sh` | 注册后二次登录验证（错误密码 / 不存在用户 / `/me` 访问） |

### 6.2 运行测试

```bash
# 全部测试
make -C server test

# 仅认证测试
bash server/tests/run_tests.sh auth
```

测试框架（`server/tests/run_tests.sh`）自动管理服务生命周期（启动 → 运行测试 → 关闭）。

---

## 7. 代码结构

```
server/
├── include/
│   ├── auth.h          # Session 结构体定义、函数声明
│   ├── db.h            # db_query_user_by_username / db_query_password_hash / db_create_user / db_seed_users
│   ├── kv_store.h      # 目录式 KV 存储接口（用户数据存储依赖）
│   └── routes.h        # route_dispatch（含 AppConfig 参数）
├── src/
│   ├── auth.c          # Session 内存管理（create/validate/destroy/extract）+ session_cleanup
│   ├── db.c            # 哈希工具、seed_demo_accounts、用户/密码查询（经 kv_store）、db_create_user
│   ├── kv_store.c      # kv_store 存储层实现
│   ├── main.c          # 入口：db_init → kv_init → db_seed_users → http_server_start
│   ├── http_server.c   # POST 请求体累积（RequestCtx）、route_dispatch 调用
│   └── routes.c        # 路由处理（handle_register/handle_login/handle_logout/handle_me）
└── tests/
    └── auth/
        ├── case_1.sh   # 登录成功
        ├── case_2.sh   # 登录失败
        ├── case_3.sh   # 当前用户
        ├── case_4.sh   # 登出
        ├── case_5.sh   # 注册成功
        ├── case_6.sh   # 注册校验失败
        ├── case_7.sh   # 注册后自动登录
        └── case_8.sh   # 注册后二次登录
```

### 7.1 路由注册方式

在 `route_dispatch()` 中按顺序匹配：

```c
/* GET /api/v1/health */
if (GET && /api/v1/health) → handle_health()

/* POST /api/v1/auth/register */
if (POST && /api/v1/auth/register) → handle_register(body, cfg)

/* POST /api/v1/auth/login */
if (POST && /api/v1/auth/login) → handle_login(body, cfg)

/* POST /api/v1/auth/logout */
if (POST && /api/v1/auth/logout) → handle_logout()

/* GET /api/v1/auth/me */
if (GET && /api/v1/auth/me) → handle_me()

/* 其他 /api/ 路径 */
→ 404

/* 非 /api/ 路径 */
→ serve_static()
```

---

## 8. 安全说明

- **密码不存明文**：数据库只存 SHA-256 哈希值（加固定 salt）。
- **密码强度校验**：注册时强制要求 ≥8 位且同时包含字母和数字，防止弱密码。
- **登录防枚举**：用户名不存在与密码错误均返回 401 + `"用户名或密码错误"`；注册接口中用户名已存在返回 409 以区分。
- **API Key 不暴露**：认证仅涉及用户名/密码与 Session token，不涉及 AI API Key。
- **Session 有效期**：24 小时过期；过期 session 通过定期清理机制回收。
- **登出失效**：登出后原 token 立即失效，不可复用。
- **当前为 demo 实现**：Session 存储在内存固定数组中，不支持多进程/多实例/持久化，生产环境需替换为 Redis 或数据库存储。