#ifndef __SQLITE_JSON_INFO_H__
#define __SQLITE_JSON_INFO_H__
#include <zlib.h>

#define GZ_CACHE_SIZE 32 * 1024

#define EOS_TRUE  1
#define EOS_FALSE 0
#define EOS_GZING 2

typedef struct {
  int ncol;
  char **colnames;
  int *coltypes;
  sqlite3_stmt *stmt;
  int initialized;
  int eos;
  int rstart;
  z_stream zs;
  char *gzbuf;
  int compress;
} sqlite_json_info_t;

void sqlite_json_info_free(void * data);

sqlite_json_info_t* sqlite_json_info_new(sqlite3_stmt* stmt, int compress);

void sqlite_json_info_set_eos(sqlite_json_info_t* sji, int eos);

void sqlite_json_info_set_rstart(sqlite_json_info_t *sji, int rstart);

int sqlite_json_info_initialized(sqlite_json_info_t *sji);

void sqlite_json_info_init(sqlite_json_info_t *sji);

#endif
