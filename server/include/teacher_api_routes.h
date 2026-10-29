#ifndef AIQB_TEACHER_API_ROUTES_H
#define AIQB_TEACHER_API_ROUTES_H
#include "routes.h"
enum MHD_Result teacher_api_route(struct MHD_Connection *conn, const char *url, const char *method,
                                  const char *body, size_t length, const AppConfig *cfg);
#endif
