/*
** Minimal C test harness for capdb fork (replaces Tcl testfixture).
*/
#ifndef CAPSUITE_H
#define CAPSUITE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CapsuiteTest CapsuiteTest;
struct CapsuiteTest {
  const char *zName;
  int (*xRun)(void);
};

extern int g_capsuite_failures;
extern int g_capsuite_runs;
extern const char *g_capsuite_filter;

#define CAPSUITE_LOG(...) do { \
  fprintf(stderr, __VA_ARGS__); \
  fflush(stderr); \
} while(0)

#define CAPSUITE_FAIL(msg) do { \
  CAPSUITE_LOG("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
  return 1; \
} while(0)

#define CAPSUITE_ASSERT(cond) do { \
  if(!(cond)) CAPSUITE_FAIL(#cond); \
} while(0)

#define CAPSUITE_ASSERT_EQ_int(got, want) do { \
  int _g = (int)(got); \
  int _w = (int)(want); \
  if(_g != _w){ \
    CAPSUITE_LOG("FAIL %s:%d: expected %d got %d\n", __FILE__, __LINE__, _w, _g); \
    return 1; \
  } \
} while(0)

#define CAPSUITE_ASSERT_EQ_str(got, want) do { \
  const char *_g = (got); \
  const char *_w = (want); \
  if(_g==0) _g = ""; \
  if(_w==0) _w = ""; \
  if(strcmp(_g, _w)!=0){ \
    CAPSUITE_LOG("FAIL %s:%d: expected '%s' got '%s'\n", __FILE__, __LINE__, _w, _g); \
    return 1; \
  } \
} while(0)

int capsuite_rm_rf(const char *zPath);
int capsuite_mkdir_p(const char *zPath);
int capsuite_write_file(const char *zPath, const char *zContent);
int capsuite_spawn(const char *const *argv, pid_t *pPid);
int capsuite_kill_wait(pid_t pid);
void capsuite_sleep_ms(int ms);

void capsuite_register_tests(CapsuiteTest *aTests, int nTests);
int capsuite_run_all(void);

#endif /* CAPSUITE_H */
