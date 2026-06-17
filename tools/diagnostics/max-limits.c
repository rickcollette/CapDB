/*
** Link this program against an SQLite library of unknown provenance in order
** to display the compile-time maximum values for various settings.
*/
#include "capdb.h"
#include <stdio.h>

static const struct {
  int eCode;
  char *zName;
} aLimit[] = {
  { CAPDB_LIMIT_LENGTH,                "CAPDB_MAX_LENGTH"               },
  { CAPDB_LIMIT_SQL_LENGTH,            "CAPDB_MAX_SQL_LENGTH"           },
  { CAPDB_LIMIT_COLUMN,                "CAPDB_MAX_COLUMN"               },
  { CAPDB_LIMIT_EXPR_DEPTH,            "CAPDB_MAX_EXPR_DEPTH"           },
  { CAPDB_LIMIT_COMPOUND_SELECT,       "CAPDB_MAX_COMPOUND_SELECT"      },
  { CAPDB_LIMIT_VDBE_OP,               "CAPDB_MAX_VDBE_OP"              },
  { CAPDB_LIMIT_FUNCTION_ARG,          "CAPDB_MAX_FUNCTION_ARG"         },
  { CAPDB_LIMIT_ATTACHED,              "CAPDB_MAX_ATTACHED"             },
  { CAPDB_LIMIT_LIKE_PATTERN_LENGTH,   "CAPDB_MAX_LIKE_PATTERN_LENGTH"  },
  { CAPDB_LIMIT_VARIABLE_NUMBER,       "CAPDB_MAX_VARIABLE_NUMBER"      },
  { CAPDB_LIMIT_TRIGGER_DEPTH,         "CAPDB_MAX_TRIGGER_DEPTH"        },
  { CAPDB_LIMIT_WORKER_THREADS,        "CAPDB_MAX_WORKER_THREADS"       },
};

static int maxLimit(capdb *db, int eCode){
  int iOrig = capdb_limit(db, eCode, 0x7fffffff);
  return capdb_limit(db, eCode, iOrig);
}

int main(int argc, char **argv){
  capdb *db;
  int j, rc;
  rc = capdb_open(":memory:", &db);
  if( rc==CAPDB_OK ){
    for(j=0; j<sizeof(aLimit)/sizeof(aLimit[0]); j++){
      printf("%-35s %10d\n", aLimit[j].zName, maxLimit(db, aLimit[j].eCode));
    }
    capdb_close(db);
  } 
}
