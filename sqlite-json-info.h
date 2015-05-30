#ifndef __SQLITE_JSON_INFO_H__
#define __SQLITE_JSON_INFO_H__
typedef struct {
  int ncol;
  char **colnames;
  int *coltypes;
  sqlite3_stmt *stmt;
  int initialized;
  int eos;
  int rstart;
  int compress;
} sqlite_json_info_t;

void sqlite_json_info_free(void * data);

sqlite_json_info_t* sqlite_json_info_new(sqlite3_stmt* stmt);

void sqlite_json_info_set_eos(sqlite_json_info_t* sji, int eos);

int sqlite_json_info_initialized(sqlite_json_info_t *sji);

void sqlite_json_info_init(sqlite_json_info_t *sji);

#endif
