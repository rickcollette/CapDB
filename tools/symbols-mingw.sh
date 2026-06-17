#!/bin/sh
#
# Run this script in a directory that contains a valid SQLite makefile in
# order to verify that unintentionally exported symbols.
#
make capdb.c

echo '****** Exported symbols from a build including RTREE && FTS4 ******'
gcc -c -DCAPDB_ENABLE_FTS3 -DCAPDB_ENABLE_RTREE \
  -DCAPDB_ENABLE_MEMORY_MANAGEMENT -DCAPDB_ENABLE_STAT3 \
  -DCAPDB_ENABLE_MEMSYS5 -DCAPDB_ENABLE_UNLOCK_NOTIFY \
  -DCAPDB_ENABLE_COLUMN_METADATA -DCAPDB_ENABLE_ATOMIC_WRITE \
  capdb.c
nm capdb.o | grep " [TD] "

echo '****** Surplus symbols from a build including RTREE & FTS4 ******'
nm capdb.o | grep " [TD] " | grep -v " .*capdb_"

echo '****** Dependencies of the core. No extensions. No OS interface *******'
gcc -c -DCAPDB_ENABLE_MEMORY_MANAGEMENT -DCAPDB_ENABLE_STAT3 \
  -DCAPDB_ENABLE_MEMSYS5 -DCAPDB_ENABLE_UNLOCK_NOTIFY \
  -DCAPDB_ENABLE_COLUMN_METADATA -DCAPDB_ENABLE_ATOMIC_WRITE \
  -DCAPDB_OS_OTHER -DCAPDB_THREADSAFE=0 \
  capdb.c
nm capdb.o | grep " U "

echo '****** Dependencies including RTREE & FTS4 *******'
gcc -c -DCAPDB_ENABLE_FTS3 -DCAPDB_ENABLE_RTREE \
  -DCAPDB_ENABLE_MEMORY_MANAGEMENT -DCAPDB_ENABLE_STAT3 \
  -DCAPDB_ENABLE_MEMSYS5 -DCAPDB_ENABLE_UNLOCK_NOTIFY \
  -DCAPDB_ENABLE_COLUMN_METADATA -DCAPDB_ENABLE_ATOMIC_WRITE \
  capdb.c
nm capdb.o | grep " U "
