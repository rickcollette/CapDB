#!/this/is/make
#^^^^ help emacs out
#
# This is a POSIX-make-compatible makefile for building the capdb
# JNI library from "dist" zip file. It must be edited to set the
# proper top-level JDK directory and, depending on the platform, add a
# platform-specific -I directory. It should build as-is with any
# 2020s-era version of gcc or clang. It requires JDK version 8 or
# higher and that JAVA_HOME points to the top-most installation
# directory of that JDK. On Ubuntu-style systems the JDK is typically
# installed under /usr/lib/jvm/java-VERSION-PLATFORM.

default: all

JAVA_HOME = /usr/lib/jvm/java-1.8.0-openjdk-amd64
CFLAGS = \
  -fPIC \
  -Isrc \
  -I$(JAVA_HOME)/include \
  -I$(JAVA_HOME)/include/linux \
  -I$(JAVA_HOME)/include/apple \
  -I$(JAVA_HOME)/include/bsd \
  -Wall

CAPDB_OPT = \
  -DCAPDB_ENABLE_RTREE \
  -DCAPDB_ENABLE_EXPLAIN_COMMENTS \
  -DCAPDB_ENABLE_STMTVTAB \
  -DCAPDB_ENABLE_DBPAGE_VTAB \
  -DCAPDB_ENABLE_DBSTAT_VTAB \
  -DCAPDB_ENABLE_BYTECODE_VTAB \
  -DCAPDB_ENABLE_OFFSET_SQL_FUNC \
  -DCAPDB_OMIT_LOAD_EXTENSION \
  -DCAPDB_OMIT_DEPRECATED \
  -DCAPDB_OMIT_SHARED_CACHE \
  -DCAPDB_THREADSAFE=1 \
  -DCAPDB_TEMP_STORE=2 \
  -DCAPDB_USE_URI=1 \
  -DCAPDB_ENABLE_FTS5 \
  -DCAPDB_DEBUG

capdb-jni.dll = libcapdb-jni.so
$(capdb-jni.dll):
	@echo "************************************************************************"; \
	echo  "*** If this fails to build, be sure to edit this makefile            ***"; \
	echo  "*** to configure it for your system.                                 ***"; \
	echo  "************************************************************************"
	$(CC) $(CFLAGS) $(CAPDB_OPT) \
		src/capdb-jni.c -shared -o $@
	@echo "Now try running it with: make test"

test.flags = -Djava.library.path=. capdb-jni-*.jar
test: $(capdb-jni.dll)
	java -jar $(test.flags)
	java -jar $(test.flags) -t 7 -r 10 -shuffle

clean:
	-rm -f $(capdb-jni.dll)

all: $(capdb-jni.dll)
