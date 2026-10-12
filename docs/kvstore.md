# kv_store — 目录式键值对存储层

## 设计概念

```
一级目录 = namespace → 数据库表 kv_{sanitized_ns}
   ↓
一个 key 下可通过 value_name 对应多个 value
```

| 概念 | 映射 |
|---|---|
| **namespace**（一级目录） | 数据库表 `kv_{name}` + `kv_tables` 元信息记录 |
| **key**（键） | 表中 `key_name` 列 |
| **value_name**（值名） | 表中 `value_name` 列 |
| **value**（值） | 表中 `value` 列 |

`(key_name, value_name)` 构成复合主键，同一 key 下允许多个命名值。

## 命名约束

`namespace` 只允许字母、数字、下划线（`a-zA-Z0-9_`），首字符不能是数字。不符合的名称返回 `-1`（无效参数）。

## 线程安全

本模块未加锁。若在多线程环境下使用同一个 `sqlite3*` 连接，调用者需自行保证串行化（如 SQLite 的 `sqlite3_busy_timeout`）。

## 所有权约定

- `char*` 返回值均由 `malloc` 分配，**调用者必须 `free()`**。
- `char**` 数组（末尾 `NULL` 哨兵）中每个字符串及数组本身均由 `malloc` 分配，调用者需依次 `free` 每个元素后再 `free` 数组。也可使用 `kv_free_str_array()` 一键释放。
- 传入的字符串参数（namespace, key, value_name, value）由调用者保证在函数返回前有效。

## API 参考

### 生命周期

```c
int kv_init(sqlite3 *db);
```
- 初始化 kv 子系统：创建 `kv_tables` 元信息表
- 可在 `db_init()` 之后调用一次，或在首次使用任意 kv API 时自动触发
- 重复调用幂等
- 返回 0 成功；-1 失败

### 读写接口

```c
int kv_set(sqlite3 *db, const char *namespace,
           const char *key, const char *value_name, const char *value);
```
- 写入一个值（`INSERT OR REPLACE`）
- `namespace` 不存在时自动创建对应表
- 若 `(namespace+key+value_name)` 已存在，原有 `value` 被覆盖
- 所有字符串参数不可为 `NULL`
- 返回 0 成功；-1 失败

```c
char *kv_get(sqlite3 *db, const char *namespace,
             const char *key, const char *value_name);
```
- 读取指定 `value_name` 的值
- 返回 `malloc` 分配的字符串（调用者 `free()`）；未找到或出错返回 `NULL`
- 参数不可为 `NULL`

```c
char *kv_get_all(sqlite3 *db, const char *namespace, const char *key);
```
- 读取某个 key 下所有 `(value_name, value)` 对
- 返回 JSON 数组字符串：`[{"name":"...","value":"..."}, ...]`
- 没有数据时返回 `"[]"`；失败返回 `NULL`
- 调用者 `free()` 返回值

### 删除接口

```c
int kv_delete(sqlite3 *db, const char *namespace,
              const char *key, const char *value_name);
```
- 删除某个 key 下的指定 `value_name`
- 返回 0 成功；-1 失败

```c
int kv_delete_key(sqlite3 *db, const char *namespace, const char *key);
```
- 删除某个 key 下的所有值
- 仅删除数据，不删除 namespace 表本身

```c
int kv_drop_namespace(sqlite3 *db, const char *namespace);
```
- 删除整个 namespace（删除对应数据表和元信息记录）
- **不可恢复**，谨慎使用

### 列举接口

```c
char **kv_list_namespaces(sqlite3 *db, int *count);
char **kv_list_keys(sqlite3 *db, const char *namespace, int *count);
char **kv_list_value_names(sqlite3 *db, const char *namespace,
                           const char *key, int *count);
```
- 返回 `NULL` 结尾的字符串数组
- `*count` 填入元素个数（不含 `NULL` 哨兵）
- 失败返回 `NULL`，`*count = 0`
- 调用者通过 `kv_free_str_array()` 释放

### 内存管理

```c
void kv_free_str_array(char **arr);
```
- 释放 `NULL` 结尾的字符串数组
- 遍历 `free` 每个元素后再 `free` 数组本身
- 传入 `NULL` 安全（无操作）

### 类型转换

```c
int kv_str_to_int(const char *str, int *out);
int kv_str_to_double(const char *str, double *out);
```
- 将字符串解析为 `int` / `double`
- 基于 `strtol` / `strtod` + 全串校验
- 拒绝 `NULL`、空串、非数字等情况
- 返回 0 成功；-1 失败

```c
int kv_str_array_to_int(char **strs, int **out, int *count);
int kv_str_array_to_double(char **strs, double **out, int *count);
```
- 将 `NULL` 结尾的字符串数组转换为 `int` / `double` 数组
- `*out` 指向 `malloc` 分配的数组（调用者 `free()`）
- 任一元素不能解析则返回 -1，`*out = NULL`，`*count = 0`

## 完整示例

```c
#include "kv_store.h"

sqlite3 *db;
sqlite3_open(":memory:", &db);
kv_init(db);

/* 写入 */
kv_set(db, "config", "server", "port", "8080");
kv_set(db, "config", "server", "host", "0.0.0.0");

/* 读取 */
char *v = kv_get(db, "config", "server", "port");
printf("port = %s\n", v);
free(v);

/* 列出所有值 */
char *json = kv_get_all(db, "config", "server");
printf("%s\n", json);
free(json);

/* 列举 namespace */
int count;
char **ns = kv_list_namespaces(db, &count);
for (int i = 0; i < count; i++) {
    printf("%s\n", ns[i]);
}
kv_free_str_array(ns);

/* 类型转换 */
int port_val;
kv_str_to_int(kv_get(db, "config", "server", "port"), &port_val);
printf("port as int = %d\n", port_val);

/* 删除 */
kv_delete(db, "config", "server", "port");
kv_drop_namespace(db, "config");

sqlite3_close(db);
```