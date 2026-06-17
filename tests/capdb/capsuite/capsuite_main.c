/*
** capsuite runner — registers and executes fork regression tests.
*/
#include "capsuite.h"
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

int g_capsuite_failures = 0;
int g_capsuite_runs = 0;
const char *g_capsuite_filter = 0;

#define CAPSUITE_MAX_TESTS 256
static CapsuiteTest g_aTests[CAPSUITE_MAX_TESTS];
static int g_nTests = 0;

void capsuite_register_tests(CapsuiteTest *aTests, int nTests){
  int i;
  for(i=0; i<nTests && g_nTests<CAPSUITE_MAX_TESTS; i++){
    g_aTests[g_nTests++] = aTests[i];
  }
}

int capsuite_rm_rf(const char *zPath){
  struct stat st;
  if( zPath==0 || zPath[0]==0 ) return 0;
  if( stat(zPath, &st)!=0 ) return 0;
  if( S_ISDIR(st.st_mode) ){
    DIR *d = opendir(zPath);
    if( d ){
      struct dirent *e;
      char buf[1024];
      while( (e=readdir(d))!=0 ){
        if( strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0 ) continue;
        snprintf(buf, sizeof(buf), "%s/%s", zPath, e->d_name);
        capsuite_rm_rf(buf);
      }
      closedir(d);
    }
    return rmdir(zPath);
  }
  return unlink(zPath);
}

int capsuite_mkdir_p(const char *zPath){
  char buf[1024];
  size_t i, n;
  if( zPath==0 ) return -1;
  n = strlen(zPath);
  if( n>=sizeof(buf) ) return -1;
  memcpy(buf, zPath, n+1);
  for(i=1; i<n; i++){
    if( buf[i]=='/' ){
      buf[i] = 0;
      if( mkdir(buf, 0755)!=0 && errno!=EEXIST ) return -1;
      buf[i] = '/';
    }
  }
  if( mkdir(buf, 0755)!=0 && errno!=EEXIST ) return -1;
  return 0;
}

int capsuite_write_file(const char *zPath, const char *zContent){
  FILE *f = fopen(zPath, "w");
  if( f==0 ) return -1;
  if( zContent ) fputs(zContent, f);
  fclose(f);
  return 0;
}

int capsuite_spawn(const char *const *argv, pid_t *pPid){
  pid_t pid = fork();
  if( pid<0 ) return -1;
  if( pid==0 ){
    execvp(argv[0], (char*const*)argv);
    _exit(127);
  }
  *pPid = pid;
  return 0;
}

int capsuite_kill_wait(pid_t pid){
  int i, st;
  if( pid<=0 ) return 0;
  kill(pid, SIGTERM);
  for(i=0; i<50; i++){
    if( waitpid(pid, &st, WNOHANG)>0 ) return 0;
    capsuite_sleep_ms(100);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &st, 0);
  return 0;
}

void capsuite_sleep_ms(int ms){
  usleep((useconds_t)ms * 1000);
}

static int capsuite_verbose(void){
  const char *z = getenv("CAPSUITE_VERBOSE");
  return z && (z[0]=='1' || z[0]=='y' || z[0]=='Y');
}

int capsuite_run_all(void){
  int i, rc;
  for(i=0; i<g_nTests; i++){
    const char *zName = g_aTests[i].zName;
    struct timespec t0, t1;
    double elapsedMs;
    if( g_capsuite_filter && strstr(zName, g_capsuite_filter)==0 ) continue;
    g_capsuite_runs++;
    if( capsuite_verbose() ) clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "Running %s...\n", zName);
    fflush(stderr);
    rc = g_aTests[i].xRun();
    if( capsuite_verbose() ){
      clock_gettime(CLOCK_MONOTONIC, &t1);
      elapsedMs = (t1.tv_sec - t0.tv_sec) * 1000.0
                + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    if( rc ){
      g_capsuite_failures++;
      fprintf(stderr, "FAILED %s", zName);
    }else{
      fprintf(stderr, "OK %s", zName);
    }
    if( capsuite_verbose() ) fprintf(stderr, " (%.0fms)", elapsedMs);
    fprintf(stderr, "\n");
    fflush(stderr);
  }
  fprintf(stderr, "%d tests, %d failures\n", g_capsuite_runs, g_capsuite_failures);
  fflush(stderr);
  return g_capsuite_failures;
}

/* External test registration */
extern void capsuite_register_pool_basic(void);
extern void capsuite_register_pool_busy(void);
extern void capsuite_register_capdb(void);

int main(int argc, char **argv){
  int i;
  setvbuf(stderr, 0, _IONBF, 0);
  for(i=1; i<argc; i++){
    if( strcmp(argv[i],"--filter")==0 && i+1<argc ){
      g_capsuite_filter = argv[++i];
    }
  }

  capsuite_register_pool_basic();
  capsuite_register_pool_busy();
#ifdef CAPSUITE_ENABLE_NETWORK
  capsuite_register_capdb();
#endif

  return capsuite_run_all() ? 1 : 0;
}
