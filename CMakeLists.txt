cmake_minimum_required(VERSION 3.16)
project(capdb VERSION 3.6.1 LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Feature options
# ---------------------------------------------------------------------------
option(CAPDB_ENABLE_POOL "Enable connection pool extension" ON)
option(CAPDB_ENABLE_NETWORK "Enable capdb TLS network layer (implies pool)" ON)
option(CAPDB_ENABLE_STORE "Enable CapDB volume store (HA foundation)" ON)
option(CAPDB_ENABLE_REPLICATION "Enable WAL replication (implies store)" ON)
option(CAPDB_ENABLE_FTS5 "Enable FTS5" ON)
option(CAPDB_ENABLE_SESSION "Enable session and changeset APIs" ON)
option(CAPDB_ENABLE_COLUMN_METADATA "Enable database, table, and origin column metadata APIs" ON)
option(CAPDB_ENABLE_MATH_FUNCTIONS "Enable math SQL functions" ON)
option(CAPDB_ENABLE_PERCENTILE "Enable percentile extension" ON)
option(CAPDB_HAVE_ZLIB "Link zlib" ON)
option(CAPDB_BUILD_TESTS "Build capsuite, capdbtest, threadtest_pool" ON)
option(CAPDB_WARNINGS "Enable -Wall -Wextra on CapDB-owned targets" ON)
option(CAPDB_WERROR "Treat warnings as errors" OFF)
option(CAPDB_ENABLE_JNI_NETWORK "Build capdb_jni remote JNI bindings" ON)

if(CAPDB_ENABLE_NETWORK)
  set(CAPDB_ENABLE_POOL ON CACHE BOOL "" FORCE)
endif()
if(CAPDB_ENABLE_REPLICATION)
  set(CAPDB_ENABLE_STORE ON CACHE BOOL "" FORCE)
endif()
if(CAPDB_ENABLE_STORE AND CAPDB_ENABLE_NETWORK)
  set(CAPDB_ENABLE_POOL ON CACHE BOOL "" FORCE)
endif()

if(CAPDB_HAVE_ZLIB)
  find_package(ZLIB)
endif()
if(CAPDB_ENABLE_NETWORK)
  find_package(OpenSSL REQUIRED)
endif()

set(CAPDB_ROOT "${CMAKE_SOURCE_DIR}")
set(CAPDB_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(CAPDB_STAGING_DIR "${CAPDB_GEN_DIR}/staging")
set(CAPDB_PY "${Python3_EXECUTABLE}" "${CAPDB_ROOT}/tools/py")
file(MAKE_DIRECTORY "${CAPDB_GEN_DIR}")

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${CMAKE_SOURCE_DIR}/compile_commands.json"
    ERROR_QUIET
  )
endif()

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
set(CAPDB_COMPILE_DEFS
  CAPDB_THREADSAFE=1
  _HAVE_CAPDB_CONFIG_H
  BUILD_capdb
)

if(CAPDB_ENABLE_POOL)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_POOL)
endif()
if(CAPDB_ENABLE_NETWORK)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_NETWORK)
endif()
if(CAPDB_ENABLE_STORE)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_STORE)
endif()
if(CAPDB_ENABLE_REPLICATION)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_REPLICATION)
endif()
if(CAPDB_ENABLE_SESSION)
  # The session implementation records row changes through the pre-update
  # hook; enabling only its public declarations produces an unusable ABI.
  list(APPEND CAPDB_COMPILE_DEFS
    CAPDB_ENABLE_SESSION
    CAPDB_ENABLE_PREUPDATE_HOOK)
endif()
if(CAPDB_ENABLE_COLUMN_METADATA)
  # sqlite3_capdb.h exposes the SQLite column-metadata compatibility names.
  # Keep the matching production symbols in every CapDB library target so a
  # consumer cannot compile successfully and then fail at final link/runtime.
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_COLUMN_METADATA)
endif()

if(CAPDB_WARNINGS)
  set(CAPDB_WARNING_FLAGS -Wall -Wextra -Wno-unused-parameter)
  if(CAPDB_WERROR)
    list(APPEND CAPDB_WARNING_FLAGS -Werror)
  endif()
endif()

function(capdb_target_warnings target)
  if(CAPDB_WARNING_FLAGS)
    target_compile_options(${target} PRIVATE ${CAPDB_WARNING_FLAGS})
  endif()
endfunction()
if(CAPDB_ENABLE_FTS5)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_FTS5)
endif()
if(CAPDB_ENABLE_MATH_FUNCTIONS)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_MATH_FUNCTIONS)
endif()
if(CAPDB_ENABLE_PERCENTILE)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_ENABLE_PERCENTILE)
endif()
if(ZLIB_FOUND)
  list(APPEND CAPDB_COMPILE_DEFS CAPDB_HAVE_ZLIB=1)
endif()

