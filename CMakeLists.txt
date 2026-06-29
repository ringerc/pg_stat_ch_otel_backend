cmake_minimum_required(VERSION 3.16)
project(pg_stat_ch VERSION 0.1.0 LANGUAGES CXX C)

if(WIN32)
  message(FATAL_ERROR "Windows is not supported")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(GitVersion)
get_git_version(GIT_VERSION)
message(STATUS "Version: ${GIT_VERSION}")

find_package(PostgreSQLServer REQUIRED)
message(STATUS "PostgreSQL ${PostgreSQLServer_VERSION} found")

include(CompilerWarnings)

# ---------------------------------------------------------------------------
# Source layout
#
# C plugin layer: src/**/*.c  (must stay pure C to avoid C++ exception
#   crossing PostgreSQL's setjmp/longjmp frames)
# C++ exporter layer: src/export/*.cc  (otel_api integration)
#
# The offending-sources check enforces this: .cc is allowed only under
# src/export/.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PG_STAT_CH_PLUGIN_SOURCES CONFIGURE_DEPENDS src/*.c)
file(GLOB_RECURSE PG_STAT_CH_EXPORTER_SOURCES CONFIGURE_DEPENDS src/export/*.cc)

file(GLOB_RECURSE PG_STAT_CH_OFFENDING_SOURCES CONFIGURE_DEPENDS
  src/*.cc src/*.cxx src/*.cpp src/*.c++
)
list(FILTER PG_STAT_CH_OFFENDING_SOURCES
  EXCLUDE REGEX "^${CMAKE_SOURCE_DIR}/src/export/.*\\.cc$")
if(PG_STAT_CH_OFFENDING_SOURCES)
  message(FATAL_ERROR
    "pg_stat_ch: C++ sources are allowed only as *.cc under src/export/.  "
    "Offending:\n  ${PG_STAT_CH_OFFENDING_SOURCES}")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(${PG_STAT_CH_EXPORTER_SOURCES}
    PROPERTIES COMPILE_OPTIONS
      "-Wglobal-constructors;-Werror=global-constructors")
endif()

add_library(pg_stat_ch MODULE
  ${PG_STAT_CH_PLUGIN_SOURCES}
  ${PG_STAT_CH_EXPORTER_SOURCES}
)

target_compile_features(pg_stat_ch PRIVATE cxx_std_17)
target_compile_definitions(pg_stat_ch PRIVATE PG_STAT_CH_VERSION="${GIT_VERSION}")
target_compile_options(pg_stat_ch PRIVATE -include libintl.h)

target_include_directories(pg_stat_ch PRIVATE
  include
  src
)

# otel_api headers — loaded at runtime via shared_preload_libraries, no link dep.
# Override via -DOTEL_API_INCLUDE_DIR=... (e.g. /src/postgres/contrib in the Dockerfile).
set(OTEL_API_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/../../postgres_otel_api"
    CACHE PATH "Parent dir containing otel_api/ include directory (local default)")
target_include_directories(pg_stat_ch SYSTEM PRIVATE
  ${OTEL_API_INCLUDE_DIR}
)

target_link_libraries(pg_stat_ch PRIVATE
  PostgreSQLServer::PostgreSQLServer
)

pg_stat_ch_set_warnings(pg_stat_ch)

set_target_properties(pg_stat_ch PROPERTIES
  PREFIX ""
  SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
)

if(APPLE)
  execute_process(COMMAND ${PG_CONFIG} --bindir
                  OUTPUT_VARIABLE PG_BINDIR
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  target_link_options(pg_stat_ch PRIVATE -bundle_loader ${PG_BINDIR}/postgres)
endif()

install(TARGETS pg_stat_ch LIBRARY DESTINATION ${PostgreSQLServer_PKGLIB_DIR})
install(FILES pg_stat_ch.control DESTINATION ${PostgreSQLServer_SHARE_DIR}/extension)
file(GLOB SQL_FILES sql/pg_stat_ch--*.sql)
install(FILES ${SQL_FILES} DESTINATION ${PostgreSQLServer_SHARE_DIR}/extension)
