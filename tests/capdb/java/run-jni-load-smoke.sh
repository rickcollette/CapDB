#!/bin/sh
# Run the JNI load smoke under a JVM.
#
# An ASan/UBSan build links libcapdb_jni.so against the sanitizer runtime.
# The JVM is not, so dlopen aborts with "ASan runtime does not come first in
# initial library list" unless that runtime is preloaded. LeakSanitizer also
# reports the JVM's own allocations; those are not a CapDB failure.
set -eu

if [ -n "${CAPDB_JNI_SANITIZER_PRELOAD:-}" ]; then
  if [ -n "${LD_PRELOAD:-}" ]; then
    LD_PRELOAD="${CAPDB_JNI_SANITIZER_PRELOAD}:${LD_PRELOAD}"
  else
    LD_PRELOAD="${CAPDB_JNI_SANITIZER_PRELOAD}"
  fi
  export LD_PRELOAD
  case ":${ASAN_OPTIONS:-}:" in
    *:detect_leaks=*) ;;
    *)
      if [ -n "${ASAN_OPTIONS:-}" ]; then
        ASAN_OPTIONS="${ASAN_OPTIONS}:detect_leaks=0"
      else
        ASAN_OPTIONS="detect_leaks=0"
      fi
      export ASAN_OPTIONS
      ;;
  esac
fi

exec "$@"