# ---------------------------------------------------------------------------
# Host tools: lemon, mkkeywordhash
# ---------------------------------------------------------------------------
add_executable(capdb_lemon tools/lemon.c)
set_target_properties(capdb_lemon PROPERTIES OUTPUT_NAME lemon)

add_executable(capdb_mkkeywordhash tools/mkkeywordhash.c)
set_target_properties(capdb_mkkeywordhash PROPERTIES OUTPUT_NAME mkkeywordhash)

# ---------------------------------------------------------------------------
# Generated sources
# ---------------------------------------------------------------------------
set(CAPDB_H "${CAPDB_GEN_DIR}/capdb.h")
set(CAPDB_C "${CAPDB_GEN_DIR}/capdb.c")
set(SHELL_C "${CAPDB_GEN_DIR}/shell.c")
set(PARSE_C "${CAPDB_GEN_DIR}/parse.c")
set(PARSE_H "${CAPDB_GEN_DIR}/parse.h")
set(OPCODES_H "${CAPDB_GEN_DIR}/opcodes.h")
set(OPCODES_C "${CAPDB_GEN_DIR}/opcodes.c")
set(KEYWORDHASH_H "${CAPDB_GEN_DIR}/keywordhash.h")
set(CTIME_C "${CAPDB_GEN_DIR}/ctime.c")
set(FTS5PARSE_C "${CAPDB_GEN_DIR}/fts5parse.c")
set(FTS5PARSE_H "${CAPDB_GEN_DIR}/fts5parse.h")
set(FTS5_C "${CAPDB_GEN_DIR}/fts5.c")

add_custom_command(
  OUTPUT "${CAPDB_H}"
  COMMAND ${CAPDB_PY}/mkcapdbh.py "${CAPDB_ROOT}" -o "${CAPDB_H}"
  DEPENDS
    tools/py/mkcapdbh.py
    core/capdb.h.in
    VERSION
    manifest
    manifest.uuid
  COMMENT "Generating capdb.h"
  VERBATIM
)

add_custom_command(
  OUTPUT "${PARSE_C}" "${PARSE_H}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CAPDB_ROOT}/tools/lempar.c" "${CAPDB_GEN_DIR}/lempar.c"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CAPDB_ROOT}/core/parse.y" "${CAPDB_GEN_DIR}/parse.y"
  COMMAND $<TARGET_FILE:capdb_lemon> parse.y
  WORKING_DIRECTORY "${CAPDB_GEN_DIR}"
  DEPENDS capdb_lemon core/parse.y "${CAPDB_ROOT}/tools/lempar.c"
  COMMENT "Generating parse.c and parse.h"
  VERBATIM
)

add_custom_command(
  OUTPUT "${KEYWORDHASH_H}"
  COMMAND $<TARGET_FILE:capdb_mkkeywordhash> > "${KEYWORDHASH_H}"
  DEPENDS capdb_mkkeywordhash tools/mkkeywordhash.c
  COMMENT "Generating keywordhash.h"
  VERBATIM
)

set(OPCODE_INPUT "${CAPDB_GEN_DIR}/mkopcode.in")
add_custom_command(
  OUTPUT "${OPCODE_INPUT}"
  COMMAND ${CMAKE_COMMAND} -E cat "${PARSE_H}" "${CAPDB_ROOT}/core/vdbe.c" > "${OPCODE_INPUT}"
  DEPENDS "${PARSE_H}" core/vdbe.c
  COMMENT "Preparing mkopcode input"
  VERBATIM
)

add_custom_command(
  OUTPUT "${OPCODES_H}"
  COMMAND ${CAPDB_PY}/mkopcode.py < "${OPCODE_INPUT}" > "${OPCODES_H}"
  DEPENDS tools/py/mkopcode.py "${OPCODE_INPUT}"
  COMMENT "Generating opcodes.h"
  VERBATIM
)

add_custom_command(
  OUTPUT "${OPCODES_C}"
  COMMAND ${CAPDB_PY}/mkopcodec.py "${OPCODES_H}" > "${OPCODES_C}"
  DEPENDS tools/py/mkopcodec.py "${OPCODES_H}"
  COMMENT "Generating opcodes.c"
  VERBATIM
)

add_custom_command(
  OUTPUT "${CTIME_C}"
  COMMAND ${CAPDB_PY}/mkctimec.py "${CTIME_C}"
  WORKING_DIRECTORY "${CAPDB_ROOT}"
  DEPENDS tools/py/mkctimec.py
  COMMENT "Generating ctime.c"
  VERBATIM
)

