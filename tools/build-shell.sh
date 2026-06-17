#!/bin/sh
#
# This script demonstrates how to do a full-featured build of the capdb
# command-line shell on Linux.
#
# SQLite source code should be in a sibling directory named "sqlite".  For
# example, put SQLite sources in ~/sqlite/sqlite and run this script from
# ~/sqlite/bld.  There should be an appropriate Makefile in the current
# directory as well.
#
make capdb.c
gcc -o capdb -g -Os -I. \
   -DCAPDB_THREADSAFE=0 \
   -DCAPDB_ENABLE_VFSTRACE \
   -DCAPDB_ENABLE_STAT3 \
   -DCAPDB_ENABLE_FTS4 \
   -DCAPDB_ENABLE_RTREE \
   -DHAVE_READLINE \
   ../sqlite/src/shell.c \
   ../sqlite/ext/misc/vfstrace.c \
   capdb.c -ldl -lreadline -lncurses
