/*
** 2022-10-16
**
** The author disclaims copyright to this source code.  In place of a
** legal notice, here is a blessing:
**
** *   May you do good and not evil.
** *   May you find forgiveness for yourself and forgive others.
** *   May you share freely, never taking more than you give.
**
*************************************************************************
** This file simply outputs capdb version information in JSON form,
** intended for embedding in the capdb JS API build.
*/
#ifdef TEST_VERSION
/*3029003 3039012*/
#define CAPDB_VERSION "X.Y.Z"
#define CAPDB_VERSION_NUMBER TEST_VERSION
#define CAPDB_SOURCE_ID "dummy"
#else
#include "capdb.h"
#endif
#include <stdio.h>
#include <string.h>
static void usage(const char *zAppName){
  puts("Emits version info about the capdb it is built against.");
  printf("Usage: %s [--quote] --INFO-FLAG:\n\n", zAppName);
  puts("  --version          Emit CAPDB_VERSION (3.X.Y)");
  puts("  --version-number   Emit CAPDB_VERSION_NUMBER (30XXYYZZ)");
  puts("  --download-version Emit /download.html version number (3XXYYZZ)");
  puts("  --source-id        Emit CAPDB_SOURCE_ID");
  puts("  --json             Emit all info in JSON form");
  puts("\nThe non-JSON formats may be modified by:\n");
  puts("  --quote            Add double quotes around output.");
}

int main(int argc, char const * const * argv){
  int fJson = 0;
  int fVersion = 0;
  int fVersionNumber = 0;
  int fDlVersion = 0;
  int dlVersion = 0;
  int fSourceInfo = 0;
  int fQuote = 0;
  int nFlags = 0;
  int i;

  for( i = 1; i < argc; ++i ){
    const char * zArg = argv[i];
    while('-'==*zArg) ++zArg;
    if( 0==strcmp("version", zArg) ){
      fVersion = 1;
    }else if( 0==strcmp("version-number", zArg) ){
      fVersionNumber = 1;
    }else if( 0==strcmp("download-version", zArg) ){
      fDlVersion = 1;
    }else if( 0==strcmp("source-id", zArg) ){
      fSourceInfo = 1;
    }else if( 0==strcmp("json", zArg) ){
      fJson = 1;
    }else if( 0==strcmp("quote", zArg) ){
      fQuote = 1;
      --nFlags;
    }else{
      printf("Unhandled flag: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
    ++nFlags;
  }

  if( 0==nFlags ) fJson = 1;

  {
    const int v = CAPDB_VERSION_NUMBER;
    int ver[4] = {0,0,0,0};
    ver[0] = (v / 1000000) * 1000000;
    ver[1] = v % 1000000 / 100 * 1000;
    ver[2] = v % 100 * 100;
    dlVersion = ver[0] + ver[1] + ver[2] + ver[3];
  }
  if( fJson ){
    printf("{\"libVersion\": \"%s\", "
           "\"libVersionNumber\": %d, "
           "\"sourceId\": \"%s\","
           "\"downloadVersion\": %d,"
           "\"scm\":{ "
           "\"sha3-256\": \"%s\","
           "\"branch\": \"" CAPDB_SCM_BRANCH "\","
           "\"tags\": \"" CAPDB_SCM_TAGS "\","
           "\"datetime\": \"" CAPDB_SCM_DATETIME "\""
           "}"
           "}"/*missing newline is intentional*/,
           CAPDB_VERSION,
           CAPDB_VERSION_NUMBER,
           CAPDB_SOURCE_ID,
           dlVersion,
           CAPDB_SOURCE_ID+20);
  }else{
    if(fQuote) printf("%c", '"');
    if( fVersion ){
      printf("%s", CAPDB_VERSION);
    }else if( fVersionNumber ){
      printf("%d", CAPDB_VERSION_NUMBER);
    }else if( fSourceInfo ){
      printf("%s", CAPDB_SOURCE_ID);
    }else if( fDlVersion ){
      printf("%d", dlVersion);
    }
    if(fQuote) printf("%c", '"');
    puts("");
  }
  return 0;
}