if(CAPDB_ENABLE_FTS5)
  add_custom_command(
    OUTPUT "${FTS5PARSE_C}" "${FTS5PARSE_H}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CAPDB_ROOT}/tools/lempar.c" "${CAPDB_GEN_DIR}/lempar.c"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CAPDB_ROOT}/extensions/fts5/fts5parse.y" "${CAPDB_GEN_DIR}/fts5parse.y"
    COMMAND $<TARGET_FILE:capdb_lemon> fts5parse.y
    WORKING_DIRECTORY "${CAPDB_GEN_DIR}"
    DEPENDS capdb_lemon extensions/fts5/fts5parse.y "${CAPDB_ROOT}/tools/lempar.c"
    COMMENT "Generating fts5parse.c and fts5parse.h"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${FTS5_C}"
    COMMAND ${CAPDB_PY}/mkfts5c.py "${FTS5_C}" --gen-dir "${CAPDB_GEN_DIR}"
    DEPENDS
      tools/py/mkfts5c.py
      "${FTS5PARSE_C}"
      "${FTS5PARSE_H}"
      extensions/fts5/fts5.h
      extensions/fts5/fts5Int.h
    COMMENT "Generating fts5.c"
    VERBATIM
  )
endif()

set(STAGING_STAMP "${CAPDB_STAGING_DIR}/.staged")
add_custom_command(
  OUTPUT "${STAGING_STAMP}"
  COMMAND ${CAPDB_PY}/stage_sources.py
    --repo-root "${CAPDB_ROOT}"
    --gen-dir "${CAPDB_GEN_DIR}"
    --staging-dir "${CAPDB_STAGING_DIR}"
  DEPENDS
    tools/py/stage_sources.py
    tools/py/staging_manifest.json
    tools/py/vdbe_compress.py
    "${CAPDB_H}"
    "${PARSE_C}"
    "${PARSE_H}"
    "${OPCODES_C}"
    "${OPCODES_H}"
    "${KEYWORDHASH_H}"
    "${CTIME_C}"
    $<$<BOOL:${CAPDB_ENABLE_FTS5}>:${FTS5_C}>
  COMMENT "Staging amalgamation sources"
  VERBATIM
)

add_custom_command(
  OUTPUT "${CAPDB_C}"
  COMMAND ${CAPDB_PY}/mkcapdb.py --srcdir "${CAPDB_STAGING_DIR}" --linemacros=0 -o "${CAPDB_C}"
  WORKING_DIRECTORY "${CAPDB_GEN_DIR}"
  DEPENDS tools/py/mkcapdb.py "${STAGING_STAMP}"
  COMMENT "Generating capdb.c amalgamation"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHELL_C}"
  COMMAND ${CAPDB_PY}/mkshellc.py "${SHELL_C}"
  DEPENDS
    tools/py/mkshellc.py
    core/shell.c.in
    extensions/intck/capdbintck.h
    extensions/intck/capdbintck.c
  COMMENT "Generating shell.c"
  VERBATIM
)

add_custom_target(capdb_codegen DEPENDS "${CAPDB_C}" "${SHELL_C}" "${CAPDB_H}")

# ---------------------------------------------------------------------------
# libcapdb (amalgamation)
# ---------------------------------------------------------------------------
add_library(capdb STATIC "${CAPDB_C}")
set_target_properties(capdb PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(capdb capdb_codegen)
target_include_directories(capdb PUBLIC "${CAPDB_GEN_DIR}")
if(CAPDB_ENABLE_NETWORK)
  target_include_directories(capdb PRIVATE
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_ROOT}/capdb/proto"
    "${CAPDB_ROOT}/capdb/tls"
    "${CAPDB_ROOT}/capdb/client"
    "${CAPDB_ROOT}/capdb/server"
    "${CAPDB_ROOT}/capdb/shell"
    "${CAPDB_ROOT}/capdb/vfs"
  )
endif()
if(CAPDB_ENABLE_POOL)
  target_include_directories(capdb PRIVATE "${CAPDB_ROOT}/capdb/pool")
endif()
target_include_directories(capdb PRIVATE
  "${CAPDB_ROOT}/core"
  "${CAPDB_ROOT}/extensions/rtree"
  "${CAPDB_ROOT}/extensions/icu"
  "${CAPDB_ROOT}/extensions/fts3"
  "${CAPDB_ROOT}/extensions/session"
  "${CAPDB_ROOT}/extensions/misc"
)
target_compile_definitions(capdb PRIVATE ${CAPDB_COMPILE_DEFS})
target_link_libraries(capdb PUBLIC Threads::Threads)
if(ZLIB_FOUND)
  target_link_libraries(capdb PUBLIC ZLIB::ZLIB)
endif()
if(CAPDB_ENABLE_NETWORK)
  target_link_libraries(capdb PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(capdb PUBLIC m dl)
endif()

add_library(capdb_shared SHARED "${CAPDB_C}")
set_target_properties(capdb_shared PROPERTIES
  OUTPUT_NAME capdb
  POSITION_INDEPENDENT_CODE ON
)
add_dependencies(capdb_shared capdb_codegen)
target_include_directories(capdb_shared PUBLIC "${CAPDB_GEN_DIR}")
if(CAPDB_ENABLE_NETWORK)
  target_include_directories(capdb_shared PRIVATE
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_ROOT}/capdb/proto"
    "${CAPDB_ROOT}/capdb/tls"
    "${CAPDB_ROOT}/capdb/client"
    "${CAPDB_ROOT}/capdb/server"
    "${CAPDB_ROOT}/capdb/shell"
    "${CAPDB_ROOT}/capdb/vfs"
  )
