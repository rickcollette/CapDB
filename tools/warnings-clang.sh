#/bin/sh
#
# Run this script in a directory with a working makefile to check for 
# compiler warnings in SQLite.
#
rm -f capdb.c shell.c
make capdb.c shell.c
echo '************* FTS4 and RTREE ****************'
scan-build gcc -c -DHAVE_STDINT_H -DCAPDB_ENABLE_FTS4 -DCAPDB_ENABLE_RTREE \
      -DCAPDB_DEBUG -DCAPDB_ENABLE_STAT3 capdb.c 2>&1 | grep -v 'ANALYZE:'
echo '********** ENABLE_STAT3. THREADSAFE=0 *******'
scan-build gcc -c -I. -DCAPDB_ENABLE_STAT3 -DCAPDB_THREADSAFE=0 \
      -DCAPDB_DEBUG \
      capdb.c shell.c -ldl 2>&1 | grep -v 'ANALYZE:'
