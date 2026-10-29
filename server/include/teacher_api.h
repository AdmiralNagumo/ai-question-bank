#ifndef AIQB_TEACHER_API_H
#define AIQB_TEACHER_API_H
#include "config.h"
#include "cJSON.h"

typedef struct {
    int teacher_id;
    int version;
    char api_key[513];
    char model[129];
    sqlite3_int64 charge_id;
} TeacherExecution;

int teacher_api_init(sqlite3 *db);
/* NULL成功，否则稳定错误code；共享main连接，无额外C数据库连接。 */
const char *teacher_api_metadata(const AppConfig *cfg, int teacher, cJSON **out);
const char *teacher_api_save(const AppConfig *cfg, int teacher, const char *key, const char *model, int enabled);
const char *teacher_api_toggle(const AppConfig *cfg, int teacher, int enabled);
const char *teacher_api_resolve(const AppConfig *cfg, int user, const char *role, int question,
                               int charge, TeacherExecution *execution, cJSON **access);
int teacher_api_refund(sqlite3 *db, sqlite3_int64 charge_id);
void teacher_api_clear(TeacherExecution *execution);
void teacher_api_clear_request(cJSON *request);
int teacher_api_error_status(const char *code);
#endif
