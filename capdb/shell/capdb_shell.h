
#if !defined(__CAPDB_SHELL_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_SHELL_H_ 1

#include "../client/capdb_client.h"
#include <stdio.h>

int shellCapdbIsUri(const char *zName);
int shellCapdbOpen(const char *zUri, capdb_conn **pp);
void shellCapdbClose(capdb_conn *p);
int shellCapdbExec(capdb_conn *p, const char *zSql, FILE *out, char **pzErrMsg);

#endif /* __CAPDB_SHELL_H_ */