endif()
if(CAPDB_ENABLE_POOL)
  target_include_directories(capdb_shared PRIVATE "${CAPDB_ROOT}/capdb/pool")
endif()
target_include_directories(capdb_shared PRIVATE
  "${CAPDB_ROOT}/core"
  "${CAPDB_ROOT}/extensions/rtree"
  "${CAPDB_ROOT}/extensions/icu"
  "${CAPDB_ROOT}/extensions/fts3"
  "${CAPDB_ROOT}/extensions/session"
  "${CAPDB_ROOT}/extensions/misc"
)
target_compile_definitions(capdb_shared PRIVATE ${CAPDB_COMPILE_DEFS})
target_link_libraries(capdb_shared PUBLIC Threads::Threads)
if(ZLIB_FOUND)
  target_link_libraries(capdb_shared PUBLIC ZLIB::ZLIB)
endif()
if(CAPDB_ENABLE_NETWORK)
  target_link_libraries(capdb_shared PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(capdb_shared PUBLIC m dl)
endif()

# ---------------------------------------------------------------------------
# CLI shell
# ---------------------------------------------------------------------------
add_executable(capdb_cli "${SHELL_C}")
add_dependencies(capdb_cli capdb_codegen)
target_compile_definitions(capdb_cli PRIVATE ${CAPDB_COMPILE_DEFS})
target_include_directories(capdb_cli PRIVATE
  "${CAPDB_GEN_DIR}"
  "${CAPDB_ROOT}/core"
  "${CAPDB_ROOT}/capdb/shell"
  "${CAPDB_ROOT}/extensions/expert"
  "${CAPDB_ROOT}/extensions/intck"
  "${CAPDB_ROOT}/extensions/recover"
  "${CAPDB_ROOT}/extensions/misc"
)
target_link_libraries(capdb_cli PRIVATE capdb)
if(UNIX)
  find_library(READLINE_LIB readline)
  find_library(NCURSES_LIB ncurses)
  if(READLINE_LIB)
    target_link_libraries(capdb_cli PRIVATE ${READLINE_LIB})
    if(NCURSES_LIB)
      target_link_libraries(capdb_cli PRIVATE ${NCURSES_LIB})
    endif()
  endif()
endif()
set_target_properties(capdb_cli PROPERTIES OUTPUT_NAME capdb)

# ---------------------------------------------------------------------------
# Volume store + cluster (HA foundation)
# ---------------------------------------------------------------------------
if(CAPDB_ENABLE_STORE)
  set(CAPDB_STORE_SRCS
    capdb/store/capdb_store.c
    capdb/store/capdb_store_vfs.c
    capdb/cluster/capdb_cluster.c
  )
  if(CAPDB_ENABLE_REPLICATION)
    list(APPEND CAPDB_STORE_SRCS
      capdb/replication/capdb_rep_io.c
      capdb/replication/capdb_rep_primary.c
      capdb/replication/capdb_rep_replica.c
      capdb/replication/capdb_rep_recovery.c
    )
  endif()
  add_library(capdb_store STATIC ${CAPDB_STORE_SRCS})
  set_target_properties(capdb_store PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(capdb_store PUBLIC
    "${CAPDB_ROOT}"
    "${CAPDB_GEN_DIR}"
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_ROOT}/capdb/store"
    "${CAPDB_ROOT}/capdb/cluster"
    "${CAPDB_ROOT}/capdb/replication"
  )
  target_compile_definitions(capdb_store PRIVATE ${CAPDB_COMPILE_DEFS})
  capdb_target_warnings(capdb_store)
  target_link_libraries(capdb_store PRIVATE Threads::Threads)
  if(CAPDB_ENABLE_REPLICATION AND CAPDB_ENABLE_NETWORK)
    target_link_libraries(capdb_store PRIVATE capdb OpenSSL::SSL OpenSSL::Crypto)
    target_include_directories(capdb_store PRIVATE
      "${CAPDB_ROOT}/capdb/proto"
      "${CAPDB_ROOT}/capdb/tls"
    )
  elseif(CAPDB_ENABLE_NETWORK)
    target_link_libraries(capdb_store PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  endif()
  target_link_libraries(capdb_shared PUBLIC capdb_store)

  add_executable(capdb-ctl tools/capdb-ctl.c)
  target_include_directories(capdb-ctl PRIVATE
    "${CAPDB_ROOT}"
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_GEN_DIR}"
  )
  target_compile_definitions(capdb-ctl PRIVATE ${CAPDB_COMPILE_DEFS})
  capdb_target_warnings(capdb-ctl)
  target_link_libraries(capdb-ctl PRIVATE
    -Wl,--start-group capdb capdb_store -Wl,--end-group
    Threads::Threads
  )
  if(CAPDB_ENABLE_NETWORK)
    target_link_libraries(capdb-ctl PRIVATE capdb_client)
  endif()
  target_link_libraries(capdb_cli PRIVATE capdb_store)
endif()

# ---------------------------------------------------------------------------
# Network client + server
# ---------------------------------------------------------------------------
if(CAPDB_ENABLE_NETWORK)
  set(CAPDB_CLIENT_SRCS
    capdb/proto/capdb_proto.c
    capdb/capdb_io.c
    capdb/tls/capdb_tls.c
    capdb/client/capdb_client.c
    capdb/vfs/capdb_vfs.c
  )
  add_library(capdb_client STATIC ${CAPDB_CLIENT_SRCS})
  set_target_properties(capdb_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(capdb_client PUBLIC
    "${CAPDB_ROOT}"
    "${CAPDB_GEN_DIR}"
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_ROOT}/capdb/proto"
    "${CAPDB_ROOT}/capdb/tls"
    "${CAPDB_ROOT}/capdb/client"
    "${CAPDB_ROOT}/capdb/vfs"
  )
  target_compile_definitions(capdb_client PRIVATE ${CAPDB_COMPILE_DEFS})
  capdb_target_warnings(capdb_client)
  target_link_libraries(capdb_client PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

  if(CAPDB_ENABLE_JNI_NETWORK)
    find_package(JNI QUIET)
    find_package(Java COMPONENTS Development QUIET)
    if(NOT JNI_FOUND AND EXISTS "/usr/lib/jvm/default-java/include/jni.h")
      set(JNI_INCLUDE_DIRS
        "/usr/lib/jvm/default-java/include"
        "/usr/lib/jvm/default-java/include/linux")
      set(JNI_FOUND TRUE)
    endif()
    if(JNI_FOUND)
      add_library(capdb_jni SHARED capdb/jni/capdb_jni.c)
      set_target_properties(capdb_jni PROPERTIES OUTPUT_NAME capdb_jni)
      target_include_directories(capdb_jni PRIVATE
        "${CAPDB_ROOT}/capdb/client"
        "${CAPDB_GEN_DIR}"
        "${CAPDB_ROOT}"
        ${JNI_INCLUDE_DIRS}
      )
      target_compile_definitions(capdb_jni PRIVATE ${CAPDB_COMPILE_DEFS})
      capdb_target_warnings(capdb_jni)
      target_link_libraries(capdb_jni PUBLIC capdb_client)
      if(Java_JAVAC_EXECUTABLE)
        add_test(NAME java_jni_wrapper_compile
          COMMAND ${Java_JAVAC_EXECUTABLE}
            -d "${CMAKE_CURRENT_BINARY_DIR}/java-classes"
            "${CMAKE_CURRENT_SOURCE_DIR}/bindings/java/org/capdb/jni/wrapper1/Capdb.java")
        add_test(NAME java_jni_runtime_load
          COMMAND ${CMAKE_COMMAND} -E env
            LD_LIBRARY_PATH=$<TARGET_FILE_DIR:capdb_jni>:$ENV{LD_LIBRARY_PATH}
            ${Java_JAVAC_EXECUTABLE}
              -d "${CMAKE_CURRENT_BINARY_DIR}/java-classes"
              "${CMAKE_CURRENT_SOURCE_DIR}/bindings/java/org/capdb/jni/wrapper1/Capdb.java"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/capdb/java/JniLoadSmoke.java")
        add_test(NAME java_jni_runtime_load_run
          COMMAND ${CMAKE_COMMAND} -E env
            LD_LIBRARY_PATH=$<TARGET_FILE_DIR:capdb_jni>:$ENV{LD_LIBRARY_PATH}
            ${Java_JAVA_EXECUTABLE}
              -cp "${CMAKE_CURRENT_BINARY_DIR}/java-classes"
              org.capdb.jni.wrapper1.JniLoadSmoke)
        set_tests_properties(java_jni_runtime_load_run PROPERTIES
          DEPENDS java_jni_runtime_load)
      endif()
    else()
      message(STATUS "JNI headers not found; capdb_jni target skipped")
    endif()
  endif()

  add_executable(capdb-server tools/capdb-server.c)
  target_include_directories(capdb-server PRIVATE
    "${CAPDB_ROOT}"
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_ROOT}/capdb/server"
    "${CAPDB_ROOT}/capdb/pool"
    "${CAPDB_GEN_DIR}"
  )
  target_compile_definitions(capdb-server PRIVATE ${CAPDB_COMPILE_DEFS})
  capdb_target_warnings(capdb-server)
  target_link_libraries(capdb-server PRIVATE capdb OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
  if(CAPDB_ENABLE_STORE)
    target_link_libraries(capdb-server PRIVATE capdb_store)
  endif()

  # IDE/clangd: capdb_server.c is amalgamated into libcapdb; this object target
  # exists only to populate compile_commands.json for the server sources.
  add_library(capdb_server_ide OBJECT EXCLUDE_FROM_ALL capdb/server/capdb_server.c)
  target_include_directories(capdb_server_ide PRIVATE
    "${CAPDB_ROOT}"
    "${CAPDB_GEN_DIR}"
    "${CAPDB_ROOT}/capdb"
    "${CAPDB_ROOT}/capdb/server"
    "${CAPDB_ROOT}/capdb/pool"
    "${CAPDB_ROOT}/capdb/proto"
    "${CAPDB_ROOT}/capdb/tls"
  )
  if(CAPDB_ENABLE_STORE)
    target_include_directories(capdb_server_ide PRIVATE
      "${CAPDB_ROOT}/capdb/store"
      "${CAPDB_ROOT}/capdb/cluster"
      "${CAPDB_ROOT}/capdb/replication"
    )
  endif()
  target_compile_definitions(capdb_server_ide PRIVATE ${CAPDB_COMPILE_DEFS})
  capdb_target_warnings(capdb_server_ide)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CAPDB_BUILD_TESTS)
  add_executable(sqlite_compat_compile tests/capdb/sqlite_compat_compile.c)
  target_include_directories(sqlite_compat_compile PRIVATE
    "${CAPDB_GEN_DIR}"
    "${CAPDB_ROOT}/compat")
  target_link_libraries(sqlite_compat_compile PRIVATE capdb_shared)
  add_test(NAME sqlite_compat_compile COMMAND sqlite_compat_compile)

  if(CAPDB_ENABLE_POOL)
    add_executable(threadtest_pool tests/capdb/threadtest_pool.c)
    target_compile_definitions(threadtest_pool PRIVATE ${CAPDB_COMPILE_DEFS})
    target_include_directories(threadtest_pool PRIVATE
      "${CAPDB_GEN_DIR}"
      "${CAPDB_ROOT}/capdb/pool"
    )
    target_link_libraries(threadtest_pool PRIVATE capdb Threads::Threads)
    if(CAPDB_ENABLE_STORE)
      target_link_libraries(threadtest_pool PRIVATE capdb_store)
    endif()
    capdb_target_warnings(threadtest_pool)
    add_test(NAME threadtest_pool COMMAND threadtest_pool)
  endif()

  if(CAPDB_ENABLE_NETWORK)
    add_executable(capdbtest tests/capdb/capdbtest.c)
    target_compile_definitions(capdbtest PRIVATE ${CAPDB_COMPILE_DEFS})
    target_include_directories(capdbtest PRIVATE
      "${CAPDB_GEN_DIR}"
      "${CAPDB_ROOT}/capdb/client"
    )
    target_link_libraries(capdbtest PRIVATE capdb_client OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    capdb_target_warnings(capdbtest)
    add_executable(capdb_nettest tests/capdb/capdb_nettest.c)
    target_compile_definitions(capdb_nettest PRIVATE ${CAPDB_COMPILE_DEFS})
    target_include_directories(capdb_nettest PRIVATE
      "${CAPDB_GEN_DIR}"
      "${CAPDB_ROOT}/capdb/client"
    )
    target_link_libraries(capdb_nettest PRIVATE capdb_client OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    capdb_target_warnings(capdb_nettest)
    add_dependencies(capdbtest capdb-server capdb_client)
    add_dependencies(capdb_nettest capdb-server capdb_client)
    if(TARGET capdb_jni)
      add_dependencies(capdb_nettest capdb_jni)
    endif()
  endif()

  if(CAPDB_ENABLE_STORE)
    add_executable(store_test tests/capdb/store_test.c)
    target_compile_definitions(store_test PRIVATE ${CAPDB_COMPILE_DEFS})
    target_include_directories(store_test PRIVATE
      "${CAPDB_GEN_DIR}"
      "${CAPDB_ROOT}/capdb/store"
    )
    target_link_libraries(store_test PRIVATE
      -Wl,--start-group capdb capdb_store -Wl,--end-group
      Threads::Threads)
    capdb_target_warnings(store_test)
    add_test(NAME store_test COMMAND store_test)
  endif()

  set(CAPSUITE_SRCS
    tests/capdb/capsuite/capsuite_main.c
    tests/capdb/capsuite/pool_basic.c
    tests/capdb/capsuite/pool_busy.c
  )
  if(CAPDB_ENABLE_NETWORK)
    list(APPEND CAPSUITE_SRCS
      tests/capdb/capsuite/capdb_harness.c
      tests/capdb/capsuite/capdb_loopback.c
    )
  endif()

  add_executable(capsuite ${CAPSUITE_SRCS})
  target_include_directories(capsuite PRIVATE
    "${CAPDB_ROOT}/tests/capdb/capsuite"
    "${CAPDB_GEN_DIR}"
    "${CAPDB_ROOT}/capdb/pool"
  )
  if(CAPDB_ENABLE_NETWORK)
    target_include_directories(capsuite PRIVATE
      "${CAPDB_ROOT}/capdb/client"
    )
  endif()
  if(CAPDB_ENABLE_NETWORK)
    target_compile_definitions(capsuite PRIVATE
      CAPSUITE_ENABLE_NETWORK
      CAPSUITE_CERT_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/certs"
    )
  endif()
  target_compile_definitions(capsuite PRIVATE ${CAPDB_COMPILE_DEFS})
  target_link_libraries(capsuite PRIVATE capdb Threads::Threads)
  if(CAPDB_ENABLE_STORE)
    target_link_libraries(capsuite PRIVATE capdb_store)
  endif()
  if(CAPDB_ENABLE_NETWORK)
    target_link_libraries(capsuite PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  endif()
  capdb_target_warnings(capsuite)
  if(CAPDB_ENABLE_NETWORK)
    add_dependencies(capsuite capdb-server capdbtest capdb_nettest)
  endif()

  add_test(NAME capsuite COMMAND capsuite --filter pool)
  if(CAPDB_ENABLE_FTS5 AND CAPDB_ENABLE_MATH_FUNCTIONS AND CAPDB_ENABLE_PERCENTILE)
    add_test(NAME extension_smoke
      COMMAND $<TARGET_FILE:capdb_cli> -batch :memory: ".read ${CMAKE_CURRENT_SOURCE_DIR}/tests/capdb/extension_smoke.sql")
  endif()
  if(CAPDB_ENABLE_NETWORK)
    set(_capdb_test_env
      ${CMAKE_COMMAND} -E env
      CAPSUITE_SERVER=$<TARGET_FILE:capdb-server>
      CAPSUITE_CLIENT_TEST=$<TARGET_FILE:capdbtest>
      CAPSUITE_NET_TEST=$<TARGET_FILE:capdb_nettest>
      CAPSUITE_VERBOSE=1
    )
    add_test(NAME capdbtest_smoke
      COMMAND ${_capdb_test_env} $<TARGET_FILE:capsuite> --filter capdb-1-loopback)
    set_tests_properties(capdbtest_smoke PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    set(_capdb_net_filters
      capdb-1-loopback
      capdb-2-auth-fail
      capdb-3-prepare-step
      capdb-4-open-switch
      capdb-5-open-in-txn
      capdb-6-vfs-read
      capdb-7-attach-denied
      capdb-8-stmt-before-exec
      capdb-9-prepare-write-pin
      capdb-10-column-types
      capdb-11-vfs-lock
      capdb-12-changes-rowid
      capdb-13-shutdown-idle
      capdb-14-session-reuse
      capdb-15-vfs-exclude
      capdb-16-vfs-delete
      capdb-17-tls-handshake
      capdb-18-vfs-write
      capdb-19-exec-blob-callback
      capdb-20-commit-open-stmt
      capdb-21-vfs-separate-path
      capdb-22-password-auth
      capdb-23-capdbvfs-sql
      capdb-24-volume-sql
    )
    if(CAPDB_ENABLE_REPLICATION)
      list(APPEND _capdb_net_filters capdb-25-rep-async)
    endif()
    if(CAPDB_ENABLE_STORE AND CAPDB_ENABLE_REPLICATION)
      list(APPEND _capdb_net_filters capdb-26-volume-multi-write capdb-27-rep-sync capdb-28-read-preference)
    endif()
    foreach(_f IN LISTS _capdb_net_filters)
      string(REPLACE "-" "_" _test ${_f})
      add_test(NAME capdb_net_${_test}
        COMMAND ${_capdb_test_env} $<TARGET_FILE:capsuite> --filter ${_f})
      set_tests_properties(capdb_net_${_test} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
    endforeach()
  endif()

  add_test(NAME capdb_test_depth_smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/capdb-test-depth-smoke.sh"
      "$<TARGET_FILE:capdb_cli>"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(capdb_test_depth_smoke PROPERTIES TIMEOUT 90)

  add_test(NAME capdb_install_tree_smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/capdb-install-smoke.sh"
      "${CMAKE_BINARY_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}/install-smoke-ctest")
  set_tests_properties(capdb_install_tree_smoke PROPERTIES TIMEOUT 120)

  add_test(NAME capdb_release_artifact_smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release.sh"
      "${CMAKE_CURRENT_BINARY_DIR}/release-smoke"
      --skip-tests)
  set_tests_properties(capdb_release_artifact_smoke PROPERTIES TIMEOUT 240)

  add_test(NAME capdb_upstream_sync_check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/upstream-sync-check.sh"
      "${CMAKE_BINARY_DIR}"
      --quick)
  set_tests_properties(capdb_upstream_sync_check PROPERTIES TIMEOUT 90)

  if(CAPDB_ENABLE_STORE)
    add_test(NAME capdb_ops_smoke
      COMMAND ${CMAKE_COMMAND} -E env
        CAPDB=$<TARGET_FILE:capdb_cli>
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/capdb-ops-smoke.sh"
        "$<TARGET_FILE:capdb-ctl>")
    set_tests_properties(capdb_ops_smoke PROPERTIES TIMEOUT 90)
  endif()
endif()

enable_testing()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS capdb capdb_shared capdb_cli
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
)
install(FILES "${CAPDB_H}" core/capdbext.h DESTINATION include)
install(FILES compat/sqlite3_capdb.h DESTINATION include)
if(CAPDB_ENABLE_NETWORK)
  install(TARGETS capdb-server RUNTIME DESTINATION bin)
  install(TARGETS capdb_client ARCHIVE DESTINATION lib)
  if(TARGET capdb_jni)
    install(TARGETS capdb_jni
      LIBRARY DESTINATION lib
      ARCHIVE DESTINATION lib
      RUNTIME DESTINATION bin)
    install(FILES bindings/java/org/capdb/jni/wrapper1/Capdb.java
      DESTINATION share/capdb/java/org/capdb/jni/wrapper1)
  endif()
  install(FILES capdb/client/capdb_client.h DESTINATION include)
endif()
if(CAPDB_ENABLE_POOL)
  install(FILES capdb/pool/capdb_pool.h DESTINATION include)
endif()
if(CAPDB_ENABLE_STORE)
  install(TARGETS capdb-ctl RUNTIME DESTINATION bin)
  install(FILES
    capdb/store/capdb_store.h
    capdb/store/capdb_store_format.h
    DESTINATION include/capdb/store
  )
  install(FILES capdb/cluster/capdb_cluster.h
    DESTINATION include/capdb/cluster
  )
  if(CAPDB_ENABLE_REPLICATION)
    install(FILES capdb/replication/capdb_rep.h
      DESTINATION include/capdb/replication
    )
  endif()
endif()

install(FILES man/capdb.1 DESTINATION share/man/man1)
if(CAPDB_ENABLE_NETWORK)
  install(FILES man/capdb-server.1 DESTINATION share/man/man1)
endif()
if(CAPDB_ENABLE_STORE)
  install(FILES man/capdb-ctl.1 DESTINATION share/man/man1)
endif()
install(FILES "${CAPDB_C}" DESTINATION src)

# Install pkg-config file
configure_file(packaging/capdb.pc.in capdb.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/capdb.pc"
  DESTINATION lib/pkgconfig)
if(CAPDB_ENABLE_NETWORK)
  configure_file(libcapdb_client.pc.in libcapdb_client.pc @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libcapdb_client.pc"
    DESTINATION lib/pkgconfig)
endif()

# Install build helper scripts
install(PROGRAMS capdb-go-build-env.sh DESTINATION bin RENAME capdb-go-env)
install(PROGRAMS capdb-rust-build-env.sh DESTINATION bin RENAME capdb-rust-env)
install(PROGRAMS capdb-python-build-env.sh DESTINATION bin RENAME capdb-python-env)

# ---------------------------------------------------------------------------
# CPack
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "capdb")
set(CPACK_PACKAGE_VENDOR "rickcollette")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "CapDB SQL engine with connection pooling and TLS networking")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/rickcollette/capdb")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md")
set(CPACK_PACKAGE_FILE_NAME
    "capdb-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "capdb-${PROJECT_VERSION}-src")
set(CPACK_SOURCE_IGNORE_FILES
  "/build[^/]*/;/dist/;/legacy/;/tests/archive/;/\\\\.git/;\\\\.o$;\\\\.a$"
)
include(CPack)

# Targets that compile against generated headers must wait for capdb_codegen.
foreach(_capdb_codegen_consumer IN ITEMS
    capdb_client capdb_store capdb-server capdb-ctl
    threadtest_pool capdbtest capdb_nettest store_test capsuite capdb_jni
)
  if(TARGET ${_capdb_codegen_consumer})
    add_dependencies(${_capdb_codegen_consumer} capdb_codegen)
  endif()
endforeach()

# ---------------------------------------------------------------------------
# clean-all — remove generated sources, lemon debris, and test temp dirs that
# `make clean` in a single build tree leaves behind. Prefer repo-root
# `make clean` (scripts/clean.sh) to sweep all build*/ trees.
# ---------------------------------------------------------------------------
add_custom_target(clean-all
  COMMAND "${CMAKE_SOURCE_DIR}/scripts/clean.sh" clean
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Clean all build trees, generated sources, and test artifacts"
  USES_TERMINAL
  VERBATIM
)
