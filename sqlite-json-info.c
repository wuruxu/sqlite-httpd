#include <stdlib.h>
#include <sqlite3.h>
#include "sqlite-json-info.h"
#include "internal-debug.h"

sqlite_json_info_t* sqlite_json_info_new(sqlite3_stmt* stmt, int compress) {
  sqlite_json_info_t *sji = calloc(1, sizeof(sqlite_json_info_t));
  int i = 0;

  if(!sji) return NULL;

  sji->compress = compress;
  sji->zs.zalloc = (alloc_func) 0;
  sji->zs.zfree = (free_func) 0;
  sji->zs.opaque = (voidpf) 0;
  if(Z_OK != deflateInit2(&sji->zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS+16, 8, Z_DEFAULT_STRATEGY)) {
    free(sji);
    return NULL;
  }

  sji->stmt = stmt;
  sji->ncol = sqlite3_column_count(stmt);
  sji->colnames = calloc(sji->ncol, sizeof(char *));
  sji->coltypes = calloc(sji->ncol, sizeof(int));
  sji->initialized = 0;
  sji->rstart = 1;
  sji->compress = compress;
  sji->eos = EOS_FALSE;

  if(NULL == sji->coltypes || NULL == sji->colnames) {
    if(sji->colnames) {
      free(sji->colnames);
    }
    free(sji);

    return NULL;
  }
  sji->gzbuf = calloc(GZ_CACHE_SIZE, sizeof(char));

  return sji;
}

void sqlite_json_info_init(sqlite_json_info_t *sji) {
  int i = 0;

  for(; i < sji->ncol; i++) {
    sji->colnames[i] = (char *)sqlite3_column_name(sji->stmt, i);
    //DD("colnames[%d] = %s, type = %d\n", i, sji->colnames[i], sqlite3_column_type(sji->stmt, i));
    sji->coltypes[i] = sqlite3_column_type(sji->stmt, i);
  }
  sji->initialized = 1;
}

int sqlite_json_info_initialized(sqlite_json_info_t *sji) {
  return sji->initialized;
}


void sqlite_json_info_set_eos(sqlite_json_info_t *sji, int eos) {
  sji->eos = eos;
}

void sqlite_json_info_set_rstart(sqlite_json_info_t *sji, int rstart) {
  sji->rstart = rstart;
}

void sqlite_json_info_free(void *data) {
  sqlite_json_info_t *sji = (sqlite_json_info_t *)data;

  if(sji->gzbuf) {
    free(sji->gzbuf);
  }

  free(sji->colnames);
  free(sji->coltypes);
  free(sji);
}
